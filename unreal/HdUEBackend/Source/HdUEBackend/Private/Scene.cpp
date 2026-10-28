#include "Scene.h"
#include "Lighting.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
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
bool FHdueScene::UpsertVolume(const HdueVolumeDesc& Desc, FString& Error) {
    const FString Id = UTF8_TO_TCHAR(Desc.id);
    const uint64 Count = uint64(Desc.width) * Desc.height * Desc.depth;
    auto* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/HdUE/M_HdUEVolume.M_HdUEVolume"));
    if (!Parent) { Error = TEXT("Volume material /Game/HdUE/M_HdUEVolume is missing; run Prepare-Content.ps1 and stage the runtime."); return false; }
    FVolume& Volume = Volumes.FindOrAdd(Id);
    if (!IsValid(Volume.Actor)) {
        Volume.Actor = World->SpawnActor<AActor>();
        auto* Component = NewObject<UHeterogeneousVolumeComponent>(Volume.Actor);
        Component->SetMobility(EComponentMobility::Movable);
        Volume.Actor->SetRootComponent(Component); Volume.Actor->AddInstanceComponent(Component);
        Component->RegisterComponent();
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
UTexture2D* FHdueScene::PreviewTexture(const FString& File, bool SRGB, FString& Error) {
    const FString Key = File + (SRGB ? TEXT("|srgb") : TEXT("|raw"));
    if (const auto* Cached = PreviewTextures.Find(Key)) return Cached->Get();
    const double ImportStart = FPlatformTime::Seconds();
    // Images decoded in parallel by Apply; files outside a batch (lights) decode here.
    UTexture2D* Texture = nullptr;
    if (FImage* Decoded = DecodedImages.Find(File)) Texture = FImageUtils::CreateTexture2DFromImage(*Decoded);
    else Texture = FImageUtils::ImportFileAsTexture2D(File);
    TextureSeconds += FPlatformTime::Seconds() - ImportStart; ++TextureImports;
    if (!Texture) { Error = FString::Printf(TEXT("Failed to import texture %s."), *File); return nullptr; }
    // Float images (EXR/HDR) are already linear; only 8-bit images carry the sRGB choice.
    if (Texture->SRGB != SRGB && Texture->GetPixelFormat() == PF_B8G8R8A8) { Texture->SRGB = SRGB; Texture->UpdateResource(); }
    PreviewTextures.Add(Key, TStrongObjectPtr<UTexture2D>(Texture));
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

// Replaces one dynamic mesh queued by Apply with a static mesh (cached draw commands, static ray-tracing geometry),
// with Nanite data when its build has finished.
void FHdueScene::TickConversions() {
    for (auto It = PendingStatic.CreateIterator(); It; ++It) {
        if (!It.Value().IsCompleted()) continue; // not built yet
        AActor* Actor = Actors.FindRef(It.Key());
        auto* Dynamic = IsValid(Actor) ? Cast<UDynamicMeshComponent>(Actor->GetRootComponent()) : nullptr;
        if (!Dynamic) { It.RemoveCurrent(); continue; }
        const TSharedPtr<FConversion> Conversion = It.Value().GetResult();
        auto* Built = NewObject<UStaticMesh>(Actor);
        FStaticMaterial Slot; Slot.UVChannelData = FMeshUVChannelInfo(1.f); // texture streaming UV density
        Built->GetStaticMaterials().Add(Slot);
        UStaticMesh::FBuildMeshDescriptionsParams Params;
        Params.bBuildSimpleCollision = false; Params.bFastBuild = true;
        Built->BuildFromMeshDescriptions({&Conversion->Description}, Params);
        if (Conversion->Nanite && Built->GetRenderData()) {
            // The fast build made LOD0 (fallback, ray tracing) and empty Nanite resources; swap in the built ones.
            // Only the Nanite resources: LOD0's CPU copies are gone, so its buffers can't be re-created.
            // Nanite's ray-tracing fallback only counts LODs with a BuffersSize, which only cooking sets.
            FStaticMeshLODResources& LOD0 = Built->GetRenderData()->LODResources[0];
            LOD0.BuffersSize = FMath::Max<uint32>(LOD0.BuffersSize, 1);
            Nanite::FResources& Nanite = *Built->GetRenderData()->NaniteResourcesPtr;
            Nanite.ReleaseResources();
            FlushRenderingCommands();
            Nanite = MoveTemp(*Conversion->Nanite);
            Nanite.InitResources(Built);
            static int32 NaniteMeshes = 0, Tried = 0;
            ++Tried; if (Built->HasValidNaniteData()) ++NaniteMeshes;
            if (Tried % 50 == 1 || PendingStatic.Num() == 1)
                UE_LOG(LogTemp, Display, TEXT("hdUE Nanite: %d of %d converted meshes have Nanite data"), NaniteMeshes, Tried);
        }
        auto* Component = NewObject<UStaticMeshComponent>(Actor);
        Component->SetMobility(EComponentMobility::Movable);
        Component->SetStaticMesh(Built);
        Component->SetMaterial(0, Dynamic->GetMaterial(0));
        Actor->SetRootComponent(Component);
        Actor->AddInstanceComponent(Component);
        Component->RegisterComponent();
        Actor->SetActorTransform(Dynamic->GetComponentTransform());
        Dynamic->DestroyComponent();
        It.RemoveCurrent();
        return; // one per tick keeps frames smooth
    }
}

namespace {
// One Hydra mesh as an FDynamicMesh3; pure CPU work, so Apply builds a batch's meshes in parallel.
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
    for (uint32 V = 0; V < Desc.vertex_count; ++V) Mesh.AppendVertex(FVector3d(Desc.positions[V * 3], Desc.positions[V * 3 + 1], Desc.positions[V * 3 + 2]));
    for (uint32 I = 0; I < Desc.index_count; I += 3) {
        const FIndex3i Vertices(Desc.indices[I], Desc.indices[I + 1], Desc.indices[I + 2]);
        const int32 Triangle = Mesh.AppendTriangle(Vertices);
        if (Triangle < 0) continue;
        const FVector3f FaceNormal = FVector3f(Mesh.GetTriNormal(Triangle));
        FIndex3i NormalIds, UVIds, ColorIds;
        for (int C = 0; C < 3; ++C) {
            const auto V = Vertices[C];
            const FVector3f Normal = Desc.normals ? FVector3f(Desc.normals[V * 3], Desc.normals[V * 3 + 1], Desc.normals[V * 3 + 2]) : FaceNormal;
            const FVector2f UV = Desc.uv ? FVector2f(Desc.uv[V * 2], Desc.uv[V * 2 + 1]) : FVector2f::ZeroVector;
            NormalIds[C] = NormalOverlay->AppendElement(Normal);
            UVIds[C] = UVOverlay->AppendElement(UV);
            ColorIds[C] = Colors->AppendElement(Desc.colors ? FVector4f(Desc.colors[V * 3], Desc.colors[V * 3 + 1], Desc.colors[V * 3 + 2], 1.f) : FVector4f(1.f, 1.f, 1.f, 1.f));
        }
        NormalOverlay->SetTriangle(Triangle, NormalIds);
        UVOverlay->SetTriangle(Triangle, UVIds);
        Colors->SetTriangle(Triangle, ColorIds);
    }
    return Mesh;
}
}
bool FHdueScene::Apply(const HdueSceneBatch& Batch, FString& Error) {
    if ((Batch.material_count && !Batch.materials) || (Batch.mesh_count && !Batch.meshes) ||
        (Batch.light_count && !Batch.lights) || (Batch.removed_count && !Batch.removed_ids)) {
        Error = TEXT("Missing scene batch data."); return false;
    }
    // Validate all mesh array bounds before mutating the scene.
    for (uint32 Index = 0; Index < Batch.mesh_count; ++Index) {
        const auto& Mesh = Batch.meshes[Index];
        if (!Mesh.id || Mesh.index_count % 3 || Mesh.vertex_count > 10000000 || (Mesh.instance_transforms && Mesh.instance_count > 10000000) ||
            (Mesh.vertex_count && !Mesh.positions) || (Mesh.index_count && !Mesh.indices)) {
            Error = TEXT("Invalid mesh descriptor."); return false;
        }
        for (uint32 I = 0; I < Mesh.index_count; ++I) {
            if (Mesh.indices[I] >= Mesh.vertex_count) { Error = TEXT("Mesh index out of range."); return false; }
        }
    }
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
    {
        // Decode this batch's new texture files in parallel: decoding them one by one on this thread took most of
        // a large scene's load (111 Sponza textures, ~19 s). Creating the UTexture2Ds stays on this thread.
        TArray<FString> Files;
        for (uint32 Index = 0; Index < Batch.material_count; ++Index) {
            const auto& Desc = Batch.materials[Index];
            if (Desc.native_asset && *Desc.native_asset) continue;
            for (int32 Slot = 0; Slot < HDUE_TEXTURE_SLOT_COUNT; ++Slot) {
                if (!Desc.textures[Slot].file || !*Desc.textures[Slot].file) continue;
                const FString File = UTF8_TO_TCHAR(Desc.textures[Slot].file);
                const bool Color = Slot == HDUE_TEXTURE_BASE_COLOR || Slot == HDUE_TEXTURE_EMISSIVE || Slot == HDUE_TEXTURE_SPECULAR_COLOR;
                if (!PreviewTextures.Contains(File + (Color ? TEXT("|srgb") : TEXT("|raw"))) && !DecodedImages.Contains(File)) Files.AddUnique(File);
            }
        }
        if (Files.Num()) {
            const double DecodeStart = FPlatformTime::Seconds();
            TArray<FImage> Images; Images.SetNum(Files.Num());
            TArray<uint8> Loaded; Loaded.SetNumZeroed(Files.Num());
            ParallelFor(Files.Num(), [&](int32 Index) { Loaded[Index] = FImageUtils::LoadImage(*Files[Index], Images[Index]) ? 1 : 0; });
            for (int32 Index = 0; Index < Files.Num(); ++Index) if (Loaded[Index]) DecodedImages.Add(Files[Index], MoveTemp(Images[Index]));
            TextureSeconds += FPlatformTime::Seconds() - DecodeStart;
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
            const TCHAR* ParentPath = Masked ? TEXT("/Game/HdUE/M_PreviewSurfaceMasked.M_PreviewSurfaceMasked")
                : Translucent ? TEXT("/Game/HdUE/M_PreviewSurfaceTranslucent.M_PreviewSurfaceTranslucent")
                : TEXT("/Game/HdUE/M_PreviewSurface.M_PreviewSurface");
            auto* Parent = LoadObject<UMaterialInterface>(nullptr, ParentPath);
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
                    const bool Color = Slot == HDUE_TEXTURE_BASE_COLOR || Slot == HDUE_TEXTURE_EMISSIVE || Slot == HDUE_TEXTURE_SPECULAR_COLOR;
                    UTexture2D* Image = PreviewTexture(UTF8_TO_TCHAR(Texture.file), Color, Error);
                    if (!Image) return false;
                    Instance->SetTextureParameterValue(*(Name + TEXT("Texture")), Image);
                    Instance->SetVectorParameterValue(*(Name + TEXT("Scale")), FLinearColor(Texture.scale[0], Texture.scale[1], Texture.scale[2], Texture.scale[3]));
                    Instance->SetVectorParameterValue(*(Name + TEXT("Bias")), FLinearColor(Texture.bias[0], Texture.bias[1], Texture.bias[2], Texture.bias[3]));
                    Instance->SetVectorParameterValue(*(Name + TEXT("Mask")), FLinearColor(Texture.channel_mask[0], Texture.channel_mask[1], Texture.channel_mask[2], Texture.channel_mask[3]));
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
        if (Desc.native_asset && *Desc.native_asset) return;
        BuiltMeshes[Index] = BuildDynamicMesh(Desc);
        if (StaticGenerated(Desc)) {
            FStaticMeshAttributes Attributes(Descriptions[Index]); Attributes.Register();
            FDynamicMeshToMeshDescription().Convert(&BuiltMeshes[Index], Descriptions[Index]);
            BuiltMeshes[Index].Clear();
        }
    });
    for (uint32 Index = 0; Index < Batch.mesh_count; ++Index) {
        const auto& Desc = Batch.meshes[Index];
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
            if (Native || Instanced || Static) {
                auto* Component = NewObject<UStaticMeshComponent>(Actor, Wanted);
                Component->SetMobility(EComponentMobility::Movable);
                Actor->SetRootComponent(Component);
                Actor->AddInstanceComponent(Component);
                Component->RegisterComponent();
            } else {
                auto* Component = NewObject<UDynamicMeshComponent>(Actor);
                Component->SetMobility(EComponentMobility::Movable);
                Component->SetEnableRaytracing(true);
                Component->SetTangentsType(EDynamicMeshComponentTangentsMode::AutoCalculated);
                Component->SetVertexColorSpaceTransformMode(EDynamicMeshVertexColorTransformMode::NoTransform);
                Actor->SetRootComponent(Component);
                Actor->AddInstanceComponent(Component);
                Component->RegisterComponent();
            }
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
                auto* Built = NewObject<UStaticMesh>(Actor);
                // Texture streaming reads UV density; cooked meshes get it from the editor.
                FStaticMaterial Slot; Slot.UVChannelData = FMeshUVChannelInfo(1.f);
                Built->GetStaticMaterials().Add(Slot);
                UStaticMesh::FBuildMeshDescriptionsParams Params;
                Params.bBuildSimpleCollision = false; Params.bFastBuild = true;
                Built->BuildFromMeshDescriptions({&Descriptions[Index]}, Params);
                if (Instanced) InstanceMesh = Built;
                else Actor->FindComponentByClass<UStaticMeshComponent>()->SetStaticMesh(Built);
            } else {
                Actor->FindComponentByClass<UDynamicMeshComponent>()->SetMesh(MoveTemp(BuiltMeshes[Index]));
                PendingStatic.Remove(Id);
                if (ConvertLater(Desc)) {
                    // The static mesh description and Nanite data are built on background workers, from a copy of the
                    // descriptor's arrays: done here, they doubled the time to the first frame (and normal-priority
                    // Nanite tasks starved its rendering for 15 s).
                    auto Copy = MakeShared<FNaniteSource>(Desc);
                    PendingStatic.Add(Id, UE::Tasks::Launch(TEXT("HdueConversion"), [Copy]() -> TSharedPtr<FConversion> {
                        TSharedPtr<FConversion> Result = MakeShared<FConversion>();
                        UE::Geometry::FDynamicMesh3 Mesh = BuildDynamicMesh(Copy->Desc);
                        FStaticMeshAttributes Attributes(Result->Description); Attributes.Register();
                        FDynamicMeshToMeshDescription().Convert(&Mesh, Result->Description);
                        if (NaniteMeshes) Result->Nanite = BuildNanite(Copy->Desc);
                        return Result;
                    }, UE::Tasks::ETaskPriority::BackgroundLow));
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
    DecodedImages.Reset(); // their textures exist now
    BindMaterials();
    if (Batch.material_count || Batch.mesh_count)
        UE_LOG(LogTemp, Display, TEXT("hdUE apply: %u materials %.0f ms (%d texture imports %.0f ms), %u meshes %.0f ms, bind %.0f ms"),
            Batch.material_count, (MaterialsDone - ApplyStart) * 1000, TextureImports, TextureSeconds * 1000, Batch.mesh_count,
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
