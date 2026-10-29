#include "Lighting.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/TextureCube.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureLightProfile.h"
#include "IESConverter.h"
#include "Misc/FileHelper.h"
#include "UObject/Package.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Math/Float16Color.h"
#include "TextureResource.h"

namespace {
FVector Axis(const HdueLight& Light,int32 Index) {
    return FVector(Light.axes[Index*3],Light.axes[Index*3+1],Light.axes[Index*3+2]);
}
FMatrix Frame(const HdueLight& Light) {
    return FMatrix(FPlane(Axis(Light,0),0),FPlane(Axis(Light,1),0),FPlane(Axis(Light,2),0),FPlane(0,0,0,1));
}
FVector CubeDirection(int32 Face,double U,double V) {
    switch(Face) {
    case 0: return FVector(1,-V,-U).GetSafeNormal();
    case 1: return FVector(-1,-V,U).GetSafeNormal();
    case 2: return FVector(U,1,V).GetSafeNormal();
    case 3: return FVector(U,-1,-V).GetSafeNormal();
    case 4: return FVector(U,-V,1).GetSafeNormal();
    default:return FVector(-U,-V,-1).GetSafeNormal();
    }
}
FLinearColor SampleLatlong(const FImage& Image,const FVector& Direction) {
    const double U=.5-FMath::Atan2(Direction.X,Direction.Z)/(2.*UE_PI);
    const double V=FMath::Acos(FMath::Clamp(Direction.Y,-1.,1.))/UE_PI;
    const double X=U*Image.SizeX-.5,Y=FMath::Clamp(V*Image.SizeY-.5,0.,double(Image.SizeY-1));
    const int32 IX=FMath::FloorToInt(X),IY=FMath::FloorToInt(Y);
    const float FX=float(X-IX),FY=float(Y-IY);
    const auto* Pixels=reinterpret_cast<const FLinearColor*>(Image.RawData.GetData());
    const auto Read=[&](int32 A,int32 B) {
        return Pixels[FMath::Clamp(B,0,Image.SizeY-1)*int64(Image.SizeX)+(A%Image.SizeX+Image.SizeX)%Image.SizeX];
    };
    return FMath::Lerp(FMath::Lerp(Read(IX,IY),Read(IX+1,IY),FX),FMath::Lerp(Read(IX,IY+1),Read(IX+1,IY+1),FX),FY);
}
bool Text(const char* Value) {return Value && Value[0] && strnlen(Value,32769)<=32768;}
}

FHdueLighting::FHdueLighting(UWorld* InWorld):World(InWorld) {}
FHdueLighting::~FHdueLighting() {Clear();}
void FHdueLighting::Clear() {
    for(auto& Entry:Actors) if(IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
    Actors.Empty(); Definitions.Empty(); Images.Empty(); LightTextures.Empty();
    if(IsValid(SkyActor)) World->DestroyActor(SkyActor);
    SkyActor=nullptr; Sky=nullptr; Environment.Reset(); DiskTexture.Reset();
    EnvironmentDirty=false; ShowBackground=false; DomeCount=Resolution=0; MaximumRadiance=0;
    LastError.Empty(); CaptureFence.BeginFence();
}
void FHdueLighting::Remove(const FString& Id) {
    if(auto* Actor=Actors.Find(Id)) {if(IsValid(*Actor)) World->DestroyActor(*Actor); Actors.Remove(Id);}
    if(const auto* Definition=Definitions.Find(Id)) EnvironmentDirty|=Definition->Data.type==HDUE_DOME_LIGHT;
    Definitions.Remove(Id);
}
bool FHdueLighting::Apply(const HdueLightBatch& Batch,FString& Error) {
    if(Batch.struct_size!=sizeof(Batch) || Batch.light_count>65536 || (Batch.light_count && !Batch.lights)) {
        Error=TEXT("Invalid extended light batch."); return false;
    }
    TArray<FDefinition> Pending; TSet<FString> Ids;
    for(uint32 I=0;I<Batch.light_count;++I) {
        const auto& D=Batch.lights[I];
        if(D.struct_size!=sizeof(D) || D.type>HDUE_CYLINDER_LIGHT || D.visible>1 || (D.flags&~31u) || !Text(D.id) ||
            D.cube_resolution<32 || D.cube_resolution>2048 || !FMath::IsPowerOfTwo(D.cube_resolution)) {
            Error=TEXT("Unsupported light descriptor or environment resolution."); return false;
        }
        if(D.texture_format>HDUE_ENVIRONMENT_LATLONG) {
            // angular, mirroredBall and cubeMapVerticalCross: this light only stays dark.
            UE_LOG(LogTemp, Warning, TEXT("hdUE light %s: only latlong (automatic) dome images are supported; light skipped."), UTF8_TO_TCHAR(D.id));
            continue;
        }
        for(double Value:D.axes) if(!FMath::IsFinite(Value)) {Error=TEXT("Nonfinite light orientation."); return false;}
        for(double Value:D.position_cm) if(!FMath::IsFinite(Value)) {Error=TEXT("Nonfinite light position."); return false;}
        for(float Value:D.color) if(!FMath::IsFinite(Value) || Value<0) {Error=TEXT("Invalid light color."); return false;}
        for(float Value:{D.intensity,D.width_cm,D.height_cm,D.length_cm,D.angle_degrees,D.diffuse,D.specular,D.cone_angle,D.cone_softness,D.ies_angle_scale})
            if(!FMath::IsFinite(Value) || Value<0) {Error=TEXT("Invalid light parameter."); return false;}
        if(FMath::Abs(Frame(D).Determinant())<1e-8 || D.angle_degrees>180 || D.cone_angle>180 || D.cone_softness>1) {
            Error=TEXT("Degenerate light frame or invalid cone/angle."); return false;
        }
        FDefinition Definition; Definition.Data=D; Definition.Id=UTF8_TO_TCHAR(D.id);
        Definition.File=D.texture_file?UTF8_TO_TCHAR(D.texture_file):TEXT(""); Definition.Data.id=nullptr; Definition.Data.texture_file=nullptr;
        Definition.Ies=D.ies_file?UTF8_TO_TCHAR(D.ies_file):TEXT(""); Definition.Data.ies_file=nullptr;
        if(Ids.Contains(Definition.Id)) {Error=TEXT("Duplicate light ID."); return false;} Ids.Add(Definition.Id);
        // Valid UsdLux that UE cannot show degrades this light only, with a warning, as Karma and Storm ignore what
        // they do not support; one such light no longer turns off every other light.
        const auto Ignore=[&](const TCHAR* What) {UE_LOG(LogTemp, Warning, TEXT("hdUE light %s: %s; ignored."), *Definition.Id, What);};
        if(D.type==HDUE_DOME_LIGHT && (D.diffuse!=1 || D.specular!=1)) {
            Ignore(TEXT("dome diffuse/specular multipliers (UE's sky light has one environment)")); Definition.Data.diffuse=Definition.Data.specular=1;
        }
        if(!Definition.Ies.IsEmpty() && (D.type==HDUE_DOME_LIGHT || D.type==HDUE_DISTANT_LIGHT || D.type==HDUE_CAMERA_DISTANT_LIGHT)) {
            Ignore(TEXT("an IES profile on a distant or dome light")); Definition.Ies.Empty();
        }
        if(FString ProfileError; !Definition.Ies.IsEmpty() && !Profile(Definition.Ies,ProfileError)) {Ignore(*ProfileError); Definition.Ies.Empty();}
        if(!Definition.File.IsEmpty() && D.type!=HDUE_DOME_LIGHT && D.type!=HDUE_RECT_LIGHT) {
            Ignore(TEXT("an image on a light other than DomeLight and RectLight")); Definition.File.Empty();
        }
        if(!Definition.File.IsEmpty()) {
            const auto Stat=IFileManager::Get().GetStatData(*Definition.File);
            auto* Cached=Images.Find(Definition.File);
            FString ImageError;
            if(!Stat.bIsValid || Stat.bIsDirectory) ImageError=TEXT("its image is not a readable file: ")+Definition.File;
            else if(!Cached || Cached->Modified!=Stat.ModificationTime || Cached->Size!=Stat.FileSize) {
                FImage Image;
                FImageEntry Entry; Entry.Modified=Stat.ModificationTime; Entry.Size=Stat.FileSize; Entry.Image=MakeShared<FImage>();
                if(!FImageUtils::LoadImage(*Definition.File,Image) || Image.NumSlices!=1 || Image.SizeX>32768 || Image.SizeY>16384)
                    ImageError=TEXT("cannot decode its image: ")+Definition.File;
                else {
                    Image.CopyTo(*Entry.Image,ERawImageFormat::RGBA32F,EGammaSpace::Linear);
                    for(const auto& Pixel:Entry.Image->AsRGBA32F()) if(!FMath::IsFinite(Pixel.R) || !FMath::IsFinite(Pixel.G) || !FMath::IsFinite(Pixel.B)) {
                        ImageError=TEXT("its HDR image has nonfinite pixels: ")+Definition.File; break;
                    }
                }
                if(ImageError.IsEmpty()) {Images.Add(Definition.File,MoveTemp(Entry)); LightTextures.Remove(Definition.File);}
            }
            if(!ImageError.IsEmpty()) {Ignore(*ImageError); Definition.File.Empty();}
            else {
                const auto& Image=*Images[Definition.File].Image;
                // Sampling is UV-normalized, so other aspects stretch over the sphere as in Karma and Storm.
                if(D.type==HDUE_DOME_LIGHT && Image.SizeX!=Image.SizeY*2) UE_LOG(LogTemp, Warning, TEXT("hdUE dome image %s is %dx%d, not 2:1; stretching it as a latitude-longitude map."), *Definition.File, Image.SizeX, Image.SizeY);
            }
        }
        Pending.Add(MoveTemp(Definition));
    }
    for(const auto& Definition:Pending) {
        if(const auto* Previous=Definitions.Find(Definition.Id)) EnvironmentDirty|=Previous->Data.type==HDUE_DOME_LIGHT;
        EnvironmentDirty|=Definition.Data.type==HDUE_DOME_LIGHT;
        if(auto* Actor=Actors.Find(Definition.Id)) {if(IsValid(*Actor)) World->DestroyActor(*Actor); Actors.Remove(Definition.Id);}
        Definitions.Add(Definition.Id,Definition);
        if(Definition.Data.type!=HDUE_DOME_LIGHT && !UpdateLocal(Definition,Error)) return false;
    }
    LastError.Empty();
    return true;
}
bool FHdueLighting::UpdateLocal(const FDefinition& Definition,FString& Error) {
    const auto& D=Definition.Data;
    auto* Actor=World->SpawnActor<AActor>(); Actors.Add(Definition.Id,Actor);
    const bool Distant=D.type==HDUE_DISTANT_LIGHT || D.type==HDUE_CAMERA_DISTANT_LIGHT;
    ULightComponent* Light=nullptr;
    if(Distant) Light=NewObject<UDirectionalLightComponent>(Actor);
    else if(D.type==HDUE_RECT_LIGHT || D.type==HDUE_DISK_LIGHT) Light=NewObject<URectLightComponent>(Actor);
    // ShapingAPI's default cone (90) already cuts emission beyond it: a sphere light is a spot up to 90 degrees.
    else if(D.type==HDUE_SPHERE_LIGHT && D.cone_angle<=90) Light=NewObject<USpotLightComponent>(Actor);
    else Light=NewObject<UPointLightComponent>(Actor);
    static bool ConeWarned=false;
    if(!ConeWarned && D.cone_angle<90 && (D.type==HDUE_RECT_LIGHT || D.type==HDUE_DISK_LIGHT || D.type==HDUE_CYLINDER_LIGHT)) {
        ConeWarned=true;
        UE_LOG(LogTemp, Warning, TEXT("hdUE light %s: shaping cones on rect, disk and cylinder lights are ignored (UE spots are sphere lights)."), *Definition.Id);
    }
    Light->SetMobility(EComponentMobility::Movable); Actor->SetRootComponent(Light); Actor->AddInstanceComponent(Light);
    Light->SetLightColor(FLinearColor(D.color[0],D.color[1],D.color[2]));
    Light->SetDiffuseScale(D.diffuse); Light->SetSpecularScale(D.specular);
    Light->SetCastShadows((D.flags&HDUE_LIGHT_SHADOWS)!=0);
    Light->bAllowMegaLights=!Distant && (D.flags&HDUE_LIGHT_MEGALIGHTS)!=0;
    Actor->SetActorLocation(FVector(D.position_cm[0],D.position_cm[1],D.position_cm[2]));
    Actor->SetActorRotation(FRotationMatrix::MakeFromXZ(-Axis(D,2),Axis(D,1)).Rotator());
    // UsdLux intensity is luminance in nits for every light; normalize divides by the size factor.
    float Intensity=D.intensity;
    if(auto* Local=Cast<ULocalLightComponent>(Light)) {
        Local->SetAttenuationRadius(100000);
        const double Radius=D.width_cm*.01;
        double Projected=UE_PI*Radius*Radius,Area=4*Projected;
        if(D.type==HDUE_RECT_LIGHT) Projected=Area=D.width_cm*D.height_cm*.0001;
        if(D.type==HDUE_DISK_LIGHT) Area=Projected=UE_PI*D.width_cm*D.height_cm*.0001;
        if(D.type==HDUE_CYLINDER_LIGHT) {Projected=2*Radius*D.length_cm*.01+UE_PI*Radius*Radius; Area=2*UE_PI*Radius*D.length_cm*.01;}
        if(D.flags&HDUE_LIGHT_NORMALIZE) Intensity/=float(FMath::Max(Area,1e-12));
        if(D.type==HDUE_RECT_LIGHT || D.type==HDUE_DISK_LIGHT) {
            // UE nits use the rect's own area, but its rect lights measured 2.05x a Lambertian emitter
            // (Lumen and Path Tracer, 0.5 m rect at 1 m); halve to match UsdLux luminance.
            Local->SetIntensityUnits(ELightUnits::Nits); Intensity*=.5f;
        } else {
            // UE point-light nits spread over the whole capsule surface; UsdLux luminance seen
            // from outside is luminous intensity over the projected area.
            Local->SetIntensityUnits(ELightUnits::Candelas); Intensity*=float(Projected);
        }
    } else if(D.type==HDUE_DISTANT_LIGHT) {
        // UsdLux: illuminance = luminance * pi * sin^2(angle/2); normalize makes intensity the illuminance in lux.
        const double Half=FMath::Clamp(FMath::DegreesToRadians(double(D.angle_degrees))/2,0.,UE_DOUBLE_PI);
        const double Sin2=FMath::Square(FMath::Sin(Half));
        const double SizeFactor=Half<=UE_DOUBLE_HALF_PI?UE_DOUBLE_PI*Sin2:(2-Sin2)*UE_DOUBLE_PI;
        if(!(D.flags&HDUE_LIGHT_NORMALIZE) && Half>0) Intensity*=float(SizeFactor);
    }
    if(auto* Point=Cast<UPointLightComponent>(Light)) {
        Point->SetSourceRadius((D.flags&HDUE_LIGHT_POINT)?0:D.width_cm);
        if(D.type==HDUE_CYLINDER_LIGHT) {Point->SetSourceLength(D.length_cm); Actor->SetActorRotation(FRotationMatrix::MakeFromZX(Axis(D,0),Axis(D,1)).Rotator());}
    }
    if(auto* Spot=Cast<USpotLightComponent>(Light)) {
        Spot->SetOuterConeAngle(FMath::Clamp(D.cone_angle,.01f,89.9f));
        Spot->SetInnerConeAngle(FMath::Clamp(D.cone_angle*(1-D.cone_softness),0.f,Spot->OuterConeAngle));
    }
    if(auto* Sun=Cast<UDirectionalLightComponent>(Light)) Sun->LightSourceAngle=D.angle_degrees;
    if(auto* Rect=Cast<URectLightComponent>(Light)) {
        Rect->SetSourceWidth(D.type==HDUE_DISK_LIGHT?D.width_cm*2:D.width_cm);
        Rect->SetSourceHeight(D.type==HDUE_DISK_LIGHT?D.height_cm*2:D.height_cm);
        // UsdLux rect lights have no barn doors; UE.s default 20 cm flaps cut grazing emission; open them fully and shorten them to the minimum.
        Rect->SetBarnDoorAngle(GetRectLightBarnDoorMaxAngle()); Rect->SetBarnDoorLength(0.1f);
        if(D.type==HDUE_DISK_LIGHT) {
            if(!DiskTexture.IsValid()) {
                DiskTexture.Reset(UTexture2D::CreateTransient(64,64,PF_FloatRGBA)); DiskTexture->SRGB=false; DiskTexture->NeverStream=true;
                auto& Mip=DiskTexture->GetPlatformData()->Mips[0]; auto* Pixels=static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
                for(int32 Y=0;Y<64;++Y) for(int32 X=0;X<64;++X) {
                    const float R=FMath::Sqrt(FMath::Square((X+.5f)/32-1)+FMath::Square((Y+.5f)/32-1));
                    const float Value=FMath::Clamp((1-R)*32,0.f,1.f); Pixels[Y*64+X]=FFloat16Color(FLinearColor(Value,Value,Value,1));
                }
                Mip.BulkData.Unlock(); DiskTexture->UpdateResource();
            }
            Rect->SetSourceTexture(DiskTexture.Get());
        } else if(!Definition.File.IsEmpty()) {
            if(!LightTextures.Contains(Definition.File)) {
                const auto& Image=*Images[Definition.File].Image;
                TStrongObjectPtr<UTexture2D> Texture(UTexture2D::CreateTransient(Image.SizeX,Image.SizeY,PF_FloatRGBA));
                Texture->SRGB=false; Texture->NeverStream=true;
                auto& Mip=Texture->GetPlatformData()->Mips[0]; auto* Pixels=static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
                const auto Source=Image.AsRGBA32F();
                for(int64 I=0;I<Source.Num();++I) Pixels[I]=FFloat16Color(Source[I]);
                Mip.BulkData.Unlock(); Texture->UpdateResource(); LightTextures.Add(Definition.File,MoveTemp(Texture));
            }
            Rect->SetSourceTexture(LightTextures[Definition.File].Get());
        }
    }
    if(auto* Local=Cast<ULocalLightComponent>(Light); Local && !Definition.Ies.IsEmpty()) {
        // UE has no IES angle scale; the profile keeps its own angles.
        if(D.ies_angle_scale!=0) UE_LOG(LogTemp, Warning, TEXT("hdUE: shaping:ies:angleScale is not supported (%s)"), *Definition.Id);
        UTextureLightProfile* Texture=Profile(Definition.Ies,Error);
        if(!Texture) return false;
        // UsdLux scales the light by the profile's candela values (iesSample * L). UE's texture holds them over their
        // maximum (IESLoader), which it keeps in Brightness: multiply it back. bUseIESBrightness would instead replace
        // the USD intensity. normalize keeps the unshaped light's power: divide by the profile's mean over the sphere.
        Local->SetIESTexture(Texture); Local->bUseIESBrightness=false;
        const float Mean=Profiles[Definition.Ies].Mean;
        Intensity*=D.ies_normalize ? (Mean>0 ? 1/Mean : 1.f) : Texture->Brightness;
    }
    Light->SetIntensity(Intensity); Light->RegisterComponent(); Actor->SetActorHiddenInGame(!D.visible);
    return true;
}
UTextureLightProfile* FHdueLighting::Profile(const FString& File,FString& Error) {
    const auto Stat=IFileManager::Get().GetStatData(*File);
    if(!Stat.bIsValid || Stat.bIsDirectory) {Error=TEXT("IES profile is not a readable file: ")+File; return nullptr;}
    if(const auto* Cached=Profiles.Find(File); Cached && Cached->Modified==Stat.ModificationTime && Cached->Size==Stat.FileSize) return Cached->Texture.Get();
    TArray<uint8> Bytes;
    if(!FFileHelper::LoadFileToArray(Bytes,*File)) {Error=TEXT("Cannot read IES profile: ")+File; return nullptr;}
    FIESConverter Converter(Bytes.GetData(),uint32(Bytes.Num()));
    if(!Converter.IsValid()) {Error=FString::Printf(TEXT("Invalid IES profile %s: %s"),*File,Converter.GetError()); return nullptr;}
    // The editor imports IES as RGBA16F source data; build the same texture's platform data directly.
    const int32 Width=int32(Converter.GetWidth()),Height=int32(Converter.GetHeight());
    if(Converter.GetRawData().Num()!=int64(Width)*Height*int64(sizeof(FFloat16Color))) {Error=TEXT("Unexpected IES texture layout: ")+File; return nullptr;}
    auto* Texture=NewObject<UTextureLightProfile>(GetTransientPackage(),NAME_None,RF_Transient);
    auto* Data=new FTexturePlatformData(); Data->SizeX=Width; Data->SizeY=Height; Data->SetNumSlices(1); Data->PixelFormat=PF_FloatRGBA;
    auto* Mip=new FTexture2DMipMap(Width,Height,1); Data->Mips.Add(Mip);
    Mip->BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(Mip->BulkData.Realloc(Converter.GetRawData().Num()),Converter.GetRawData().GetData(),Converter.GetRawData().Num());
    Mip->BulkData.Unlock();
    Texture->SetPlatformData(Data);
    Texture->AddressX=TA_Clamp; Texture->AddressY=TA_Clamp; Texture->SRGB=false; Texture->NeverStream=true;
    Texture->CompressionSettings=TC_HDR; Texture->Filter=TF_Bilinear;
    Texture->Brightness=Converter.GetBrightness(); Texture->TextureMultiplier=Converter.GetMultiplier();
    Texture->UpdateResource();
    // Mean of the normalized profile over the sphere: columns are vertical angles 0..180 degrees (IESLoader), each
    // weighted by its solid angle; rows are horizontal angles.
    const auto* Texels=reinterpret_cast<const FFloat16Color*>(Converter.GetRawData().GetData());
    double Sum=0,Weight=0;
    for(int32 X=0;X<Width;++X) {
        const double W=FMath::Sin((X+.5)/Width*UE_DOUBLE_PI);
        for(int32 Y=0;Y<Height;++Y) {Sum+=W*Texels[int64(Y)*Width+X].R.GetFloat(); Weight+=W;}
    }
    auto& Entry=Profiles.FindOrAdd(File); Entry.Modified=Stat.ModificationTime; Entry.Size=Stat.FileSize; Entry.Texture.Reset(Texture);
    Entry.Mean=float(Weight>0?Sum/Weight:0);
    return Texture;
}
bool FHdueLighting::RebuildEnvironment(FString& Error) {
    DomeCount=0; Resolution=0; ShowBackground=false; MaximumRadiance=0;
    TArray<const FDefinition*> Domes;
    for(const auto& Entry:Definitions) if(Entry.Value.Data.type==HDUE_DOME_LIGHT && Entry.Value.Data.visible && Entry.Value.Data.intensity>0) {
        Domes.Add(&Entry.Value); ++DomeCount; Resolution=FMath::Max(Resolution,Entry.Value.Data.cube_resolution);
        ShowBackground|=(Entry.Value.Data.flags&HDUE_LIGHT_BACKGROUND)!=0;
    }
    if(Domes.IsEmpty()) {
        if(IsValid(SkyActor)) World->DestroyActor(SkyActor);
        SkyActor=nullptr; Sky=nullptr; Environment.Reset(); RadianceScale=1; CaptureFence.BeginFence(); return true;
    }
    TArray<FLinearColor> Pixels; Pixels.SetNumZeroed(int64(Resolution)*Resolution*6);
    for(const auto* Dome:Domes) {
        const auto& D=Dome->Data; const auto Inverse=Frame(D).Inverse();
        const FLinearColor Weight(D.color[0]*D.intensity,D.color[1]*D.intensity,D.color[2]*D.intensity,0);
        const FImage* Image=Dome->File.IsEmpty()?nullptr:Images[Dome->File].Image.Get();
        for(int32 Face=0;Face<6;++Face) for(uint32 Y=0;Y<Resolution;++Y) for(uint32 X=0;X<Resolution;++X) {
            const FVector Local=Inverse.TransformVector(CubeDirection(Face,2*(X+.5)/Resolution-1,2*(Y+.5)/Resolution-1)).GetSafeNormal();
            auto Color=Image?SampleLatlong(*Image,Local):FLinearColor::White;
            Color.R=FMath::Max(0.f,Color.R); Color.G=FMath::Max(0.f,Color.G); Color.B=FMath::Max(0.f,Color.B);
            Pixels[(int64(Face)*Resolution+Y)*Resolution+X]+=Color*Weight;
        }
    }
    for(const auto& Color:Pixels) {
        if(!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G) || !FMath::IsFinite(Color.B)) {Error=TEXT("HDR environment overflow."); return false;}
        MaximumRadiance=FMath::Max(MaximumRadiance,FMath::Max3(Color.R,Color.G,Color.B));
    }
    // Keep bright sun texels representable in the half-float cube. Restore the
    // scale after convolution and during background drawing; never clamp to LDR.
    RadianceScale=FMath::Max(1.f,MaximumRadiance/1024.f);
    TStrongObjectPtr<UTextureCube> Cube(UTextureCube::CreateTransient(Resolution,Resolution,PF_FloatRGBA));
    Cube->SRGB=false; Cube->NeverStream=true;
    auto& Mip=Cube->GetPlatformData()->Mips[0]; auto* Destination=static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
    for(int64 I=0;I<Pixels.Num();++I) {auto Color=Pixels[I]/RadianceScale; Color.A=1; Destination[I]=FFloat16Color(Color);}
    Mip.BulkData.Unlock(); Cube->UpdateResource(); Environment=MoveTemp(Cube);
    if(!IsValid(SkyActor)) {
        SkyActor=World->SpawnActor<AActor>(); Sky=NewObject<USkyLightComponent>(SkyActor);
        Sky->SetMobility(EComponentMobility::Movable); Sky->SourceType=SLS_SpecifiedCubemap;
        Sky->bLowerHemisphereIsBlack=false; Sky->CubemapResolution=Resolution;
        SkyActor->SetRootComponent(Sky); SkyActor->AddInstanceComponent(Sky); Sky->RegisterComponent();
    }
    Sky->CubemapResolution=Resolution;
    Sky->SetIntensity(RadianceScale); Sky->SetLightColor(FLinearColor::White); Sky->SetCubemap(Environment.Get());
    Sky->SetCastShadows(Domes.ContainsByPredicate([](const auto* Dome){return (Dome->Data.flags&HDUE_LIGHT_SHADOWS)!=0;}));
    Sky->RecaptureSky(); USkyLightComponent::UpdateSkyCaptureContents(World); CaptureFence.BeginFence();
    return true;
}
void FHdueLighting::Tick() {
    if(EnvironmentDirty) {EnvironmentDirty=false; LastError.Empty(); RebuildEnvironment(LastError);}
}
bool FHdueLighting::Ready() const {return !EnvironmentDirty && LastError.IsEmpty() && CaptureFence.IsFenceComplete() && (!DomeCount || (Sky && Sky->GetProcessedSkyTexture()));}
bool FHdueLighting::BackgroundVisible() const {return DomeCount && ShowBackground;}
FTextureRHIRef FHdueLighting::EnvironmentTexture() const {return Environment.IsValid() && Environment->GetResource()?Environment->GetResource()->TextureRHI:FTextureRHIRef();}
void FHdueLighting::Status(HdueLightingStatus& Status) const {
    Status.light_count=Definitions.Num(); Status.dome_count=DomeCount; Status.environment_pending=!Ready();
    Status.cube_resolution=Resolution; Status.environment_max_radiance=MaximumRadiance;
}
void FHdueLighting::UpdateCamera(const FVector& Eye,const FVector& Target,const FVector& Up) {
    const FVector Forward=(Target-Eye).GetSafeNormal(),Right=FVector::CrossProduct(Up,Forward).GetSafeNormal();
    const FVector CameraUp=FVector::CrossProduct(Forward,Right).GetSafeNormal();
    for(const auto& Entry:Definitions) if(Entry.Value.Data.type==HDUE_CAMERA_DISTANT_LIGHT) {
        const auto* Actor=Actors.Find(Entry.Key); if(!Actor || !IsValid(*Actor)) continue;
        const FVector Relative=Axis(Entry.Value.Data,0);
        (*Actor)->SetActorRotation((Forward*Relative.X+Right*Relative.Y+CameraUp*Relative.Z).Rotation());
    }
}
