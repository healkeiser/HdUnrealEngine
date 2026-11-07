#include "Scene.h"
#include "Misc/PackageName.h"
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
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Async/ParallelFor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "RHI.h"
#include "Algo/AllOf.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HeterogeneousVolumeComponent.h"
#include "GroomAsset.h"
#include "GroomBuilder.h"
#include "GroomComponent.h"
#include "HairAttributes.h"
#include "HairDescription.h"
#include "HairStrandsDatas.h"
#include "Engine/VolumeTexture.h"
#include "TextureResource.h"
#include "ImageCore.h"
#include "RenderUtils.h"
#include "ImageCoreUtils.h"
#include "ispc_texcomp.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "Engine/TextureLODSettings.h"
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
#include "Algo/AnyOf.h"
#include "UDIMUtilities.h"
#include "VirtualTextureBuild.h"
#include <atomic>

FHdueLoadCounters GHdueLoad;

FHdueScene::FHdueScene(UWorld* InWorld, bool InInteractive)
    : Assets(InWorld), Lighting(MakeUnique<FHdueLighting>(InWorld)), World(InWorld), Interactive(InInteractive) {}
FHdueScene::~FHdueScene() {
    // Launched builds finish first, as UE finishes a static mesh's async build before it destroys the mesh
    // (UStaticMesh::IsReadyForFinishDestroy) and the running ones at exit (FStaticMeshCompilingManager::Shutdown):
    // their Nanite builds hold the task workers, and UE shutting down under them crashed in a readback copy pass.
    for (const auto& [Key, Pending] : PendingStatic) Pending.Task.Wait();
    for (const FPendingStatic& Retired : RetiredBuilds) Retired.Task.Wait();
    for (const auto& Build : TextureBuilds)
        if (Build->Task.IsValid()) Build->Task.Wait();
    // The load counters are the process's: builds this scene drops count as done, or a later session's progress
    // (Houdini recreates its renderer after the first frame of a large stage) never reached its total.
    for (const auto& Build : TextureBuilds)
        if (!Build->Task.IsValid()) ++GHdueLoad.TexturesDone;
    GHdueLoad.MeshBuildsDone += uint32(PendingStatic.Num());
    for (auto& Entry : Actors)
        if (IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
}
TArray<TStrongObjectPtr<UTexture2D>> FHdueScene::GatherVirtualTextures() const {
    TSet<const UMaterialInterface*> Seen;  // materials are shared by many meshes: their textures are listed once
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
                if (!Material || Seen.Contains(Material)) continue;
                Seen.Add(Material);
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
// Whether a mesh's Nanite data fits in UE's root page pool, reserving its pages if so. UE's pool is one buffer of at
// most 2 GB on NVIDIA (4 GB on AMD, the GPU's largest buffer view), less the streaming pool, and UE ends the process
// when a root page does not fit ("Cannot allocate more root pages", NaniteStreamingManager.cpp). Runtime meshes keep
// one root page each and stream the rest (BuildNanite), so the pool holds tens of thousands of meshes; a scene past it
// keeps its remaining meshes as regular static meshes instead, with a warning. 10% stays free: released meshes return
// their pages when UE's garbage collection frees them, after this count does.
bool FHdueScene::NaniteFits(const FString& Id, const Nanite::FResources* Nanite) {
    if (const uint32* Previous = NaniteRootPages.Find(Id)) {
        NaniteRootPagesUsed -= *Previous;
        NaniteRootPages.Remove(Id);
    }
    if (!Nanite) return false;
    static const auto* StreamingPool =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.Streaming.StreamingPoolSize"));
    const uint64 PoolMB =
        FMath::Min<uint64>(IsRHIDeviceAMD() ? 4095 : 2048, GRHIGlobals.MaxViewSizeBytesForNonTypedBuffer >> 20);
    const uint64 RootMB = PoolMB - FMath::Min<uint64>(PoolMB, StreamingPool ? uint64(StreamingPool->GetInt()) : 512);
    const int64 Budget = int64((RootMB << 20) >> NANITE_ROOT_PAGE_GPU_SIZE_BITS) * 9 / 10;
    if (NaniteRootPagesUsed + Nanite->NumRootPages > Budget) {
        static bool Warned = false;
        if (!Warned)
            UE_LOG(LogTemp, Warning,
                   TEXT("hdUE: UE's Nanite root page pool is full (%lld of %lld pages); further meshes render without "
                        "Nanite"),
                   NaniteRootPagesUsed, Budget);
        Warned = true;
        return false;
    }
    NaniteRootPages.Add(Id, Nanite->NumRootPages);
    NaniteRootPagesUsed += Nanite->NumRootPages;
    return true;
}
void FHdueScene::Remove(const FString& Id) {
    // Root pages are counted per shared mesh key, returned once UE destroys the shared mesh (the SharedMeshes sweep).
    Lighting->Remove(Id);
    RemoveVolume(Id);
    if (FGroom* Groom = Grooms.Find(Id)) {
        if (IsValid(Groom->Actor)) World->DestroyActor(Groom->Actor);
        Grooms.Remove(Id);
    }
    if (AActor** Actor = Actors.Find(Id)) {
        if (IsValid(*Actor)) World->DestroyActor(*Actor);
        Actors.Remove(Id);
    }
    Materials.Remove(Id);
    MaterialTextures.Remove(Id);
    OneSided.Remove(Id);
    MeshMaterials.Remove(Id);
    SingleSided.Remove(Id);
    TranslucentStatic.Remove(Id);
    PrimIds.Remove(Id);
    WetBindings.Remove(Id);
    GeometryRevisions.Remove(Id);
    ForgetPending(Id);
}
// Drops a mesh's background build: a queued one at once, a launched one once it finishes (TickConversions), holding its
// reserved memory until then. Removed meshes kept their geometry copy and still had cards and Nanite built. A build
// shared by other meshes of its key goes on for them.
void FHdueScene::ForgetPending(const FString& Id) {
    FString Key;
    if (!PendingKeys.RemoveAndCopyValue(Id, Key)) return;
    FPendingStatic& Shared = PendingStatic.FindChecked(Key);
    Shared.Ids.Remove(Id);
    if (Shared.Ids.Num()) {
        ++(Shared.Task.IsValid() ? ShrunkBuilds : QueuedShrunk);
        return;
    }
    FPendingStatic Pending;
    PendingStatic.RemoveAndCopyValue(Key, Pending);
    ++GHdueLoad.MeshBuildsDone;  // dropped: no longer waited for
    QuickMemory -= Pending.QuickMemory;
    if (!Pending.Task.IsValid()) {
        ++QueuedDropped;
        return;
    }
    ++DroppedBuilds;
    RetiredBuilds.Add(MoveTemp(Pending));
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

// Curves as UE hair strands: the groom asset UE's importer would build, made from memory with UE's own builder
// (GroomAsset.cpp BuildHairGroup_Strands): one group, guides generated from every strand (no simulation), so no
// interpolation data; positions uncompressed, as a cooked runtime uploads its in-memory bulk data as is. Drawn by a
// groom component with UE's default hair material.
bool FHdueScene::UpsertGroom(const HdueGroomDesc& Desc, FString& Error) {
    const FString Id = UTF8_TO_TCHAR(Desc.id);
    FHairDescription Description;
    Description.InitializeVertices(int32(Desc.point_count));
    Description.InitializeStrands(int32(Desc.curve_count));
    for (uint32 I = 0; I < Desc.point_count; ++I) {
        SetHairVertexAttribute(Description, FVertexID(int32(I)), HairAttribute::Vertex::Position,
                               FVector3f(Desc.points[I * 3], Desc.points[I * 3 + 1], Desc.points[I * 3 + 2]));
        SetHairVertexAttribute(Description, FVertexID(int32(I)), HairAttribute::Vertex::Width,
                               FMath::Max(0.f, Desc.widths_cm[I]));
    }
    for (uint32 I = 0; I < Desc.curve_count; ++I)
        SetHairStrandAttribute(Description, FStrandID(int32(I)), HairAttribute::Strand::VertexCount,
                               int32(Desc.curve_point_counts[I]));
    TStrongObjectPtr<UGroomAsset> Asset(NewObject<UGroomAsset>(GetTransientPackage()));
    Asset->SetNumGroup(1);
    auto& Interpolation = Asset->GetHairGroupsInterpolation()[0];
    Interpolation.InterpolationSettings.GuideType = EGroomGuideType::Generated;
    Interpolation.InterpolationSettings.HairToGuideDensity = 1.f;
    FHairDescriptionGroups Groups;
    // (FHairDescriptionGroups::IsValid is not exported for modular builds: the group count stands in.)
    if (!FGroomBuilder::BuildHairDescriptionGroups(Description, Groups) || Groups.HairGroups.Num() != 1) {
        Error = TEXT("Cannot build hair strands from the curves of ") + Id;
        return false;
    }
    FHairStrandsDatas Strands, Guides;
    auto& Info = Asset->GetHairGroupsInfo()[0];
    FGroomBuilder::BuildData(Groups.HairGroups[0], Interpolation, Info, Strands, Guides);
    auto& Platform = Asset->GetHairGroupsPlatformData()[0];
    FGroomBuilder::BuildBulkData(Groups.HairGroups[0].Info, Guides, Platform.Guides.BulkData, false);
    FGroomBuilder::BuildBulkData(Groups.HairGroups[0].Info, Strands, Platform.Strands.BulkData, false);
    Platform.Strands.InterpolationBulkData.Reset();
    FGroomBuilder::BuildClusterBulkData(Strands, Groups.Bounds.SphereRadius, Asset->GetHairGroupsLOD()[0],
                                        Platform.Strands.ClusterBulkData);
    Asset->GetHairGroupsRendering()[0].ShadowSettings.bUseHairRaytracingGeometry = true;  // the Path Tracer's strands
    // No cards or meshes, but UGroomAsset::InitCardsResources/InitMeshesResources check their LOD arrays match the
    // group's LOD count (the editor's cache build sizes them the same way).
    const int32 LODCount = Asset->GetHairGroupsLOD()[0].LODs.Num();
    Platform.Cards.LODs.SetNum(LODCount);
    Platform.Meshes.LODs.SetNum(LODCount);
    Asset->UpdateHairGroupsInfo();
    Asset->InitResources();
    if (!Asset->AreGroupsValid()) {
        Error = TEXT("UE rejected the hair strands of ") + Id;
        return false;
    }
    FGroom& Groom = Grooms.FindOrAdd(Id);
    if (IsValid(Groom.Actor)) World->DestroyActor(Groom.Actor);
    Groom.Actor = World->SpawnActor<AActor>();
    AddRootComponent<UGroomComponent>(Groom.Actor, [&](UGroomComponent& Component) {
        Component.SetGroomAsset(Asset.Get());
        // UGroomComponent::PostLoad's call: a spawned component is never loaded.
        Component.PrecachePSOs();
    });
    Groom.Asset = MoveTemp(Asset);
    Groom.Actor->SetActorHiddenInGame(!Desc.visible);
    return true;
}
bool FHdueScene::UpsertVolume(const HdueVolumeDesc& Desc, FString& Error) {
    const FString Id = UTF8_TO_TCHAR(Desc.id);
    const uint64 Count = uint64(Desc.width) * Desc.height * Desc.depth;
    // A bound volume material (a prepared Substrate Builder one: Volume domain, heterogeneous volumes usage), else
    // hdUE's M_HdUEVolume. Both read the same grids and parameters.
    UMaterialInterface* Parent = nullptr;
    bool Bound = false;  // the bound material is used: its parameters apply
    if (Desc.material && *Desc.material) {
        Parent = LoadObject<UMaterialInterface>(nullptr, UTF8_TO_TCHAR(Desc.material));
        const UMaterial* Base = Parent ? Parent->GetMaterial() : nullptr;
        if (!Base || Base->MaterialDomain != MD_Volume || !Base->GetUsageByFlag(MATUSAGE_HeterogeneousVolumes)) {
            UE_LOG(LogTemp, Warning,
                   TEXT("hdUE: volume %s material %hs is missing or not a heterogeneous volume material; using "
                        "M_HdUEVolume"),
                   *Id, Desc.material);
            Parent = nullptr;
        }
        Bound = Parent != nullptr;
    }
    if (!Parent) Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUEVolume.M_HdUEVolume"));
    if (!Parent) {
        Error =
            TEXT("Volume material /Game/HdUE/M_HdUEVolume is missing; run Prepare-Content.ps1 and stage the runtime.");
        return false;
    }
    FVolume& Volume = Volumes.FindOrAdd(Id);
    if (!IsValid(Volume.Actor)) {
        Volume.Actor = World->SpawnActor<AActor>();
        AddRootComponent<UHeterogeneousVolumeComponent>(Volume.Actor, [](UHeterogeneousVolumeComponent&) {});
    }
    if (!Volume.Material.IsValid() || Volume.Material->Parent != Parent)
        Volume.Material.Reset(UMaterialInstanceDynamic::Create(Parent, World));
    auto* Component = Volume.Actor->FindComponentByClass<UHeterogeneousVolumeComponent>();
    // ponytail: half-float dense 3D textures; sparse grids beyond a few hundred voxels per side cost memory.
    const auto Fill = [&](TStrongObjectPtr<UVolumeTexture>& Slot, const float* Values) -> UVolumeTexture* {
        UVolumeTexture* Texture = Slot.Get();
        if (!Texture || Texture->GetSizeX() != int32(Desc.width) || Texture->GetSizeY() != int32(Desc.height) ||
            Texture->GetSizeZ() != int32(Desc.depth)) {
            Texture = UVolumeTexture::CreateTransient(Desc.width, Desc.height, Desc.depth, PF_R16F);
            if (!Texture) return nullptr;
            Texture->SRGB = false;
            Texture->Filter = TF_Trilinear;
            Texture->NeverStream = true;
            Slot.Reset(Texture);
        }
        auto& Mip = Texture->GetPlatformData()->Mips[0];
        auto* Voxels = static_cast<FFloat16*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
        for (uint64 I = 0; I < Count; ++I) Voxels[I] = FFloat16(FMath::Max(0.f, Values[I]));
        Mip.BulkData.Unlock();
        Texture->UpdateResource();
        return Texture;
    };
    if (Count) {
        UVolumeTexture* Texture = Fill(Volume.Texture, Desc.density);
        if (!Texture) {
            Error = TEXT("Cannot create the volume texture.");
            return false;
        }
        Volume.Material->SetTextureParameterValue(TEXT("Density"), Texture);
    }
    // Emission: UE's BlackBody of the field times its intensity (M_HdUEVolume); intensity 0 without a field.
    if (Count && Desc.emission) {
        UVolumeTexture* Texture = Fill(Volume.Emission, Desc.emission);
        if (!Texture) {
            Error = TEXT("Cannot create the volume emission texture.");
            return false;
        }
        Volume.Material->SetTextureParameterValue(TEXT("Emission"), Texture);
        Volume.Material->SetScalarParameterValue(TEXT("EmissionIntensity"), Desc.emission_intensity);
        Volume.Material->SetScalarParameterValue(TEXT("TemperatureScale"), Desc.temperature_scale);
    } else {
        Volume.Emission.Reset();
        Volume.Material->SetScalarParameterValue(TEXT("EmissionIntensity"), 0.f);
    }
    Volume.Material->SetVectorParameterValue(TEXT("Albedo"),
                                             FLinearColor(Desc.albedo[0], Desc.albedo[1], Desc.albedo[2], 1));
    Volume.Material->SetScalarParameterValue(TEXT("ExtinctionScale"), Desc.extinction_scale);
    // A bound volume material's public and generated parameters (its graph's values and live edits).
    if (Bound && Desc.parameter_count) {
        if (const HdueResult Code =
                HdueApplyMaterialParameters(*Volume.Material.Get(), Desc.parameters, Desc.parameter_count, Error);
            Code != HDUE_OK)
            return false;
    }
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
        // Not a teleport: UE keeps each instance's previous transform for the velocity buffer (motion vectors).
        Instances->BatchUpdateInstancesTransforms(0, Transforms, true, true, false);
    } else if (Count == 1)
        Actor->SetActorTransform(ToTransform(0), false, nullptr, ETeleportType::TeleportPhysics);
}
void FHdueScene::UpdateMeshes(HdueMeshUpdate* Meshes, uint32 Count) {
    if (IdMode) {
        FString Ignored;
        SetIdMode(0, Ignored);
    }
    for (uint32 Index = 0; Index < Count; ++Index) {
        auto& Mesh = Meshes[Index];
        const FString Id = UTF8_TO_TCHAR(Mesh.id);
        AActor* Actor = Actors.FindRef(Id);
        const auto* Root = IsValid(Actor) ? Actor->GetRootComponent() : nullptr;
        const auto* Static = Cast<UStaticMeshComponent>(Root);
        const UStaticMesh* Built = Static ? Static->GetStaticMesh() : nullptr;
        const auto Translucent = [&](const FString& MaterialId) {
            const auto* Material = Materials.Find(MaterialId);
            return Material && IsTranslucentBlendMode(*Material->Get());
        };
        const FString MaterialId = Mesh.material_id ? UTF8_TO_TCHAR(Mesh.material_id) : TEXT("");
        // Apply's choices that depend on these fields: a static mesh built here (not editable) or a dynamic one, and
        // Nanite for opaque and masked materials only, compared with the mesh as built: its material may have changed
        // blend mode in place (the delegate then sends its meshes here), as UE's Nanite proxy re-audits its materials'
        // blend modes when they change (Nanite::IsSupportedBlendMode, NaniteResources.cpp).
        const bool WantStatic = Cast<UInstancedStaticMeshComponent>(Root) || !(Mesh.flags & HDUE_MESH_EDITABLE);
        if (!Root || !MeshMaterials.Contains(Id) || bool(Static) != WantStatic ||
            // A mesh waiting for its background build without a first pass draws none (Apply).
            (Static && ((!Built && !PendingKeys.Contains(Id)) ||
                        (Built && Built->GetOuter() != GetTransientPackage()) ||  // generated here, not a native asset
                        TranslucentStatic.Contains(Id) != Translucent(MaterialId)))) {
            Mesh.needs_upload = 1;
            continue;
        }
        Mesh.needs_upload = 0;
        Actor->SetActorHiddenInGame(!Mesh.visible);
        MeshMaterials.Add(Id, MaterialId);
        if (Mesh.flags & HDUE_MESH_SINGLE_SIDED)
            SingleSided.Add(Id);
        else
            SingleSided.Remove(Id);
        PrimIds.Add(Id, Mesh.prim_id);
    }
    BindMaterials();
}
namespace {
// For comparing the two paths (unreal:cvar:r:HdUE:IdSwap = 1): every mesh swaps to M_HdUEId, switch or not.
TAutoConsoleVariable<int32> CVarIdSwap(TEXT("r.HdUE.IdSwap"), 0,
                                       TEXT("ID passes: 1 swaps every mesh to M_HdUEId instead of switching the "
                                            "materials that have the HdUEIdPass switch."));
}  // namespace
bool FHdueScene::SetIdMode(int32 Mode, FString& Error) {
    if (Mode == IdMode) return true;
    const bool On = Mode != 0;
    auto* IdMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUEId.M_HdUEId"));
    if (On && !IdMaterial) {
        Error = TEXT("ID material /Game/HdUE/M_HdUEId is missing; run Prepare-Content.ps1 and stage the runtime.");
        return false;
    }
    // Materials with the HdUEIdPass switch (every material hdUE generates: the Preview Surface and Standard Surface
    // masters, M_Glass, prepared MaterialX and Substrate Builder graphs) draw the ID themselves when custom primitive
    // data 5 is set: a GPU Scene update, where SetMaterial rebuilds the mesh's render state. Other meshes (native
    // assets) swap to M_HdUEId.
    const FHashedMaterialParameterInfo Switch(TEXT("HdUEIdPass"));
    const auto Switches = [&](const UMeshComponent* Mesh) {
        float Unused;
        if (CVarIdSwap.GetValueOnGameThread()) return false;
        for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
            if (const auto* Material = Mesh->GetMaterial(Slot);
                !Material || !Material->GetScalarParameterDefaultValue(Switch, Unused))
                return false;
        return Mesh->GetNumMaterials() > 0;
    };
    int32 Switched = 0, Swapped = 0;
    const auto Apply = [&](AActor* Actor, int32 Id) {
        TArray<UMeshComponent*> Meshes;
        Actor->GetComponents(Meshes);
        for (auto* Mesh : Meshes) {
            // A Heterogeneous Volume draws no ID (no GBuffer), and its SetMaterial rebuilds its SVT binding: the ID
            // material left it empty for every later frame.
            if (Mesh->IsA<UHeterogeneousVolumeComponent>()) continue;
            if (On) {
                // Exact in the float16 readback: both halves stay below 2048.
                // Instance mode: instanced components emit their per-instance id (custom data 3), the rest -1.
                const bool Instances = Mode == 2 && Mesh->IsA<UInstancedStaticMeshComponent>();
                Mesh->SetCustomPrimitiveDataVector4f(0, Id >= 0 && Mode == 1
                                                            ? FVector4f(float(Id / 2048), float(Id % 2048), 1, 0)
                                                            : FVector4f(0, 0, 0, 0));
                Mesh->SetCustomPrimitiveDataFloat(4, Instances ? 1.f : 0.f);
                // Already swapped (prim to instance IDs): no switch, and SetMaterial does nothing.
                if (Switches(Mesh)) {
                    Mesh->SetCustomPrimitiveDataFloat(5, 1.f);
                    ++Switched;
                } else {
                    for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot) Mesh->SetMaterial(Slot, IdMaterial);
                    ++Swapped;
                }
            } else if (Switches(Mesh)) {
                Mesh->SetCustomPrimitiveDataFloat(5, 0.f);
            } else {
                for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
                    if (IdMaterial && Mesh->GetMaterial(Slot) == IdMaterial) {
                        Mesh->EmptyOverrideMaterials();  // native skeletal meshes kept the ID material before
                        break;
                    }
            }
        }
    };
    for (const auto& Entry : Actors)
        if (IsValid(Entry.Value)) {
            const int32* Id = PrimIds.Find(Entry.Key);
            Apply(Entry.Value, Id ? *Id : -1);
        }
    // ponytail: native catalog assets carry no Hydra prim ID; they read as background in the ID pass.
    for (auto* Actor : Assets.GetActors())
        if (IsValid(Actor)) Apply(Actor, -1);
    // Read by tests: meshes of hdUE materials must all switch.
    if (On && !IdMode)
        UE_LOG(LogTemp, Log, TEXT("hdUE ID pass: %d meshes switched, %d swapped to M_HdUEId"), Switched, Swapped);
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
// The top mips UE's cooker strips from a world texture (TextureLODSettings.cpp CalculateLODBias, as a cook evaluates
// it: the World group's LODBias plus r.GlobalTextureLODBias, the level kept clamped to the group's
// MinLODSize..MaxLODSize; a virtual texture takes LODBias_VT and MaxLODSize_VT instead, and no minimum). A cooked
// texture already lacks them, so UE's runtime skips both terms; hdUE builds its platform data at runtime and strips
// them itself. Read on the game thread when a batch starts: as in UE, a change applies to textures built after.
struct FCookLODBias {
    int32 Bias = 0, MinLOD = 0, MaxLOD = 30, BiasVT = 0, MaxLODVT = 0;
    static FCookLODBias Current() {
        FCookLODBias Out;
        if (const UDeviceProfile* Profile = UDeviceProfileManager::Get().GetActiveProfile())
            if (const UTextureLODSettings* Settings = Profile->GetTextureLODSettings()) {
                const FTextureLODGroup& Group = Settings->GetTextureLODGroup(TEXTUREGROUP_World);
                Out.Bias = Group.LODBias;
                Out.MinLOD = FMath::CeilLogTwo(FMath::Max(Group.MinLODSize, 1));
                Out.MaxLOD = FMath::CeilLogTwo(FMath::Max(Group.MaxLODSize, 1));
                Out.BiasVT = Group.LODBias_VT;
                Out.MaxLODVT = Group.MaxLODSize_VT > 0 ? FMath::CeilLogTwo(Group.MaxLODSize_VT) : 0;
            }
        static const IConsoleVariable* Global =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.GlobalTextureLODBias"));
        if (Global) {
            Out.Bias += Global->GetInt();
            Out.BiasVT += Global->GetInt();
        }
        return Out;
    }
    int32 Strip(int32 Width, int32 Height) const {
        const int32 Top = FImageCoreUtils::GetMipCountFromDimensions(Width, Height, 0, false) - 1;
        return Top - FMath::Clamp(FMath::Clamp(Top - Bias, MinLOD, MaxLOD), 0, Top);
    }
    // A virtual texture's whole size (its UDIM blocks side by side).
    int32 StripVirtual(int32 Width, int32 Height) const {
        const int32 Top = FImageCoreUtils::GetMipCountFromDimensions(Width, Height, 0, false) - 1;
        return Top - FMath::Clamp(FMath::Clamp(Top - BiasVT, 0, MaxLODVT ? MaxLODVT : Top), 0, Top);
    }
    FString Key() const { return FString::Printf(TEXT("|lod %d %d %d"), Bias, MinLOD, MaxLOD); }
};
// A texture's mipped (and, for 8-bit images, block-compressed) platform data, on a worker thread. BC needs whole 4x4
// blocks at the top level; other sizes and float images (EXR/HDR) stay uncompressed.
TUniquePtr<FTexturePlatformData> PrepareTexture(FImage&& Image, FHdueScene::FPreviewTexture& Texture, bool SRGB,
                                                const FCookLODBias& LOD) {
    if (!ERawImageFormat::IsHDR(Image.Format) && Image.Format != ERawImageFormat::BGRA8) {  // grey, 16-bit
        FImage Converted;
        Image.CopyTo(Converted, ERawImageFormat::BGRA8, Image.GammaSpace);
        Image = MoveTemp(Converted);
    }
    TArray<FImage> Chain = MipChain(MoveTemp(Image));
    if (Texture.Coverage > 0.f && Chain[0].Format == ERawImageFormat::BGRA8)  // ponytail: float cutouts keep plain mips
        ScaleAlphaCoverage(Chain, Texture.Read, Texture.Coverage);
    Chain.RemoveAt(0, LOD.Strip(Chain[0].SizeX, Chain[0].SizeY));
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
// the materials read: reloading a scene skips decoding its images, most of a texture load. Entries unused for a week go
// when a process first uses the cache, as the Nanite cache's do (a hit counts as a use).
constexpr int32 CacheVersion = 2;
FString CachePath(const FString& File, const FHdueScene::FPreviewTexture& Texture, bool SRGB, const FCookLODBias& LOD) {
    static const FString Root = [] {
        const FString Folder = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA")),
                                               TEXT("hdUE"), TEXT("TextureCache"));
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *(Folder / TEXT("*")), true, false);  // entries and abandoned temporaries
        for (const FString& Name : Files)
            if (FDateTime::UtcNow() - IFileManager::Get().GetTimeStamp(*(Folder / Name)) > FTimespan::FromDays(7))
                IFileManager::Get().Delete(*(Folder / Name), false, false, true);
        return Folder;
    }();
    const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
    if (!Stat.bIsValid) return FString();
    const FString Key =
        FString::Printf(TEXT("%s|%lld|%lld|%d|%d|%d|%d"), *FPaths::ConvertRelativePathToFull(File),
                        Stat.ModificationTime.GetTicks(), Stat.FileSize, SRGB, Texture.Read, Texture.Vector,
                        CacheVersion) +
        (Texture.Coverage > 0.f ? FString::Printf(TEXT("|coverage up %g"), Texture.Coverage) : FString()) +
        (LOD.Bias ? LOD.Key() : FString());  // keys of unbiased textures stay as they were
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
    if (In->IsError() || Data->Mips.IsEmpty()) return nullptr;
    In.Reset();
    IFileManager::Get().SetTimeStamp(*Path, FDateTime::UtcNow());  // a hit counts as a use (eviction)
    return Data;
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
bool IsUdim(const FString& File) { return File.Contains(TEXT("<UDIM>")); }
// A virtual texture of File's UDIM tiles, or of File alone (one block) for the other textures of a UDIM material, with
// the 2D textures' decoding, mips, alpha coverage and block formats. Built once into hdUE's virtual texture cache
// (VirtualTextureBuild.cpp): a set already built from the same tiles and choices loads from there undecoded.
TUniquePtr<FTexturePlatformData> PrepareVirtualTexture(const FString& File, FHdueScene::FPreviewTexture& Texture,
                                                       const FCookLODBias& LOD, FString& Error) {
    TMap<int32, FString> Found;
    if (IsUdim(File))
        Found = HdueUdimTiles(File);
    else
        Found.Add(1001, File);
    const TArray<TPair<int32, FString>> Tiles = Found.Array();
    if (Tiles.IsEmpty()) {
        Error = TEXT("no UDIM tile on disk");
        return nullptr;
    }
    FString Source = FString::Printf(TEXT("%d|%d|%d|%d|%g|%d|%d|%d|%d|%d"), CacheVersion, Texture.Read, Texture.Vector,
                                     Texture.SRGB, Texture.Coverage, Texture.Address[0], Texture.Address[1],
                                     BlockCompression(), LOD.BiasVT, LOD.MaxLODVT);
    for (const auto& [Index, Path] : Tiles) {
        const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
        Source += FString::Printf(TEXT("|%d|%s|%lld|%lld"), Index, *FPaths::ConvertRelativePathToFull(Path),
                                  Stat.ModificationTime.GetTicks(), Stat.FileSize);
    }
    const FString Key = FString::Printf(
        TEXT("%016llx"), CityHash64(reinterpret_cast<const char*>(*Source), Source.Len() * sizeof(TCHAR)));
    if (auto Cached = HdueLoadVirtualTexture(Key)) {
        // The repacked channels (BC4/BC5) the materials read, as the build chose them.
        if (!IsBlockCompressedFormat(Cached->PixelFormat) || BlockFormat(Texture) == Cached->PixelFormat) {
            ++GHdueLoad.VirtualCached;
            return Cached;
        }
    }
    TArray<FHdueVirtualTextureBlock> Blocks;
    Blocks.SetNum(Tiles.Num());
    std::atomic<int32> Unread = INDEX_NONE;
    ParallelFor(Tiles.Num(), [&](int32 Index) {
        UE::TextureUtilitiesCommon::ExtractUDIMCoordinates(Tiles[Index].Key, Blocks[Index].X, Blocks[Index].Y);
        if (!FImageUtils::LoadImage(*Tiles[Index].Value, Blocks[Index].Image)) Unread = Index;
    });
    if (Unread != INDEX_NONE) {
        Error = TEXT("cannot read ") + Tiles[Unread].Value;
        return nullptr;
    }
    // The cook's stripped top mips: every tile halved as often, at load, so the full-resolution tiles never stay.
    int32 Columns = 1, Rows = 1, Width = 1, Height = 1;
    for (const auto& Block : Blocks) {
        Columns = FMath::Max(Columns, Block.X + 1);
        Rows = FMath::Max(Rows, Block.Y + 1);
        Width = FMath::Max(Width, Block.Image.SizeX);
        Height = FMath::Max(Height, Block.Image.SizeY);
    }
    const int32 Strip = LOD.StripVirtual(Width * Columns, Height * Rows);
    ParallelFor(Blocks.Num(), [&](int32 Index) {
        FImage& Image = Blocks[Index].Image;
        for (int32 Level = 0; Level < Strip && (Image.SizeX > 1 || Image.SizeY > 1); ++Level) {
            FImage Next(FMath::Max(1, Image.SizeX / 2), FMath::Max(1, Image.SizeY / 2), Image.Format, Image.GammaSpace);
            HalveImage(Image, Next);
            Image = MoveTemp(Next);
        }
    });
    // One format for every tile, as UE converts a UDIM set's mismatched images (InterchangeTextureFactory.cpp): half
    // floats if any tile is HDR, BGRA8 otherwise.
    const bool Float =
        Algo::AnyOf(Blocks, [](const auto& Block) { return ERawImageFormat::IsHDR(Block.Image.Format); });
    const ERawImageFormat::Type Raw = Float ? ERawImageFormat::RGBA16F : ERawImageFormat::BGRA8;
    for (auto& Block : Blocks)
        if (Block.Image.Format != Raw) {
            FImage Converted;
            Block.Image.CopyTo(Converted, Raw, Block.Image.GammaSpace);
            Block.Image = MoveTemp(Converted);
        }
    const bool Compress = !Float && BlockCompression();
    FHdueVirtualTextureCodec Codec;
    Codec.Format = Compress ? BlockFormat(Texture) : FImageCoreUtils::GetPixelFormatForRawImageFormat(Raw);
    Codec.Mips = [&](FImage&& Image) {
        TArray<FImage> Chain = MipChain(MoveTemp(Image));
        if (Texture.Coverage > 0.f && !Float) ScaleAlphaCoverage(Chain, Texture.Read, Texture.Coverage);
        return Chain;
    };
    Codec.Encode = [&](const FImage& Tile, uint8* Out) {
        if (Compress)
            CompressMip(Tile, Codec.Format, Texture, Out);
        else
            FMemory::Memcpy(Out, Tile.RawData.GetData(), Tile.RawData.Num());
    };
    auto Built = HdueBuildVirtualTexture(MoveTemp(Blocks), Codec, Texture.Address, Key, Error);
    if (Built) ++GHdueLoad.VirtualBuilt;
    return Built;
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
// A UDIM set wraps: UE places tile rows for UVs with V flipped (1 - v), which puts tiles above 1001 at negative V
// (VirtualTextureDataBuilder.cpp, BlockY), and UE's UDIM textures keep the default wrap.
TextureAddress TextureWrap(const HdueTextureDesc& Texture, int32 Axis) {
    return IsUdim(UTF8_TO_TCHAR(Texture.file)) ? TA_Wrap : TextureWrap(Texture.wrap[Axis]);
}
// An image's size from its header (the first 256 KB: PNG and JPEG headers come first), or 0 x 0 when that is not enough
// to tell (EXR, op: images), cached per file version: a COP export rewrites the same path at another size.
FIntPoint ImageSize(const FString& File) {
    static FCriticalSection Lock;
    static TMap<FString, FIntPoint> Sizes;
    const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
    const FString Key = FString::Printf(TEXT("%s|%lld|%lld"), *File, Stat.ModificationTime.GetTicks(), Stat.FileSize);
    {
        FScopeLock Scope(&Lock);
        if (const FIntPoint* Known = Sizes.Find(Key)) return *Known;
    }
    FIntPoint Size(0, 0);
    if (TUniquePtr<FArchive> In{IFileManager::Get().CreateFileReader(*File, FILEREAD_Silent)}) {
        TArray<uint8> Head;
        Head.SetNumUninitialized(int32(FMath::Min<int64>(In->TotalSize(), 256 * 1024)));
        In->Serialize(Head.GetData(), Head.Num());
        IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        const EImageFormat Format = Module.DetectImageFormat(Head.GetData(), Head.Num());
        if (Format == EImageFormat::PNG || Format == EImageFormat::JPEG)
            if (const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
                Wrapper && Wrapper->SetCompressed(Head.GetData(), Head.Num()))
                Size = FIntPoint(int32(Wrapper->GetWidth()), int32(Wrapper->GetHeight()));
    }
    FScopeLock Scope(&Lock);
    Sizes.Add(Key, Size);
    return Size;
}
// A material with a UDIM texture draws on a VT parent, every texture of it virtual: UE's USD import does the same (its
// VT reference material, UniversalMaterialSchemaHandler.cpp), as a material's sampler is virtual or not. With
// r.VT.EnableAutoImport, so does one with a texture of at least AutoVTSize x AutoVTSize texels (4096, UE's Texture
// Import setting), as UE's texture import makes such textures virtual (TextureImportSettings.cpp,
// ShouldTextureBeVirtualByAutoImportSize): its textures then stream from hdUE's virtual texture cache on disk instead
// of staying whole in memory. hdUE reads AutoVTSize from the engine ini
// ([/Script/TextureUtilitiesCommon.TextureImportSettings]), where UE reads it from the editor's, which runtime builds
// do not load.
bool VirtualMaterial(const HdueMaterialDesc& Desc) {
    for (const auto& Texture : Desc.textures)
        if (Texture.file && IsUdim(UTF8_TO_TCHAR(Texture.file))) return true;
    static const IConsoleVariable* AutoImport =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.VT.EnableAutoImport"));
    if (!AutoImport || !AutoImport->GetInt()) return false;
    static const int64 AutoSize = [] {
        int32 Value = 4096;
        GConfig->GetInt(TEXT("/Script/TextureUtilitiesCommon.TextureImportSettings"), TEXT("AutoVTSize"), Value,
                        GEngineIni);
        return int64(Value);
    }();
    if (AutoSize <= 0) return false;
    for (const auto& Texture : Desc.textures)
        if (Texture.file && *Texture.file) {
            const FIntPoint Size = ImageSize(UTF8_TO_TCHAR(Texture.file));
            if (int64(Size.X) * Size.Y >= AutoSize * AutoSize) return true;
        }
    return false;
}
// One texture per file, decoding and wrap (UE keeps the sampler's address mode on the texture), the clip value its
// mips keep the coverage of (other uses of the file keep plain mips), and whether it is virtual.
FString TextureKey(const FString& File, const HdueTextureDesc& Texture, int32 Slot, float Coverage, bool Virtual) {
    return FString::Printf(TEXT("%s|%s|%d|%d|%g%s"), *File, TextureSRGB(Texture, Slot) ? TEXT("srgb") : TEXT("raw"),
                           int32(TextureWrap(Texture, 0)), int32(TextureWrap(Texture, 1)), Coverage,
                           Virtual ? TEXT("|vt") : TEXT(""));
}
// Inputs the Preview Surface parents read through <Slot>Mask; the others (colour, normal) read RGB.
bool ScalarSlot(int32 Slot) { return !hdue_texture_is_color(Slot) && Slot != HDUE_TEXTURE_NORMAL; }
// One file's texture variants (Apply), on a worker: a virtual texture's chunks, or the image decoded once and each
// variant mipped and block-compressed (or read from the texture cache) into its Pending platform data.
void BuildTextureFile(const FString& File, TArrayView<FHdueScene::FPreviewTexture* const> Entries,
                      const FCookLODBias& LOD) {
    ON_SCOPE_EXIT { ++GHdueLoad.TexturesDone; };
    FImage Image;
    for (int32 Variant = 0; Variant < Entries.Num(); ++Variant) {
        FHdueScene::FPreviewTexture& Entry = *Entries[Variant];
        if (Entry.Virtual) {
            FString VirtualError;
            Entry.Pending = PrepareVirtualTexture(File, Entry, LOD, VirtualError);
            if (!Entry.Pending) UE_LOG(LogTemp, Warning, TEXT("hdUE virtual texture %s: %s."), *File, *VirtualError);
            continue;
        }
        const bool SRGB = Entry.SRGB;
        const FString Cache = CachePath(File, Entry, SRGB, LOD);
        if (!Cache.IsEmpty() && (Entry.Pending = ReadCache(Cache, Entry))) continue;
        if (!Image.RawData.Num() && !FImageUtils::LoadImage(*File, Image)) return;
        const bool Last = Variant + 1 == Entries.Num();
        Entry.Pending = PrepareTexture(Last ? MoveTemp(Image) : FImage(Image), Entry, SRGB, LOD);
        if (!Cache.IsEmpty() && IsBlockCompressedFormat(Entry.Pending->PixelFormat)) WriteCache(Cache, *Entry.Pending);
    }
}
const TCHAR* const SlotNames[HDUE_TEXTURE_SLOT_COUNT] = {
    TEXT("BaseColor"),    TEXT("Emissive"),     TEXT("Metallic"),  TEXT("Roughness"), TEXT("Opacity"),
    TEXT("Normal"),       TEXT("Displacement"), TEXT("Occlusion"), TEXT("Clearcoat"), TEXT("ClearcoatRoughness"),
    TEXT("SpecularColor")};
// A Preview Surface parent's input: its texture (scale, bias and channel mask in the texture's stored channels), or
// without one its constant.
void BindTextureSlot(UMaterialInstanceDynamic& Instance, int32 Slot, const HdueTextureDesc& Texture, bool Udim,
                     const FHdueScene::FPreviewTexture* Image, const FLinearColor& Constant) {
    const FString Name = SlotNames[Slot];
    if (!Image) {
        Instance.SetVectorParameterValue(*(Name + TEXT("Scale")), FLinearColor(0, 0, 0, 0));
        Instance.SetVectorParameterValue(*(Name + TEXT("Bias")), Constant);
        return;
    }
    // A repacked texture (BC4/BC5) stores the channels this input reads first.
    const auto Stored = [&](const float* Value) {
        if (Image->Channels.IsEmpty()) return FLinearColor(Value[0], Value[1], Value[2], Value[3]);
        float Packed[4] = {};
        for (int32 Channel = 0; Channel < Image->Channels.Num(); ++Channel)
            Packed[Channel] = Value[Image->Channels[Channel]];
        return FLinearColor(Packed[0], Packed[1], Packed[2], Packed[3]);
    };
    Instance.SetTextureParameterValue(*(Name + TEXT("Texture")), Image->Texture.Get());
    Instance.SetVectorParameterValue(*(Name + TEXT("Scale")), Stored(Texture.scale));
    Instance.SetVectorParameterValue(*(Name + TEXT("Bias")), Stored(Texture.bias));
    Instance.SetVectorParameterValue(*(Name + TEXT("Mask")), Stored(Texture.channel_mask));
    FLinearColor Set(0, 0, 0, 0);
    Set.Component(FMath::Min<uint32>(Texture.uv_set, 3)) = 1;
    Instance.SetVectorParameterValue(*(Name + TEXT("UVSet")), Set);
    // A UDIM set is the whole UV plane's texture: it is not masked outside [0,1].
    if ((Texture.wrap[0] == HDUE_WRAP_BLACK || Texture.wrap[1] == HDUE_WRAP_BLACK) && !Udim)
        Instance.SetVectorParameterValue(*(Name + TEXT("Black")),
                                         FLinearColor(Texture.wrap[0] == HDUE_WRAP_BLACK ? 1.f : 0.f,
                                                      Texture.wrap[1] == HDUE_WRAP_BLACK ? 1.f : 0.f, 0, 0));
}
// Memory a background texture build needs, after UE's estimate for a texture build (TextureBuildUtilities.cpp,
// GetBuildRequiredMemoryEstimate): per pixel the source, a float RGBA mip chain (16 bytes, 4/3 for the mips) and the
// encoder's working memory, about 48 bytes, plus 1 MB. The size comes from the image header (read for the virtual
// texture choice, VirtualMaterial). A UDIM set builds all its tiles at once (PrepareVirtualTexture) and, as UE's
// virtual texture estimate (GetVirtualTextureRequiredMemoryEstimate), counts every tile's source with its output;
// hdUE's build mips in the 8-bit format, without UE's float copy, so a tile pixel takes at most 8 bytes decoded (16-bit
// PNG) and then 4 with its mips (4/3) and encoded tiles: 10 bytes. A tile without a PNG or JPEG header counts 48 bytes
// per byte of file. The flat 48 per file byte estimated door_frame_albedo.<UDIM>.png on NVIDIA's Da Vinci Workshop at
// 4460 MB, and every build queued behind it waited (TickTextures starts them in order, as UE's asset compilation
// queue).
int64 TextureBuildMemory(const FString& File) {
    constexpr int64 BytesPerPixel = 48;
    int64 Pixels = 0;
    if (IsUdim(File)) {
        int64 Memory = 1024 * 1024;
        for (const auto& [Tile, Path] : HdueUdimTiles(File)) {
            const FIntPoint Size = ImageSize(Path);
            Memory += Size.X ? int64(Size.X) * Size.Y * 10
                             : FMath::Max<int64>(0, IFileManager::Get().FileSize(*Path)) * BytesPerPixel;
        }
        return Memory;
    } else {
        const FIntPoint Size = ImageSize(File);
        Pixels = int64(Size.X) * Size.Y;
        if (!Pixels) Pixels = FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
    }
    return Pixels * BytesPerPixel + 1024 * 1024;
}
// A texture entry's build settings, without its texture or data: what a background build works on.
FHdueScene::FPreviewTexture TextureSettings(const FHdueScene::FPreviewTexture& Entry) {
    FHdueScene::FPreviewTexture Settings;
    Settings.Read = Entry.Read;
    Settings.Vector = Entry.Vector;
    Settings.SRGB = Entry.SRGB;
    Settings.Address[0] = Entry.Address[0];
    Settings.Address[1] = Entry.Address[1];
    Settings.Coverage = Entry.Coverage;
    Settings.Virtual = Entry.Virtual;
    return Settings;
}
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
        // A virtual texture streams its tiles from the chunks in VTData, kept in memory (Apply-ResidencyPatch.ps1).
        Created->VirtualTextureStreaming = Texture->Pending->VTData != nullptr;
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
            const FString Package = Instance->Parent->GetPackage()->GetName() + TEXT("OneSided");
            // Asked first: LoadObject of a package that does not exist logs a LogStreaming warning (SkipPackage),
            // LOAD_NoWarn or not, and cooked MaterialX parents have no twin.
            if (auto* Parent = FPackageName::DoesPackageExist(Package)
                                   ? LoadObject<UMaterialInterface>(nullptr, *(Package + TEXT(".") + Name))
                                   : nullptr) {
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
            // A Nanite mesh whose material turned translucent keeps its previous material until the re-upload
            // UpdateMeshes asked for: UE draws only opaque and masked materials with Nanite (NaniteResourcesHelper.h)
            // and finds no fallback mesh to draw instead, so the component had no proxy (StaticMeshComponentHelper.h).
            const auto* Static = Cast<UStaticMeshComponent>(Component);
            if (Static && Static->GetStaticMesh() && Static->GetStaticMesh()->HasValidNaniteData() &&
                IsTranslucentBlendMode(*Desired) && !TranslucentStatic.Contains(Entry.Key))
                continue;
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
// A static mesh LOD's GPU buffers from build vertices (any thread). No CPU copies once the GPU has the buffers, as
// UE's runtime mesh builds (FBuildMeshDescriptionsParams::bAllowCpuAccess false); they were as large as the delegate's
// copy of the scene. Lumen cards read the buffers before InitResources; native picking reads cooked native meshes only.
void InitLOD(FStaticMeshLODResources& LOD, const TArray<FStaticMeshBuildVertex>& Vertices,
             const TArray<uint32>& Indices) {
    const int32 Count = Vertices.Num();
    LOD.VertexBuffers.PositionVertexBuffer.Init(Vertices, false);
    FStaticMeshVertexBufferFlags Flags;
    Flags.bNeedsCPUAccess = false;
    Flags.bUseBackwardsCompatibleF16TruncUVs = false;
    LOD.VertexBuffers.StaticMeshVertexBuffer.Init(Vertices, HdueVertexUVs, Flags);
    if (Vertices.ContainsByPredicate([](const FStaticMeshBuildVertex& V) { return V.Color != FColor::White; }))
        LOD.VertexBuffers.ColorVertexBuffer.Init(Vertices, false);
    else
        LOD.VertexBuffers.ColorVertexBuffer.InitFromColorArray(&FColor::White, Count, 0, false);
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
}
// An array of plain structs as its count and bytes (the Nanite cache's fallback mesh).
template <class T>
void SerializeRaw(FArchive& Ar, TArray<T>& Array) {
    int32 Count = Array.Num();
    Ar << Count;
    if (Ar.IsLoading()) {
        if (Count < 0 || int64(Count) * int64(sizeof(T)) > Ar.TotalSize() - Ar.Tell()) {
            Ar.SetError();
            return;
        }
        Array.SetNumUninitialized(Count);
    }
    Ar.Serialize(Array.GetData(), int64(Count) * sizeof(T));
}
// The Nanite build's fallback mesh as a static mesh's LOD 0, as UE's static mesh build makes it (BuildNanite,
// StaticMeshBuilder.cpp): what draws where Nanite does not, and what ray tracing (Lumen's, the Path Tracer) traces.
TUniquePtr<FStaticMeshRenderData> FallbackRenderData(const TArray<FStaticMeshBuildVertex>& Vertices,
                                                     const TArray<uint32>& Indices) {
    auto Data = MakeUnique<FStaticMeshRenderData>();
    Data->AllocateLODResources(1);
    InitLOD(Data->LODResources[0], Vertices, Indices);
    return Data;
}
// The key under which identical generated meshes share one static mesh (FHdueScene::SharedMeshes): their arrays, and
// whether a translucent material keeps them off Nanite (whose fallback mesh is coarse), which makes another mesh.
FString MeshKey(const HdueMeshDesc& Desc, bool Translucent) {
    FBlake3 Hasher;
    const auto Add = [&](const void* Data, uint64 Size) {
        if (Data) Hasher.Update(Data, Size);
    };
    const uint32 Header[] = {Desc.vertex_count,
                             Desc.index_count,
                             Desc.uv_set_count,
                             uint32(Desc.normals != nullptr),
                             uint32(Desc.colors != nullptr),
                             uint32(Translucent)};
    Add(Header, sizeof(Header));
    Add(Desc.positions, Desc.vertex_count * 12ull);
    Add(Desc.normals, Desc.vertex_count * 12ull);
    Add(Desc.uv, Desc.vertex_count * 8ull * FMath::Max(1u, Desc.uv_set_count));
    Add(Desc.colors, Desc.vertex_count * 12ull);
    Add(Desc.indices, Desc.index_count * 4ull);
    return LexToString(Hasher.Finalize());
}
// Nanite data for one generated mesh (any thread). Its root pages (one per mesh, UE's default minimum residency) stay
// on the GPU; the other pages stay in memory in StreamablePages and stream to the GPU when the view needs them, as the
// editor streams them (UE's non-editor path reads only cooked files: Apply-ResidencyPatch.ps1 adds the memory path).
//
// Fallback receives the builder's fallback mesh, at UE's default relative error (Fallback Target Auto:
// r.Nanite.Builder.FallbackTargetAutoRelativeError above r.Nanite.Builder.FallbackTriangleThreshold). A full-resolution
// LOD 0 and its ray-tracing BLAS took more GPU memory than the Nanite data, and Windows charges the process a commit
// for its GPU allocations: NVIDIA's Da Vinci Workshop added about 200 bytes per triangle and ended at a 38 GB job
// limit.
TSharedPtr<Nanite::FResources> BuildNanite(const HdueMeshDesc& Desc, TUniquePtr<FStaticMeshRenderData>& Fallback) {
    if (!Desc.vertex_count || Desc.index_count < 3) return nullptr;
    const uint32 Triangles = Desc.index_count / 3;
    FMeshNaniteSettings Settings;
    Settings.bEnabled = true;
    Settings.TargetMinimumResidencyInKB = 0;
    // Fallback Target Auto, as Nanite::CorrectFallbackSettings applies it (NaniteHelper.cpp; MeshBuilderCommon does not
    // compile into this game target).
    static const auto* FallbackThreshold =
        IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Nanite.Builder.FallbackTriangleThreshold"));
    static const auto* FallbackError =
        IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.Nanite.Builder.FallbackTargetAutoRelativeError"));
    Settings.FallbackPercentTriangles = 1.f;
    Settings.FallbackRelativeError =
        int32(Triangles) <= FallbackThreshold->GetValueOnAnyThread() ? 0.f : FallbackError->GetValueOnAnyThread();
    // The Nanite build is most of a scene's conversion (450 meshes, 4.4M triangles: 10.5 s of 11.2 s), so builds are
    // kept across runs as UE's derived data cache keeps a static mesh's (FStaticMeshRenderData::Cache, StaticMesh.cpp):
    // keyed by the builder's version and the mesh's content, the resources as Serialize writes them (bCooked false).
    // UE's cache itself is unavailable here: GetCache() creates none where RequiresCookedData() (cooked runtimes,
    // DerivedDataCacheModule.cpp), so this keeps a file per mesh under Saved/NaniteCache.
    FBlake3 Hasher;
    const FString Version = Nanite::IBuilderModule::Get().GetVersionString();
    Hasher.Update(*Version, Version.Len() * sizeof(TCHAR));
    Hasher.Update("fallback key", 12);  // entries without the fallback mesh, or keyed by the input, are not reused
    Hasher.Update(&Settings.FallbackRelativeError, sizeof(float));
    // The builder's input follows from the descriptor alone (HydraVertex), so the descriptor's key stands for it, and a
    // cache hit builds no input.
    const FString Key = MeshKey(Desc, false);
    Hasher.Update(*Key, Key.Len() * sizeof(TCHAR));
    const FString Path =
        FPaths::ProjectSavedDir() / TEXT("NaniteCache") / LexToString(Hasher.Finalize()) + TEXT(".nanite");
    auto Resources = MakeShared<Nanite::FResources>();
    TArray<uint8> Bytes;
    if (FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent)) {
        FMemoryReader Reader(Bytes);  // not persistent: see the writer
        Resources->Serialize(Reader, nullptr, false);
        // The streaming pages follow the resources (see the writer), restored as UE's DDC rebuild restores them.
        int64 Streamable = 0;
        Reader << Streamable;
        if (!Reader.IsError() && Streamable >= 0 && Reader.Tell() + Streamable < Reader.TotalSize()) {
            if (Streamable) {
                Resources->StreamablePages.Lock(LOCK_READ_WRITE);
                Reader.Serialize(Resources->StreamablePages.Realloc(Streamable), Streamable);
                Resources->StreamablePages.Unlock();
                Resources->StreamablePages.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
            }
            TArray<FStaticMeshBuildVertex> Vertices;
            TArray<uint32> Indices;
            SerializeRaw(Reader, Vertices);
            SerializeRaw(Reader, Indices);
            if (!Reader.IsError() && Reader.AtEnd() && Indices.Num() >= 3) {
                Fallback = FallbackRenderData(Vertices, Indices);
                IFileManager::Get().SetTimeStamp(*Path, FDateTime::UtcNow());  // a hit counts as a use (eviction)
                return Resources;
            }
        }
        *Resources = Nanite::FResources();
    }
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
    Nanite::IBuilderModule::FOutputMeshData Output;
    if (!Nanite::IBuilderModule::Get().Build(*Resources, Input, &Output, nullptr, nullptr, Settings) ||
        Output.TriangleIndices.Num() < 3)
        return nullptr;
    TArray<FStaticMeshBuildVertex> Vertices;
    Vertices.SetNum(Output.Vertices.Position.Num());
    const int32 OutputSets = Output.Vertices.UVs.Num();
    for (int32 V = 0; V < Vertices.Num(); ++V) {
        FStaticMeshBuildVertex& Vertex = Vertices[V];
        Vertex.Position = Output.Vertices.Position[V];
        Vertex.TangentX = Output.Vertices.TangentX[V];
        Vertex.TangentY = Output.Vertices.TangentY[V];
        Vertex.TangentZ = Output.Vertices.TangentZ[V];
        for (int32 Set = 0; Set < HdueVertexUVs; ++Set)  // the last set repeated, as HydraVertex fills them
            Vertex.UVs[Set] =
                OutputSets ? Output.Vertices.UVs[FMath::Min(Set, OutputSets - 1)][V] : FVector2f::ZeroVector;
        Vertex.Color = Output.Vertices.Color.IsValidIndex(V) ? Output.Vertices.Color[V] : FColor::White;
    }
    Fallback = FallbackRenderData(Vertices, Output.TriangleIndices);
    // Runtime builds cannot save bulk data (USE_RUNTIME_BULKDATA, BulkData.cpp) but still read its header; a
    // non-persistent archive skips it both ways, so the streaming pages are appended raw after the resources.
    // Identical meshes share a key: one written by another task is kept as it is.
    if (IFileManager::Get().FileExists(*Path)) return Resources;
    Bytes.Reset();
    FMemoryWriter Writer(Bytes);
    Resources->Serialize(Writer, nullptr, false);
    int64 Streamable = Resources->StreamablePages.GetBulkDataSize();
    Writer << Streamable;
    if (Streamable) {
        Writer.Serialize(const_cast<void*>(Resources->StreamablePages.LockReadOnly()), Streamable);
        Resources->StreamablePages.Unlock();
    }
    SerializeRaw(Writer, Vertices);  // the fallback mesh follows the pages
    SerializeRaw(Writer, Output.TriangleIndices);
    // Another task may build the same mesh: write a private file, then move it into place, once (UE's Move retries for
    // seconds while the other task's file is in the way).
    const FString Temporary = FPaths::CreateTempFilename(*FPaths::GetPath(Path), TEXT("nanite"));
    if (FFileHelper::SaveArrayToFile(Bytes, *Temporary) &&
        !IFileManager::Get().Move(*Path, *Temporary, true, false, false, true))
        IFileManager::Get().Delete(*Temporary, false, false, true);
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
// With Nanite data, the Nanite build's fallback mesh (BuildNanite) replaces the full-resolution LOD 0, keeping its
// cards.
void FinishStaticMesh(UStaticMesh* Built, TUniquePtr<FStaticMeshRenderData> RenderData, const FBoxSphereBounds& Bounds,
                      Nanite::FResources* Nanite, TUniquePtr<FStaticMeshRenderData> Fallback) {
    if (Nanite && Fallback) {
        Swap(Fallback->LODResources[0].CardRepresentationData, RenderData->LODResources[0].CardRepresentationData);
        RenderData = MoveTemp(Fallback);
    }
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
    }
    // UV tangents (~4 us per triangle) come with the cards: interactive hosts show the first build sooner and get them
    // with the background rebuild.
    if (Cards && Desc.uv) HydraTangents(Vertices, Indices);
    FConversion Result{MakeUnique<FStaticMeshRenderData>(), FBoxSphereBounds(FBox(Bounds)), nullptr};
    Result.RenderData->AllocateLODResources(1);
    if (Indices.IsEmpty()) return Result;
    FStaticMeshLODResources& LOD = Result.RenderData->LODResources[0];
    InitLOD(LOD, Vertices, Indices);
    if (Cards) LOD.CardRepresentationData = HdueMeshCards(LOD, Result.Bounds);
    return Result;
}

// Replaces one static mesh queued by Apply with its version with Lumen cards (and Nanite data) once that build has
// finished. Both must be attached before a mesh's GPU resources exist, so that version is a second static mesh.
// Interactive hosts' first-pass meshes (Apply): their estimated GPU memory, in MB.
static TAutoConsoleVariable<int32> CVarFirstPassMemory(
    TEXT("r.HdUE.InteractiveFirstPassMemory"), 2048,
    TEXT("Interactive hosts: estimated GPU memory (MB) of the full-resolution meshes drawn while their Lumen cards and "
         "Nanite data build in the background; meshes beyond it appear when their build is done. 0: none."));
// UE's Editor.AsyncAssetCompilationMaxMemoryUsage for hdUE's background mesh builds (UE's applies to the editor only).
static TAutoConsoleVariable<int32> CVarMeshBuildMaxMemoryUsage(
    TEXT("r.HdUE.MeshBuildMaxMemoryUsage"), 0,
    TEXT("Interactive hosts: 0 - background mesh and texture builds start while system memory is available; N - keep "
         "their estimated memory under N GB."));

// Interactive hosts: binds background texture builds (Apply) into the inputs waiting for them, and into those inputs'
// one-sided twins and wet surface copies, which copied the material before the texture was there. UE restarts path
// tracing on the parameter change (UMaterialInstance's parameter update). At most 8 ms of it per tick.
void FHdueScene::TickTextures() {
    if (TextureBuilds.IsEmpty()) return;
    // Queued builds start while their memory fits, as the mesh builds do (TickConversions); one always runs. Launching
    // every file at once, beside the mesh builds, ended NVIDIA's Da Vinci Workshop at a 38 GB job memory limit.
    int64 Available = AvailableBuildMemory();
    TextureBuilds.RemoveAll([&](const TSharedPtr<FTextureBuild>& Build) {
        if (Build->Task.IsValid() || Build->Epoch == TextureEpoch) return false;
        ++GHdueLoad.TexturesDone;  // queued before a scene reset: dropped, no longer waited for
        return true;
    });
    for (const TSharedPtr<FTextureBuild>& Build : TextureBuilds) {
        if (Build->Task.IsValid()) continue;
        if (ReservedMemory && Build->Memory > Available) break;
        Available -= Build->Memory;
        ReservedMemory += Build->Memory;
        Build->Task =
            UE::Tasks::Launch(TEXT("HdueTexture"), MoveTemp(Build->Work), UE::Tasks::ETaskPriority::BackgroundNormal);
        Build->Launched = FPlatformTime::Seconds();
    }
    const double Start = FPlatformTime::Seconds();
    TMultiMap<const UMaterialInterface*, UMaterialInstanceDynamic*> Copies;
    bool CopiesGathered = false;
    for (int32 Index = 0; Index < TextureBuilds.Num() && FPlatformTime::Seconds() - Start < 0.008; ++Index) {
        const TSharedPtr<FTextureBuild> Build = TextureBuilds[Index];
        if (!Build->Task.IsValid() || !Build->Task.IsCompleted()) continue;  // queued, or still building
        TextureBuilds.RemoveAt(Index--);
        ReservedMemory -= Build->Memory;
        if (Build->Epoch != TextureEpoch) continue;  // the scene was reset meanwhile
        for (int32 Variant = 0; Variant < Build->Keys.Num(); ++Variant) {
            const FString& Key = Build->Keys[Variant];
            FPreviewTexture& Built = Build->Entries[Variant];
            if (int32* Count = PendingTextures.Find(Key); Count && --*Count <= 0) PendingTextures.Remove(Key);
            FPreviewTexture* Entry = PreviewTextures.Find(Key);
            // A later batch's inputs read channels this build left out: its own build replaces this one.
            if (!Entry || (Built.Read | Entry->Read) != Built.Read || (Entry->Vector && !Built.Vector)) continue;
            if (Built.Pending) {
                Entry->Channels = Built.Channels;
                Entry->Pending = MoveTemp(Built.Pending);
            }
            if (PendingTextures.Contains(Key)) continue;
            FString Error;
            const FPreviewTexture* Image = PreviewTexture(Key, Error);
            TArray<FTextureSlot> Slots;
            TextureSlots.MultiFind(Key, Slots);
            TextureSlots.Remove(Key);
            if (!Image) {
                UE_LOG(LogTemp, Warning, TEXT("hdUE: %s; %d inputs stay untextured."), *Error, Slots.Num());
                continue;
            }
            if (!CopiesGathered) {
                CopiesGathered = true;
                for (const auto& [Id, Twin] : OneSided)
                    if (auto* Copy = Cast<UMaterialInstanceDynamic>(Twin.Material.Get())) Copies.Add(Twin.Base, Copy);
                for (const auto& [Id, Wet] : WetBindings)
                    if (Wet->Material) Copies.Add(Wet->Base, Wet->Material.Get());
            }
            for (const FTextureSlot& Slot : Slots) {
                UMaterialInstanceDynamic* Instance = Slot.Instance.Get();
                if (!Instance) continue;
                BindTextureSlot(*Instance, Slot.Slot, Slot.Texture, Slot.Udim, Image, FLinearColor());
                TArray<UMaterialInstanceDynamic*> Dependent;
                Copies.MultiFind(Instance, Dependent);
                for (UMaterialInstanceDynamic* Copy : Dependent)
                    BindTextureSlot(*Copy, Slot.Slot, Slot.Texture, Slot.Udim, Image, FLinearColor());
            }
        }
    }
}
FString FHdueScene::DescribePending() const {
    int32 Queued = 0, Running = 0;
    for (const auto& [Key, Pending] : PendingStatic) ++(Pending.Task.IsValid() ? Running : Queued);
    FString Out = FString::Printf(TEXT("%d mesh builds (%d running), %d textures"), PendingStatic.Num(), Running,
                                  PendingTextures.Num());
    const double Now = FPlatformTime::Seconds();
    int32 Listed = 0;
    for (const auto& Build : TextureBuilds) {
        if (Listed++ == 8) {
            Out += TEXT(", ...");
            break;
        }
        Out += Build->Task.IsValid()
                   ? FString::Printf(TEXT("; %s running %.0f s"), *Build->File, Now - Build->Launched)
                   : FString::Printf(TEXT("; %s queued (%lld MB)"), *Build->File, Build->Memory >> 20);
    }
    return Out + FString::Printf(TEXT("; %lld MB reserved, %lld MB available"), ReservedMemory >> 20,
                                 AvailableBuildMemory() >> 20);
}
// Memory for more background builds (meshes and textures): the available physical and virtual memory less 64 MB and
// what started builds reserve, within r.HdUE.MeshBuildMaxMemoryUsage.
int64 FHdueScene::AvailableBuildMemory() const {
    const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
    int64 Available = int64(FMath::Min(Stats.AvailablePhysical, Stats.AvailableVirtual)) - 64 * 1024 * 1024;
    Available -= ReservedMemory;
    if (const int64 Limit = int64(CVarMeshBuildMaxMemoryUsage.GetValueOnGameThread()) << 30)
        Available = FMath::Min(Available, Limit - ReservedMemory);
    return Available;
}
void FHdueScene::TickConversions() {
    TickTextures();
    // Builds start while their memory fits, as UE's editor schedules asset builds (FMemoryBoundQueuedThreadPoolWrapper,
    // AssetCompilingManager.cpp, editor-only): an estimate of 7 times the source mesh (UStaticMesh::
    // GetBuildRequiredMemoryEstimate, StaticMesh.cpp) against the available physical and virtual memory less 64 MB,
    // with builds already started (or finished and waiting to be swapped in) holding theirs.
    ConversionsBusy |= PendingStatic.Num() || RetiredBuilds.Num();
    RetiredBuilds.RemoveAll([&](const FPendingStatic& Retired) {
        if (!Retired.Task.IsCompleted()) return false;
        ReservedMemory -= Retired.Memory;
        return true;
    });
    if (PendingStatic.Num()) {
        int64 Available = AvailableBuildMemory();
        for (auto& [Key, Pending] : PendingStatic) {
            if (Pending.Task.IsValid()) continue;
            if (ReservedMemory && Pending.Memory > Available) break;  // one build always runs
            Available -= Pending.Memory;
            ReservedMemory += Pending.Memory;
            Pending.Task = UE::Tasks::Launch(
                TEXT("HdueConversion"),
                [Source = MoveTemp(Pending.Source)]() -> TSharedPtr<FConversion> {
                    auto Result = MakeShared<FConversion>(BuildStatic(Source->Desc, true));
                    if (Source->Nanite) Result->Nanite = BuildNanite(Source->Desc, Result->Fallback);
                    return Result;
                },
                UE::Tasks::ETaskPriority::BackgroundLow);
        }
    }
    for (auto It = PendingStatic.CreateIterator(); It; ++It) {
        if (!It.Value().Task.IsValid() || !It.Value().Task.IsCompleted()) continue;  // queued or not built yet
        ReservedMemory -= It.Value().Memory;
        QuickMemory -= It.Value().QuickMemory;  // released with the first-pass mesh it replaces
        // Every mesh of the key draws the one converted mesh, as offline hosts share it (Apply).
        TArray<UStaticMeshComponent*> Components;
        for (const FString& Id : It.Value().Ids) {
            PendingKeys.Remove(Id);
            AActor* Actor = Actors.FindRef(Id);
            if (auto* Component = IsValid(Actor) ? Cast<UStaticMeshComponent>(Actor->GetRootComponent()) : nullptr)
                Components.Add(Component);
        }
        const TSharedPtr<FConversion> Conversion = It.Value().Task.GetResult();
        ++GHdueLoad.MeshBuildsDone;
        if (!Components.Num()) {
            It.RemoveCurrent();
            continue;
        }
        const FString& Key = It.Key();
        // Nanite draws opaque and masked materials only.
        const bool Nanite =
            NaniteFits(Key, Conversion->Nanite && !It.Value().Translucent ? Conversion->Nanite.Get() : nullptr);
        UStaticMesh* Built = NewStaticMesh(GetTransientPackage());
        FinishStaticMesh(Built, MoveTemp(Conversion->RenderData), Conversion->Bounds,
                         Nanite ? Conversion->Nanite.Get() : nullptr, MoveTemp(Conversion->Fallback));
        SharedMeshes.Add(Key, Built);
        static int32 NaniteMeshes = 0, Tried = 0;
        ++Tried;
        if (Built->HasValidNaniteData()) ++NaniteMeshes;
        if (Tried % 50 == 1 || PendingStatic.Num() == 1)
            UE_LOG(LogTemp, Log, TEXT("hdUE Nanite: %d of %d converted meshes have Nanite data"), NaniteMeshes, Tried);
        for (UStaticMeshComponent* Component : Components) Component->SetStaticMesh(Built);
        ++SwappedBuilds;
        SwappedMeshes += Components.Num();
        It.RemoveCurrent();
        return;  // one per tick keeps frames smooth
    }
    if (!ConversionsBusy || PendingStatic.Num() || RetiredBuilds.Num()) return;
    // All builds done: every generated static mesh should now draw its key's converted mesh (SharedMeshes holds only
    // those here), and no memory stays reserved.
    ConversionsBusy = false;
    TSet<const UStaticMesh*> Converted;
    for (const auto& [Key, Mesh] : SharedMeshes) Converted.Add(Mesh.Get());
    int32 Generated = 0, Unconverted = 0;
    for (const auto& [Id, Actor] : Actors) {
        const auto* Component = IsValid(Actor) ? Cast<UStaticMeshComponent>(Actor->GetRootComponent()) : nullptr;
        const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
        if (!Mesh || Mesh->GetOuter() != GetTransientPackage()) continue;  // a native asset
        ++Generated;
        Unconverted += !Converted.Contains(Mesh);
    }
    const FString Summary = FString::Printf(
        TEXT("hdUE conversions done: %d builds swapped into %d meshes, %d dropped and %d shrunk by edits while they "
             "ran, %d dropped and %d shrunk while queued; %d of %d generated meshes unconverted; %lld bytes reserved"),
        SwappedBuilds, SwappedMeshes, DroppedBuilds, ShrunkBuilds, QueuedDropped, QueuedShrunk, Unconverted, Generated,
        ReservedMemory);
    if (Unconverted || ReservedMemory) {
        UE_LOG(LogTemp, Warning, TEXT("%s"), *Summary);
    } else {
        UE_LOG(LogTemp, Log, TEXT("%s"), *Summary);
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
        (Batch.removed_count && !Batch.removed_ids)) {
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
        MeshMaterials.Empty();
        SingleSided.Empty();
        OneSided.Empty();
        Materials.Empty();
        PreviewTextures.Empty();
        MaterialTextures.Empty();
        // Builds still running finish unused (TickTextures drops them by their epoch).
        ++TextureEpoch;
        PendingTextures.Empty();
        TextureSlots.Empty();
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
        // This batch's new textures, built on workers (below); PreviewTexture creates the UTexture2Ds on this thread.
        // A texture is rebuilt when inputs read channels its BC4/BC5 repacking left out.
        TMap<FString, TArray<FString>> Files;  // each file's variants to build (sRGB decoding, wrap), by key
        for (uint32 Index = 0; Index < Batch.material_count; ++Index) {
            const auto& Desc = Batch.materials[Index];
            TArray<FString>* Used = Desc.id ? &MaterialTextures.FindOrAdd(UTF8_TO_TCHAR(Desc.id)) : nullptr;
            if (Used) Used->Reset();
            if (Desc.native_asset && *Desc.native_asset) continue;
            const bool Virtual = VirtualMaterial(Desc);
            for (int32 Slot = 0; Slot < HDUE_TEXTURE_SLOT_COUNT; ++Slot) {
                const auto& Texture = Desc.textures[Slot];
                if (!Texture.file || !*Texture.file) continue;
                const FString File = UTF8_TO_TCHAR(Texture.file),
                              Key = TextureKey(File, Texture, Slot, CoverageThreshold(Desc, Slot), Virtual);
                const bool Vector = !ScalarSlot(Slot);
                uint8 Read = 0;
                for (int32 Channel = 0; Channel < 4; ++Channel)
                    if (!Vector && Texture.channel_mask[Channel] != 0.f) Read |= 1 << Channel;
                if (Used) Used->AddUnique(Key);
                FPreviewTexture& Entry = PreviewTextures.FindOrAdd(Key);
                if ((Entry.Texture || PendingTextures.Contains(Key)) && (Entry.Read | Read) == Entry.Read &&
                    (Entry.Vector || !Vector))
                    continue;
                Entry.Read |= Read;
                Entry.Vector |= Vector;
                Entry.SRGB = TextureSRGB(Texture, Slot);
                Entry.Coverage = CoverageThreshold(Desc, Slot);
                Entry.Virtual = Virtual;
                for (int32 Axis = 0; Axis < 2; ++Axis) Entry.Address[Axis] = uint8(TextureWrap(Texture, Axis));
                Files.FindOrAdd(File).AddUnique(Key);
            }
        }
        // Textures no material uses any more go (look-dev cycling through images kept every one resident); one a
        // material still binds stays alive through it. Not those a build or a slot still waits for.
        {
            TSet<FString> Bound;
            for (const auto& [Id, Keys] : MaterialTextures) Bound.Append(Keys);
            for (auto It = PreviewTextures.CreateIterator(); It; ++It)
                if (!Bound.Contains(It.Key()) && !PendingTextures.Contains(It.Key()) &&
                    !TextureSlots.Contains(It.Key()))
                    It.RemoveCurrent();
        }
        if (Files.Num()) {
            const double PrepareStart = FPlatformTime::Seconds();
            const TArray<TPair<FString, TArray<FString>>> Work = Files.Array();
            const FCookLODBias LOD = FCookLODBias::Current();
            GHdueLoad.TexturesTotal += uint32(Work.Num());
            if (Interactive) {
                // Interactive hosts build each file in the background, as UE's editor compiles textures
                // asynchronously (FTextureCompilingManager) and draws a default meanwhile: building them here held
                // Apply, and the Hydra Sync waiting on it held Houdini's UI, for 208 s on NVIDIA's Da Vinci Workshop
                // with an empty virtual texture cache. Their inputs draw the material's constants until TickTextures
                // binds each texture in.
                // They queue until their memory fits (TickTextures), with the background mesh builds.
                for (const auto& [File, Keys] : Work) {
                    auto Build = MakeShared<FTextureBuild>();
                    Build->File = File;
                    Build->Keys = Keys;
                    Build->Epoch = TextureEpoch;
                    Build->Memory = TextureBuildMemory(File);
                    for (const FString& Key : Keys) {
                        Build->Entries.Add(TextureSettings(PreviewTextures[Key]));
                        ++PendingTextures.FindOrAdd(Key);
                    }
                    // The build outlives its task: TextureBuilds holds it until TickTextures sees the task done, and
                    // the destructor waits for running tasks.
                    Build->Work = [Raw = &Build.Get(), LOD] {
                        TArray<FPreviewTexture*> Entries;
                        for (FPreviewTexture& Entry : Raw->Entries) Entries.Add(&Entry);
                        BuildTextureFile(Raw->File, Entries, LOD);
                    };
                    TextureBuilds.Add(MoveTemp(Build));
                }
            } else {
                // Decode, mip and block-compress this batch's new textures on all cores: decoding them one by one on
                // this thread took most of a large scene's load (111 Sponza textures, ~19 s), and uncompressed they
                // held ~9 GB.
                ParallelFor(Work.Num(), [&](int32 Index) {
                    TArray<FPreviewTexture*> Entries;
                    for (const FString& Key : Work[Index].Value) Entries.Add(&PreviewTextures[Key]);
                    BuildTextureFile(Work[Index].Key, Entries, LOD);
                });
            }
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
            // MaterialX transmission refracts on the translucent Standard Surface parent, as UE's Interchange imports
            // it (Substrate-StandardSurface-Translucent, coloured transmittance, Refraction from its IOR output).
            const bool Transmissive = !Masked && Desc.transmission > 0.f;
            const bool Translucent = Transmissive || (!Masked && (Opacity < 1.f || HasTexture(HDUE_TEXTURE_OPACITY)));
            // MaterialX subsurface and sheen, and clear coats, need the layered Standard Surface parents (UE's
            // Substrate standard_surface: a coat slab vertically layered over the base, several slabs per pixel);
            // everything else stays on the single-slab Preview Surface parents. The specular workflow and see-through
            // materials keep the base slab's second specular lobe (haziness) as their coat.
            const bool Coat = (Desc.clearcoat > 0.f || HasTexture(HDUE_TEXTURE_CLEARCOAT)) &&
                              !(Desc.flags & HDUE_MATERIAL_SPECULAR_WORKFLOW);
            const bool Layered = Transmissive || (!Translucent && (Desc.subsurface > 0.f || Desc.sheen > 0.f || Coat));
            const bool Thin = (Desc.flags & HDUE_MATERIAL_THIN_WALLED) != 0;
            const bool Virtual = VirtualMaterial(Desc);
            const FString ParentName =
                FString(Transmissive
                            ? FString::Printf(TEXT("M_StandardSurfaceTranslucent%s"), Thin ? TEXT("Thin") : TEXT(""))
                        : Layered ? FString::Printf(TEXT("M_StandardSurface%s%s"), Masked ? TEXT("Masked") : TEXT(""),
                                                    Thin ? TEXT("Thin") : TEXT(""))
                        : Masked ? TEXT("M_PreviewSurfaceMasked")
                        : Translucent ? TEXT("M_PreviewSurfaceTranslucent")
                                      : TEXT("M_PreviewSurface")) +
                (Virtual ? TEXT("VT") : TEXT(""));  // virtual samplers (VirtualMaterial)
            auto* Parent = LoadObject<UMaterialInterface>(
                nullptr, *FString::Printf(TEXT("/Game/HdUE/%s.%s"), *ParentName, *ParentName));
            if (Parent) {
                auto* Instance = UMaterialInstanceDynamic::Create(Parent, World);
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
                    // A texture that cannot be read leaves its input untextured, with a warning, instead of failing
                    // every material of the batch. ponytail: the input's constant, not UsdUVTexture's fallback; carry
                    // it if needed. One still building (Interactive) draws the constant until TickTextures binds it.
                    const FPreviewTexture* Image = nullptr;
                    const bool Udim = HasTexture(Slot) && IsUdim(UTF8_TO_TCHAR(Texture.file));
                    if (HasTexture(Slot)) {
                        const FString Key = TextureKey(UTF8_TO_TCHAR(Texture.file), Texture, Slot,
                                                       CoverageThreshold(Desc, Slot), Virtual);
                        FString TextureError;
                        if (PendingTextures.Contains(Key)) {
                            FTextureSlot& Waiting = TextureSlots.Add(Key);
                            Waiting.Instance = Instance;
                            Waiting.Slot = Slot;
                            Waiting.Texture = Texture;
                            Waiting.Texture.file = nullptr;  // the batch's strings do not outlive Apply
                            Waiting.Udim = Udim;
                        } else if (!(Image = PreviewTexture(Key, TextureError)))
                            UE_LOG(LogTemp, Warning, TEXT("hdUE material %s: %s; %s stays untextured."),
                                   UTF8_TO_TCHAR(Desc.id), *TextureError, SlotNames[Slot]);
                    }
                    BindTextureSlot(*Instance, Slot, Texture, Udim, Image, Constants[Slot]);
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
                    if (Transmissive) {
                        Instance->SetScalarParameterValue(TEXT("Transmission"), Desc.transmission);
                        Instance->SetVectorParameterValue(TEXT("TransmissionColor"), Color(Desc.transmission_color));
                        Instance->SetScalarParameterValue(TEXT("TransmissionDepth"), Desc.transmission_depth);
                    }
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
    // Identical generated meshes share one static mesh, as UE's USD importer shares one asset between identical prims
    // (UUsdAssetCache3, keyed by a hash of the mesh: USDGeomMeshTranslator.cpp): NVIDIA's Da Vinci Workshop references
    // the same assets thousands of times without USD instancing, and each copy held its own render data, Nanite pages
    // and ray-tracing geometry. Keys are hashed on all cores; the first mesh of a key in the batch builds it, the
    // others (and later batches, while it lives) reuse it. Interactive hosts run one background conversion per key
    // (PendingStatic), whose first pass every mesh of the key draws meanwhile, and which TickConversions swaps into
    // every mesh of the key.
    for (auto It = SharedMeshes.CreateIterator(); It; ++It)
        if (!It.Value().IsValid()) {
            NaniteFits(It.Key(), nullptr);  // its root pages are back once UE has destroyed it
            It.RemoveCurrent();
        }
    TArray<FString> Keys;
    TArray<bool> Translucent, Build;  // Build: the first mesh of a key no live static mesh has
    Keys.SetNum(Batch.mesh_count);
    Translucent.Init(false, Batch.mesh_count);
    Build.Init(false, Batch.mesh_count);
    ParallelFor(Batch.mesh_count, [&](int32 Index) {
        const auto& Desc = Batch.meshes[Index];
        if (!ValidMeshes[Index] || !StaticGenerated(Desc)) return;
        const auto* Material = Desc.material_id ? Materials.Find(UTF8_TO_TCHAR(Desc.material_id)) : nullptr;
        Translucent[Index] = Material && IsTranslucentBlendMode(*Material->Get());
        Keys[Index] = MeshKey(Desc, Translucent[Index]);
    });
    {
        TSet<FString> Claimed;
        for (int32 Index = 0; Index < Keys.Num(); ++Index) {
            // A key whose background build is pending (interactive) keeps its first pass for later batches too.
            bool Seen =
                Keys[Index].IsEmpty() || SharedMeshes.Contains(Keys[Index]) || PendingStatic.Contains(Keys[Index]);
            if (!Seen) Claimed.Add(Keys[Index], &Seen);
            Build[Index] = !Seen;
        }
    }
    // Interactive hosts draw a first-pass static mesh (no cards, no Nanite) while a key's background build runs,
    // smallest meshes first, while the first passes' estimated GPU memory stays under
    // r.HdUE.InteractiveFirstPassMemory. The others draw nothing until their build swaps in, as UE draws no static mesh
    // while it compiles (UStaticMeshComponent::ShouldCreateRenderState, StaticMeshComponent.cpp). Full-resolution first
    // passes of NVIDIA's Da Vinci Workshop (553M vertices) needed more than the GPU has, and Windows charges D3D12
    // allocations to the process's commit.
    TArray<int64> FirstPass;  // estimated bytes of the meshes that get one
    FirstPass.Init(0, Batch.mesh_count);
    if (Interactive) {
        TArray<int32> Smallest;
        for (int32 Index = 0; Index < Build.Num(); ++Index)
            if (Build[Index]) Smallest.Add(Index);
        Smallest.Sort([&](int32 A, int32 B) { return Batch.meshes[A].index_count < Batch.meshes[B].index_count; });
        const int64 Budget = int64(CVarFirstPassMemory.GetValueOnGameThread()) << 20;
        for (const int32 Index : Smallest) {
            const auto& Desc = Batch.meshes[Index];
            // Per vertex: position, tangents, 4 half-float UVs and colour (BuildStatic: a vertex per corner without
            // normals), plus 32-bit indices; doubled for the ray-tracing geometry and upload copies. Plus one: nonzero
            // marks the meshes that get one, empty ones included.
            const int64 Bytes =
                2 * (40 * int64(Desc.normals ? Desc.vertex_count : Desc.index_count) + 4 * int64(Desc.index_count)) + 1;
            if (QuickMemory + Bytes > Budget) break;  // sorted: the larger ones do not fit either
            QuickMemory += Bytes;
            FirstPass[Index] = Bytes;
        }
    }
    TMap<FString, UStaticMesh*> BatchBuilt;  // static meshes built in this batch, for its other meshes of the key
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
            if (!ValidMeshes[Index] || (Desc.native_asset && *Desc.native_asset) ||
                (!Keys[Index].IsEmpty() && !Build[Index]))
                return;
            if (StaticGenerated(Desc)) {
                if (Interactive && !FirstPass[Index]) return;
                StaticData[Index] = BuildStatic(Desc, !Interactive);
                if (NaniteMeshes && !Interactive && !Translucent[Index] &&  // Nanite draws opaque and masked only
                    !(Desc.flags & HDUE_MESH_NO_NANITE))
                    StaticData[Index].Nanite = BuildNanite(Desc, StaticData[Index].Fallback);
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
            // The same mesh in another form (a static mesh made wet uploads dynamic): its geometry revision goes on, as
            // the delegate's does, or wet bindings for this upload are rejected as obsolete.
            const uint64 Revision = GeometryRevisions.FindRef(Id);
            Remove(Id);
            if (Revision) GeometryRevisions.Add(Id, Revision);
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
            UE_LOG(LogTemp, Log, TEXT("hdUE native mesh %s: NaniteData=%d"), *Mesh->GetPathName(),
                   Mesh->HasValidNaniteData());
            Actor->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Mesh);
        } else {
            // A background build for the previous geometry finishes unused; one for the same key goes on.
            if (!Static || PendingKeys.FindRef(Id) != Keys[Index]) ForgetPending(Id);
            if (Static) {
                const FString& Key = Keys[Index];
                if (Translucent[Index])
                    TranslucentStatic.Add(Id);
                else
                    TranslucentStatic.Remove(Id);
                UStaticMesh* Built = BatchBuilt.FindRef(Key);
                FPendingStatic* Pending = Interactive ? PendingStatic.Find(Key) : nullptr;
                if (!Built && Pending)
                    Built = Pending->Quick.Get();  // the key's build is queued or running: its first pass, if any
                else if (!Built && !Build[Index])
                    Built = SharedMeshes.FindRef(Key).Get();  // a finished shared mesh
                if (!Built && Build[Index] && (!Interactive || FirstPass[Index])) {
                    // Static meshes belong to no actor: the components that draw them keep them alive.
                    Built = NewStaticMesh(GetTransientPackage());
                    // Nanite draws opaque and masked materials only.
                    const bool Nanite =
                        NaniteFits(Key, StaticData[Index].Nanite && !Translucent[Index] ? StaticData[Index].Nanite.Get()
                                                                                        : nullptr);
                    FinishStaticMesh(Built, MoveTemp(StaticData[Index].RenderData), StaticData[Index].Bounds,
                                     Nanite ? StaticData[Index].Nanite.Get() : nullptr,
                                     MoveTemp(StaticData[Index].Fallback));
                    BatchBuilt.Add(Key, Built);
                    if (!Interactive) SharedMeshes.Add(Key, Built);
                }
                if (Instanced)
                    InstanceMesh = Built;
                else
                    Actor->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Built);
                if (Pending) {
                    Pending->Ids.Add(Id);  // the key's build swaps in here too
                    PendingKeys.Add(Id, Key);
                } else if (Interactive && Build[Index]) {
                    // Cards, Nanite data and the converted mesh's render data are built on background workers, from a
                    // copy of the descriptor's arrays: done here, Nanite doubled the time to the first frame (and
                    // normal-priority Nanite tasks starved its rendering for 15 s). TickConversions starts them.
                    auto Copy = MakeShared<FNaniteSource>(Desc);
                    Copy->Nanite = NaniteMeshes && !Translucent[Index] && !(Desc.flags & HDUE_MESH_NO_NANITE);
                    const int64 Source = Copy->Positions.GetAllocatedSize() + Copy->Normals.GetAllocatedSize() +
                                         Copy->UV.GetAllocatedSize() + Copy->Colors.GetAllocatedSize() +
                                         Copy->Indices.GetAllocatedSize();
                    PendingStatic.Add(Key, {Copy, 7 * Source, {}, Translucent[Index], {Id}, Built, FirstPass[Index]});
                    ++GHdueLoad.MeshBuildsLaunched;
                    PendingKeys.Add(Id, Key);
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
    const double MeshesDone = FPlatformTime::Seconds();
    BindMaterials();
    if (Batch.material_count || Batch.mesh_count)
        UE_LOG(LogTemp, Log,
               TEXT("hdUE apply: %u materials %.0f ms (%d textures: prepare %.0f ms, create %.0f ms), %u meshes %.0f "
                    "ms, bind %.0f ms"),
               Batch.material_count, (MaterialsDone - ApplyStart) * 1000, TextureImports, PrepareSeconds * 1000,
               TextureSeconds * 1000, Batch.mesh_count, (MeshesDone - MaterialsDone) * 1000,
               (FPlatformTime::Seconds() - MeshesDone) * 1000);
    return true;
}
void FHdueScene::UpdateCameraLights(const FVector& Eye, const FVector& Target, const FVector& Up) {
    Lighting->UpdateCamera(Eye, Target, Up);
}
