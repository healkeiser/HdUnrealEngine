#include "Lighting.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PrimitiveComponent.h"
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
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/UnrealType.h"

// Text for one property of Container (FPostProcessSettings, a component); struct fields can also be given as bare
// numbers, objects as cooked paths (/Game/..., /Engine/...), loaded here; empty or None clears an object. Shared with
// set_post_process (Bridge.cpp), compiled in the editor target too.
bool ImportPropertyValue(FProperty* Property, const FString& Text, void* Container) {
    void* Value = Property->ContainerPtrToValuePtr<void>(Container);
    if (const auto* Object = CastField<FObjectPropertyBase>(Property)) {
        const bool None = Text.IsEmpty() || Text == TEXT("None");
        UObject* Loaded = None ? nullptr : LoadObject<UObject>(nullptr, *Text);
        if (!None && (!Loaded || !Loaded->IsA(Object->PropertyClass))) return false;
        Object->SetObjectPropertyValue(Value, Loaded);
        return true;
    }
    const auto* Struct = CastField<FStructProperty>(Property);
    if (Struct && !Text.Contains(TEXT("="))) {
        TArray<FString> Numbers;
        Text.Replace(TEXT("("), TEXT(" "))
            .Replace(TEXT(")"), TEXT(" "))
            .Replace(TEXT(","), TEXT(" "))
            .ParseIntoArrayWS(Numbers);
        if (Struct->Struct == TBaseStructure<FColor>::Get()) {
            // Bare numbers are a linear colour (a USD color3f), stored as sRGB bytes as UE's colour picker does; the
            // fields themselves are in memory order (B, G, R, A).
            if (Numbers.Num() < 3 || Numbers.Num() > 4 ||
                Numbers.ContainsByPredicate([](const FString& N) { return !N.IsNumeric(); }))
                return false;
            const FLinearColor Linear(FCString::Atof(*Numbers[0]), FCString::Atof(*Numbers[1]),
                                      FCString::Atof(*Numbers[2]),
                                      Numbers.Num() == 4 ? FCString::Atof(*Numbers[3]) : 1.f);
            *static_cast<FColor*>(Value) = Linear.ToFColor(true);
            return true;
        }
        int32 Index = 0;
        for (TFieldIterator<FNumericProperty> Field(Struct->Struct); Field && Index < Numbers.Num(); ++Field, ++Index) {
            if (!Numbers[Index].IsNumeric()) return false;
            if (Field->IsFloatingPoint())
                Field->SetFloatingPointPropertyValue(Field->ContainerPtrToValuePtr<void>(Value),
                                                     FCString::Atod(*Numbers[Index]));
            else
                Field->SetIntPropertyValue(Field->ContainerPtrToValuePtr<void>(Value),
                                           FCString::Atoi64(*Numbers[Index]));
        }
        return Index > 0 && Index == Numbers.Num();
    }
    const TCHAR* End = Property->ImportText_Direct(*Text, Value, nullptr, PPF_None);
    return End && !*End;
}
namespace {
FVector Axis(const HdueLight& Light, int32 Index) {
    return FVector(Light.axes[Index * 3], Light.axes[Index * 3 + 1], Light.axes[Index * 3 + 2]);
}
FMatrix Frame(const HdueLight& Light) {
    return FMatrix(FPlane(Axis(Light, 0), 0), FPlane(Axis(Light, 1), 0), FPlane(Axis(Light, 2), 0), FPlane(0, 0, 0, 1));
}
FVector CubeDirection(int32 Face, double U, double V) {
    switch (Face) {
        case 0:
            return FVector(1, -V, -U).GetSafeNormal();
        case 1:
            return FVector(-1, -V, U).GetSafeNormal();
        case 2:
            return FVector(U, 1, V).GetSafeNormal();
        case 3:
            return FVector(U, -1, -V).GetSafeNormal();
        case 4:
            return FVector(U, -V, 1).GetSafeNormal();
        default:
            return FVector(-U, -V, -1).GetSafeNormal();
    }
}
FLinearColor SampleLatlong(const FImage& Image, const FVector& Direction) {
    const double U = .5 - FMath::Atan2(Direction.X, Direction.Z) / (2. * UE_PI);
    const double V = FMath::Acos(FMath::Clamp(Direction.Y, -1., 1.)) / UE_PI;
    const double X = U * Image.SizeX - .5, Y = FMath::Clamp(V * Image.SizeY - .5, 0., double(Image.SizeY - 1));
    const int32 IX = FMath::FloorToInt(X), IY = FMath::FloorToInt(Y);
    const float FX = float(X - IX), FY = float(Y - IY);
    const auto* Pixels = reinterpret_cast<const FLinearColor*>(Image.RawData.GetData());
    const auto Read = [&](int32 A, int32 B) {
        return Pixels[FMath::Clamp(B, 0, Image.SizeY - 1) * int64(Image.SizeX) +
                      (A % Image.SizeX + Image.SizeX) % Image.SizeX];
    };
    return FMath::Lerp(FMath::Lerp(Read(IX, IY), Read(IX + 1, IY), FX),
                       FMath::Lerp(Read(IX, IY + 1), Read(IX + 1, IY + 1), FX), FY);
}
bool Text(const char* Value) { return Value && Value[0] && strnlen(Value, 32769) <= 32768; }
}  // namespace

FHdueLighting::FHdueLighting(UWorld* InWorld) : World(InWorld) {}
FHdueLighting::~FHdueLighting() { Clear(); }
void FHdueLighting::Clear() {
    for (auto& Entry : Actors)
        if (IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
    Actors.Empty();
    Definitions.Empty();
    LightFunctions.Empty();
    Images.Empty();
    LightTextures.Empty();
    Objects.Empty();
    if (IsValid(SkyActor)) World->DestroyActor(SkyActor);
    SkyActor = nullptr;
    Sky = nullptr;
    Environment.Reset();
    DiskTexture.Reset();
    EnvironmentDirty = false;
    ShowBackground = false;
    DomeCount = Resolution = 0;
    MaximumRadiance = 0;
    LastError.Empty();
    CaptureFence.BeginFence();
}
void FHdueLighting::Remove(const FString& Id) {
    LightFunctions.Remove(Id);
    if (auto* Actor = Actors.Find(Id)) {
        if (IsValid(*Actor)) World->DestroyActor(*Actor);
        Actors.Remove(Id);
    }
    if (const auto* Definition = Definitions.Find(Id)) EnvironmentDirty |= Definition->Data.type == HDUE_DOME_LIGHT;
    Definitions.Remove(Id);
}
bool FHdueLighting::Apply(const HdueLightBatch& Batch, FString& Error) {
    if (Batch.struct_size != sizeof(Batch) || Batch.light_count > 65536 || (Batch.light_count && !Batch.lights) ||
        (Batch.flags & ~uint32(HDUE_LIGHT_BATCH_LINKS)) ||
        (Batch.object_count && (!Batch.object_ids || !Batch.object_links))) {
        Error = TEXT("Invalid extended light batch.");
        return false;
    }
    TMap<FString, uint32> Links;
    for (uint32 I = 0; I < Batch.object_count; ++I) {
        if (!Text(Batch.object_ids[I]) || (Batch.object_links[I] & ~uint32(HDUE_LINK_CHANNELS | HDUE_LINK_NO_SHADOW))) {
            Error = TEXT("Invalid light link.");
            return false;
        }
        Links.Add(UTF8_TO_TCHAR(Batch.object_ids[I]), Batch.object_links[I]);
    }
    TArray<FDefinition> Pending;
    TSet<FString> Ids;
    for (uint32 I = 0; I < Batch.light_count; ++I) {
        const auto& D = Batch.lights[I];
        if (D.struct_size != sizeof(D) || D.type > HDUE_CYLINDER_LIGHT || D.visible > 1 || (D.flags & ~31u) ||
            (D.lighting_channels & ~uint32(HDUE_LINK_CHANNELS)) || !Text(D.id) || D.cube_resolution < 32 ||
            D.cube_resolution > 2048 || !FMath::IsPowerOfTwo(D.cube_resolution) || D.property_count > 1024 ||
            (D.property_count && (!D.property_names || !D.property_values))) {
            Error = TEXT("Unsupported light descriptor or environment resolution.");
            return false;
        }
        TArray<TPair<FString, FString>> Properties;
        for (uint32 P = 0; P < D.property_count; ++P) {
            if (!Text(D.property_names[P]) || !D.property_values[P]) {
                Error = TEXT("Invalid light property.");
                return false;
            }
            Properties.Emplace(UTF8_TO_TCHAR(D.property_names[P]), UTF8_TO_TCHAR(D.property_values[P]));
        }
        if (D.texture_format > HDUE_ENVIRONMENT_LATLONG) {
            // angular, mirroredBall and cubeMapVerticalCross: this light only stays dark.
            UE_LOG(LogTemp, Warning,
                   TEXT("hdUE light %s: only latlong (automatic) dome images are supported; light skipped."),
                   UTF8_TO_TCHAR(D.id));
            continue;
        }
        for (double Value : D.axes)
            if (!FMath::IsFinite(Value)) {
                Error = TEXT("Nonfinite light orientation.");
                return false;
            }
        for (double Value : D.position_cm)
            if (!FMath::IsFinite(Value)) {
                Error = TEXT("Nonfinite light position.");
                return false;
            }
        for (float Value : D.color)
            if (!FMath::IsFinite(Value) || Value < 0) {
                Error = TEXT("Invalid light color.");
                return false;
            }
        for (float Value : {D.intensity, D.width_cm, D.height_cm, D.length_cm, D.angle_degrees, D.diffuse, D.specular,
                            D.cone_angle, D.cone_softness, FMath::Abs(D.ies_angle_scale)})
            if (!FMath::IsFinite(Value) || Value < 0) {
                Error = TEXT("Invalid light parameter.");
                return false;
            }
        if (FMath::Abs(Frame(D).Determinant()) < 1e-8 || D.angle_degrees > 180 || D.cone_angle > 180 ||
            D.cone_softness > 1) {
            Error = TEXT("Degenerate light frame or invalid cone/angle.");
            return false;
        }
        FDefinition Definition;
        Definition.Data = D;
        Definition.Id = UTF8_TO_TCHAR(D.id);
        Definition.File = D.texture_file ? UTF8_TO_TCHAR(D.texture_file) : TEXT("");
        Definition.Data.id = nullptr;
        Definition.Data.texture_file = nullptr;
        Definition.Ies = D.ies_file ? UTF8_TO_TCHAR(D.ies_file) : TEXT("");
        Definition.Data.ies_file = nullptr;
        Definition.Properties = MoveTemp(Properties);
        Definition.Data.property_names = Definition.Data.property_values = nullptr;
        Definition.Data.property_count = 0;
        if (Ids.Contains(Definition.Id)) {
            Error = TEXT("Duplicate light ID.");
            return false;
        }
        Ids.Add(Definition.Id);
        // Valid UsdLux that UE cannot show degrades this light only, with a warning, as Karma and Storm ignore what
        // they do not support; one such light no longer turns off every other light.
        const auto Ignore = [&](const TCHAR* What) {
            UE_LOG(LogTemp, Warning, TEXT("hdUE light %s: %s; ignored."), *Definition.Id, What);
        };
        if (D.type == HDUE_DOME_LIGHT && (D.diffuse != 1 || D.specular != 1)) {
            Ignore(TEXT("dome diffuse/specular multipliers (UE's sky light has one environment)"));
            Definition.Data.diffuse = Definition.Data.specular = 1;
        }
        if (!Definition.Ies.IsEmpty() &&
            (D.type == HDUE_DOME_LIGHT || D.type == HDUE_DISTANT_LIGHT || D.type == HDUE_CAMERA_DISTANT_LIGHT)) {
            Ignore(TEXT("an IES profile on a distant or dome light"));
            Definition.Ies.Empty();
        }
        if (FString ProfileError;
            !Definition.Ies.IsEmpty() && !Profile(Definition.Ies, D.ies_angle_scale, ProfileError)) {
            Ignore(*ProfileError);
            Definition.Ies.Empty();
        }
        if (!Definition.File.IsEmpty() && D.type != HDUE_DOME_LIGHT && D.type != HDUE_RECT_LIGHT) {
            Ignore(TEXT("an image on a light other than DomeLight and RectLight"));
            Definition.File.Empty();
        }
        Definition.LightFunction = D.light_function_file ? UTF8_TO_TCHAR(D.light_function_file) : TEXT("");
        Definition.Data.light_function_file = nullptr;
        if (!Definition.LightFunction.IsEmpty() && D.type == HDUE_DOME_LIGHT) {
            Ignore(TEXT("a light function on a DomeLight (UE's sky light has none)"));
            Definition.LightFunction.Empty();
        }
        if (FString ImageError =
                Definition.LightFunction.IsEmpty() ? FString() : LoadImageFile(Definition.LightFunction);
            !ImageError.IsEmpty()) {
            Ignore(*(TEXT("light function: ") + ImageError));
            Definition.LightFunction.Empty();
        }
        if (!Definition.File.IsEmpty()) {
            const FString ImageError = LoadImageFile(Definition.File);
            if (!ImageError.IsEmpty()) {
                Ignore(*ImageError);
                Definition.File.Empty();
            } else {
                const auto& Image = *Images[Definition.File].Image;
                // Sampling is UV-normalized, so other aspects stretch over the sphere as in Karma and Storm.
                if (D.type == HDUE_DOME_LIGHT && Image.SizeX != Image.SizeY * 2)
                    UE_LOG(LogTemp, Warning,
                           TEXT("hdUE dome image %s is %dx%d, not 2:1; stretching it as a latitude-longitude map."),
                           *Definition.File, Image.SizeX, Image.SizeY);
            }
        }
        Pending.Add(MoveTemp(Definition));
    }
    for (const auto& Definition : Pending) {
        if (const auto* Previous = Definitions.Find(Definition.Id))
            EnvironmentDirty |= Previous->Data.type == HDUE_DOME_LIGHT;
        EnvironmentDirty |= Definition.Data.type == HDUE_DOME_LIGHT;
        if (auto* Actor = Actors.Find(Definition.Id)) {
            if (IsValid(*Actor)) World->DestroyActor(*Actor);
            Actors.Remove(Definition.Id);
        }
        Definitions.Add(Definition.Id, Definition);
        if (Definition.Data.type != HDUE_DOME_LIGHT && !UpdateLocal(Definition, Error)) return false;
    }
    if (Batch.flags & HDUE_LIGHT_BATCH_LINKS) Objects = MoveTemp(Links);
    LastError.Empty();
    return true;
}
void FHdueLighting::Link(const FString& Id, UPrimitiveComponent* Mesh) const {
    if (!Mesh) return;
    const uint32* Found = Objects.Find(Id);
    const uint32 Flags = Found ? *Found : HDUE_LINK_CHANNELS;
    const FLightingChannels& Channels = Mesh->LightingChannels;
    if (Channels.bChannel0 != bool(Flags & 1) || Channels.bChannel1 != bool(Flags & 2) ||
        Channels.bChannel2 != bool(Flags & 4))
        Mesh->SetLightingChannels(Flags & 1, Flags & 2, Flags & 4);
    if (Mesh->CastShadow != !(Flags & HDUE_LINK_NO_SHADOW)) Mesh->SetCastShadow(!(Flags & HDUE_LINK_NO_SHADOW));
}
bool FHdueLighting::UpdateLocal(const FDefinition& Definition, FString& Error) {
    const auto& D = Definition.Data;
    auto* Actor = World->SpawnActor<AActor>();
    Actors.Add(Definition.Id, Actor);
    const bool Distant = D.type == HDUE_DISTANT_LIGHT || D.type == HDUE_CAMERA_DISTANT_LIGHT;
    ULightComponent* Light = nullptr;
    if (Distant)
        Light = NewObject<UDirectionalLightComponent>(Actor);
    else if (D.type == HDUE_RECT_LIGHT || D.type == HDUE_DISK_LIGHT)
        Light = NewObject<URectLightComponent>(Actor);
    // A sphere light with ShapingAPI is a spot, as UE's USD import makes it (UsdUtils::GetActorTypeForPrim); the API's
    // default 90-degree cone is the lower hemisphere (UsdLux). Wider cones have no UE spot and stay point lights.
    else if (D.type == HDUE_SPHERE_LIGHT && D.cone_angle <= 90)
        Light = NewObject<USpotLightComponent>(Actor);
    else
        Light = NewObject<UPointLightComponent>(Actor);
    static bool ConeWarned = false;
    if (!ConeWarned && D.cone_angle < 90 &&
        (D.type == HDUE_RECT_LIGHT || D.type == HDUE_DISK_LIGHT || D.type == HDUE_CYLINDER_LIGHT)) {
        ConeWarned = true;
        UE_LOG(LogTemp, Warning,
               TEXT("hdUE light %s: shaping cones on rect, disk and cylinder lights are ignored (UE spots are sphere "
                    "lights)."),
               *Definition.Id);
    }
    Light->SetMobility(EComponentMobility::Movable);
    Actor->SetRootComponent(Light);
    Actor->AddInstanceComponent(Light);
    Light->SetLightColor(FLinearColor(D.color[0], D.color[1], D.color[2]));
    Light->SetDiffuseScale(D.diffuse);
    Light->SetSpecularScale(D.specular);
    Light->SetCastShadows((D.flags & HDUE_LIGHT_SHADOWS) != 0);
    Light->bAllowMegaLights = !Distant && (D.flags & HDUE_LIGHT_MEGALIGHTS) != 0;
    if (const uint32 Channels = D.lighting_channels) {  // light linking; sky lights have no channels
        Light->LightingChannels.bChannel0 = (Channels & 1) != 0;
        Light->LightingChannels.bChannel1 = (Channels & 2) != 0;
        Light->LightingChannels.bChannel2 = (Channels & 4) != 0;
    }
    Actor->SetActorLocation(FVector(D.position_cm[0], D.position_cm[1], D.position_cm[2]));
    Actor->SetActorRotation(FRotationMatrix::MakeFromXZ(-Axis(D, 2), Axis(D, 1)).Rotator());
    // UsdLux intensity is luminance in nits for every light; normalize divides by the size factor.
    float Intensity = D.intensity;
    if (auto* Local = Cast<ULocalLightComponent>(Light)) {
        Local->SetAttenuationRadius(100000);
        const double Radius = D.width_cm * .01;
        double Projected = UE_PI * Radius * Radius, Area = 4 * Projected;
        if (D.type == HDUE_RECT_LIGHT) Projected = Area = D.width_cm * D.height_cm * .0001;
        if (D.type == HDUE_DISK_LIGHT) Area = Projected = UE_PI * D.width_cm * D.height_cm * .0001;
        if (D.type == HDUE_CYLINDER_LIGHT) {
            Projected = 2 * Radius * D.length_cm * .01 + UE_PI * Radius * Radius;
            Area = 2 * UE_PI * Radius * D.length_cm * .01;
        }
        if (D.flags & HDUE_LIGHT_NORMALIZE) Intensity /= float(FMath::Max(Area, 1e-12));
        if (D.type == HDUE_RECT_LIGHT || D.type == HDUE_DISK_LIGHT) {
            // UE stores nits times the rect's area (URectLightComponent::ComputeLightBrightness), and its proxy
            // divides that by half the area (FRectLightSceneProxy::GetLightShaderParameters: Color /= 0.5 * W * H),
            // which deferred lighting and the Path Tracer emit as radiance: 2x the nits. Halve to emit UsdLux's.
            Local->SetIntensityUnits(ELightUnits::Nits);
            Intensity *= .5f;
        } else {
            // UE point-light nits multiply by the whole capsule surface (UPointLightComponent::ComputeLightBrightness),
            // but sphere lights emit power / (pi r^2) (PathTracingPointLight.ush): 4x the nits. Candelas times the
            // projected area emit UsdLux's luminance.
            Local->SetIntensityUnits(ELightUnits::Candelas);
            Intensity *= float(Projected);
        }
    } else if (D.type == HDUE_DISTANT_LIGHT) {
        // UsdLux: illuminance = luminance * pi * sin^2(angle/2); normalize makes intensity the illuminance in lux.
        const double Half = FMath::Clamp(FMath::DegreesToRadians(double(D.angle_degrees)) / 2, 0., UE_DOUBLE_PI);
        const double Sin2 = FMath::Square(FMath::Sin(Half));
        const double SizeFactor = Half <= UE_DOUBLE_HALF_PI ? UE_DOUBLE_PI * Sin2 : (2 - Sin2) * UE_DOUBLE_PI;
        if (!(D.flags & HDUE_LIGHT_NORMALIZE) && Half > 0) Intensity *= float(SizeFactor);
    }
    if (auto* Point = Cast<UPointLightComponent>(Light)) {
        Point->SetSourceRadius((D.flags & HDUE_LIGHT_POINT) ? 0 : D.width_cm);
        if (D.type == HDUE_CYLINDER_LIGHT) {
            Point->SetSourceLength(D.length_cm);
            Actor->SetActorRotation(FRotationMatrix::MakeFromZX(Axis(D, 0), Axis(D, 1)).Rotator());
        }
    }
    if (auto* Spot = Cast<USpotLightComponent>(Light)) {
        // UE clamps spots to 89 degrees (USpotLightComponent::GetCosHalfConeAngle), but at 89 both Lumen and the Path
        // Tracer lit only part of the cone (a quadrant of a Cornell box below a downward spot; 88 lit all of it).
        // ponytail: 88-degree cap for that UE bug; raise it when UE renders its own 89-degree maximum whole.
        Spot->SetOuterConeAngle(FMath::Clamp(D.cone_angle, .01f, 88.f));
        Spot->SetInnerConeAngle(FMath::Clamp(D.cone_angle * (1 - D.cone_softness), 0.f, Spot->OuterConeAngle));
    }
    if (auto* Sun = Cast<UDirectionalLightComponent>(Light)) Sun->LightSourceAngle = D.angle_degrees;
    if (auto* Rect = Cast<URectLightComponent>(Light)) {
        Rect->SetSourceWidth(D.type == HDUE_DISK_LIGHT ? D.width_cm * 2 : D.width_cm);
        Rect->SetSourceHeight(D.type == HDUE_DISK_LIGHT ? D.height_cm * 2 : D.height_cm);
        // UsdLux rect lights have no barn doors; UE.s default 20 cm flaps cut grazing emission; open them fully and
        // shorten them to the minimum.
        Rect->SetBarnDoorAngle(GetRectLightBarnDoorMaxAngle());
        Rect->SetBarnDoorLength(0.1f);
        if (D.type == HDUE_DISK_LIGHT) {
            if (!DiskTexture.IsValid()) {
                DiskTexture.Reset(UTexture2D::CreateTransient(64, 64, PF_FloatRGBA));
                DiskTexture->SRGB = false;
                DiskTexture->NeverStream = true;
                auto& Mip = DiskTexture->GetPlatformData()->Mips[0];
                auto* Pixels = static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
                for (int32 Y = 0; Y < 64; ++Y)
                    for (int32 X = 0; X < 64; ++X) {
                        const float R =
                            FMath::Sqrt(FMath::Square((X + .5f) / 32 - 1) + FMath::Square((Y + .5f) / 32 - 1));
                        const float Value = FMath::Clamp((1 - R) * 32, 0.f, 1.f);
                        Pixels[Y * 64 + X] = FFloat16Color(FLinearColor(Value, Value, Value, 1));
                    }
                Mip.BulkData.Unlock();
                DiskTexture->UpdateResource();
            }
            Rect->SetSourceTexture(DiskTexture.Get());
        } else if (!Definition.File.IsEmpty())
            Rect->SetSourceTexture(ImageTexture(Definition.File));
    }
    // unreal:lightFunction:texture: UE's light function from an image, an instance of the cooked M_HdUELightFunction.
    LightFunctions.Remove(Definition.Id);
    if (!Definition.LightFunction.IsEmpty()) {
        auto* Parent =
            LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUELightFunction.M_HdUELightFunction"));
        if (!Parent) {
            UE_LOG(LogTemp, Warning,
                   TEXT("hdUE light %s: /Game/HdUE/M_HdUELightFunction is missing; stage a runtime "
                        "prepared with Prepare-Content.ps1."),
                   *Definition.Id);
        } else {
            TStrongObjectPtr<UMaterialInstanceDynamic> Function(UMaterialInstanceDynamic::Create(Parent, nullptr));
            Function->SetTextureParameterValue(TEXT("Texture"), ImageTexture(Definition.LightFunction));
            Light->SetLightFunctionMaterial(Function.Get());
            LightFunctions.Add(Definition.Id, MoveTemp(Function));
        }
    }
    if (auto* Local = Cast<ULocalLightComponent>(Light); Local && !Definition.Ies.IsEmpty()) {
        const auto* Entry = Profile(Definition.Ies, D.ies_angle_scale, Error);
        if (!Entry) return false;
        UTextureLightProfile* Texture = Entry->Texture.Get();
        // UsdLux scales the light by the profile's candela values (iesSample * L). UE's texture holds them over their
        // maximum (IESLoader), which it keeps in Brightness: multiply it back. bUseIESBrightness would instead replace
        // the USD intensity. normalize keeps the unshaped light's power: divide by the profile's mean over the sphere.
        Local->SetIESTexture(Texture);
        Local->bUseIESBrightness = false;
        const float Mean = Entry->Mean;
        Intensity *= D.ies_normalize ? (Mean > 0 ? 1 / Mean : 1.f) : Texture->Brightness;
    }
    Light->SetIntensity(Intensity);
    // unreal:light:<Property> last, so UE's own properties can override the UsdLux translation (barn doors, ...). The
    // component is not registered yet, so its scene proxy is created with them.
    for (const auto& [Name, Text] : Definition.Properties) {
        FProperty* Property = Light->GetClass()->FindPropertyByName(*Name);
        if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || !ImportPropertyValue(Property, Text, Light))
            UE_LOG(LogTemp, Warning, TEXT("hdUE light %s: %s = '%s' skipped (no such %s property, or a bad value)"),
                   *Definition.Id, *Name, *Text, *Light->GetClass()->GetName());
    }
    Light->RegisterComponent();
    Actor->SetActorHiddenInGame(!D.visible);
    return true;
}
FString FHdueLighting::LoadImageFile(const FString& File) {
    const auto Stat = IFileManager::Get().GetStatData(*File);
    if (!Stat.bIsValid || Stat.bIsDirectory) return TEXT("its image is not a readable file: ") + File;
    if (const auto* Cached = Images.Find(File);
        Cached && Cached->Modified == Stat.ModificationTime && Cached->Size == Stat.FileSize)
        return {};
    FImage Image;
    FImageEntry Entry;
    Entry.Modified = Stat.ModificationTime;
    Entry.Size = Stat.FileSize;
    Entry.Image = MakeShared<FImage>();
    if (!FImageUtils::LoadImage(*File, Image) || Image.NumSlices != 1 || Image.SizeX > 32768 || Image.SizeY > 16384)
        return TEXT("cannot decode its image: ") + File;
    Image.CopyTo(*Entry.Image, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
    for (const auto& Pixel : Entry.Image->AsRGBA32F())
        if (!FMath::IsFinite(Pixel.R) || !FMath::IsFinite(Pixel.G) || !FMath::IsFinite(Pixel.B))
            return TEXT("its HDR image has nonfinite pixels: ") + File;
    Images.Add(File, MoveTemp(Entry));
    LightTextures.Remove(File);
    return {};
}
UTexture2D* FHdueLighting::ImageTexture(const FString& File) {
    if (const auto* Existing = LightTextures.Find(File)) return Existing->Get();
    const auto& Image = *Images[File].Image;
    TStrongObjectPtr<UTexture2D> Texture(UTexture2D::CreateTransient(Image.SizeX, Image.SizeY, PF_FloatRGBA));
    Texture->SRGB = false;
    Texture->NeverStream = true;
    auto& Mip = Texture->GetPlatformData()->Mips[0];
    auto* Pixels = static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
    const auto Source = Image.AsRGBA32F();
    for (int64 I = 0; I < Source.Num(); ++I) Pixels[I] = FFloat16Color(Source[I]);
    Mip.BulkData.Unlock();
    Texture->UpdateResource();
    return LightTextures.Add(File, MoveTemp(Texture)).Get();
}
const FHdueLighting::FProfileEntry* FHdueLighting::Profile(const FString& File, float AngleScale, FString& Error) {
    const auto Stat = IFileManager::Get().GetStatData(*File);
    if (!Stat.bIsValid || Stat.bIsDirectory) {
        Error = TEXT("IES profile is not a readable file: ") + File;
        return nullptr;
    }
    const FString Key = AngleScale ? File + TEXT("|") + FString::SanitizeFloat(AngleScale) : File;
    if (const auto* Cached = Profiles.Find(Key);
        Cached && Cached->Modified == Stat.ModificationTime && Cached->Size == Stat.FileSize)
        return Cached;
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *File)) {
        Error = TEXT("Cannot read IES profile: ") + File;
        return nullptr;
    }
    FIESConverter Converter(Bytes.GetData(), uint32(Bytes.Num()));
    if (!Converter.IsValid()) {
        Error = FString::Printf(TEXT("Invalid IES profile %s: %s"), *File, Converter.GetError());
        return nullptr;
    }
    // The editor imports IES as RGBA16F source data; build the same texture's platform data directly.
    const int32 Width = int32(Converter.GetWidth()), Height = int32(Converter.GetHeight());
    if (Converter.GetRawData().Num() != int64(Width) * Height * int64(sizeof(FFloat16Color))) {
        Error = TEXT("Unexpected IES texture layout: ") + File;
        return nullptr;
    }
    auto* Texture = NewObject<UTextureLightProfile>(GetTransientPackage(), NAME_None, RF_Transient);
    auto* Data = new FTexturePlatformData();
    Data->SizeX = Width;
    Data->SizeY = Height;
    Data->SetNumSlices(1);
    Data->PixelFormat = PF_FloatRGBA;
    auto* Mip = new FTexture2DMipMap(Width, Height, 1);
    Data->Mips.Add(Mip);
    const auto* Texels = reinterpret_cast<const FFloat16Color*>(Converter.GetRawData().GetData());
    TArray<FFloat16Color> Pixels(Texels, Width * Height);
    // shaping:ies:angleScale remaps the vertical angle theta (texture x: 0..180 degrees, IESLoader) before sampling
    // the profile: theta / s for s > 0, (theta - pi) / -s + pi for s < 0 (UsdLux ShapingAPI). Clamped to the profile.
    if (AngleScale)
        for (int32 X = 0; X < Width; ++X) {
            const double Theta = double(X) / Width * UE_DOUBLE_PI;
            const double Source =
                AngleScale > 0 ? Theta / AngleScale : (Theta - UE_DOUBLE_PI) / -AngleScale + UE_DOUBLE_PI;
            const double Column = FMath::Clamp(Source / UE_DOUBLE_PI * Width, 0., double(Width - 1));
            const int32 A = FMath::FloorToInt32(Column), B = FMath::Min(A + 1, Width - 1);
            for (int32 Y = 0; Y < Height; ++Y)
                Pixels[int64(Y) * Width + X] =
                    FFloat16Color(FMath::Lerp(FLinearColor(Texels[int64(Y) * Width + A]),
                                              FLinearColor(Texels[int64(Y) * Width + B]), float(Column - A)));
        }
    Mip->BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(Mip->BulkData.Realloc(Pixels.Num() * sizeof(FFloat16Color)), Pixels.GetData(),
                    Pixels.Num() * sizeof(FFloat16Color));
    Mip->BulkData.Unlock();
    Texture->SetPlatformData(Data);
    Texture->AddressX = TA_Clamp;
    Texture->AddressY = TA_Clamp;
    Texture->SRGB = false;
    Texture->NeverStream = true;
    Texture->CompressionSettings = TC_HDR;
    Texture->Filter = TF_Bilinear;
    Texture->Brightness = Converter.GetBrightness();
    Texture->TextureMultiplier = Converter.GetMultiplier();
    Texture->UpdateResource();
    // Mean of the file's normalized profile over the sphere (normalize uses the file's power, before angleScale):
    // columns are vertical angles 0..180 degrees (IESLoader), each weighted by its solid angle; rows are horizontal.
    double Sum = 0, Weight = 0;
    for (int32 X = 0; X < Width; ++X) {
        const double W = FMath::Sin((X + .5) / Width * UE_DOUBLE_PI);
        for (int32 Y = 0; Y < Height; ++Y) {
            Sum += W * Texels[int64(Y) * Width + X].R.GetFloat();
            Weight += W;
        }
    }
    auto& Entry = Profiles.FindOrAdd(Key);
    Entry.Modified = Stat.ModificationTime;
    Entry.Size = Stat.FileSize;
    Entry.Texture.Reset(Texture);
    Entry.Mean = float(Weight > 0 ? Sum / Weight : 0);
    return &Entry;
}
bool FHdueLighting::RebuildEnvironment(FString& Error) {
    DomeCount = 0;
    Resolution = 0;
    ShowBackground = false;
    MaximumRadiance = 0;
    TArray<const FDefinition*> Domes;
    for (const auto& Entry : Definitions)
        if (Entry.Value.Data.type == HDUE_DOME_LIGHT && Entry.Value.Data.visible && Entry.Value.Data.intensity > 0) {
            Domes.Add(&Entry.Value);
            ++DomeCount;
            Resolution = FMath::Max(Resolution, Entry.Value.Data.cube_resolution);
            ShowBackground |= (Entry.Value.Data.flags & HDUE_LIGHT_BACKGROUND) != 0;
        }
    if (Domes.IsEmpty()) {
        if (IsValid(SkyActor)) World->DestroyActor(SkyActor);
        SkyActor = nullptr;
        Sky = nullptr;
        Environment.Reset();
        RadianceScale = 1;
        CaptureFence.BeginFence();
        return true;
    }
    TArray<FLinearColor> Pixels;
    Pixels.SetNumZeroed(int64(Resolution) * Resolution * 6);
    for (const auto* Dome : Domes) {
        const auto& D = Dome->Data;
        const auto Inverse = Frame(D).Inverse();
        const FLinearColor Weight(D.color[0] * D.intensity, D.color[1] * D.intensity, D.color[2] * D.intensity, 0);
        const FImage* Image = Dome->File.IsEmpty() ? nullptr : Images[Dome->File].Image.Get();
        for (int32 Face = 0; Face < 6; ++Face)
            for (uint32 Y = 0; Y < Resolution; ++Y)
                for (uint32 X = 0; X < Resolution; ++X) {
                    const FVector Local = Inverse
                                              .TransformVector(CubeDirection(Face, 2 * (X + .5) / Resolution - 1,
                                                                             2 * (Y + .5) / Resolution - 1))
                                              .GetSafeNormal();
                    auto Color = Image ? SampleLatlong(*Image, Local) : FLinearColor::White;
                    Color.R = FMath::Max(0.f, Color.R);
                    Color.G = FMath::Max(0.f, Color.G);
                    Color.B = FMath::Max(0.f, Color.B);
                    Pixels[(int64(Face) * Resolution + Y) * Resolution + X] += Color * Weight;
                }
    }
    for (const auto& Color : Pixels) {
        if (!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G) || !FMath::IsFinite(Color.B)) {
            Error = TEXT("HDR environment overflow.");
            return false;
        }
        MaximumRadiance = FMath::Max(MaximumRadiance, FMath::Max3(Color.R, Color.G, Color.B));
    }
    // Keep bright sun texels representable in the half-float cube. Restore the
    // scale after convolution and during background drawing; never clamp to LDR.
    RadianceScale = FMath::Max(1.f, MaximumRadiance / 1024.f);
    TStrongObjectPtr<UTextureCube> Cube(UTextureCube::CreateTransient(Resolution, Resolution, PF_FloatRGBA));
    Cube->SRGB = false;
    Cube->NeverStream = true;
    auto& Mip = Cube->GetPlatformData()->Mips[0];
    auto* Destination = static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
    for (int64 I = 0; I < Pixels.Num(); ++I) {
        auto Color = Pixels[I] / RadianceScale;
        Color.A = 1;
        Destination[I] = FFloat16Color(Color);
    }
    Mip.BulkData.Unlock();
    Cube->UpdateResource();
    Environment = MoveTemp(Cube);
    if (!IsValid(SkyActor)) {
        SkyActor = World->SpawnActor<AActor>();
        Sky = NewObject<USkyLightComponent>(SkyActor);
        Sky->SetMobility(EComponentMobility::Movable);
        Sky->SourceType = SLS_SpecifiedCubemap;
        Sky->bLowerHemisphereIsBlack = false;
        Sky->CubemapResolution = Resolution;
        SkyActor->SetRootComponent(Sky);
        SkyActor->AddInstanceComponent(Sky);
        Sky->RegisterComponent();
    }
    Sky->CubemapResolution = Resolution;
    Sky->SetIntensity(RadianceScale);
    Sky->SetLightColor(FLinearColor::White);
    Sky->SetCubemap(Environment.Get());
    Sky->SetCastShadows(
        Domes.ContainsByPredicate([](const auto* Dome) { return (Dome->Data.flags & HDUE_LIGHT_SHADOWS) != 0; }));
    Sky->RecaptureSky();
    USkyLightComponent::UpdateSkyCaptureContents(World);
    CaptureFence.BeginFence();
    return true;
}
void FHdueLighting::Tick() {
    if (EnvironmentDirty) {
        EnvironmentDirty = false;
        LastError.Empty();
        RebuildEnvironment(LastError);
    }
}
bool FHdueLighting::Ready() const {
    return !EnvironmentDirty && LastError.IsEmpty() && CaptureFence.IsFenceComplete() &&
           (!DomeCount || (Sky && Sky->GetProcessedSkyTexture()));
}
bool FHdueLighting::BackgroundVisible() const { return DomeCount && ShowBackground; }
FTextureRHIRef FHdueLighting::EnvironmentTexture() const {
    return Environment.IsValid() && Environment->GetResource() ? Environment->GetResource()->TextureRHI
                                                               : FTextureRHIRef();
}
void FHdueLighting::Status(HdueLightingStatus& Status) const {
    Status.light_count = Definitions.Num();
    Status.dome_count = DomeCount;
    Status.environment_pending = !Ready();
    Status.cube_resolution = Resolution;
    Status.environment_max_radiance = MaximumRadiance;
}
void FHdueLighting::UpdateCamera(const FVector& Eye, const FVector& Target, const FVector& Up) {
    const FVector Forward = (Target - Eye).GetSafeNormal(), Right = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    const FVector CameraUp = FVector::CrossProduct(Forward, Right).GetSafeNormal();
    for (const auto& Entry : Definitions)
        if (Entry.Value.Data.type == HDUE_CAMERA_DISTANT_LIGHT) {
            const auto* Actor = Actors.Find(Entry.Key);
            if (!Actor || !IsValid(*Actor)) continue;
            const FVector Relative = Axis(Entry.Value.Data, 0);
            (*Actor)->SetActorRotation((Forward * Relative.X + Right * Relative.Y + CameraUp * Relative.Z).Rotation());
        }
}
