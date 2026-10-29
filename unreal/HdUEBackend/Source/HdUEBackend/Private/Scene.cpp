#include "Scene.h"
#include "Lighting.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/NonManifoldMappingSupport.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ImageUtils.h"
#include "Async/ParallelFor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "RHI.h"
#include "Algo/AllOf.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HeterogeneousVolumeComponent.h"
#include "Engine/VolumeTexture.h"
#include "TextureResource.h"
#include "DynamicMeshToMeshDescription.h"
#include "StaticMeshAttributes.h"
#include "ImageCore.h"
#include "RenderUtils.h"
#include "ImageCoreUtils.h"
#include "ispc_texcomp.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "Hash/CityHash.h"
#include "NaniteBuilder.h"
#include "Rendering/NaniteResources.h"
#include "StaticMeshResources.h"
#include "Tasks/Task.h"

FHdueScene::FHdueScene(UWorld* InWorld) : Assets(InWorld), Lighting(MakeUnique<FHdueLighting>(InWorld)), World(InWorld) {}
FHdueScene::~FHdueScene() {
    for (auto& Entry : Actors) if (IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
}
bool FHdueScene::HasRefractiveMaterials() const {
    for (const auto& Entry : Materials) {
        const auto* Base = Entry.Value->GetMaterial();
        if (Base && Base->BlendMode == BLEND_TranslucentColoredTransmittance &&
            (Base->RefractionMethod == RM_IndexOfRefraction || Base->RefractionMethod == RM_IndexOfRefractionFromF0)) return true;
    }
    for (auto* Actor : Assets.GetActors()) {
        TArray<UMeshComponent*> Meshes; Actor->GetComponents(Meshes);
        for (const auto* Mesh : Meshes) for (int32 I=0; I<Mesh->GetNumMaterials(); ++I) {
            const auto* Material=Mesh->GetMaterial(I); const auto* Base=Material?Material->GetMaterial():nullptr;
            if (Base && Base->BlendMode==BLEND_TranslucentColoredTransmittance &&
                (Base->RefractionMethod==RM_IndexOfRefraction || Base->RefractionMethod==RM_IndexOfRefractionFromF0)) return true;
        }
    }
    return false;
}
TArray<TStrongObjectPtr<UTexture2D>> FHdueScene::GatherVirtualTextures() const {
    TSet<UTexture2D*> Unique;
    TArray<TStrongObjectPtr<UTexture2D>> Result;
    TArray<AActor*> All = Assets.GetActors();
    for (const auto& Entry : Actors) All.Add(Entry.Value);
    for (auto* Actor : All) {
        if (!IsValid(Actor) || Actor->IsHidden()) continue;
        TArray<UMeshComponent*> Meshes; Actor->GetComponents(Meshes);
        for (const auto* Mesh : Meshes) {
        if (!Mesh->IsVisible()) continue;
        for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index) {
            const auto* Material = Mesh->GetMaterial(Index);
            if (!Material) continue;
            TArray<UTexture*> Textures;
            Material->GetUsedTextures(Textures);
            for (UTexture* Texture : Textures) {
                auto* Texture2D = Cast<UTexture2D>(Texture);
                if (Texture2D && Texture2D->VirtualTextureStreaming && !Unique.Contains(Texture2D)) {
                    Unique.Add(Texture2D);
                    Result.Emplace(Texture2D);
                }
            }
        }
        }
    }
    return Result;
}
void FHdueScene::Remove(const FString& Id) {
    Lighting->Remove(Id);
    RemoveVolume(Id);
    CameraLightDirections.Remove(Id);
    if (AActor** Actor = Actors.Find(Id)) {
        if (IsValid(*Actor)) World->DestroyActor(*Actor);
        Actors.Remove(Id);
    }
    Materials.Remove(Id);
    MeshMaterials.Remove(Id);
    PrimIds.Remove(Id);
    WetBindings.Remove(Id);
    GeometryRevisions.Remove(Id);
}
namespace {
// The root component of an hdUE actor: movable and render-only. Collision stays off, so UE neither cooks physics
// for generated meshes (which runtime builds cannot provide) nor runs queries against them.
template <class T> T* AddRootComponent(AActor* Actor, TFunctionRef<void(T&)> Configure, UClass* Class = T::StaticClass()) {
    T* Component = NewObject<T>(Actor, Class);
    Component->SetMobility(EComponentMobility::Movable);
    if (auto* Primitive = Cast<UPrimitiveComponent>(Component)) {
        Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Primitive->SetCanEverAffectNavigation(false);
    }
    Configure(*Component);
    Actor->SetRootComponent(Component);
    Actor->AddInstanceComponent(Component);
    Component->RegisterComponent();
    return Component;
}
}

bool FHdueScene::UpsertVolume(const HdueVolumeDesc& Desc, FString& Error) {
    const FString Id = UTF8_TO_TCHAR(Desc.id);
    const uint64 Count = uint64(Desc.width) * Desc.height * Desc.depth;
    auto* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUEVolume.M_HdUEVolume"));
    if (!Parent) { Error = TEXT("Volume material /Game/HdUE/M_HdUEVolume is missing; run Prepare-Content.ps1 and stage the runtime."); return false; }
    FVolume& Volume = Volumes.FindOrAdd(Id);
    if (!IsValid(Volume.Actor)) {
        Volume.Actor = World->SpawnActor<AActor>();
        AddRootComponent<UHeterogeneousVolumeComponent>(Volume.Actor, [](UHeterogeneousVolumeComponent&) {});
        Volume.Material.Reset(UMaterialInstanceDynamic::Create(Parent, World));
    }
    auto* Component = Volume.Actor->FindComponentByClass<UHeterogeneousVolumeComponent>();
    if (Count) {
        // ponytail: half-float dense 3D texture; sparse grids beyond a few hundred voxels per side cost memory.
        UVolumeTexture* Texture = Volume.Texture.Get();
        if (!Texture || Texture->GetSizeX() != int32(Desc.width) || Texture->GetSizeY() != int32(Desc.height) || Texture->GetSizeZ() != int32(Desc.depth)) {
            Texture = UVolumeTexture::CreateTransient(Desc.width, Desc.height, Desc.depth, PF_R16F);
            if (!Texture) { Error = TEXT("Cannot create the volume texture."); return false; }
            Texture->SRGB = false; Texture->Filter = TF_Trilinear; Texture->NeverStream = true;
            Volume.Texture.Reset(Texture);
        }
        auto& Mip = Texture->GetPlatformData()->Mips[0];
        auto* Voxels = static_cast<FFloat16*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
        for (uint64 I = 0; I < Count; ++I) Voxels[I] = FFloat16(FMath::Max(0.f, Desc.density[I]));
        Mip.BulkData.Unlock();
        Texture->UpdateResource();
        Volume.Material->SetTextureParameterValue(TEXT("Density"), Texture);
    }
    Volume.Material->SetVectorParameterValue(TEXT("Albedo"), FLinearColor(Desc.albedo[0], Desc.albedo[1], Desc.albedo[2], 1));
    Volume.Material->SetScalarParameterValue(TEXT("ExtinctionScale"), Desc.extinction_scale);
    Component->SetMaterial(0, Volume.Material.Get());
    Component->SetVolumeResolution(FIntVector(Desc.width, Desc.height, Desc.depth));
    FMatrix Matrix;
    for (int R = 0; R < 4; ++R) for (int C = 0; C < 4; ++C) Matrix.M[R][C] = Desc.transform[R * 4 + C];
    Volume.Actor->SetActorTransform(FTransform(Matrix));
    // UVW_j = dot((world, 1), row_j): column j of the inverse voxel transform over the resolution.
    const FMatrix Inverse = Matrix.Inverse();
    const double Size[3] = {double(FMath::Max(1u, Desc.width)), double(FMath::Max(1u, Desc.height)), double(FMath::Max(1u, Desc.depth))};
    for (int32 Axis = 0; Axis < 3; ++Axis)
        Volume.Material->SetVectorParameterValue(*FString::Printf(TEXT("WorldToUVW%d"), Axis), FLinearColor(
            float(Inverse.M[0][Axis] / Size[Axis]), float(Inverse.M[1][Axis] / Size[Axis]), float(Inverse.M[2][Axis] / Size[Axis]), float(Inverse.M[3][Axis] / Size[Axis])));
    Volume.Actor->SetActorHiddenInGame(!Desc.visible || !Count);
    return true;
}
void FHdueScene::RemoveVolume(const FString& Id) {
    if (FVolume* Volume = Volumes.Find(Id)) {
        if (IsValid(Volume->Actor)) World->DestroyActor(Volume->Actor);
        Volumes.Remove(Id);
    }
}
void FHdueScene::SetTransforms(const FString& Id, uint32 Count, const double* Matrices) {
    AActor* Actor = Actors.FindRef(Id);
    if (!IsValid(Actor) || !Count) return;
    const auto ToTransform = [&](uint32 Index) {
        FMatrix Matrix;
        for (int R = 0; R < 4; ++R) for (int C = 0; C < 4; ++C) Matrix.M[R][C] = Matrices[Index * 16 + R * 4 + C];
        return FTransform(Matrix);
    };
    if (auto* Instances = Cast<UInstancedStaticMeshComponent>(Actor->GetRootComponent())) {
        if (int32(Count) != Instances->GetInstanceCount()) return;
        TArray<FTransform> Transforms; Transforms.Reserve(Count);
        for (uint32 I = 0; I < Count; ++I) Transforms.Add(ToTransform(I));
        Instances->BatchUpdateInstancesTransforms(0, Transforms, true, true, true);
    } else if (Count == 1) Actor->SetActorTransform(ToTransform(0), false, nullptr, ETeleportType::TeleportPhysics);
}
bool FHdueScene::SetIdMode(int32 Mode, FString& Error) {
    if (Mode == IdMode) return true;
    const bool On = Mode != 0;
    UMaterialInterface* IdMaterial = nullptr;
    if (On && !(IdMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUEId.M_HdUEId")))) {
        Error = TEXT("ID material /Game/HdUE/M_HdUEId is missing; run Prepare-Content.ps1 and stage the runtime."); return false;
    }
    const auto Swap = [&](AActor* Actor, int32 Id) {
        TArray<UMeshComponent*> Meshes; Actor->GetComponents(Meshes);
        for (auto* Mesh : Meshes) {
            if (On) {
                // Exact in the float16 readback: both halves stay below 2048.
                // Instance mode: instanced components emit their per-instance id (custom data 3), the rest -1.
                const bool Instances = Mode == 2 && Mesh->IsA<UInstancedStaticMeshComponent>();
                Mesh->SetCustomPrimitiveDataVector4f(0, Id >= 0 && Mode == 1 ? FVector4f(float(Id / 2048), float(Id % 2048), 1, 0) : FVector4f(0, 0, 0, 0));
                Mesh->SetCustomPrimitiveDataFloat(4, Instances ? 1.f : 0.f);
                for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot) Mesh->SetMaterial(Slot, IdMaterial);
            } else if (auto* Static = Cast<UStaticMeshComponent>(Mesh)) Static->EmptyOverrideMaterials();
        }
    };
    for (const auto& Entry : Actors) if (IsValid(Entry.Value)) { const int32* Id = PrimIds.Find(Entry.Key); Swap(Entry.Value, Id ? *Id : -1); }
    // ponytail: native catalog assets carry no Hydra prim ID; they read as background in the ID pass.
    for (auto* Actor : Assets.GetActors()) if (IsValid(Actor)) Swap(Actor, -1);
    IdMode = Mode;
    if (!On) BindMaterials();
    return true;
}
namespace {
// A decoded image and its mip levels down to 1x1, in the pixel format its texture uses. Transient textures get no
// mips of their own: one level aliased fine detail (moire on fabric) and read full-size texels at any distance.
// The next mip level: a 2x2 box filter (edge texels repeat on odd sizes). 8-bit sRGB averages in linear light through
// lookup tables; UE's general ResizeImage filtered every texel in float and more than doubled texture load time.
void HalveImage(const FImage& Source, FImage& Target) {
    const int32 SW = Source.SizeX, SH = Source.SizeY, TW = Target.SizeX, TH = Target.SizeY;
    const auto Tap = [&](int32 X, int32 Y) { return int64(FMath::Min(Y, SH - 1)) * SW + FMath::Min(X, SW - 1); };
    if (Source.Format == ERawImageFormat::BGRA8) {
        static const auto Tables = [] {
            struct { float ToLinear[256]; uint8 ToSRGB[4096]; } T;
            for (int32 I = 0; I < 256; ++I) T.ToLinear[I] = FLinearColor::FromSRGBColor(FColor(I, 0, 0)).R;
            for (int32 I = 0; I < 4096; ++I) T.ToSRGB[I] = FLinearColor(I / 4095.f, 0, 0).ToFColor(true).R;
            return T;
        }();
        const bool SRGB = Source.GammaSpace == EGammaSpace::sRGB;
        const FColor* In = reinterpret_cast<const FColor*>(Source.RawData.GetData());
        FColor* Out = reinterpret_cast<FColor*>(Target.RawData.GetData());
        ParallelFor(TH, [&](int32 Y) {
            for (int32 X = 0; X < TW; ++X) {
                const FColor Texels[4] = {In[Tap(2 * X, 2 * Y)], In[Tap(2 * X + 1, 2 * Y)], In[Tap(2 * X, 2 * Y + 1)], In[Tap(2 * X + 1, 2 * Y + 1)]};
                const auto Average = [&](uint8 FColor::*Channel, bool Linear) {
                    if (!Linear) return uint8((Texels[0].*Channel + Texels[1].*Channel + Texels[2].*Channel + Texels[3].*Channel + 2) / 4);
                    float Sum = 0; for (const FColor& Texel : Texels) Sum += Tables.ToLinear[Texel.*Channel];
                    return Tables.ToSRGB[FMath::Clamp(int32(Sum * (4095.f / 4.f) + .5f), 0, 4095)];
                };
                Out[int64(Y) * TW + X] = FColor(Average(&FColor::R, SRGB), Average(&FColor::G, SRGB), Average(&FColor::B, SRGB), Average(&FColor::A, false));
            }
        });
        return;
    }
    FImageCore::ResizeImage(Source, Target); // float and other formats: rare, keep UE's filter
}
TArray<FImage> MipChain(FImage&& Image) {
    ERawImageFormat::Type Raw;
    FImageCoreUtils::GetPixelFormatForRawImageFormat(Image.Format, &Raw);
    TArray<FImage> Chain;
    if (Image.Format == Raw) Chain.Add(MoveTemp(Image));
    else Image.CopyTo(Chain.AddDefaulted_GetRef(), Raw, Image.GammaSpace);
    while (Chain.Last().SizeX > 1 || Chain.Last().SizeY > 1) {
        const FImage& Previous = Chain.Last();
        FImage Next(FMath::Max(1, Previous.SizeX / 2), FMath::Max(1, Previous.SizeY / 2), Raw, Previous.GammaSpace);
        HalveImage(Previous, Next);
        Chain.Add(MoveTemp(Next));
    }
    return Chain;
}
// Block compression by ispc_texcomp.dll, delay-loaded from the engine's third-party binaries (staged with the runtime).
// Without it, textures stay uncompressed.
bool BlockCompression() {
    static const bool Loaded = FPlatformProcess::GetDllHandle(*(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Intel/ISPCTexComp/Win64-Release/ispc_texcomp.dll"))) != nullptr;
    return Loaded;
}
// The block format for what a texture's materials read: one or two channels read only by scalar inputs are repacked
// into BC4/BC5 (Texture.Channels names their source channels); colour, normals and three- or four-channel data are BC7.
EPixelFormat BlockFormat(FHdueScene::FPreviewTexture& Texture) {
    Texture.Channels.Reset();
    if (Texture.Vector) return PF_BC7;
    for (uint8 Channel = 0; Channel < 4; ++Channel) if (Texture.Read & (1 << Channel)) Texture.Channels.Add(Channel);
    if (Texture.Channels.Num() == 1) return PF_BC4;
    if (Texture.Channels.Num() == 2) return PF_BC5;
    Texture.Channels.Reset();
    return PF_BC7;
}
// One BGRA8 mip level as Format's blocks, a row of blocks per task. Blocks past the image (mips below 4x4, odd sizes)
// repeat its edge texels.
void CompressMip(const FImage& Mip, EPixelFormat Format, const FHdueScene::FPreviewTexture& Texture, uint8* Out) {
    const int32 BlocksX = FMath::DivideAndRoundUp(Mip.SizeX, 4), BlocksY = FMath::DivideAndRoundUp(Mip.SizeY, 4);
    const int32 Texel = Format == PF_BC4 ? 1 : Format == PF_BC5 ? 2 : 4; // encoder input: R8, RG8 or RGBA8
    static const uint8 RGBA[4] = {0, 1, 2, 3}, Bytes[4] = {2, 1, 0, 3}; // byte of each RGBA channel in an FColor
    const uint8* Source = Texture.Channels.IsEmpty() ? RGBA : Texture.Channels.GetData();
    bc7_enc_settings Settings;
    // ultrafast: ~60 Mpixel/s per core; veryfast was 12x slower (5 s of a Sponza load on 32 cores).
    if (Texture.Read & 8) GetProfile_alpha_ultrafast(&Settings); else GetProfile_ultrafast(&Settings);
    const uint8* In = Mip.RawData.GetData();
    ParallelFor(BlocksY, [&](int32 Row) {
        TArray<uint8> Strip; Strip.SetNumUninitialized(BlocksX * 16 * Texel);
        uint8* Packed = Strip.GetData();
        for (int32 Y = 0; Y < 4; ++Y) {
            const uint8* Line = In + int64(FMath::Min(Row * 4 + Y, Mip.SizeY - 1)) * Mip.SizeX * 4;
            for (int32 X = 0; X < BlocksX * 4; ++X, Packed += Texel)
                for (int32 C = 0; C < Texel; ++C) Packed[C] = Line[FMath::Min(X, Mip.SizeX - 1) * 4 + Bytes[Source[C]]];
        }
        rgba_surface Surface{Strip.GetData(), BlocksX * 4, 4, BlocksX * 4 * Texel};
        uint8* Blocks = Out + int64(Row) * BlocksX * GPixelFormats[Format].BlockBytes;
        if (Format == PF_BC4) CompressBlocksBC4(&Surface, Blocks);
        else if (Format == PF_BC5) CompressBlocksBC5(&Surface, Blocks);
        else CompressBlocksBC7(&Surface, Blocks, &Settings);
    });
}
// A texture's mipped (and, for 8-bit images, block-compressed) platform data, on a worker thread. BC needs whole 4x4
// blocks at the top level; other sizes and float images (EXR/HDR) stay uncompressed.
TUniquePtr<FTexturePlatformData> PrepareTexture(FImage&& Image, FHdueScene::FPreviewTexture& Texture, bool SRGB) {
    if (!ERawImageFormat::IsHDR(Image.Format) && Image.Format != ERawImageFormat::BGRA8) { // grey, 16-bit
        FImage Converted; Image.CopyTo(Converted, ERawImageFormat::BGRA8, Image.GammaSpace); Image = MoveTemp(Converted);
    }
    const TArray<FImage> Chain = MipChain(MoveTemp(Image));
    const bool Compress = Chain[0].Format == ERawImageFormat::BGRA8 && Chain[0].SizeX % 4 == 0 && Chain[0].SizeY % 4 == 0 && BlockCompression();
    auto Data = MakeUnique<FTexturePlatformData>();
    Data->SizeX = Chain[0].SizeX; Data->SizeY = Chain[0].SizeY; Data->SetNumSlices(1);
    Data->PixelFormat = Compress ? BlockFormat(Texture) : FImageCoreUtils::GetPixelFormatForRawImageFormat(Chain[0].Format);
    if (!Compress) Texture.Channels.Reset();
    const FPixelFormatInfo& Info = GPixelFormats[Data->PixelFormat];
    for (const FImage& Level : Chain) {
        auto* Mip = new FTexture2DMipMap(Level.SizeX, Level.SizeY, 1);
        Data->Mips.Add(Mip);
        const int64 Size = int64(FMath::DivideAndRoundUp(Level.SizeX, Info.BlockSizeX)) * FMath::DivideAndRoundUp(Level.SizeY, Info.BlockSizeY) * Info.BlockBytes;
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        auto* Out = static_cast<uint8*>(Mip->BulkData.Realloc(Size));
        if (Compress) CompressMip(Level, Data->PixelFormat, Texture, Out);
        else FMemory::Memcpy(Out, Level.RawData.GetData(), Size);
        Mip->BulkData.Unlock();
    }
    return Data;
}
// Block-compressed textures cached in %LOCALAPPDATA%/hdUE/TextureCache, keyed by the file, its time and size, and what
// the materials read: reloading a scene skips decoding its images, most of a texture load.
// ponytail: never evicted; delete the folder to reclaim space.
constexpr int32 CacheVersion = 2;
FString CachePath(const FString& File, const FHdueScene::FPreviewTexture& Texture, bool SRGB) {
    static const FString Root = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA")), TEXT("hdUE"), TEXT("TextureCache"));
    const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
    if (!Stat.bIsValid) return FString();
    const FString Key = FString::Printf(TEXT("%s|%lld|%lld|%d|%d|%d|%d"), *FPaths::ConvertRelativePathToFull(File), Stat.ModificationTime.GetTicks(),
        Stat.FileSize, SRGB, Texture.Read, Texture.Vector, CacheVersion);
    return Root / FString::Printf(TEXT("%016llx.bin"), CityHash64(reinterpret_cast<const char*>(*Key), Key.Len() * sizeof(TCHAR)));
}
// The cached platform data (mip sizes and blocks straight into bulk data), or null.
TUniquePtr<FTexturePlatformData> ReadCache(const FString& Path, FHdueScene::FPreviewTexture& Texture) {
    TUniquePtr<FArchive> In(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!In) return nullptr;
    int32 Version = 0, SizeX = 0, SizeY = 0, Format = 0, Mips = 0;
    *In << Version << SizeX << SizeY << Format << Mips;
    if (In->IsError() || Version != CacheVersion || Format != BlockFormat(Texture)) return nullptr;
    auto Data = MakeUnique<FTexturePlatformData>();
    Data->SizeX = SizeX; Data->SizeY = SizeY; Data->SetNumSlices(1); Data->PixelFormat = EPixelFormat(Format);
    for (int32 Level = 0; Level < Mips && !In->IsError(); ++Level) {
        int32 Width = 0, Height = 0; int64 Size = 0;
        *In << Width << Height << Size;
        if (In->IsError() || Size <= 0 || Size > In->TotalSize()) return nullptr;
        auto* Mip = new FTexture2DMipMap(Width, Height, 1);
        Data->Mips.Add(Mip);
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        In->Serialize(Mip->BulkData.Realloc(Size), Size);
        Mip->BulkData.Unlock();
    }
    return In->IsError() || Data->Mips.IsEmpty() ? nullptr : MoveTemp(Data);
}
void WriteCache(const FString& Path, FTexturePlatformData& Data) {
    const FString Temporary = Path + FGuid::NewGuid().ToString(); // renamed when complete: readers never see a partial file
    TUniquePtr<FArchive> Out(IFileManager::Get().CreateFileWriter(*Temporary, FILEWRITE_Silent));
    if (!Out) return;
    int32 Version = CacheVersion, Format = Data.PixelFormat, Mips = Data.Mips.Num();
    *Out << Version << Data.SizeX << Data.SizeY << Format << Mips;
    for (FTexture2DMipMap& Mip : Data.Mips) {
        int32 Width = Mip.SizeX, Height = Mip.SizeY; // 16-bit fields
        int64 Size = Mip.BulkData.GetBulkDataSize();
        *Out << Width << Height << Size;
        Out->Serialize(Mip.BulkData.Lock(LOCK_READ_ONLY), Size);
        Mip.BulkData.Unlock();
    }
    const bool Written = Out->Close() && !Out->IsError();
    Out.Reset();
    if (!Written || !IFileManager::Get().Move(*Path, *Temporary, true, true, false, true)) IFileManager::Get().Delete(*Temporary, false, false, true);
}
FString TextureKey(const FString& File, bool SRGB) { return File + (SRGB ? TEXT("|srgb") : TEXT("|raw")); }
// Inputs the Preview Surface parents read through <Slot>Mask; the others (colour, normal) read RGB.
bool ScalarSlot(int32 Slot) { return !hdue_texture_is_color(Slot) && Slot != HDUE_TEXTURE_NORMAL; }
}
// A texture Apply prepared, created on this thread (UObjects and render resources need it) on first use.
const FHdueScene::FPreviewTexture* FHdueScene::PreviewTexture(const FString& File, bool SRGB, FString& Error) {
    FPreviewTexture* Texture = PreviewTextures.Find(TextureKey(File, SRGB));
    if (Texture && Texture->Pending) {
        const double CreateStart = FPlatformTime::Seconds();
        auto* Created = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
        Created->bNotOfflineProcessed = true;
        Created->SRGB = SRGB && !IsFloatFormat(Texture->Pending->PixelFormat); // float images are already linear
        Created->SetPlatformData(Texture->Pending.Release());
        Created->UpdateResource();
        Texture->Texture.Reset(Created);
        TextureSeconds += FPlatformTime::Seconds() - CreateStart; ++TextureImports;
    }
    if (!Texture || !Texture->Texture) { Error = FString::Printf(TEXT("Failed to import texture %s."), *File); return nullptr; }
    return Texture;
}
void FHdueScene::BindMaterials() {
    for (const auto& Entry : MeshMaterials) {
        AActor** Actor = Actors.Find(Entry.Key);
        if (!Actor || !IsValid(*Actor)) continue;
        auto* Component = (*Actor)->FindComponentByClass<UMeshComponent>();
        if (const auto* Material = Materials.Find(Entry.Value)) {
            UMaterialInterface* Desired=Material->Get();
            if (const auto* Wet = WetBindings.Find(Entry.Key); Wet && (*Wet)->Base == Material->Get() &&
                (*Wet)->Revision == GeometryRevisions.FindRef(Entry.Key)) Desired=(*Wet)->Material.Get();
            if (Component->GetMaterial(0)!=Desired) Component->SetMaterial(0,Desired);
        }
    }
}
namespace {
bool Interactive_() { static const bool Value = FParse::Param(FCommandLine::Get(), TEXT("hdUEInteractive")); return Value; }
// Nanite data for one generated mesh (any thread). Every page is a root page: outside the editor, UE streams non-root
// pages only from cooked files on disk, and these meshes exist only in memory.
TSharedPtr<Nanite::FResources> BuildNanite(const HdueMeshDesc& Desc) {
    if (!Desc.vertex_count || Desc.index_count < 3) return nullptr;
    Nanite::IBuilderModule::FInputMeshData Input;
    Input.Vertices.Empty(int32(Desc.vertex_count), 1, 0);
    FBox3f Bounds(ForceInit);
    for (uint32 V = 0; V < Desc.vertex_count; ++V) {
        const FVector3f Position(Desc.positions[V * 3], Desc.positions[V * 3 + 1], Desc.positions[V * 3 + 2]);
        FVector3f Normal = Desc.normals ? FVector3f(Desc.normals[V * 3], Desc.normals[V * 3 + 1], Desc.normals[V * 3 + 2]) : FVector3f::UpVector;
        if (!Normal.Normalize()) Normal = FVector3f::UpVector;
        const FVector3f Tangent = (FMath::Abs(Normal.X) < .9f ? FVector3f::ForwardVector : FVector3f::RightVector).Cross(Normal).GetSafeNormal();
        Input.Vertices.Position.Add(Position);
        Input.Vertices.TangentZ.Add(Normal);
        Input.Vertices.TangentX.Add(Tangent);
        Input.Vertices.TangentY.Add(Normal.Cross(Tangent));
        Input.Vertices.UVs[0].Add(Desc.uv ? FVector2f(Desc.uv[V * 2], Desc.uv[V * 2 + 1]) : FVector2f::ZeroVector);
        if (Desc.colors) Input.Vertices.Color.Add(FLinearColor(Desc.colors[V * 3], Desc.colors[V * 3 + 1], Desc.colors[V * 3 + 2]).ToFColor(false));
        Bounds += Position;
    }
    // The builder's vertex view drops all-white colours but its format still counts them: keep none in that case.
    if (!Input.Vertices.Color.ContainsByPredicate([](const FColor& C) { return C != FColor::White; })) Input.Vertices.Color.Empty();
    const uint32 Triangles = Desc.index_count / 3;
    Input.TriangleIndices = TArray<uint32>(Desc.indices, int32(Triangles * 3));
    Input.TriangleCounts.Add(Triangles);
    Input.MaterialIndices.Init(0, int32(Triangles));
    Nanite::FMeshDataSection Section;
    Section.MaterialIndex = 0; Section.FirstIndex = 0; Section.NumTriangles = Triangles;
    Section.MinVertexIndex = 0; Section.MaxVertexIndex = Desc.vertex_count - 1;
    Input.Sections.Add(Section);
    Input.VertexBounds = FBounds3f(Bounds.Min, Bounds.Max);
    Input.NumTexCoords = 1;
    FMeshNaniteSettings Settings;
    Settings.bEnabled = true;
    Settings.TargetMinimumResidencyInKB = MAX_uint32;
    auto Resources = MakeShared<Nanite::FResources>();
    if (!Nanite::IBuilderModule::Get().Build(*Resources, Input, nullptr, nullptr, nullptr, Settings)) return nullptr;
    return Resources;
}
}

namespace {
// UE's fast BuildFromMeshDescriptions path, split so the CPU half runs off the game thread (30-60 ms per mesh there
// stalled frames for the first minutes after a load) and Nanite data is attached before the GPU resources exist.
UStaticMesh* NewStaticMesh(UObject* Outer) {
    auto* Mesh = NewObject<UStaticMesh>(Outer);
    FStaticMaterial Slot; Slot.UVChannelData = FMeshUVChannelInfo(1.f); // texture streaming reads UV density
    Mesh->GetStaticMaterials().Add(Slot);
    return Mesh;
}
// Any thread: reads only the mesh's material slots.
TUniquePtr<FStaticMeshRenderData> BuildRenderData(const UStaticMesh& Mesh, const FMeshDescription& Description) {
    auto Data = MakeUnique<FStaticMeshRenderData>();
    Data->AllocateLODResources(1);
    const_cast<UStaticMesh&>(Mesh).BuildFromMeshDescription(Description, Data->LODResources[0]);
    return Data;
}
// Game thread.
void FinishStaticMesh(UStaticMesh* Built, TUniquePtr<FStaticMeshRenderData> RenderData, const FBoxSphereBounds& Bounds, Nanite::FResources* Nanite) {
    Built->SetRenderData(MoveTemp(RenderData));
    FStaticMeshRenderData& Data = *Built->GetRenderData();
    if (Nanite) {
        // Nanite's ray-tracing fallback only counts LODs with a BuffersSize, which only cooking sets.
        Data.LODResources[0].BuffersSize = FMath::Max<uint32>(Data.LODResources[0].BuffersSize, 1);
        *Data.NaniteResourcesPtr = MoveTemp(*Nanite);
    }
    if (Built->bSupportRayTracing && IsRayTracingAllowed()) Data.InitializeRayTracingRepresentationFromRenderingLODs();
    Built->InitResources();
    Data.Bounds = Bounds;
    Built->CalculateExtendedBounds();
    Data.ScreenSize[0].Default = 1.f;
}
}

// Replaces one dynamic mesh queued by Apply with a static mesh (cached draw commands, static ray-tracing geometry),
// with Nanite data when its build has finished.
void FHdueScene::TickConversions() {
    RetiredConversions.RemoveAll([](const FPendingConversion& Retired) { return Retired.Task.IsCompleted(); });
    for (auto It = PendingStatic.CreateIterator(); It; ++It) {
        if (!It.Value().Task.IsCompleted()) continue; // not built yet
        AActor* Actor = Actors.FindRef(It.Key());
        auto* Dynamic = IsValid(Actor) ? Cast<UDynamicMeshComponent>(Actor->GetRootComponent()) : nullptr;
        if (!Dynamic) { It.RemoveCurrent(); continue; }
        const TSharedPtr<FConversion> Conversion = It.Value().Task.GetResult();
        const UMaterialInterface* Material = Dynamic->GetMaterial(0);
        const bool NaniteMaterial = !Material || !IsTranslucentBlendMode(*Material); // Nanite draws opaque and masked only
        UStaticMesh* Built = It.Value().Mesh.Get();
        FinishStaticMesh(Built, MoveTemp(Conversion->RenderData), Conversion->Bounds, NaniteMaterial ? Conversion->Nanite.Get() : nullptr);
        if (Conversion->Nanite && NaniteMaterial) {
            static int32 NaniteMeshes = 0, Tried = 0;
            ++Tried; if (Built->HasValidNaniteData()) ++NaniteMeshes;
            if (Tried % 50 == 1 || PendingStatic.Num() == 1)
                UE_LOG(LogTemp, Display, TEXT("hdUE Nanite: %d of %d converted meshes have Nanite data"), NaniteMeshes, Tried);
        }
        AddRootComponent<UStaticMeshComponent>(Actor, [&](UStaticMeshComponent& Component) {
            Component.SetStaticMesh(Built);
            Component.SetMaterial(0, Dynamic->GetMaterial(0));
        });
        Actor->SetActorTransform(Dynamic->GetComponentTransform());
        Dynamic->DestroyComponent();
        It.RemoveCurrent();
        return; // one per tick keeps frames smooth
    }
}

namespace {
// One Hydra mesh as an FDynamicMesh3; pure CPU work, so Apply builds a batch's meshes in parallel. Each vertex has
// one normal, UV and colour element, shared by its triangles (element ID = vertex ID). A triangle FDynamicMesh3
// rejects on shared vertices (a non-manifold edge, or a repeated or opposite-wound copy as on two-sided cards) gets
// copies of its vertices, as FMeshDescriptionToDynamicMesh does; FNonManifoldMappingSupport maps copies back to
// their source vertex (surface masks are per source vertex).
UE::Geometry::FDynamicMesh3 BuildDynamicMesh(const HdueMeshDesc& Desc) {
    using namespace UE::Geometry;
    FDynamicMesh3 Mesh;
    Mesh.EnableAttributes();
    Mesh.Attributes()->SetNumNormalLayers(1);
    Mesh.Attributes()->SetNumUVLayers(1);
    Mesh.Attributes()->EnablePrimaryColors();
    auto* Colors = Mesh.Attributes()->PrimaryColors();
    auto* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
    auto* UVOverlay = Mesh.Attributes()->PrimaryUV();
    const auto Append = [&](uint32 V) {
        NormalOverlay->AppendElement(Desc.normals ? FVector3f(Desc.normals[V * 3], Desc.normals[V * 3 + 1], Desc.normals[V * 3 + 2]) : FVector3f::ZeroVector);
        UVOverlay->AppendElement(Desc.uv ? FVector2f(Desc.uv[V * 2], Desc.uv[V * 2 + 1]) : FVector2f::ZeroVector);
        Colors->AppendElement(Desc.colors ? FVector4f(Desc.colors[V * 3], Desc.colors[V * 3 + 1], Desc.colors[V * 3 + 2], 1.f) : FVector4f(1.f, 1.f, 1.f, 1.f));
        return Mesh.AppendVertex(FVector3d(Desc.positions[V * 3], Desc.positions[V * 3 + 1], Desc.positions[V * 3 + 2]));
    };
    for (uint32 V = 0; V < Desc.vertex_count; ++V) Append(V);
    TArray<int32> Sources; // per vertex, once a copy exists
    for (uint32 I = 0; I < Desc.index_count; I += 3) {
        FIndex3i Vertices(Desc.indices[I], Desc.indices[I + 1], Desc.indices[I + 2]);
        int32 Triangle = Mesh.AppendTriangle(Vertices);
        if (Triangle == FDynamicMesh3::NonManifoldID || Triangle == FDynamicMesh3::DuplicateTriangleID) {
            if (Sources.IsEmpty()) for (uint32 V = 0; V < Desc.vertex_count; ++V) Sources.Add(int32(V));
            for (int C = 0; C < 3; ++C) { Sources.Add(Vertices[C]); Vertices[C] = Append(Desc.indices[I + C]); }
            Triangle = Mesh.AppendTriangle(Vertices);
        }
        if (Triangle < 0) continue; // degenerate: a repeated vertex
        NormalOverlay->SetTriangle(Triangle, Vertices);
        UVOverlay->SetTriangle(Triangle, Vertices);
        Colors->SetTriangle(Triangle, Vertices);
    }
    if (!Desc.normals) FMeshNormals::InitializeOverlayToPerTriangleNormals(NormalOverlay);
    if (!Sources.IsEmpty()) FNonManifoldMappingSupport::AttachNonManifoldVertexMappingData(Sources, Mesh);
    return Mesh;
}
}
bool FHdueScene::Apply(const HdueSceneBatch& Batch, FString& Error) {
    if ((Batch.material_count && !Batch.materials) || (Batch.mesh_count && !Batch.meshes) ||
        (Batch.light_count && !Batch.lights) || (Batch.removed_count && !Batch.removed_ids)) {
        Error = TEXT("Missing scene batch data."); return false;
    }
    // Validate mesh array bounds before mutating the scene. An invalid mesh is skipped (and its previous version
    // removed) with a warning naming it; the rest of the batch still applies. Arrays are int32-addressed in UE.
    TArray<bool> ValidMeshes; ValidMeshes.Init(true, int32(Batch.mesh_count));
    ParallelFor(int32(Batch.mesh_count), [&](int32 Index) {
        const auto& Mesh = Batch.meshes[Index];
        const TCHAR* Problem = !Mesh.id ? TEXT("no ID") : Mesh.index_count % 3 ? TEXT("index count is not a multiple of 3")
            : Mesh.vertex_count > uint32(MAX_int32 / 3) || Mesh.index_count > uint32(MAX_int32) || (Mesh.instance_transforms && Mesh.instance_count > uint32(MAX_int32 / 16))
            ? TEXT("too large for 32-bit arrays") : (Mesh.vertex_count && !Mesh.positions) || (Mesh.index_count && !Mesh.indices) ? TEXT("missing positions or indices") : nullptr;
        for (uint32 I = 0; !Problem && I < Mesh.index_count; ++I) if (Mesh.indices[I] >= Mesh.vertex_count) Problem = TEXT("index out of range");
        if (!Problem) return;
        ValidMeshes[Index] = false;
        UE_LOG(LogTemp, Warning, TEXT("hdUE: skipped mesh %s (%u vertices, %u indices): %s."), Mesh.id ? UTF8_TO_TCHAR(Mesh.id) : TEXT("without ID"), Mesh.vertex_count, Mesh.index_count, Problem);
    });
    if (IdMode) { FString Ignored; SetIdMode(0, Ignored); }
    if (Batch.reset_scene) {
        Lighting->Clear();
        Assets.Reset();
        for (const auto& Entry : Actors) if (IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
        // Preserve the native host's bootstrap map until an explicit reset.
        // Never sweep another streamed Level's actors during a scene update.
        TArray<AActor*> Fixtures;
        for (TActorIterator<AActor> It(World); It; ++It)
            if (It->GetLevel()==World->PersistentLevel && (It->FindComponentByClass<UMeshComponent>() || It->FindComponentByClass<ULightComponent>())) Fixtures.Add(*It);
        for (auto* A : Fixtures) World->DestroyActor(A);
        Actors.Empty(); CameraLightDirections.Empty(); MeshMaterials.Empty(); Materials.Empty(); PreviewTextures.Empty(); PrimIds.Empty();
        for (const auto& Entry : Volumes) if (IsValid(Entry.Value.Actor)) World->DestroyActor(Entry.Value.Actor);
        Volumes.Empty();
        WetBindings.Empty(); GeometryRevisions.Empty();
    }
    for (uint32 Index = 0; Index < Batch.removed_count; ++Index) {
        if (Batch.removed_ids[Index]) Remove(UTF8_TO_TCHAR(Batch.removed_ids[Index]));
    }
    const double ApplyStart = FPlatformTime::Seconds();
    RetiredMaterials.Reset(); // replaced by the previous Apply; frames that used them have long finished
    TextureSeconds = 0; TextureImports = 0;
    double PrepareSeconds = 0;
    {
        // Decode, mip and block-compress this batch's new textures on all cores: decoding them one by one on this
        // thread took most of a large scene's load (111 Sponza textures, ~19 s), and uncompressed they held ~9 GB.
        // PreviewTexture creates the UTexture2Ds on this thread. A texture is rebuilt when inputs read channels its
        // BC4/BC5 repacking left out.
        TMap<FString, uint8> Files; // variants to build: bit 0 raw, bit 1 sRGB
        for (uint32 Index = 0; Index < Batch.material_count; ++Index) {
            const auto& Desc = Batch.materials[Index];
            if (Desc.native_asset && *Desc.native_asset) continue;
            for (int32 Slot = 0; Slot < HDUE_TEXTURE_SLOT_COUNT; ++Slot) {
                const auto& Texture = Desc.textures[Slot];
                if (!Texture.file || !*Texture.file) continue;
                const FString File = UTF8_TO_TCHAR(Texture.file);
                const bool SRGB = hdue_texture_is_color(Slot) != 0, Vector = !ScalarSlot(Slot);
                uint8 Read = 0;
                for (int32 Channel = 0; Channel < 4; ++Channel) if (!Vector && Texture.channel_mask[Channel] != 0.f) Read |= 1 << Channel;
                FPreviewTexture& Entry = PreviewTextures.FindOrAdd(TextureKey(File, SRGB));
                if (Entry.Texture && (Entry.Read | Read) == Entry.Read && (Entry.Vector || !Vector)) continue;
                Entry.Read |= Read; Entry.Vector |= Vector;
                Files.FindOrAdd(File) |= 1 << int32(SRGB);
            }
        }
        if (Files.Num()) {
            const double PrepareStart = FPlatformTime::Seconds();
            const TArray<TPair<FString, uint8>> Work = Files.Array();
            ParallelFor(Work.Num(), [&](int32 Index) {
                FImage Image;
                for (const bool SRGB : {false, true}) {
                    if (!(Work[Index].Value & (1 << int32(SRGB)))) continue;
                    FPreviewTexture& Entry = PreviewTextures[TextureKey(Work[Index].Key, SRGB)];
                    const FString Cache = CachePath(Work[Index].Key, Entry, SRGB);
                    if (!Cache.IsEmpty() && (Entry.Pending = ReadCache(Cache, Entry))) continue;
                    if (!Image.RawData.Num() && !FImageUtils::LoadImage(*Work[Index].Key, Image)) return;
                    const bool Last = SRGB || !(Work[Index].Value & 2);
                    Entry.Pending = PrepareTexture(Last ? MoveTemp(Image) : FImage(Image), Entry, SRGB);
                    if (!Cache.IsEmpty() && IsBlockCompressedFormat(Entry.Pending->PixelFormat)) WriteCache(Cache, *Entry.Pending);
                }
            });
            PrepareSeconds = FPlatformTime::Seconds() - PrepareStart;
        }
    }
    for (uint32 Index = 0; Index < Batch.material_count; ++Index) {
        const auto& Desc = Batch.materials[Index];
        if (!Desc.id) { Error = TEXT("Material has no ID."); return false; }
        UMaterialInterface* Material = nullptr;
        if (Desc.native_asset && *Desc.native_asset) {
            Material = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (Material) {
                const auto* Entry=Materials.Find(UTF8_TO_TCHAR(Desc.id));
                auto* Existing=Entry?Cast<UMaterialInstanceDynamic>(Entry->Get()):nullptr;
                if (Existing && Existing->Parent==Material) { Existing->ClearParameterValues(); Material=Existing; }
                else Material=UMaterialInstanceDynamic::Create(Material,World);
            }
        } else {
            if (!FMath::IsFinite(Desc.base_color[3])) { Error = TEXT("Material opacity must be finite."); return false; }
            const float Opacity = FMath::Clamp(Desc.base_color[3], 0.f, 1.f);
            const auto HasTexture = [&](int32 Slot) { return Desc.textures[Slot].file && *Desc.textures[Slot].file; };
            // UsdPreviewSurface: opacityThreshold > 0 is cutout; otherwise partial opacity is see-through.
            const bool Masked = Desc.opacity_threshold > 0.f;
            const bool Translucent = !Masked && (Opacity < 1.f || HasTexture(HDUE_TEXTURE_OPACITY));
            // MaterialX subsurface and sheen need the layered Standard Surface parents (UE's Substrate standard_surface,
            // several slabs per pixel); everything else stays on the single-slab Preview Surface parents.
            const bool Layered = !Translucent && (Desc.subsurface > 0.f || Desc.sheen > 0.f);
            const bool Thin = (Desc.flags & HDUE_MATERIAL_THIN_WALLED) != 0;
            const FString ParentName = Layered ? FString::Printf(TEXT("M_StandardSurface%s%s"), Masked ? TEXT("Masked") : TEXT(""), Thin ? TEXT("Thin") : TEXT(""))
                : Masked ? TEXT("M_PreviewSurfaceMasked") : Translucent ? TEXT("M_PreviewSurfaceTranslucent") : TEXT("M_PreviewSurface");
            auto* Parent = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/HdUE/%s.%s"), *ParentName, *ParentName));
            if (Parent) {
                auto* Instance = UMaterialInstanceDynamic::Create(Parent, World);
                static const TCHAR* Slots[HDUE_TEXTURE_SLOT_COUNT] = {TEXT("BaseColor"), TEXT("Emissive"), TEXT("Metallic"), TEXT("Roughness"), TEXT("Opacity"), TEXT("Normal"), TEXT("Displacement"),
                    TEXT("Occlusion"), TEXT("Clearcoat"), TEXT("ClearcoatRoughness"), TEXT("SpecularColor")};
                const FLinearColor Constants[HDUE_TEXTURE_SLOT_COUNT] = {
                    FLinearColor(Desc.base_color[0], Desc.base_color[1], Desc.base_color[2], 1),
                    FLinearColor(Desc.emissive_color[0], Desc.emissive_color[1], Desc.emissive_color[2], 0),
                    FLinearColor(FVector4f(FMath::Clamp(Desc.metallic, 0.f, 1.f))),
                    FLinearColor(FVector4f(FMath::Clamp(Desc.roughness, 0.f, 1.f))),
                    FLinearColor(FVector4f(Opacity)),
                    FLinearColor(0, 0, 1, 0),
                    FLinearColor(FVector4f(Desc.displacement)),
                    FLinearColor(FVector4f(Desc.occlusion)), FLinearColor(FVector4f(Desc.clearcoat)),
                    FLinearColor(FVector4f(Desc.clearcoat_roughness)),
                    FLinearColor(Desc.specular_color[0], Desc.specular_color[1], Desc.specular_color[2], 1)};
                for (int32 Slot = 0; Slot < HDUE_TEXTURE_SLOT_COUNT; ++Slot) {
                    const auto& Texture = Desc.textures[Slot];
                    const FString Name = Slots[Slot];
                    if (!HasTexture(Slot)) {
                        Instance->SetVectorParameterValue(*(Name + TEXT("Scale")), FLinearColor(0, 0, 0, 0));
                        Instance->SetVectorParameterValue(*(Name + TEXT("Bias")), Constants[Slot]);
                        continue;
                    }
                    const FPreviewTexture* Image = PreviewTexture(UTF8_TO_TCHAR(Texture.file), hdue_texture_is_color(Slot) != 0, Error);
                    if (!Image) return false;
                    // A repacked texture (BC4/BC5) stores the channels this input reads first.
                    const auto Stored = [&](const float* Value) {
                        if (Image->Channels.IsEmpty()) return FLinearColor(Value[0], Value[1], Value[2], Value[3]);
                        float Packed[4] = {};
                        for (int32 Channel = 0; Channel < Image->Channels.Num(); ++Channel) Packed[Channel] = Value[Image->Channels[Channel]];
                        return FLinearColor(Packed[0], Packed[1], Packed[2], Packed[3]);
                    };
                    Instance->SetTextureParameterValue(*(Name + TEXT("Texture")), Image->Texture.Get());
                    Instance->SetVectorParameterValue(*(Name + TEXT("Scale")), Stored(Texture.scale));
                    Instance->SetVectorParameterValue(*(Name + TEXT("Bias")), Stored(Texture.bias));
                    Instance->SetVectorParameterValue(*(Name + TEXT("Mask")), Stored(Texture.channel_mask));
                }
                static const float Identity[6] = {1, 0, 0, 0, 1, 0};
                const bool Unset = Algo::AllOf(Desc.uv_transform, [](float Value) { return Value == 0.f; });
                const float* M = Unset ? Identity : Desc.uv_transform;
                Instance->SetVectorParameterValue(TEXT("UVRow0"), FLinearColor(M[0], M[1], M[2], 0));
                Instance->SetVectorParameterValue(TEXT("UVRow1"), FLinearColor(M[3], M[4], M[5], 0));
                if (Masked) Instance->SetScalarParameterValue(TEXT("OpacityThreshold"), Desc.opacity_threshold);
                // ior -> dielectric F0 = ((ior - 1) / (ior + 1))^2; UE's Specular 0.5 is F0 0.04, so Specular = F0 / 0.08.
                const float Ior = Desc.ior > 0.f ? Desc.ior : 1.5f;
                const float F0 = FMath::Square((Ior - 1.f) / (Ior + 1.f));
                Instance->SetScalarParameterValue(TEXT("SpecularLevel"), FMath::Clamp(F0 / 0.08f, 0.f, 1.f));
                Instance->SetScalarParameterValue(TEXT("UseSpecularWorkflow"), (Desc.flags & HDUE_MATERIAL_SPECULAR_WORKFLOW) ? 1.f : 0.f);
                if (Layered) {
                    const auto Color = [](const float* C) { return FLinearColor(C[0], C[1], C[2], 1); };
                    Instance->SetScalarParameterValue(TEXT("SpecularIOR"), Ior);
                    Instance->SetVectorParameterValue(TEXT("CoatColor"), Color(Desc.coat_color));
                    Instance->SetScalarParameterValue(TEXT("Subsurface"), Desc.subsurface);
                    Instance->SetVectorParameterValue(TEXT("SubsurfaceColor"), Color(Desc.subsurface_color));
                    Instance->SetVectorParameterValue(TEXT("SubsurfaceRadius"), Color(Desc.subsurface_radius));
                    Instance->SetScalarParameterValue(TEXT("SubsurfaceBaseTexture"), (Desc.flags & HDUE_MATERIAL_SUBSURFACE_BASE_TEXTURE) ? 1.f : 0.f);
                    Instance->SetScalarParameterValue(TEXT("Sheen"), Desc.sheen);
                    Instance->SetVectorParameterValue(TEXT("SheenColor"), Color(Desc.sheen_color));
                    Instance->SetScalarParameterValue(TEXT("SheenRoughness"), Desc.sheen_roughness);
                    Instance->SetScalarParameterValue(TEXT("SheenBaseTexture"), (Desc.flags & HDUE_MATERIAL_SHEEN_BASE_TEXTURE) ? 1.f : 0.f);
                }
                Instance->SetScalarParameterValue(TEXT("UseVertexColor"), (Desc.flags & HDUE_MATERIAL_VERTEX_COLOR) ? 1.f : 0.f);
                Material = Instance;
            }
        }
        if (!Material) { Error = TEXT("Material asset missing. Prepare/cook content or check the native asset path."); return false; }
        // A replaced material can still be referenced by frames in flight on the render thread (ray-tracing instance
        // gathering crashed on a material edit); keep it until the next Apply.
        if (auto* Previous = Materials.Find(UTF8_TO_TCHAR(Desc.id)); Previous && Previous->Get() != Material) RetiredMaterials.Add(MoveTemp(*Previous));
        Materials.Add(UTF8_TO_TCHAR(Desc.id), TStrongObjectPtr<UMaterialInterface>(Material));
    }
    const double MaterialsDone = FPlatformTime::Seconds();
    // Assemble the generated meshes on all cores; components and assets are created below on this thread. Meshes that
    // are not edited after upload become static meshes: UDynamicMeshComponent rebuilt its draw commands and dynamic
    // ray-tracing instances every frame.
    TArray<UE::Geometry::FDynamicMesh3> BuiltMeshes; BuiltMeshes.SetNum(Batch.mesh_count);
    TArray<FMeshDescription> Descriptions; Descriptions.SetNum(Batch.mesh_count);
    // Nanite for converted meshes (interactive hosts); HDUE_NANITE=0 keeps them regular static meshes.
    static const bool NaniteMeshes = Interactive_() && FPlatformMisc::GetEnvironmentVariable(TEXT("HDUE_NANITE")) != TEXT("0");
    if (NaniteMeshes) Nanite::IBuilderModule::Get(); // load the builder on this thread before worker tasks use it
    // Interactive hosts (Houdini's viewport) convert generated meshes to static ones in the background, one per tick
    // (TickConversions): building static render data on this thread made a large scene's load 4x slower. Offline
    // hosts (husk) keep dynamic meshes, which are cheaper for a few frames.
    static const bool Interactive = FParse::Param(FCommandLine::Get(), TEXT("hdUEInteractive"));
    const auto StaticGenerated = [&](const HdueMeshDesc& Desc) {
        return !(Desc.native_asset && *Desc.native_asset) && Desc.instance_transforms != nullptr;
    };
    const auto ConvertLater = [&](const HdueMeshDesc& Desc) {
        return Interactive && !(Desc.native_asset && *Desc.native_asset) && !Desc.instance_transforms && !(Desc.flags & HDUE_MESH_EDITABLE);
    };
    ParallelFor(int32(Batch.mesh_count), [&](int32 Index) {
        const auto& Desc = Batch.meshes[Index];
        if (!ValidMeshes[Index] || (Desc.native_asset && *Desc.native_asset)) return;
        BuiltMeshes[Index] = BuildDynamicMesh(Desc);
        if (StaticGenerated(Desc)) {
            FStaticMeshAttributes Attributes(Descriptions[Index]); Attributes.Register();
            FDynamicMeshToMeshDescription().Convert(&BuiltMeshes[Index], Descriptions[Index]);
            BuiltMeshes[Index].Clear();
        }
    });
    for (uint32 Index = 0; Index < Batch.mesh_count; ++Index) {
        const auto& Desc = Batch.meshes[Index];
        if (!ValidMeshes[int32(Index)]) { if (Desc.id) Remove(UTF8_TO_TCHAR(Desc.id)); continue; }
        const FString Id = UTF8_TO_TCHAR(Desc.id);
        AActor* Actor = Actors.FindRef(Id);
        const bool Native = Desc.native_asset && *Desc.native_asset;
        // Hydra instancer prototypes draw through one ISM: the native asset, or a static mesh built here.
        const bool Instanced = Desc.instance_transforms != nullptr;
        const bool Static = StaticGenerated(Desc);
        UClass* Wanted = Instanced ? UInstancedStaticMeshComponent::StaticClass() : (Native || Static) ? UStaticMeshComponent::StaticClass() : UDynamicMeshComponent::StaticClass();
        if (Actor && (!Actor->GetRootComponent() || Actor->GetRootComponent()->GetClass() != Wanted)) {
            Remove(Id); Actor = nullptr;
        }
        if (!Actor) {
            Actor = World->SpawnActor<AActor>();
            Actors.Add(Id, Actor);
            if (Native || Instanced || Static) AddRootComponent<UStaticMeshComponent>(Actor, [](UStaticMeshComponent&) {}, Wanted);
            else AddRootComponent<UDynamicMeshComponent>(Actor, [](UDynamicMeshComponent& Component) {
                Component.SetEnableRaytracing(true);
                Component.SetTangentsType(EDynamicMeshComponentTangentsMode::AutoCalculated);
                Component.SetVertexColorSpaceTransformMode(EDynamicMeshVertexColorTransformMode::NoTransform);
            });
        }
        Actor->SetActorHiddenInGame(!Desc.visible);
        FMatrix Matrix;
        for (int R = 0; R < 4; ++R) for (int C = 0; C < 4; ++C) Matrix.M[R][C] = Instanced ? (R == C) : Desc.transform[R * 4 + C];
        Actor->SetActorTransform(FTransform(Matrix));
        UStaticMesh* InstanceMesh = nullptr;
        if (Native && Instanced) {
            InstanceMesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (!InstanceMesh) { Error = TEXT("Native StaticMesh not found in cooked content."); return false; }
        } else if (Native) {
            auto* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (!Mesh) { Error = TEXT("Native StaticMesh not found in cooked content."); return false; }
            if ((Desc.flags & HDUE_MESH_REQUIRE_NANITE) && !Mesh->HasValidNaniteData()) { Error = TEXT("Native asset has no cooked Nanite data."); return false; }
            UE_LOG(LogTemp, Display, TEXT("hdUE native mesh %s: NaniteData=%d"), *Mesh->GetPathName(), Mesh->HasValidNaniteData());
            Actor->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Mesh);
        } else {
            using namespace UE::Geometry;
            if (Static) {
                auto* Built = NewStaticMesh(Actor);
                FinishStaticMesh(Built, BuildRenderData(*Built, Descriptions[Index]), Descriptions[Index].GetBounds(), nullptr);
                if (Instanced) InstanceMesh = Built;
                else Actor->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Built);
            } else {
                Actor->FindComponentByClass<UDynamicMeshComponent>()->SetMesh(MoveTemp(BuiltMeshes[Index]));
                // A conversion still running keeps its mesh alive until it finishes (its worker uses it).
                if (FPendingConversion Previous; PendingStatic.RemoveAndCopyValue(Id, Previous) && !Previous.Task.IsCompleted()) RetiredConversions.Add(MoveTemp(Previous));
                if (ConvertLater(Desc)) {
                    // The static mesh description and Nanite data are built on background workers, from a copy of the
                    // descriptor's arrays: done here, they doubled the time to the first frame (and normal-priority
                    // Nanite tasks starved its rendering for 15 s).
                    auto Copy = MakeShared<FNaniteSource>(Desc);
                    UStaticMesh* Mesh = NewStaticMesh(Actor);
                    PendingStatic.Add(Id, {TStrongObjectPtr<UStaticMesh>(Mesh), UE::Tasks::Launch(TEXT("HdueConversion"), [Copy, Mesh]() -> TSharedPtr<FConversion> {
                        TSharedPtr<FConversion> Result = MakeShared<FConversion>();
                        UE::Geometry::FDynamicMesh3 Dynamic = BuildDynamicMesh(Copy->Desc);
                        FMeshDescription Description;
                        FStaticMeshAttributes Attributes(Description); Attributes.Register();
                        FDynamicMeshToMeshDescription().Convert(&Dynamic, Description);
                        Result->RenderData = BuildRenderData(*Mesh, Description);
                        Result->Bounds = Description.GetBounds();
                        if (NaniteMeshes) Result->Nanite = BuildNanite(Copy->Desc);
                        return Result;
                    }, UE::Tasks::ETaskPriority::BackgroundLow)});
                }
            }
            ++GeometryRevisions.FindOrAdd(Id); ++GeometryUploads;
        }
        if (Instanced) {
            auto* Instances = Actor->FindComponentByClass<UInstancedStaticMeshComponent>();
            Instances->SetStaticMesh(InstanceMesh);
            TArray<FTransform> Transforms; Transforms.Reserve(Desc.instance_count);
            for (uint32 I = 0; I < Desc.instance_count; ++I) {
                FMatrix Instance;
                for (int R = 0; R < 4; ++R) for (int C = 0; C < 4; ++C) Instance.M[R][C] = Desc.instance_transforms[I * 16 + R * 4 + C];
                Transforms.Add(FTransform(Instance));
            }
            Instances->ClearInstances();
            // Per-instance displayColor (0..2) for M_PreviewSurface's PerInstanceCustomData3Vector and
            // the Hydra instanceId (3) for M_HdUEId's instance pass.
            Instances->SetNumCustomDataFloats(4);
            Instances->AddInstances(Transforms, false, true);
            for (uint32 I = 0; I < Desc.instance_count; ++I) {
                float Data[4] = {1.f, 1.f, 1.f, float(Desc.instance_ids ? Desc.instance_ids[I] : int32(I))};
                if (Desc.instance_colors) FMemory::Memcpy(Data, Desc.instance_colors + I * 3, sizeof(float) * 3);
                Instances->SetCustomData(int32(I), MakeArrayView(Data, 4), I + 1 == Desc.instance_count);
            }
        }
        MeshMaterials.Add(Id, Desc.material_id ? UTF8_TO_TCHAR(Desc.material_id) : TEXT(""));
        PrimIds.Add(Id, Desc.prim_id);
    }
    for (uint32 Index = 0; Index < Batch.light_count; ++Index) {
        const auto& Desc = Batch.lights[Index];
        if (!Desc.id || Desc.type > HDUE_CAMERA_DISTANT_LIGHT) { Error = TEXT("Unsupported light."); return false; }
        const FString Id = UTF8_TO_TCHAR(Desc.id);
        Remove(Id);
        auto* Actor = World->SpawnActor<AActor>();
        Actors.Add(Id, Actor);
        ULightComponent* Light = (Desc.type == HDUE_DISTANT_LIGHT || Desc.type == HDUE_CAMERA_DISTANT_LIGHT) ? static_cast<ULightComponent*>(NewObject<UDirectionalLightComponent>(Actor)) :
            Desc.type == HDUE_SPHERE_LIGHT ? static_cast<ULightComponent*>(NewObject<UPointLightComponent>(Actor)) : static_cast<ULightComponent*>(NewObject<URectLightComponent>(Actor));
        Light->SetMobility(EComponentMobility::Movable);
        Actor->SetRootComponent(Light); Actor->AddInstanceComponent(Light);
        Light->RegisterComponent();
        Light->SetLightColor(FLinearColor(Desc.color[0], Desc.color[1], Desc.color[2]));
        Light->SetIntensity(FMath::Max(0.f, Desc.intensity));
        Actor->SetActorLocation(FVector(Desc.position[0], Desc.position[1], Desc.position[2]));
        Actor->SetActorRotation(FVector(Desc.direction[0], Desc.direction[1], Desc.direction[2]).Rotation());
        Actor->SetActorHiddenInGame(!Desc.visible);
        if (Desc.type == HDUE_CAMERA_DISTANT_LIGHT)
            CameraLightDirections.Add(Id, FVector(Desc.direction[0], Desc.direction[1], Desc.direction[2]));
        if (auto* Point = Cast<UPointLightComponent>(Light)) { Point->SetAttenuationRadius(100000); Point->SetSourceRadius(FMath::Max(0.1f, Desc.width_cm)); }
        if (auto* Rect = Cast<URectLightComponent>(Light)) { Rect->SetSourceWidth(Desc.width_cm); Rect->SetSourceHeight(Desc.height_cm); Rect->SetAttenuationRadius(100000); }
    }
    const double MeshesDone = FPlatformTime::Seconds();
    BindMaterials();
    if (Batch.material_count || Batch.mesh_count)
        UE_LOG(LogTemp, Display, TEXT("hdUE apply: %u materials %.0f ms (%d textures: prepare %.0f ms, create %.0f ms), %u meshes %.0f ms, bind %.0f ms"),
            Batch.material_count, (MaterialsDone - ApplyStart) * 1000, TextureImports, PrepareSeconds * 1000, TextureSeconds * 1000, Batch.mesh_count,
            (MeshesDone - MaterialsDone) * 1000, (FPlatformTime::Seconds() - MeshesDone) * 1000);
    return true;
}
void FHdueScene::UpdateCameraLights(const FVector& Eye, const FVector& Target, const FVector& Up) {
    Lighting->UpdateCamera(Eye,Target,Up);
    const FVector Forward = (Target - Eye).GetSafeNormal();
    const FVector Right = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    const FVector CameraUp = FVector::CrossProduct(Forward, Right).GetSafeNormal();
    for (const auto& Entry : CameraLightDirections) {
        AActor* Actor = Actors.FindRef(Entry.Key);
        if (!IsValid(Actor)) continue;
        const FVector Direction = (Forward * Entry.Value.X + Right * Entry.Value.Y + CameraUp * Entry.Value.Z).GetSafeNormal();
        Actor->SetActorLocationAndRotation(Eye, Direction.Rotation());
    }
}
