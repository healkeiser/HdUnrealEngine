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
#include "ImageCore.h"
#include "RenderUtils.h"
#include "ImageCoreUtils.h"
#include "ispc_texcomp.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Hash/Blake3.h"
#include "Hash/CityHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "NaniteBuilder.h"
#include "Rendering/NaniteResources.h"
#include "RayTracingGeometryManagerInterface.h"
#include "StaticMeshResources.h"
#include "Tasks/Task.h"
#include "mikktspace.h"

FHdueScene::FHdueScene(UWorld* InWorld, bool InInteractive)
    : Assets(InWorld), Lighting(MakeUnique<FHdueLighting>(InWorld)), World(InWorld), Interactive(InInteractive) {}
FHdueScene::~FHdueScene() {
    for (auto& Entry : Actors)
        if (IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
}
TArray<TStrongObjectPtr<UTexture2D>> FHdueScene::GatherVirtualTextures() const {
    TSet<UTexture2D*> Unique;
    TArray<TStrongObjectPtr<UTexture2D>> Result;
    TArray<AActor*> All = Assets.GetActors();
    for (const auto& Entry : Actors) All.Add(Entry.Value);
    for (auto* Actor : All) {
        if (!IsValid(Actor) || Actor->IsHidden()) continue;
        TArray<UMeshComponent*> Meshes;
        Actor->GetComponents(Meshes);
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
    OneSided.Remove(Id);
    MeshMaterials.Remove(Id);
    SingleSided.Remove(Id);
    PrimIds.Remove(Id);
    WetBindings.Remove(Id);
    GeometryRevisions.Remove(Id);
}
namespace {
// The root component of an hdUE actor: movable and render-only. Collision stays off, so UE neither cooks physics
// for generated meshes (which runtime builds cannot provide) nor runs queries against them.
template <class T>
T* AddRootComponent(AActor* Actor, TFunctionRef<void(T&)> Configure, UClass* Class = T::StaticClass()) {
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
}  // namespace

bool FHdueScene::UpsertVolume(const HdueVolumeDesc& Desc, FString& Error) {
    const FString Id = UTF8_TO_TCHAR(Desc.id);
    const uint64 Count = uint64(Desc.width) * Desc.height * Desc.depth;
    auto* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUEVolume.M_HdUEVolume"));
    if (!Parent) {
        Error =
            TEXT("Volume material /Game/HdUE/M_HdUEVolume is missing; run Prepare-Content.ps1 and stage the runtime.");
        return false;
    }
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
        if (!Texture || Texture->GetSizeX() != int32(Desc.width) || Texture->GetSizeY() != int32(Desc.height) ||
            Texture->GetSizeZ() != int32(Desc.depth)) {
            Texture = UVolumeTexture::CreateTransient(Desc.width, Desc.height, Desc.depth, PF_R16F);
            if (!Texture) {
                Error = TEXT("Cannot create the volume texture.");
                return false;
            }
            Texture->SRGB = false;
            Texture->Filter = TF_Trilinear;
            Texture->NeverStream = true;
            Volume.Texture.Reset(Texture);
        }
        auto& Mip = Texture->GetPlatformData()->Mips[0];
        auto* Voxels = static_cast<FFloat16*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
        for (uint64 I = 0; I < Count; ++I) Voxels[I] = FFloat16(FMath::Max(0.f, Desc.density[I]));
        Mip.BulkData.Unlock();
        Texture->UpdateResource();
        Volume.Material->SetTextureParameterValue(TEXT("Density"), Texture);
    }
    Volume.Material->SetVectorParameterValue(TEXT("Albedo"),
                                             FLinearColor(Desc.albedo[0], Desc.albedo[1], Desc.albedo[2], 1));
    Volume.Material->SetScalarParameterValue(TEXT("ExtinctionScale"), Desc.extinction_scale);
    Component->SetMaterial(0, Volume.Material.Get());
    Component->SetVolumeResolution(FIntVector(Desc.width, Desc.height, Desc.depth));
    FMatrix Matrix;
    for (int R = 0; R < 4; ++R)
        for (int C = 0; C < 4; ++C) Matrix.M[R][C] = Desc.transform[R * 4 + C];
    Volume.Actor->SetActorTransform(FTransform(Matrix));
    // UVW_j = dot((world, 1), row_j): column j of the inverse voxel transform over the resolution.
    const FMatrix Inverse = Matrix.Inverse();
    const double Size[3] = {double(FMath::Max(1u, Desc.width)), double(FMath::Max(1u, Desc.height)),
                            double(FMath::Max(1u, Desc.depth))};
    for (int32 Axis = 0; Axis < 3; ++Axis)
        Volume.Material->SetVectorParameterValue(
            *FString::Printf(TEXT("WorldToUVW%d"), Axis),
            FLinearColor(float(Inverse.M[0][Axis] / Size[Axis]), float(Inverse.M[1][Axis] / Size[Axis]),
                         float(Inverse.M[2][Axis] / Size[Axis]), float(Inverse.M[3][Axis] / Size[Axis])));
    Volume.Actor->SetActorHiddenInGame(!Desc.visible || !Count);
    return true;
}
void FHdueScene::RemoveVolume(const FString& Id) {
    if (FVolume* Volume = Volumes.Find(Id)) {
        if (IsValid(Volume->Actor)) World->DestroyActor(Volume->Actor);
        Volumes.Remove(Id);
    }
}
void FHdueScene::ApplyLinks() {
    for (const auto& Entry : Actors)
        if (IsValid(Entry.Value)) Lighting->Link(Entry.Key, Cast<UPrimitiveComponent>(Entry.Value->GetRootComponent()));
}
void FHdueScene::SetTransforms(const FString& Id, uint32 Count, const double* Matrices) {
    AActor* Actor = Actors.FindRef(Id);
    if (!IsValid(Actor) || !Count) return;
    const auto ToTransform = [&](uint32 Index) {
        FMatrix Matrix;
        for (int R = 0; R < 4; ++R)
            for (int C = 0; C < 4; ++C) Matrix.M[R][C] = Matrices[Index * 16 + R * 4 + C];
        return FTransform(Matrix);
    };
    if (auto* Instances = Cast<UInstancedStaticMeshComponent>(Actor->GetRootComponent())) {
        if (int32(Count) != Instances->GetInstanceCount()) return;
        TArray<FTransform> Transforms;
        Transforms.Reserve(Count);
        for (uint32 I = 0; I < Count; ++I) Transforms.Add(ToTransform(I));
        Instances->BatchUpdateInstancesTransforms(0, Transforms, true, true, true);
    } else if (Count == 1)
        Actor->SetActorTransform(ToTransform(0), false, nullptr, ETeleportType::TeleportPhysics);
}
bool FHdueScene::SetIdMode(int32 Mode, FString& Error) {
    if (Mode == IdMode) return true;
    const bool On = Mode != 0;
    UMaterialInterface* IdMaterial = nullptr;
    if (On && !(IdMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUEId.M_HdUEId")))) {
        Error = TEXT("ID material /Game/HdUE/M_HdUEId is missing; run Prepare-Content.ps1 and stage the runtime.");
        return false;
    }
    const auto Swap = [&](AActor* Actor, int32 Id) {
        TArray<UMeshComponent*> Meshes;
        Actor->GetComponents(Meshes);
        for (auto* Mesh : Meshes) {
            if (On) {
                // Exact in the float16 readback: both halves stay below 2048.
                // Instance mode: instanced components emit their per-instance id (custom data 3), the rest -1.
                const bool Instances = Mode == 2 && Mesh->IsA<UInstancedStaticMeshComponent>();
                Mesh->SetCustomPrimitiveDataVector4f(0, Id >= 0 && Mode == 1
                                                            ? FVector4f(float(Id / 2048), float(Id % 2048), 1, 0)
                                                            : FVector4f(0, 0, 0, 0));
                Mesh->SetCustomPrimitiveDataFloat(4, Instances ? 1.f : 0.f);
                for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot) Mesh->SetMaterial(Slot, IdMaterial);
            } else if (auto* Static = Cast<UStaticMeshComponent>(Mesh))
                Static->EmptyOverrideMaterials();
        }
    };
    for (const auto& Entry : Actors)
        if (IsValid(Entry.Value)) {
            const int32* Id = PrimIds.Find(Entry.Key);
            Swap(Entry.Value, Id ? *Id : -1);
        }
    // ponytail: native catalog assets carry no Hydra prim ID; they read as background in the ID pass.
    for (auto* Actor : Assets.GetActors())
        if (IsValid(Actor)) Swap(Actor, -1);
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
            struct {
                float ToLinear[256];
                uint8 ToSRGB[4096];
            } T;
            for (int32 I = 0; I < 256; ++I) T.ToLinear[I] = FLinearColor::FromSRGBColor(FColor(I, 0, 0)).R;
            for (int32 I = 0; I < 4096; ++I) T.ToSRGB[I] = FLinearColor(I / 4095.f, 0, 0).ToFColor(true).R;
            return T;
        }();
        const bool SRGB = Source.GammaSpace == EGammaSpace::sRGB;
        const FColor* In = reinterpret_cast<const FColor*>(Source.RawData.GetData());
        FColor* Out = reinterpret_cast<FColor*>(Target.RawData.GetData());
        ParallelFor(TH, [&](int32 Y) {
            for (int32 X = 0; X < TW; ++X) {
                const FColor Texels[4] = {In[Tap(2 * X, 2 * Y)], In[Tap(2 * X + 1, 2 * Y)], In[Tap(2 * X, 2 * Y + 1)],
                                          In[Tap(2 * X + 1, 2 * Y + 1)]};
                const auto Average = [&](uint8 FColor::* Channel, bool Linear) {
                    if (!Linear)
                        return uint8(
                            (Texels[0].*Channel + Texels[1].*Channel + Texels[2].*Channel + Texels[3].*Channel + 2) /
                            4);
                    float Sum = 0;
                    for (const FColor& Texel : Texels) Sum += Tables.ToLinear[Texel.*Channel];
                    return Tables.ToSRGB[FMath::Clamp(int32(Sum * (4095.f / 4.f) + .5f), 0, 4095)];
                };
                Out[int64(Y) * TW + X] = FColor(Average(&FColor::R, SRGB), Average(&FColor::G, SRGB),
                                                Average(&FColor::B, SRGB), Average(&FColor::A, false));
            }
        });
        return;
    }
    FImageCore::ResizeImage(Source, Target);  // float and other formats: rare, keep UE's filter
}
TArray<FImage> MipChain(FImage&& Image) {
    ERawImageFormat::Type Raw;
    FImageCoreUtils::GetPixelFormatForRawImageFormat(Image.Format, &Raw);
    TArray<FImage> Chain;
    if (Image.Format == Raw)
        Chain.Add(MoveTemp(Image));
    else
        Image.CopyTo(Chain.AddDefaulted_GetRef(), Raw, Image.GammaSpace);
    while (Chain.Last().SizeX > 1 || Chain.Last().SizeY > 1) {
        const FImage& Previous = Chain.Last();
        FImage Next(FMath::Max(1, Previous.SizeX / 2), FMath::Max(1, Previous.SizeY / 2), Raw, Previous.GammaSpace);
        HalveImage(Previous, Next);
        Chain.Add(MoveTemp(Next));
    }
    return Chain;
}
// UE's "scale mips for alpha coverage" (UTexture::bDoScaleMipsForAlphaCoverage, TextureCompressorModule.cpp
// ComputeAlphaCoverage / ComputeAlphaScale / GenerateMip2x2): a mip's channel is the 2x2 average of the previous level
// times the scale, found by binary search in [0, 4] (8 steps), that gives the level the top level's coverage (texels >=
// Threshold). Levels stay unclamped floats down the chain, clamped only when stored, as in UE. Two changes, both ones
// UE's ComputeAlphaScale comment names but keeps for stable cooks: the scale is searched on the level it scales (UE
// measures the previous level, a level late: thin branches then lost more than plain mips), and the closest tested
// scale wins (UE returns an untested midpoint: 2x2 and 1x1 levels went fully opaque). And a level is only ever scaled
// up: with a clip below 0.5, averaging grows chunky cutouts (Sponza's cypress leaves: 6.4% of texels at the top, 9.5%
// at 32x32), and thinning them back left Lumen's tree 12% less covered than the Path Tracer's, against 3% more
// unscaled. Without it, thin cutouts (branches) average below the clip value and vanish with distance.
void ScaleAlphaCoverage(TArray<FImage>& Chain, uint8 Read, float Threshold) {
    static const uint8 Bytes[4] = {2, 1, 0, 3};  // byte of each RGBA channel in an FColor
    const auto Coverage = [Threshold](const TArray<float>& Level, float Scale) {
        int64 Covered = 0;
        for (float Value : Level) Covered += Value * Scale >= Threshold;
        return float(Covered) / float(Level.Num());
    };
    for (int32 Channel = 0; Channel < 4; ++Channel) {
        if (!(Read & (1 << Channel))) continue;
        TArray<float> Previous;
        Previous.SetNumUninitialized(int64(Chain[0].SizeX) * Chain[0].SizeY);
        for (int64 I = 0; I < Previous.Num(); ++I) Previous[I] = Chain[0].RawData[I * 4 + Bytes[Channel]] / 255.f;
        const float Goal = Coverage(Previous, 1);
        for (int32 Level = 1; Level < Chain.Num(); ++Level) {
            const int32 SW = Chain[Level - 1].SizeX, SH = Chain[Level - 1].SizeY;
            FImage& Mip = Chain[Level];
            TArray<float> Next;
            Next.SetNumUninitialized(int64(Mip.SizeX) * Mip.SizeY);
            const auto Tap = [&](int32 X, int32 Y) {
                return Previous[int64(FMath::Min(Y, SH - 1)) * SW + FMath::Min(X, SW - 1)];
            };
            for (int32 Y = 0; Y < Mip.SizeY; ++Y)
                for (int32 X = 0; X < Mip.SizeX; ++X)
                    Next[int64(Y) * Mip.SizeX + X] = (Tap(2 * X, 2 * Y) + Tap(2 * X + 1, 2 * Y) +
                                                      Tap(2 * X, 2 * Y + 1) + Tap(2 * X + 1, 2 * Y + 1)) *
                                                     .25f;
            float Min = 0, Max = 4, Scale = 1, Best = 1, BestError = 2;
            for (int32 Step = 0; Step < 8; ++Step) {
                const float Computed = Coverage(Next, Scale), Error = FMath::Abs(Computed - Goal);
                if (Error < BestError) BestError = Error, Best = Scale;
                if (Error < KINDA_SMALL_NUMBER || (Step == 0 && Computed > Goal)) break;  // only ever thickens
                (Computed < Goal ? Min : Max) = Scale;
                Scale = (Min + Max) * .5f;
            }
            for (int64 I = 0; I < Next.Num(); ++I) {
                Next[I] *= Best;
                Mip.RawData[I * 4 + Bytes[Channel]] = uint8(FMath::Clamp(FMath::RoundToInt(Next[I] * 255.f), 0, 255));
            }
            Previous = MoveTemp(Next);
        }
    }
}
// Block compression by ispc_texcomp.dll, delay-loaded from the engine's third-party binaries (staged with the runtime).
// Without it, textures stay uncompressed.
bool BlockCompression() {
    static const bool Loaded =
        FPlatformProcess::GetDllHandle(
            *(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Intel/ISPCTexComp/Win64-Release/ispc_texcomp.dll"))) !=
        nullptr;
    return Loaded;
}
// The block format for what a texture's materials read: one or two channels read only by scalar inputs are repacked
// into BC4/BC5 (Texture.Channels names their source channels); colour, normals and three- or four-channel data are BC7.
EPixelFormat BlockFormat(FHdueScene::FPreviewTexture& Texture) {
    Texture.Channels.Reset();
    if (Texture.Vector) return PF_BC7;
    for (uint8 Channel = 0; Channel < 4; ++Channel)
        if (Texture.Read & (1 << Channel)) Texture.Channels.Add(Channel);
    if (Texture.Channels.Num() == 1) return PF_BC4;
    if (Texture.Channels.Num() == 2) return PF_BC5;
    Texture.Channels.Reset();
    return PF_BC7;
}
// One BGRA8 mip level as Format's blocks, a row of blocks per task. Blocks past the image (mips below 4x4, odd sizes)
// repeat its edge texels.
void CompressMip(const FImage& Mip, EPixelFormat Format, const FHdueScene::FPreviewTexture& Texture, uint8* Out) {
    const int32 BlocksX = FMath::DivideAndRoundUp(Mip.SizeX, 4), BlocksY = FMath::DivideAndRoundUp(Mip.SizeY, 4);
    const int32 Texel = Format == PF_BC4 ? 1 : Format == PF_BC5 ? 2 : 4;  // encoder input: R8, RG8 or RGBA8
    static const uint8 RGBA[4] = {0, 1, 2, 3}, Bytes[4] = {2, 1, 0, 3};   // byte of each RGBA channel in an FColor
    const uint8* Source = Texture.Channels.IsEmpty() ? RGBA : Texture.Channels.GetData();
    bc7_enc_settings Settings;
    // ultrafast: ~60 Mpixel/s per core; veryfast was 12x slower (5 s of a Sponza load on 32 cores).
    if (Texture.Read & 8)
        GetProfile_alpha_ultrafast(&Settings);
    else
        GetProfile_ultrafast(&Settings);
    const uint8* In = Mip.RawData.GetData();
    ParallelFor(BlocksY, [&](int32 Row) {
        TArray<uint8> Strip;
        Strip.SetNumUninitialized(BlocksX * 16 * Texel);
        uint8* Packed = Strip.GetData();
        for (int32 Y = 0; Y < 4; ++Y) {
            const uint8* Line = In + int64(FMath::Min(Row * 4 + Y, Mip.SizeY - 1)) * Mip.SizeX * 4;
            for (int32 X = 0; X < BlocksX * 4; ++X, Packed += Texel)
                for (int32 C = 0; C < Texel; ++C) Packed[C] = Line[FMath::Min(X, Mip.SizeX - 1) * 4 + Bytes[Source[C]]];
        }
        rgba_surface Surface{Strip.GetData(), BlocksX * 4, 4, BlocksX * 4 * Texel};
        uint8* Blocks = Out + int64(Row) * BlocksX * GPixelFormats[Format].BlockBytes;
        if (Format == PF_BC4)
            CompressBlocksBC4(&Surface, Blocks);
        else if (Format == PF_BC5)
            CompressBlocksBC5(&Surface, Blocks);
        else
            CompressBlocksBC7(&Surface, Blocks, &Settings);
    });
}
// A texture's mipped (and, for 8-bit images, block-compressed) platform data, on a worker thread. BC needs whole 4x4
// blocks at the top level; other sizes and float images (EXR/HDR) stay uncompressed.
TUniquePtr<FTexturePlatformData> PrepareTexture(FImage&& Image, FHdueScene::FPreviewTexture& Texture, bool SRGB) {
    if (!ERawImageFormat::IsHDR(Image.Format) && Image.Format != ERawImageFormat::BGRA8) {  // grey, 16-bit
        FImage Converted;
        Image.CopyTo(Converted, ERawImageFormat::BGRA8, Image.GammaSpace);
        Image = MoveTemp(Converted);
    }
    TArray<FImage> Chain = MipChain(MoveTemp(Image));
    if (Texture.Coverage > 0.f && Chain[0].Format == ERawImageFormat::BGRA8)  // ponytail: float cutouts keep plain mips
        ScaleAlphaCoverage(Chain, Texture.Read, Texture.Coverage);
    const bool Compress = Chain[0].Format == ERawImageFormat::BGRA8 && Chain[0].SizeX % 4 == 0 &&
                          Chain[0].SizeY % 4 == 0 && BlockCompression();
    auto Data = MakeUnique<FTexturePlatformData>();
    Data->SizeX = Chain[0].SizeX;
    Data->SizeY = Chain[0].SizeY;
    Data->SetNumSlices(1);
    Data->PixelFormat =
        Compress ? BlockFormat(Texture) : FImageCoreUtils::GetPixelFormatForRawImageFormat(Chain[0].Format);
    if (!Compress) Texture.Channels.Reset();
    const FPixelFormatInfo& Info = GPixelFormats[Data->PixelFormat];
    for (const FImage& Level : Chain) {
        auto* Mip = new FTexture2DMipMap(Level.SizeX, Level.SizeY, 1);
        Data->Mips.Add(Mip);
        const int64 Size = int64(FMath::DivideAndRoundUp(Level.SizeX, Info.BlockSizeX)) *
                           FMath::DivideAndRoundUp(Level.SizeY, Info.BlockSizeY) * Info.BlockBytes;
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        auto* Out = static_cast<uint8*>(Mip->BulkData.Realloc(Size));
        if (Compress)
            CompressMip(Level, Data->PixelFormat, Texture, Out);
        else
            FMemory::Memcpy(Out, Level.RawData.GetData(), Size);
        Mip->BulkData.Unlock();
    }
    return Data;
}
// Block-compressed textures cached in %LOCALAPPDATA%/hdUE/TextureCache, keyed by the file, its time and size, and what
// the materials read: reloading a scene skips decoding its images, most of a texture load.
// ponytail: never evicted; delete the folder to reclaim space.
constexpr int32 CacheVersion = 2;
FString CachePath(const FString& File, const FHdueScene::FPreviewTexture& Texture, bool SRGB) {
    static const FString Root = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA")),
                                                TEXT("hdUE"), TEXT("TextureCache"));
    const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
    if (!Stat.bIsValid) return FString();
    const FString Key =
        FString::Printf(TEXT("%s|%lld|%lld|%d|%d|%d|%d"), *FPaths::ConvertRelativePathToFull(File),
                        Stat.ModificationTime.GetTicks(), Stat.FileSize, SRGB, Texture.Read, Texture.Vector,
                        CacheVersion) +
        (Texture.Coverage > 0.f ? FString::Printf(TEXT("|coverage up %g"), Texture.Coverage) : FString());
    return Root / FString::Printf(TEXT("%016llx.bin"),
                                  CityHash64(reinterpret_cast<const char*>(*Key), Key.Len() * sizeof(TCHAR)));
}
// The cached platform data (mip sizes and blocks straight into bulk data), or null.
TUniquePtr<FTexturePlatformData> ReadCache(const FString& Path, FHdueScene::FPreviewTexture& Texture) {
    TUniquePtr<FArchive> In(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!In) return nullptr;
    int32 Version = 0, SizeX = 0, SizeY = 0, Format = 0, Mips = 0;
    *In << Version << SizeX << SizeY << Format << Mips;
    if (In->IsError() || Version != CacheVersion || Format != BlockFormat(Texture)) return nullptr;
    auto Data = MakeUnique<FTexturePlatformData>();
    Data->SizeX = SizeX;
    Data->SizeY = SizeY;
    Data->SetNumSlices(1);
    Data->PixelFormat = EPixelFormat(Format);
    for (int32 Level = 0; Level < Mips && !In->IsError(); ++Level) {
        int32 Width = 0, Height = 0;
        int64 Size = 0;
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
    const FString Temporary =
        Path + FGuid::NewGuid().ToString();  // renamed when complete: readers never see a partial file
    TUniquePtr<FArchive> Out(IFileManager::Get().CreateFileWriter(*Temporary, FILEWRITE_Silent));
    if (!Out) return;
    int32 Version = CacheVersion, Format = Data.PixelFormat, Mips = Data.Mips.Num();
    *Out << Version << Data.SizeX << Data.SizeY << Format << Mips;
    for (FTexture2DMipMap& Mip : Data.Mips) {
        int32 Width = Mip.SizeX, Height = Mip.SizeY;  // 16-bit fields
        int64 Size = Mip.BulkData.GetBulkDataSize();
        *Out << Width << Height << Size;
        Out->Serialize(Mip.BulkData.Lock(LOCK_READ_ONLY), Size);
        Mip.BulkData.Unlock();
    }
    const bool Written = Out->Close() && !Out->IsError();
    Out.Reset();
    if (!Written || !IFileManager::Get().Move(*Path, *Temporary, true, true, false, true))
        IFileManager::Get().Delete(*Temporary, false, false, true);
}
// sourceColorSpace decides sRGB decoding of 8-bit texels; auto leaves it to the slot (colour or data).
bool TextureSRGB(const HdueTextureDesc& Texture, int32 Slot) {
    return Texture.encoding == HDUE_TEXTURE_ENCODING_AUTO ? hdue_texture_is_color(Slot) != 0
                                                          : Texture.encoding == HDUE_TEXTURE_ENCODING_SRGB;
}
// Black is clamp here; the parent material masks texels outside [0,1] (<Slot>Black).
TextureAddress TextureWrap(uint32 Wrap) {
    return Wrap == HDUE_WRAP_REPEAT ? TA_Wrap : Wrap == HDUE_WRAP_MIRROR ? TA_Mirror : TA_Clamp;
}
// The texel value a masked material's opacity texture is clipped at, (clip - bias) / scale of the channel it reads, or
// 0 for any other use. ponytail: inverted maps (scale <= 0) keep plain mips.
float CoverageThreshold(const HdueMaterialDesc& Desc, int32 Slot) {
    if (Slot != HDUE_TEXTURE_OPACITY || Desc.opacity_threshold <= 0.f) return 0;
    const HdueTextureDesc& Texture = Desc.textures[Slot];
    for (int32 Channel = 0; Channel < 4; ++Channel)
        if (Texture.channel_mask[Channel] != 0.f) {
            const float Scale = Texture.scale[Channel],
                        Texel = Scale > 0.f ? (Desc.opacity_threshold - Texture.bias[Channel]) / Scale : 0.f;
            return Texel > 0.f && Texel <= 1.f ? Texel : 0.f;
        }
    return 0;
}
// One texture per file, decoding and wrap (UE keeps the sampler's address mode on the texture), and the clip value its
// mips keep the coverage of: other uses of the file keep plain mips.
FString TextureKey(const FString& File, const HdueTextureDesc& Texture, int32 Slot, float Coverage) {
    return FString::Printf(TEXT("%s|%s|%d|%d|%g"), *File, TextureSRGB(Texture, Slot) ? TEXT("srgb") : TEXT("raw"),
                           int32(TextureWrap(Texture.wrap[0])), int32(TextureWrap(Texture.wrap[1])), Coverage);
}
// Inputs the Preview Surface parents read through <Slot>Mask; the others (colour, normal) read RGB.
bool ScalarSlot(int32 Slot) { return !hdue_texture_is_color(Slot) && Slot != HDUE_TEXTURE_NORMAL; }
}  // namespace
// A texture Apply prepared, created on this thread (UObjects and render resources need it) on first use.
const FHdueScene::FPreviewTexture* FHdueScene::PreviewTexture(const FString& Key, FString& Error) {
    FPreviewTexture* Texture = PreviewTextures.Find(Key);
    if (Texture && Texture->Pending) {
        const double CreateStart = FPlatformTime::Seconds();
        auto* Created = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
        Created->bNotOfflineProcessed = true;
        // float images are already linear
        Created->SRGB = Texture->SRGB && !IsFloatFormat(Texture->Pending->PixelFormat);
        Created->AddressX = TextureAddress(Texture->Address[0]);
        Created->AddressY = TextureAddress(Texture->Address[1]);
        // UpdateResource hands the mips to the render thread. Discardable bulk data moves them there: copying them
        // (FTexturePlatformData::TryLoadMips) was 99% of texture creation, ~680 ms for Sponza's 111 textures, and kept
        // a CPU copy of every texture for the scene's lifetime. ponytail: the resource can't be rebuilt from this
        // texture; it is never updated again (create a new one).
        for (FTexture2DMipMap& Mip : Texture->Pending->Mips) Mip.BulkData.SetBulkDataFlags(BULKDATA_AlwaysAllowDiscard);
        Created->SetPlatformData(Texture->Pending.Release());
        Created->UpdateResource();
        Texture->Texture.Reset(Created);
        TextureSeconds += FPlatformTime::Seconds() - CreateStart;
        ++TextureImports;
    }
    if (!Texture || !Texture->Texture) {
        Error = FString::Printf(TEXT("Failed to import texture %s."), *Key);
        return nullptr;
    }
    return Texture;
}
// A single-sided mesh draws its generated material's twin on the one-sided parent (<Parent>OneSided, same folder),
// with the same parameters. Native and thin-walled materials have none and stay as they are.
UMaterialInterface* FHdueScene::MeshMaterial(const FString& MeshId) {
    const FString& Id = MeshMaterials.FindRef(MeshId);
    const auto* Material = Materials.Find(Id);
    if (!Material) return nullptr;
    if (!SingleSided.Contains(MeshId)) return Material->Get();
    FOneSided& Twin = OneSided.FindOrAdd(Id);
    if (Twin.Base != Material->Get()) {
        Twin.Base = Material->Get();
        if (Twin.Material) RetiredMaterials.Add(MoveTemp(Twin.Material));  // frames in flight may still draw it
        Twin.Material.Reset();
        auto* Instance = Cast<UMaterialInstanceDynamic>(Twin.Base);
        if (Instance && Instance->Parent) {
            const FString Name = Instance->Parent->GetName() + TEXT("OneSided");
            const FString Path = Instance->Parent->GetPackage()->GetName() + TEXT("OneSided.") + Name;
            if (auto* Parent = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet)) {
                auto* OneSidedInstance = UMaterialInstanceDynamic::Create(Parent, World);
                OneSidedInstance->CopyParameterOverrides(Instance);
                Twin.Material.Reset(OneSidedInstance);
            }
        }
    }
    return Twin.Material ? Twin.Material.Get() : Material->Get();
}
void FHdueScene::BindMaterials() {
    for (const auto& Entry : MeshMaterials) {
        AActor** Actor = Actors.Find(Entry.Key);
        if (!Actor || !IsValid(*Actor)) continue;
        auto* Component = (*Actor)->FindComponentByClass<UMeshComponent>();
        if (const auto* Material = Materials.Find(Entry.Value)) {
            UMaterialInterface* Desired = MeshMaterial(Entry.Key);
            if (const auto* Wet = WetBindings.Find(Entry.Key);
                Wet && (*Wet)->Base == Material->Get() && (*Wet)->Revision == GeometryRevisions.FindRef(Entry.Key))
                Desired = (*Wet)->Material.Get();
            if (Component->GetMaterial(0) != Desired) Component->SetMaterial(0, Desired);
        }
    }
}
namespace {
// One Hydra vertex for static and Nanite meshes (any thread). Hydra meshes carry no tangents: the frame around the
// normal is arbitrary until HydraTangents derives it from the UVs. Colours stay linear in 8 bits, as dynamic meshes
// store them.
FVector3f HydraNormal(const HdueMeshDesc& Desc, uint32 V) {
    return Desc.normals ? FVector3f(Desc.normals[V * 3], Desc.normals[V * 3 + 1], Desc.normals[V * 3 + 2])
                        : FVector3f::UpVector;
}
FStaticMeshBuildVertex HydraVertex(const HdueMeshDesc& Desc, uint32 V, FVector3f Normal) {
    if (!Normal.Normalize()) Normal = FVector3f::UpVector;
    FStaticMeshBuildVertex Vertex;
    Vertex.Position = FVector3f(Desc.positions[V * 3], Desc.positions[V * 3 + 1], Desc.positions[V * 3 + 2]);
    Vertex.TangentZ = Normal;
    Vertex.TangentX =
        (FMath::Abs(Normal.X) < .9f ? FVector3f::ForwardVector : FVector3f::RightVector).Cross(Normal).GetSafeNormal();
    Vertex.TangentY = Normal.Cross(Vertex.TangentX);
    for (int32 Set = 0; Set < HdueVertexUVs; ++Set) {
        const float* UV =
            Desc.uv ? Desc.uv + (size_t(FMath::Min(Set, HdueUVSets(Desc) - 1)) * Desc.vertex_count + V) * 2 : nullptr;
        Vertex.UVs[Set] = UV ? FVector2f(UV[0], UV[1]) : FVector2f::ZeroVector;
    }
    Vertex.Color =
        Desc.colors ? FLinearColor(Desc.colors[V * 3], Desc.colors[V * 3 + 1], Desc.colors[V * 3 + 2]).ToFColor(false)
                    : FColor::White;
    return Vertex;
}
// MikkTSpace tangents from UV 0 (any thread), as UE's Nanite builder computes them for indexed vertices: normal maps
// are baked against this frame. A vertex shared by triangles keeps the last triangle's tangent.
struct FTangentMesh {
    TArray<FStaticMeshBuildVertex>& Vertices;
    const TArray<uint32>& Indices;
};
FStaticMeshBuildVertex& CornerOf(const SMikkTSpaceContext* Context, int Face, int Vert) {
    const auto* Mesh = static_cast<FTangentMesh*>(Context->m_pUserData);
    return Mesh->Vertices[Mesh->Indices[Face * 3 + Vert]];
}
void HydraTangents(TArray<FStaticMeshBuildVertex>& Vertices, const TArray<uint32>& Indices) {
    SMikkTSpaceInterface Interface = {};
    Interface.m_getNumFaces = [](const SMikkTSpaceContext* Context) {
        return static_cast<FTangentMesh*>(Context->m_pUserData)->Indices.Num() / 3;
    };
    Interface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
    Interface.m_getPosition = [](const SMikkTSpaceContext* Context, float Out[3], int Face, int Vert) {
        FMemory::Memcpy(Out, &CornerOf(Context, Face, Vert).Position, 12);
    };
    Interface.m_getNormal = [](const SMikkTSpaceContext* Context, float Out[3], int Face, int Vert) {
        FMemory::Memcpy(Out, &CornerOf(Context, Face, Vert).TangentZ, 12);
    };
    Interface.m_getTexCoord = [](const SMikkTSpaceContext* Context, float Out[2], int Face, int Vert) {
        FMemory::Memcpy(Out, &CornerOf(Context, Face, Vert).UVs[0], 8);
    };
    Interface.m_setTSpaceBasic = [](const SMikkTSpaceContext* Context, const float Tangent[3], float Sign, int Face,
                                    int Vert) {
        FStaticMeshBuildVertex& Vertex = CornerOf(Context, Face, Vert);
        Vertex.TangentX = FVector3f(Tangent[0], Tangent[1], Tangent[2]);
        Vertex.TangentY = -Sign * Vertex.TangentZ.Cross(Vertex.TangentX);  // UE's V runs down
    };
    FTangentMesh Mesh{Vertices, Indices};
    SMikkTSpaceContext Context = {&Interface, &Mesh, true};
    genTangSpaceDefault(&Context);
}
// Nanite data for one generated mesh (any thread). Every page is a root page: outside the editor, UE streams non-root
// pages only from cooked files on disk, and these meshes exist only in memory.
TSharedPtr<Nanite::FResources> BuildNanite(const HdueMeshDesc& Desc) {
    if (!Desc.vertex_count || Desc.index_count < 3) return nullptr;
    Nanite::IBuilderModule::FInputMeshData Input;
    const int32 Sets = HdueUVSets(Desc);
    Input.Vertices.Empty(int32(Desc.vertex_count), Sets, 0);
    FBox3f Bounds(ForceInit);
    for (uint32 V = 0; V < Desc.vertex_count; ++V) {
        const FStaticMeshBuildVertex Vertex = HydraVertex(Desc, V, HydraNormal(Desc, V));
        Input.Vertices.Position.Add(Vertex.Position);
        Input.Vertices.TangentZ.Add(Vertex.TangentZ);
        Input.Vertices.TangentX.Add(Vertex.TangentX);
        Input.Vertices.TangentY.Add(Vertex.TangentY);
        for (int32 Set = 0; Set < Sets; ++Set) Input.Vertices.UVs[Set].Add(Vertex.UVs[Set]);
        if (Desc.colors) Input.Vertices.Color.Add(Vertex.Color);
        Bounds += Vertex.Position;
    }
    // The builder's vertex view drops all-white colours but its format still counts them: keep none in that case.
    if (!Input.Vertices.Color.ContainsByPredicate([](const FColor& C) { return C != FColor::White; }))
        Input.Vertices.Color.Empty();
    const uint32 Triangles = Desc.index_count / 3;
    Input.TriangleIndices = TArray<uint32>(Desc.indices, int32(Triangles * 3));
    Input.TriangleCounts.Add(Triangles);
    Input.MaterialIndices.Init(0, int32(Triangles));
    Nanite::FMeshDataSection Section;
    Section.MaterialIndex = 0;
    Section.FirstIndex = 0;
    Section.NumTriangles = Triangles;
    Section.MinVertexIndex = 0;
    Section.MaxVertexIndex = Desc.vertex_count - 1;
    Input.Sections.Add(Section);
    Input.VertexBounds = FBounds3f(Bounds.Min, Bounds.Max);
    Input.NumTexCoords = uint8(Sets);
    FMeshNaniteSettings Settings;
    Settings.bEnabled = true;
    Settings.TargetMinimumResidencyInKB = MAX_uint32;
    // The Nanite build is most of a scene's conversion (450 meshes, 4.4M triangles: 10.5 s of 11.2 s), so builds are
    // kept across runs as UE's derived data cache keeps a static mesh's (FStaticMeshRenderData::Cache, StaticMesh.cpp):
    // keyed by the builder's version and the mesh's content, the resources as Serialize writes them (bCooked false).
    // UE's cache itself is unavailable here: GetCache() creates none where RequiresCookedData() (cooked runtimes,
    // DerivedDataCacheModule.cpp), so this keeps a file per mesh under Saved/NaniteCache.
    FBlake3 Hasher;
    const FString Version = Nanite::IBuilderModule::Get().GetVersionString();
    Hasher.Update(*Version, Version.Len() * sizeof(TCHAR));
    const auto Add = [&](const auto& Array) { Hasher.Update(Array.GetData(), Array.Num() * Array.GetTypeSize()); };
    Add(Input.Vertices.Position);
    Add(Input.Vertices.TangentX);
    Add(Input.Vertices.TangentY);
    Add(Input.Vertices.TangentZ);
    for (const auto& UVs : Input.Vertices.UVs) Add(UVs);
    Add(Input.Vertices.Color);
    Add(Input.TriangleIndices);
    const FString Path =
        FPaths::ProjectSavedDir() / TEXT("NaniteCache") / LexToString(Hasher.Finalize()) + TEXT(".nanite");
    auto Resources = MakeShared<Nanite::FResources>();
    TArray<uint8> Bytes;
    if (FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent)) {
        FMemoryReader Reader(Bytes);  // not persistent: see the writer
        Resources->Serialize(Reader, nullptr, false);
        if (!Reader.IsError()) {
            IFileManager::Get().SetTimeStamp(*Path, FDateTime::UtcNow());  // a hit counts as a use (eviction)
            return Resources;
        }
        *Resources = Nanite::FResources();
    }
    if (!Nanite::IBuilderModule::Get().Build(*Resources, Input, nullptr, nullptr, nullptr, Settings)) return nullptr;
    // Every page is a root page, so StreamablePages (bulk data) is empty. Runtime builds cannot save bulk data
    // (USE_RUNTIME_BULKDATA, BulkData.cpp) but still read its header; a non-persistent archive skips it both ways.
    if (Resources->StreamablePages.GetBulkDataSize()) return Resources;
    Bytes.Reset();
    FMemoryWriter Writer(Bytes);
    Resources->Serialize(Writer, nullptr, false);
    // Another task may build the same mesh: write a private file, then move it into place.
    const FString Temporary = FPaths::CreateTempFilename(*FPaths::GetPath(Path), TEXT("nanite"));
    if (FFileHelper::SaveArrayToFile(Bytes, *Temporary) && !IFileManager::Get().Move(*Path, *Temporary, true))
        IFileManager::Get().Delete(*Temporary);
    return Resources;
}
UStaticMesh* NewStaticMesh(UObject* Outer) {
    auto* Mesh = NewObject<UStaticMesh>(Outer);
    FStaticMaterial Slot;
    Slot.UVChannelData = FMeshUVChannelInfo(1.f);  // texture streaming reads UV density
    Mesh->GetStaticMaterials().Add(Slot);
    return Mesh;
}
// Game thread: Nanite data is attached before the GPU resources exist.
void FinishStaticMesh(UStaticMesh* Built, TUniquePtr<FStaticMeshRenderData> RenderData, const FBoxSphereBounds& Bounds,
                      Nanite::FResources* Nanite) {
    Built->SetRenderData(MoveTemp(RenderData));
    FStaticMeshRenderData& Data = *Built->GetRenderData();
    if (Nanite) {
        // Nanite's ray-tracing fallback only counts LODs with a BuffersSize, which only cooking sets.
        Data.LODResources[0].BuffersSize = FMath::Max<uint32>(Data.LODResources[0].BuffersSize, 1);
        *Data.NaniteResourcesPtr = MoveTemp(*Nanite);
    }
    if (Built->bSupportRayTracing && IsRayTracingAllowed()) Data.InitializeRayTracingRepresentationFromRenderingLODs();
    Built->InitResources();
#if RHI_RAYTRACING
    // A Nanite proxy leaves its ray-tracing fallback out while that BLAS waits for its build, which UE starts only
    // when the next frame renders: Lumen's first frame then traced an empty scene, and its radiance caches kept that
    // sky light through the warmup (a roofed room under a sky was 31% too bright after 4 frames, Sponza 4.5 times
    // after 16). Build it before the proxy exists.
    if (Nanite && Data.RayTracingProxy) {
        ENQUEUE_RENDER_COMMAND(HdueBuildRayTracing)(
            [Proxy = Data.RayTracingProxy](FRHICommandListImmediate& RHICmdList) {
                TArray<const FRayTracingGeometry*> Geometries;
                for (const FStaticMeshRayTracingProxyLOD& LOD : Proxy->LODs) Geometries.Add(LOD.RayTracingGeometry);
                GRayTracingGeometryManager->ForceBuildIfPending(RHICmdList, Geometries);
            });
    }
#endif
    Data.Bounds = Bounds;
    Built->CalculateExtendedBounds();
    Data.ScreenSize[0].Default = 1.f;
}
}  // namespace
// Any thread: one Hydra mesh's static render data, with a vertex per Hydra vertex (each has one normal, UV and colour).
// Meshes without normals get a vertex per triangle corner with the triangle's normal, as dynamic meshes draw them.
// UE's BuildFromMeshDescription went through an FMeshDescription with a vertex per corner, whose edge bookkeeping took
// 7 s for Sponza's largest mesh. Without Lumen cards (MeshCards.cpp), Lumen's surface cache has nothing for the mesh
// and its bounces go black.
FCardRepresentationData* HdueMeshCards(const FStaticMeshLODResources& LOD, const FBoxSphereBounds& Bounds);
FHdueScene::FConversion FHdueScene::BuildStatic(const HdueMeshDesc& Desc, bool Cards) {
    const bool Faceted = !Desc.normals;
    const int32 Count = int32(Faceted ? Desc.index_count : Desc.vertex_count);
    TArray<FStaticMeshBuildVertex> Vertices;
    Vertices.SetNumUninitialized(Count);
    TArray<uint32> Indices(Desc.indices, int32(Desc.index_count));
    const auto Position = [&](uint32 V) {
        return FVector3f(Desc.positions[V * 3], Desc.positions[V * 3 + 1], Desc.positions[V * 3 + 2]);
    };
    FBox3f Bounds(ForceInit);
    bool Colored = false;
    for (int32 I = 0; I < Count; ++I) {
        uint32 V = uint32(I);
        FVector3f Normal;
        if (Faceted) {
            const uint32* Triangle = Desc.indices + I / 3 * 3;
            Normal =
                (Position(Triangle[1]) - Position(Triangle[0])).Cross(Position(Triangle[2]) - Position(Triangle[0]));
            V = Indices[I];
            Indices[I] = uint32(I);
        } else
            Normal = HydraNormal(Desc, V);
        Vertices[I] = HydraVertex(Desc, V, Normal);
        Bounds += Vertices[I].Position;
        Colored |= Vertices[I].Color != FColor::White;
    }
    // UV tangents (~4 us per triangle) come with the cards: interactive hosts show the first build sooner and get them
    // with the background rebuild.
    if (Cards && Desc.uv) HydraTangents(Vertices, Indices);
    FConversion Result{MakeUnique<FStaticMeshRenderData>(), FBoxSphereBounds(FBox(Bounds)), nullptr};
    Result.RenderData->AllocateLODResources(1);
    if (Indices.IsEmpty()) return Result;
    FStaticMeshLODResources& LOD = Result.RenderData->LODResources[0];
    LOD.VertexBuffers.PositionVertexBuffer.Init(Vertices);
    FStaticMeshVertexBufferFlags Flags;
    Flags.bNeedsCPUAccess = true;
    Flags.bUseBackwardsCompatibleF16TruncUVs = false;
    LOD.VertexBuffers.StaticMeshVertexBuffer.Init(Vertices, HdueVertexUVs, Flags);
    if (Colored)
        LOD.VertexBuffers.ColorVertexBuffer.Init(Vertices);
    else
        LOD.VertexBuffers.ColorVertexBuffer.InitFromSingleColor(FColor::White, Count);
    LOD.bHasColorVertexData = true;
    FStaticMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
    Section.MaterialIndex = 0;
    Section.FirstIndex = 0;
    Section.NumTriangles = Indices.Num() / 3;
    Section.MinVertexIndex = 0;
    Section.MaxVertexIndex = Count - 1;
    Section.bCastShadow = true;
    LOD.IndexBuffer.SetIndices(
        Indices, Count - 1 > MAX_uint16 ? EIndexBufferStride::Force32Bit : EIndexBufferStride::Force16Bit);
    if (Cards) LOD.CardRepresentationData = HdueMeshCards(LOD, Result.Bounds);
    return Result;
}

// Replaces one static mesh queued by Apply with its version with Lumen cards (and Nanite data) once that build has
// finished. Both must be attached before a mesh's GPU resources exist, so that version is a second static mesh.
// UE's Editor.AsyncAssetCompilationMaxMemoryUsage for hdUE's background mesh builds (UE's applies to the editor only).
static TAutoConsoleVariable<int32> CVarMeshBuildMaxMemoryUsage(
    TEXT("r.HdUE.MeshBuildMaxMemoryUsage"), 0,
    TEXT("Interactive hosts: 0 - background mesh builds start while system memory is available; N - keep their "
         "estimated "
         "memory under N GB."));

void FHdueScene::TickConversions() {
    // Builds start while their memory fits, as UE's editor schedules asset builds (FMemoryBoundQueuedThreadPoolWrapper,
    // AssetCompilingManager.cpp, editor-only): an estimate of 7 times the source mesh (UStaticMesh::
    // GetBuildRequiredMemoryEstimate, StaticMesh.cpp) against the available physical and virtual memory less 64 MB,
    // with builds already started (or finished and waiting to be swapped in) holding theirs.
    if (PendingStatic.Num()) {
        const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
        int64 Available = int64(FMath::Min(Stats.AvailablePhysical, Stats.AvailableVirtual)) - 64 * 1024 * 1024;
        Available -= ReservedMemory;
        if (const int64 Limit = int64(CVarMeshBuildMaxMemoryUsage.GetValueOnGameThread()) << 30)
            Available = FMath::Min(Available, Limit - ReservedMemory);
        for (auto& [Id, Pending] : PendingStatic) {
            if (Pending.Task.IsValid()) continue;
            if (ReservedMemory && Pending.Memory > Available) break;  // one build always runs
            Available -= Pending.Memory;
            ReservedMemory += Pending.Memory;
            Pending.Task = UE::Tasks::Launch(
                TEXT("HdueConversion"),
                [Source = MoveTemp(Pending.Source)]() -> TSharedPtr<FConversion> {
                    auto Result = MakeShared<FConversion>(BuildStatic(Source->Desc, true));
                    if (Source->Nanite) Result->Nanite = BuildNanite(Source->Desc);
                    return Result;
                },
                UE::Tasks::ETaskPriority::BackgroundLow);
        }
    }
    for (auto It = PendingStatic.CreateIterator(); It; ++It) {
        if (!It.Value().Task.IsValid() || !It.Value().Task.IsCompleted()) continue;  // queued or not built yet
        ReservedMemory -= It.Value().Memory;
        AActor* Actor = Actors.FindRef(It.Key());
        auto* Component = IsValid(Actor) ? Cast<UStaticMeshComponent>(Actor->GetRootComponent()) : nullptr;
        const TSharedPtr<FConversion> Conversion = It.Value().Task.GetResult();
        if (!Component) {
            It.RemoveCurrent();
            continue;
        }
        const UMaterialInterface* Material = Component->GetMaterial(0);
        // Nanite draws opaque and masked materials only.
        const bool Nanite = Conversion->Nanite && !(Material && IsTranslucentBlendMode(*Material));
        UStaticMesh* Built = NewStaticMesh(Actor);
        FinishStaticMesh(Built, MoveTemp(Conversion->RenderData), Conversion->Bounds,
                         Nanite ? Conversion->Nanite.Get() : nullptr);
        static int32 NaniteMeshes = 0, Tried = 0;
        ++Tried;
        if (Built->HasValidNaniteData()) ++NaniteMeshes;
        if (Tried % 50 == 1 || PendingStatic.Num() == 1)
            UE_LOG(LogTemp, Display, TEXT("hdUE Nanite: %d of %d converted meshes have Nanite data"), NaniteMeshes,
                   Tried);
        Component->SetStaticMesh(Built);
        It.RemoveCurrent();
        return;  // one per tick keeps frames smooth
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
    const int32 Sets = HdueUVSets(Desc);
    Mesh.Attributes()->SetNumUVLayers(HdueVertexUVs);
    Mesh.Attributes()->EnablePrimaryColors();
    auto* Colors = Mesh.Attributes()->PrimaryColors();
    auto* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
    const auto Append = [&](uint32 V) {
        NormalOverlay->AppendElement(
            Desc.normals ? FVector3f(Desc.normals[V * 3], Desc.normals[V * 3 + 1], Desc.normals[V * 3 + 2])
                         : FVector3f::ZeroVector);
        for (int32 Set = 0; Set < HdueVertexUVs; ++Set) {
            const float* UV =
                Desc.uv ? Desc.uv + (size_t(FMath::Min(Set, Sets - 1)) * Desc.vertex_count + V) * 2 : nullptr;
            Mesh.Attributes()->GetUVLayer(Set)->AppendElement(UV ? FVector2f(UV[0], UV[1]) : FVector2f::ZeroVector);
        }
        Colors->AppendElement(Desc.colors
                                  ? FVector4f(Desc.colors[V * 3], Desc.colors[V * 3 + 1], Desc.colors[V * 3 + 2], 1.f)
                                  : FVector4f(1.f, 1.f, 1.f, 1.f));
        return Mesh.AppendVertex(
            FVector3d(Desc.positions[V * 3], Desc.positions[V * 3 + 1], Desc.positions[V * 3 + 2]));
    };
    for (uint32 V = 0; V < Desc.vertex_count; ++V) Append(V);
    TArray<int32> Sources;  // per vertex, once a copy exists
    for (uint32 I = 0; I < Desc.index_count; I += 3) {
        FIndex3i Vertices(Desc.indices[I], Desc.indices[I + 1], Desc.indices[I + 2]);
        int32 Triangle = Mesh.AppendTriangle(Vertices);
        if (Triangle == FDynamicMesh3::NonManifoldID || Triangle == FDynamicMesh3::DuplicateTriangleID) {
            if (Sources.IsEmpty())
                for (uint32 V = 0; V < Desc.vertex_count; ++V) Sources.Add(int32(V));
            for (int C = 0; C < 3; ++C) {
                Sources.Add(Vertices[C]);
                Vertices[C] = Append(Desc.indices[I + C]);
            }
            Triangle = Mesh.AppendTriangle(Vertices);
        }
        if (Triangle < 0) continue;  // degenerate: a repeated vertex
        NormalOverlay->SetTriangle(Triangle, Vertices);
        for (int32 Set = 0; Set < HdueVertexUVs; ++Set)
            Mesh.Attributes()->GetUVLayer(Set)->SetTriangle(Triangle, Vertices);
        Colors->SetTriangle(Triangle, Vertices);
    }
    if (!Desc.normals) FMeshNormals::InitializeOverlayToPerTriangleNormals(NormalOverlay);
    if (!Sources.IsEmpty()) FNonManifoldMappingSupport::AttachNonManifoldVertexMappingData(Sources, Mesh);
    return Mesh;
}
}  // namespace
bool FHdueScene::Apply(const HdueSceneBatch& Batch, FString& Error) {
    if ((Batch.material_count && !Batch.materials) || (Batch.mesh_count && !Batch.meshes) ||
        (Batch.light_count && !Batch.lights) || (Batch.removed_count && !Batch.removed_ids)) {
        Error = TEXT("Missing scene batch data.");
        return false;
    }
    // Validate mesh array bounds before mutating the scene. An invalid mesh is skipped (and its previous version
    // removed) with a warning naming it; the rest of the batch still applies. Arrays are int32-addressed in UE.
    TArray<bool> ValidMeshes;
    ValidMeshes.Init(true, int32(Batch.mesh_count));
    ParallelFor(int32(Batch.mesh_count), [&](int32 Index) {
        const auto& Mesh = Batch.meshes[Index];
        const TCHAR* Problem = !Mesh.id               ? TEXT("no ID")
                               : Mesh.index_count % 3 ? TEXT("index count is not a multiple of 3")
                               : Mesh.vertex_count > uint32(MAX_int32 / 3) || Mesh.index_count > uint32(MAX_int32) ||
                                       (Mesh.instance_transforms && Mesh.instance_count > uint32(MAX_int32 / 16)) ||
                                       uint64(Mesh.vertex_count) * 2 * HdueUVSets(Mesh) > uint64(MAX_int32)
                                   ? TEXT("too large for 32-bit arrays")
                               : (Mesh.vertex_count && !Mesh.positions) || (Mesh.index_count && !Mesh.indices)
                                   ? TEXT("missing positions or indices")
                                   : nullptr;
        for (uint32 I = 0; !Problem && I < Mesh.index_count; ++I)
            if (Mesh.indices[I] >= Mesh.vertex_count) Problem = TEXT("index out of range");
        if (!Problem) return;
        ValidMeshes[Index] = false;
        UE_LOG(LogTemp, Warning, TEXT("hdUE: skipped mesh %s (%u vertices, %u indices): %s."),
               Mesh.id ? UTF8_TO_TCHAR(Mesh.id) : TEXT("without ID"), Mesh.vertex_count, Mesh.index_count, Problem);
    });
    if (IdMode) {
        FString Ignored;
        SetIdMode(0, Ignored);
    }
    if (Batch.reset_scene) {
        Lighting->Clear();
        Assets.Reset();
        for (const auto& Entry : Actors)
            if (IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
        // Preserve the native host's bootstrap map until an explicit reset.
        // Never sweep another streamed Level's actors during a scene update.
        TArray<AActor*> Fixtures;
        for (TActorIterator<AActor> It(World); It; ++It)
            if (It->GetLevel() == World->PersistentLevel &&
                (It->FindComponentByClass<UMeshComponent>() || It->FindComponentByClass<ULightComponent>()))
                Fixtures.Add(*It);
        for (auto* A : Fixtures) World->DestroyActor(A);
        Actors.Empty();
        CameraLightDirections.Empty();
        MeshMaterials.Empty();
        SingleSided.Empty();
        OneSided.Empty();
        Materials.Empty();
        PreviewTextures.Empty();
        PrimIds.Empty();
        for (const auto& Entry : Volumes)
            if (IsValid(Entry.Value.Actor)) World->DestroyActor(Entry.Value.Actor);
        Volumes.Empty();
        WetBindings.Empty();
        GeometryRevisions.Empty();
    }
    for (uint32 Index = 0; Index < Batch.removed_count; ++Index) {
        if (Batch.removed_ids[Index]) Remove(UTF8_TO_TCHAR(Batch.removed_ids[Index]));
    }
    const double ApplyStart = FPlatformTime::Seconds();
    RetiredMaterials.Reset();  // replaced by the previous Apply; frames that used them have long finished
    TextureSeconds = 0;
    TextureImports = 0;
    double PrepareSeconds = 0;
    {
        // Decode, mip and block-compress this batch's new textures on all cores: decoding them one by one on this
        // thread took most of a large scene's load (111 Sponza textures, ~19 s), and uncompressed they held ~9 GB.
        // PreviewTexture creates the UTexture2Ds on this thread. A texture is rebuilt when inputs read channels its
        // BC4/BC5 repacking left out.
        TMap<FString, TArray<FString>> Files;  // each file's variants to build (sRGB decoding, wrap), by key
        for (uint32 Index = 0; Index < Batch.material_count; ++Index) {
            const auto& Desc = Batch.materials[Index];
            if (Desc.native_asset && *Desc.native_asset) continue;
            for (int32 Slot = 0; Slot < HDUE_TEXTURE_SLOT_COUNT; ++Slot) {
                const auto& Texture = Desc.textures[Slot];
                if (!Texture.file || !*Texture.file) continue;
                const FString File = UTF8_TO_TCHAR(Texture.file),
                              Key = TextureKey(File, Texture, Slot, CoverageThreshold(Desc, Slot));
                const bool Vector = !ScalarSlot(Slot);
                uint8 Read = 0;
                for (int32 Channel = 0; Channel < 4; ++Channel)
                    if (!Vector && Texture.channel_mask[Channel] != 0.f) Read |= 1 << Channel;
                FPreviewTexture& Entry = PreviewTextures.FindOrAdd(Key);
                if (Entry.Texture && (Entry.Read | Read) == Entry.Read && (Entry.Vector || !Vector)) continue;
                Entry.Read |= Read;
                Entry.Vector |= Vector;
                Entry.SRGB = TextureSRGB(Texture, Slot);
                Entry.Coverage = CoverageThreshold(Desc, Slot);
                for (int32 Axis = 0; Axis < 2; ++Axis) Entry.Address[Axis] = uint8(TextureWrap(Texture.wrap[Axis]));
                Files.FindOrAdd(File).AddUnique(Key);
            }
        }
        if (Files.Num()) {
            const double PrepareStart = FPlatformTime::Seconds();
            const TArray<TPair<FString, TArray<FString>>> Work = Files.Array();
            ParallelFor(Work.Num(), [&](int32 Index) {
                FImage Image;
                const TArray<FString>& Keys = Work[Index].Value;
                for (int32 Variant = 0; Variant < Keys.Num(); ++Variant) {
                    FPreviewTexture& Entry = PreviewTextures[Keys[Variant]];
                    const bool SRGB = Entry.SRGB;
                    const FString Cache = CachePath(Work[Index].Key, Entry, SRGB);
                    if (!Cache.IsEmpty() && (Entry.Pending = ReadCache(Cache, Entry))) continue;
                    if (!Image.RawData.Num() && !FImageUtils::LoadImage(*Work[Index].Key, Image)) return;
                    const bool Last = Variant + 1 == Keys.Num();
                    Entry.Pending = PrepareTexture(Last ? MoveTemp(Image) : FImage(Image), Entry, SRGB);
                    if (!Cache.IsEmpty() && IsBlockCompressedFormat(Entry.Pending->PixelFormat))
                        WriteCache(Cache, *Entry.Pending);
                }
            });
            PrepareSeconds = FPlatformTime::Seconds() - PrepareStart;
        }
    }
    for (uint32 Index = 0; Index < Batch.material_count; ++Index) {
        const auto& Desc = Batch.materials[Index];
        if (!Desc.id) {
            Error = TEXT("Material has no ID.");
            return false;
        }
        UMaterialInterface* Material = nullptr;
        if (Desc.native_asset && *Desc.native_asset) {
            Material = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (Material) {
                const auto* Entry = Materials.Find(UTF8_TO_TCHAR(Desc.id));
                auto* Existing = Entry ? Cast<UMaterialInstanceDynamic>(Entry->Get()) : nullptr;
                if (Existing && Existing->Parent == Material) {
                    Existing->ClearParameterValues();
                    Material = Existing;
                } else
                    Material = UMaterialInstanceDynamic::Create(Material, World);
            }
        } else {
            if (!FMath::IsFinite(Desc.base_color[3])) {
                Error = TEXT("Material opacity must be finite.");
                return false;
            }
            const float Opacity = FMath::Clamp(Desc.base_color[3], 0.f, 1.f);
            const auto HasTexture = [&](int32 Slot) { return Desc.textures[Slot].file && *Desc.textures[Slot].file; };
            // UsdPreviewSurface: opacityThreshold > 0 is cutout; otherwise partial opacity is see-through.
            const bool Masked = Desc.opacity_threshold > 0.f;
            const bool Translucent = !Masked && (Opacity < 1.f || HasTexture(HDUE_TEXTURE_OPACITY));
            // MaterialX subsurface and sheen, and clear coats, need the layered Standard Surface parents (UE's
            // Substrate standard_surface: a coat slab vertically layered over the base, several slabs per pixel);
            // everything else stays on the single-slab Preview Surface parents. The specular workflow and see-through
            // materials keep the base slab's second specular lobe (haziness) as their coat.
            const bool Coat = (Desc.clearcoat > 0.f || HasTexture(HDUE_TEXTURE_CLEARCOAT)) &&
                              !(Desc.flags & HDUE_MATERIAL_SPECULAR_WORKFLOW);
            const bool Layered = !Translucent && (Desc.subsurface > 0.f || Desc.sheen > 0.f || Coat);
            const bool Thin = (Desc.flags & HDUE_MATERIAL_THIN_WALLED) != 0;
            const FString ParentName =
                Layered       ? FString::Printf(TEXT("M_StandardSurface%s%s"), Masked ? TEXT("Masked") : TEXT(""),
                                          Thin ? TEXT("Thin") : TEXT(""))
                      : Masked ? TEXT("M_PreviewSurfaceMasked")
                : Translucent ? TEXT("M_PreviewSurfaceTranslucent")
                              : TEXT("M_PreviewSurface");
            auto* Parent = LoadObject<UMaterialInterface>(
                nullptr, *FString::Printf(TEXT("/Game/HdUE/%s.%s"), *ParentName, *ParentName));
            if (Parent) {
                auto* Instance = UMaterialInstanceDynamic::Create(Parent, World);
                static const TCHAR* Slots[HDUE_TEXTURE_SLOT_COUNT] = {
                    TEXT("BaseColor"),          TEXT("Emissive"),     TEXT("Metallic"),
                    TEXT("Roughness"),          TEXT("Opacity"),      TEXT("Normal"),
                    TEXT("Displacement"),       TEXT("Occlusion"),    TEXT("Clearcoat"),
                    TEXT("ClearcoatRoughness"), TEXT("SpecularColor")};
                const FLinearColor Constants[HDUE_TEXTURE_SLOT_COUNT] = {
                    FLinearColor(Desc.base_color[0], Desc.base_color[1], Desc.base_color[2], 1),
                    FLinearColor(Desc.emissive_color[0], Desc.emissive_color[1], Desc.emissive_color[2], 0),
                    FLinearColor(FVector4f(FMath::Clamp(Desc.metallic, 0.f, 1.f))),
                    FLinearColor(FVector4f(FMath::Clamp(Desc.roughness, 0.f, 1.f))),
                    FLinearColor(FVector4f(Opacity)),
                    FLinearColor(0, 0, 1, 0),
                    FLinearColor(FVector4f(Desc.displacement)),
                    FLinearColor(FVector4f(Desc.occlusion)),
                    FLinearColor(FVector4f(Desc.clearcoat)),
                    FLinearColor(FVector4f(Desc.clearcoat_roughness)),
                    FLinearColor(Desc.specular_color[0], Desc.specular_color[1], Desc.specular_color[2], 1)};
                for (int32 Slot = 0; Slot < HDUE_TEXTURE_SLOT_COUNT; ++Slot) {
                    const auto& Texture = Desc.textures[Slot];
                    const FString Name = Slots[Slot];
                    // A texture that cannot be read leaves its input untextured, with a warning, instead of failing
                    // every material of the batch. ponytail: the input's constant, not UsdUVTexture's fallback; carry
                    // it if needed.
                    const FPreviewTexture* Image = nullptr;
                    if (HasTexture(Slot)) {
                        FString TextureError;
                        Image = PreviewTexture(
                            TextureKey(UTF8_TO_TCHAR(Texture.file), Texture, Slot, CoverageThreshold(Desc, Slot)),
                            TextureError);
                        if (!Image)
                            UE_LOG(LogTemp, Warning, TEXT("hdUE material %s: %s; %s stays untextured."),
                                   UTF8_TO_TCHAR(Desc.id), *TextureError, *Name);
                    }
                    if (!Image) {
                        Instance->SetVectorParameterValue(*(Name + TEXT("Scale")), FLinearColor(0, 0, 0, 0));
                        Instance->SetVectorParameterValue(*(Name + TEXT("Bias")), Constants[Slot]);
                        continue;
                    }
                    // A repacked texture (BC4/BC5) stores the channels this input reads first.
                    const auto Stored = [&](const float* Value) {
                        if (Image->Channels.IsEmpty()) return FLinearColor(Value[0], Value[1], Value[2], Value[3]);
                        float Packed[4] = {};
                        for (int32 Channel = 0; Channel < Image->Channels.Num(); ++Channel)
                            Packed[Channel] = Value[Image->Channels[Channel]];
                        return FLinearColor(Packed[0], Packed[1], Packed[2], Packed[3]);
                    };
                    Instance->SetTextureParameterValue(*(Name + TEXT("Texture")), Image->Texture.Get());
                    Instance->SetVectorParameterValue(*(Name + TEXT("Scale")), Stored(Texture.scale));
                    Instance->SetVectorParameterValue(*(Name + TEXT("Bias")), Stored(Texture.bias));
                    Instance->SetVectorParameterValue(*(Name + TEXT("Mask")), Stored(Texture.channel_mask));
                    FLinearColor Set(0, 0, 0, 0);
                    Set.Component(FMath::Min<uint32>(Texture.uv_set, 3)) = 1;
                    Instance->SetVectorParameterValue(*(Name + TEXT("UVSet")), Set);
                    if (Texture.wrap[0] == HDUE_WRAP_BLACK || Texture.wrap[1] == HDUE_WRAP_BLACK)
                        Instance->SetVectorParameterValue(
                            *(Name + TEXT("Black")),
                            FLinearColor(Texture.wrap[0] == HDUE_WRAP_BLACK ? 1.f : 0.f,
                                         Texture.wrap[1] == HDUE_WRAP_BLACK ? 1.f : 0.f, 0, 0));
                }
                static const float Identity[6] = {1, 0, 0, 0, 1, 0};
                const bool Unset = Algo::AllOf(Desc.uv_transform, [](float Value) { return Value == 0.f; });
                const float* M = Unset ? Identity : Desc.uv_transform;
                Instance->SetVectorParameterValue(TEXT("UVRow0"), FLinearColor(M[0], M[1], M[2], 0));
                Instance->SetVectorParameterValue(TEXT("UVRow1"), FLinearColor(M[3], M[4], M[5], 0));
                if (Masked) Instance->SetScalarParameterValue(TEXT("OpacityThreshold"), Desc.opacity_threshold);
                // ior -> dielectric F0 = ((ior - 1) / (ior + 1))^2; UE's Specular 0.5 is F0 0.04, so Specular = F0 /
                // 0.08, left above 1 for ior > 1.8 (diamond): ComputeF0 does not saturate it.
                const float Ior = Desc.ior > 0.f ? Desc.ior : 1.5f;
                const float F0 = FMath::Square((Ior - 1.f) / (Ior + 1.f));
                Instance->SetScalarParameterValue(TEXT("SpecularLevel"), F0 / 0.08f);
                Instance->SetScalarParameterValue(TEXT("UseSpecularWorkflow"),
                                                  (Desc.flags & HDUE_MATERIAL_SPECULAR_WORKFLOW) ? 1.f : 0.f);
                if (Layered) {
                    const auto Color = [](const float* C) { return FLinearColor(C[0], C[1], C[2], 1); };
                    Instance->SetScalarParameterValue(TEXT("SpecularIOR"), Ior);
                    Instance->SetVectorParameterValue(TEXT("CoatColor"), Color(Desc.coat_color));
                    Instance->SetScalarParameterValue(TEXT("Subsurface"), Desc.subsurface);
                    Instance->SetVectorParameterValue(TEXT("SubsurfaceColor"), Color(Desc.subsurface_color));
                    Instance->SetVectorParameterValue(TEXT("SubsurfaceRadius"), Color(Desc.subsurface_radius));
                    Instance->SetScalarParameterValue(TEXT("SubsurfaceBaseTexture"),
                                                      (Desc.flags & HDUE_MATERIAL_SUBSURFACE_BASE_TEXTURE) ? 1.f : 0.f);
                    Instance->SetScalarParameterValue(TEXT("Sheen"), Desc.sheen);
                    Instance->SetVectorParameterValue(TEXT("SheenColor"), Color(Desc.sheen_color));
                    Instance->SetScalarParameterValue(TEXT("SheenRoughness"), Desc.sheen_roughness);
                    Instance->SetScalarParameterValue(TEXT("SheenBaseTexture"),
                                                      (Desc.flags & HDUE_MATERIAL_SHEEN_BASE_TEXTURE) ? 1.f : 0.f);
                }
                Instance->SetScalarParameterValue(TEXT("UseVertexColor"),
                                                  (Desc.flags & HDUE_MATERIAL_VERTEX_COLOR) ? 1.f : 0.f);
                Material = Instance;
            }
        }
        if (!Material) {
            Error = TEXT("Material asset missing. Prepare/cook content or check the native asset path.");
            return false;
        }
        // A replaced material can still be referenced by frames in flight on the render thread (ray-tracing instance
        // gathering crashed on a material edit); keep it until the next Apply.
        if (auto* Previous = Materials.Find(UTF8_TO_TCHAR(Desc.id)); Previous && Previous->Get() != Material)
            RetiredMaterials.Add(MoveTemp(*Previous));
        Materials.Add(UTF8_TO_TCHAR(Desc.id), TStrongObjectPtr<UMaterialInterface>(Material));
    }
    const double MaterialsDone = FPlatformTime::Seconds();
    // Assemble the generated meshes on all cores; components and assets are created below on this thread. Meshes that
    // are not edited after upload become static meshes: UDynamicMeshComponent rebuilt its draw commands and dynamic
    // ray-tracing instances every frame, and building its scene proxies held the first frame of a large scene on the
    // game thread (3.8 s for Sponza's 454 meshes), and UE's dynamic meshes get Lumen cards from their bounds only,
    // which left Lumen's bounces inside concave meshes black. Wet surface effects edit their meshes and keep them
    // dynamic. Card and Nanite timing follow Interactive (see Scene.h).
    const auto StaticGenerated = [&](const HdueMeshDesc& Desc) {
        return !(Desc.native_asset && *Desc.native_asset) &&
               (Desc.instance_transforms || !(Desc.flags & HDUE_MESH_EDITABLE));
    };
    // Nanite for static meshes and instancer prototypes, built with their cards: without it, every instance of a
    // scattered prototype drew all its triangles (Jungle Ruins' 8.7 million instances hung the GPU).
    // HDUE_NANITE=0 keeps regular static meshes.
    static const bool NaniteMeshes = FPlatformMisc::GetEnvironmentVariable(TEXT("HDUE_NANITE")) != TEXT("0");
    // Load the builder on this thread before worker tasks use it, and fill its version string, which BuildNanite's
    // cache key reads on workers: GetVersionString builds a function-local static string without a lock
    // (NaniteBuilder.cpp), and two workers filling it at once corrupted the heap.
    if (NaniteMeshes) Nanite::IBuilderModule::Get().GetVersionString();
    static const bool Evicted = [] {
        // Cached Nanite builds (BuildNanite) unused for a week go, as the OCIO texture cache's do.
        const FString Folder = FPaths::ProjectSavedDir() / TEXT("NaniteCache");
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *(Folder / TEXT("*.nanite")), true, false);
        for (const FString& File : Files)
            if (FDateTime::UtcNow() - IFileManager::Get().GetTimeStamp(*(Folder / File)) > FTimespan::FromDays(7))
                IFileManager::Get().Delete(*(Folder / File));
        return true;
    }();
    TArray<UE::Geometry::FDynamicMesh3> BuiltMeshes;
    BuiltMeshes.SetNum(Batch.mesh_count);
    TArray<FConversion> StaticData;
    StaticData.SetNum(Batch.mesh_count);
    // Largest meshes first, one per task: the last large mesh started bounds the batch.
    TArray<int32> Order;
    Order.SetNumUninitialized(Batch.mesh_count);
    for (int32 Index = 0; Index < Order.Num(); ++Index) Order[Index] = Index;
    Order.Sort([&](int32 A, int32 B) { return Batch.meshes[A].index_count > Batch.meshes[B].index_count; });
    ParallelFor(
        Order.Num(),
        [&](int32 Task) {
            const int32 Index = Order[Task];
            const auto& Desc = Batch.meshes[Index];
            if (!ValidMeshes[Index] || (Desc.native_asset && *Desc.native_asset)) return;
            if (StaticGenerated(Desc)) {
                StaticData[Index] = BuildStatic(Desc, !Interactive);
                if (NaniteMeshes && !Interactive) StaticData[Index].Nanite = BuildNanite(Desc);
            } else
                BuiltMeshes[Index] = BuildDynamicMesh(Desc);
        },
        EParallelForFlags::Unbalanced);
    for (uint32 Index = 0; Index < Batch.mesh_count; ++Index) {
        const auto& Desc = Batch.meshes[Index];
        if (!ValidMeshes[int32(Index)]) {
            if (Desc.id) Remove(UTF8_TO_TCHAR(Desc.id));
            continue;
        }
        const FString Id = UTF8_TO_TCHAR(Desc.id);
        AActor* Actor = Actors.FindRef(Id);
        const bool Native = Desc.native_asset && *Desc.native_asset;
        // Hydra instancer prototypes draw through one ISM: the native asset, or a static mesh built here.
        const bool Instanced = Desc.instance_transforms != nullptr;
        const bool Static = StaticGenerated(Desc);
        UClass* Wanted = Instanced            ? UInstancedStaticMeshComponent::StaticClass()
                         : (Native || Static) ? UStaticMeshComponent::StaticClass()
                                              : UDynamicMeshComponent::StaticClass();
        if (Actor && (!Actor->GetRootComponent() || Actor->GetRootComponent()->GetClass() != Wanted)) {
            Remove(Id);
            Actor = nullptr;
        }
        if (!Actor) {
            Actor = World->SpawnActor<AActor>();
            Actors.Add(Id, Actor);
            if (Native || Instanced || Static)
                AddRootComponent<UStaticMeshComponent>(Actor, [](UStaticMeshComponent&) {}, Wanted);
            else
                AddRootComponent<UDynamicMeshComponent>(Actor, [](UDynamicMeshComponent& Component) {
                    Component.SetEnableRaytracing(true);
                    Component.SetTangentsType(EDynamicMeshComponentTangentsMode::AutoCalculated);
                    Component.SetVertexColorSpaceTransformMode(EDynamicMeshVertexColorTransformMode::NoTransform);
                });
        }
        Actor->SetActorHiddenInGame(!Desc.visible);
        FMatrix Matrix;
        for (int R = 0; R < 4; ++R)
            for (int C = 0; C < 4; ++C) Matrix.M[R][C] = Instanced ? (R == C) : Desc.transform[R * 4 + C];
        Actor->SetActorTransform(FTransform(Matrix));
        UStaticMesh* InstanceMesh = nullptr;
        if (Native && Instanced) {
            InstanceMesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (!InstanceMesh) {
                Error = TEXT("Native StaticMesh not found in cooked content.");
                return false;
            }
        } else if (Native) {
            auto* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (!Mesh) {
                Error = TEXT("Native StaticMesh not found in cooked content.");
                return false;
            }
            if ((Desc.flags & HDUE_MESH_REQUIRE_NANITE) && !Mesh->HasValidNaniteData()) {
                Error = TEXT("Native asset has no cooked Nanite data.");
                return false;
            }
            UE_LOG(LogTemp, Display, TEXT("hdUE native mesh %s: NaniteData=%d"), *Mesh->GetPathName(),
                   Mesh->HasValidNaniteData());
            Actor->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Mesh);
        } else {
            // A background build for the previous geometry finishes unused (its memory is released with it).
            if (const FPendingStatic* Previous = PendingStatic.Find(Id)) {
                if (Previous->Task.IsValid()) ReservedMemory -= Previous->Memory;
                PendingStatic.Remove(Id);
            }
            if (Static) {
                auto* Built = NewStaticMesh(Actor);
                // Nanite draws opaque and masked materials only.
                const auto* Material = Desc.material_id ? Materials.Find(UTF8_TO_TCHAR(Desc.material_id)) : nullptr;
                const bool Nanite = StaticData[Index].Nanite && !(Material && IsTranslucentBlendMode(*Material->Get()));
                FinishStaticMesh(Built, MoveTemp(StaticData[Index].RenderData), StaticData[Index].Bounds,
                                 Nanite ? StaticData[Index].Nanite.Get() : nullptr);
                if (Instanced)
                    InstanceMesh = Built;
                else
                    Actor->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Built);
                if (Interactive) {
                    // Cards, Nanite data and the converted mesh's render data are built on background workers, from a
                    // copy of the descriptor's arrays: done here, Nanite doubled the time to the first frame (and
                    // normal-priority Nanite tasks starved its rendering for 15 s). TickConversions starts them.
                    auto Copy = MakeShared<FNaniteSource>(Desc);
                    Copy->Nanite = NaniteMeshes;
                    const int64 Source = Copy->Positions.GetAllocatedSize() + Copy->Normals.GetAllocatedSize() +
                                         Copy->UV.GetAllocatedSize() + Copy->Colors.GetAllocatedSize() +
                                         Copy->Indices.GetAllocatedSize();
                    PendingStatic.Add(Id, {Copy, 7 * Source, {}});
                }
            } else
                Actor->FindComponentByClass<UDynamicMeshComponent>()->SetMesh(MoveTemp(BuiltMeshes[Index]));
            ++GeometryRevisions.FindOrAdd(Id);
            ++GeometryUploads;
        }
        if (Instanced) {
            auto* Instances = Actor->FindComponentByClass<UInstancedStaticMeshComponent>();
            Instances->SetStaticMesh(InstanceMesh);
            TArray<FTransform> Transforms;
            Transforms.Reserve(Desc.instance_count);
            for (uint32 I = 0; I < Desc.instance_count; ++I) {
                FMatrix Instance;
                for (int R = 0; R < 4; ++R)
                    for (int C = 0; C < 4; ++C) Instance.M[R][C] = Desc.instance_transforms[I * 16 + R * 4 + C];
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
        if (Desc.flags & HDUE_MESH_SINGLE_SIDED)
            SingleSided.Add(Id);
        else
            SingleSided.Remove(Id);
        PrimIds.Add(Id, Desc.prim_id);
        Lighting->Link(Id, Cast<UPrimitiveComponent>(Actor->GetRootComponent()));
    }
    for (uint32 Index = 0; Index < Batch.light_count; ++Index) {
        const auto& Desc = Batch.lights[Index];
        if (!Desc.id || Desc.type > HDUE_CAMERA_DISTANT_LIGHT) {
            Error = TEXT("Unsupported light.");
            return false;
        }
        const FString Id = UTF8_TO_TCHAR(Desc.id);
        Remove(Id);
        auto* Actor = World->SpawnActor<AActor>();
        Actors.Add(Id, Actor);
        ULightComponent* Light = (Desc.type == HDUE_DISTANT_LIGHT || Desc.type == HDUE_CAMERA_DISTANT_LIGHT)
                                     ? static_cast<ULightComponent*>(NewObject<UDirectionalLightComponent>(Actor))
                                 : Desc.type == HDUE_SPHERE_LIGHT
                                     ? static_cast<ULightComponent*>(NewObject<UPointLightComponent>(Actor))
                                     : static_cast<ULightComponent*>(NewObject<URectLightComponent>(Actor));
        Light->SetMobility(EComponentMobility::Movable);
        Actor->SetRootComponent(Light);
        Actor->AddInstanceComponent(Light);
        Light->RegisterComponent();
        Light->SetLightColor(FLinearColor(Desc.color[0], Desc.color[1], Desc.color[2]));
        Light->SetIntensity(FMath::Max(0.f, Desc.intensity));
        Actor->SetActorLocation(FVector(Desc.position[0], Desc.position[1], Desc.position[2]));
        Actor->SetActorRotation(FVector(Desc.direction[0], Desc.direction[1], Desc.direction[2]).Rotation());
        Actor->SetActorHiddenInGame(!Desc.visible);
        if (Desc.type == HDUE_CAMERA_DISTANT_LIGHT)
            CameraLightDirections.Add(Id, FVector(Desc.direction[0], Desc.direction[1], Desc.direction[2]));
        if (auto* Point = Cast<UPointLightComponent>(Light)) {
            Point->SetAttenuationRadius(100000);
            Point->SetSourceRadius(FMath::Max(0.1f, Desc.width_cm));
        }
        if (auto* Rect = Cast<URectLightComponent>(Light)) {
            Rect->SetSourceWidth(Desc.width_cm);
            Rect->SetSourceHeight(Desc.height_cm);
            Rect->SetAttenuationRadius(100000);
        }
    }
    const double MeshesDone = FPlatformTime::Seconds();
    BindMaterials();
    if (Batch.material_count || Batch.mesh_count)
        UE_LOG(LogTemp, Display,
               TEXT("hdUE apply: %u materials %.0f ms (%d textures: prepare %.0f ms, create %.0f ms), %u meshes %.0f "
                    "ms, bind %.0f ms"),
               Batch.material_count, (MaterialsDone - ApplyStart) * 1000, TextureImports, PrepareSeconds * 1000,
               TextureSeconds * 1000, Batch.mesh_count, (MeshesDone - MaterialsDone) * 1000,
               (FPlatformTime::Seconds() - MeshesDone) * 1000);
    return true;
}
void FHdueScene::UpdateCameraLights(const FVector& Eye, const FVector& Target, const FVector& Up) {
    Lighting->UpdateCamera(Eye, Target, Up);
    const FVector Forward = (Target - Eye).GetSafeNormal();
    const FVector Right = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    const FVector CameraUp = FVector::CrossProduct(Forward, Right).GetSafeNormal();
    for (const auto& Entry : CameraLightDirections) {
        AActor* Actor = Actors.FindRef(Entry.Key);
        if (!IsValid(Actor)) continue;
        const FVector Direction =
            (Forward * Entry.Value.X + Right * Entry.Value.Y + CameraUp * Entry.Value.Z).GetSafeNormal();
        Actor->SetActorLocationAndRotation(Eye, Direction.Rotation());
    }
}
