#include "Scene.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/NonManifoldMappingSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "Engine/World.h"
#include "UDynamicMesh.h"
#include "RHI.h"

namespace {
// A mesh's primary colours per triangle corner, and back: the wet mask takes them over (VertexColor.R).
TArray<FVector4f> CornerColors(const UE::Geometry::FDynamicMesh3& Mesh) {
    TArray<FVector4f> Corners;
    const auto* Colors = Mesh.Attributes() ? Mesh.Attributes()->PrimaryColors() : nullptr;
    if (!Colors) return Corners;
    Corners.Reserve(Mesh.TriangleCount() * 3);
    for (int32 T : Mesh.TriangleIndicesItr()) {
        const auto Elements = Colors->GetTriangle(T);
        for (int C = 0; C < 3; ++C)
            Corners.Add(Colors->IsElement(Elements[C]) ? Colors->GetElement(Elements[C]) : FVector4f(1.f));
    }
    return Corners;
}
void SetCornerColors(UDynamicMeshComponent& Component, const TFunctionRef<FVector4f(int32 T, int C)> Color) {
    using namespace UE::Geometry;
    auto& Mesh = Component.GetDynamicMesh()->GetMeshRef();
    Mesh.Attributes()->EnablePrimaryColors();
    auto* Colors = Mesh.Attributes()->PrimaryColors();
    Colors->ClearElements();
    for (int32 T : Mesh.TriangleIndicesItr()) {
        FIndex3i Elements;
        for (int C = 0; C < 3; ++C) Elements[C] = Colors->AppendElement(Color(T, C));
        Colors->SetTriangle(T, Elements);
    }
    Component.FastNotifyColorsUpdated();
}
// Puts a binding's saved mesh colours back, if the mesh is still the geometry (revision) they were taken from.
void RestoreColors(UDynamicMeshComponent& Component, TArray<FVector4f>& Saved, bool SameGeometry) {
    if (Saved.IsEmpty()) return;
    if (SameGeometry && Saved.Num() == Component.GetDynamicMesh()->GetMeshRef().TriangleCount() * 3) {
        int32 Corner = 0;
        SetCornerColors(Component, [&](int32, int) { return Saved[Corner++]; });
    }
    Saved.Empty();
}
}  // namespace

bool FHdueScene::SurfaceReady() const { return SurfaceFence.IsFenceComplete(); }
HdueSurfaceStatus FHdueScene::SurfaceStatus() const {
    return {sizeof(HdueSurfaceStatus),
            uint32(WetBindings.Num()),
            SurfaceReady() ? 0u : 1u,
            0,
            MaskUploads,
            MaskBytes,
            GeometryUploads,
            SurfaceUpdates};
}
HdueResult FHdueScene::ApplySurface(const HdueSurfaceBatch& Batch, FString& Error) {
    if (Batch.struct_size != sizeof(Batch) || Batch.binding_count > 100000 ||
        (Batch.binding_count && !Batch.bindings)) {
        Error = TEXT("Invalid surface batch.");
        return HDUE_INVALID_ARGUMENT;
    }
    struct FPrepared {
        FString Id;
        UDynamicMeshComponent* Component;
        TSharedPtr<FWet> Wet;
        bool UploadMask;
        float Amount, Roughness;
        uint32 Flags;
    };
    TArray<FPrepared> Prepared;
    TSet<FString> Seen;
    // Validate the whole snapshot before replacing any live binding or buffer.
    for (uint32 I = 0; I < Batch.binding_count; ++I) {
        const auto& D = Batch.bindings[I];
        if (D.struct_size != sizeof(D) || !D.object_id || !*D.object_id || !D.variant_asset || !D.preview_asset ||
            D.reserved || (D.flags & ~(HDUE_WET_MASK_PREVIEW | HDUE_SURFACE_GRAPH)) || !FMath::IsFinite(D.amount) ||
            !FMath::IsFinite(D.roughness) || D.mask_count > 30000000 || (D.mask_count && !D.mask)) {
            Error = TEXT("Invalid wet binding or non-finite scalar.");
            return HDUE_INVALID_ARGUMENT;
        }
        const FString Id = UTF8_TO_TCHAR(D.object_id);
        if (Seen.Contains(Id)) {
            Error = TEXT("Duplicate wet object ID: ") + Id;
            return HDUE_INVALID_ARGUMENT;
        }
        Seen.Add(Id);
        AActor* Actor = Actors.FindRef(Id);
        auto* Component = IsValid(Actor) ? Actor->FindComponentByClass<UDynamicMeshComponent>() : nullptr;
        if (!Component) {
            Error = TEXT("Wet requires a generic dynamic mesh: ") + Id;
            return HDUE_NOT_SUPPORTED;
        }
        if (!D.geometry_revision || GeometryRevisions.FindRef(Id) != D.geometry_revision) {
            Error = TEXT("Obsolete wet geometry revision: ") + Id;
            return HDUE_STALE_FRAME;
        }
        if (D.amount <= 0.f && !(D.flags & (HDUE_WET_MASK_PREVIEW | HDUE_SURFACE_GRAPH))) continue;
        const auto& Mesh = Component->GetDynamicMesh()->GetMeshRef();
        // Masks are per uploaded vertex; BuildDynamicMesh's copies of non-manifold vertices map back to theirs.
        const UE::Geometry::FNonManifoldMappingSupport Sources(Mesh);
        int32 Uploaded = 0;
        for (const int32 V : Mesh.VertexIndicesItr()) Uploaded += !Sources.IsNonManifoldVertexID(V);
        if (D.mask_count && D.mask_count != uint32(Uploaded)) {
            Error = TEXT("Wet mask count does not match the mesh's vertices: ") + Id;
            return HDUE_INVALID_ARGUMENT;
        }
        auto New = MakeShared<FWet>();
        const auto* Old = WetBindings.Find(Id);
        if (D.mask_count) {
            New->Mask.Reserve(D.mask_count);
            for (uint32 V = 0; V < D.mask_count; ++V) {
                if (!FMath::IsFinite(D.mask[V])) {
                    Error = TEXT("Non-finite wet mask: ") + Id;
                    return HDUE_INVALID_ARGUMENT;
                }
                New->Mask.Add(FMath::Clamp(D.mask[V], 0.f, 1.f));
            }
        }
        New->Revision = D.geometry_revision;
        if (Old && (*Old)->Revision == New->Revision) New->Colors = (*Old)->Colors;  // the mesh's, saved before
        const auto* Base = Materials.Find(MeshMaterials.FindRef(Id));
        if (!Base) {
            Error = TEXT("Wet base material not found: ") + Id;
            return HDUE_INVALID_STATE;
        }
        New->Base = Base->Get();
        const char* SelectedAsset = (D.flags & HDUE_WET_MASK_PREVIEW) ? D.preview_asset : D.variant_asset;
        New->Asset = UTF8_TO_TCHAR(SelectedAsset);
        if (!New->Asset.StartsWith(TEXT("/Game/"))) {
            Error = TEXT("Wet variant must be a cooked /Game asset.");
            return HDUE_INVALID_ARGUMENT;
        }
        auto* Parent = LoadObject<UMaterialInterface>(nullptr, *New->Asset);
        if (!Parent || Parent->GetBlendMode() != BLEND_Opaque ||
            !Parent->GetMaterial()->HasSubstrateFrontMaterialConnected()) {
            Error = TEXT("Prepared opaque wet variant is unavailable: ") + New->Asset;
            return HDUE_NOT_SUPPORTED;
        }
        const auto* Resource = Parent->GetMaterial()->GetMaterialResource(GMaxRHIShaderPlatform);
        if (!Resource || !Resource->GetGameThreadShaderMap() ||
            Resource->MaterialGetSubstrateClosureCount_GameThread() > 4 ||
            Resource->MaterialGetSubstrateUintPerPixel_GameThread() * 4 > 128) {
            Error = TEXT("Wet variant exceeds shader budget or has no compiled shader: ") + New->Asset;
            return HDUE_NOT_SUPPORTED;
        }
        const auto* BaseResource = New->Base->GetMaterialResource(GMaxRHIShaderPlatform);
        if (!(D.flags & (HDUE_WET_MASK_PREVIEW | HDUE_SURFACE_GRAPH)) &&
            (!BaseResource || Resource->MaterialGetSubstrateClosureCount_GameThread() <=
                                  BaseResource->MaterialGetSubstrateClosureCount_GameThread())) {
            Error = TEXT("Water slab was simplified; prepare a base graph with room for another closure: ") + Id;
            return HDUE_NOT_SUPPORTED;
        }
        const bool Reuse = Old && (*Old)->Base == New->Base && (*Old)->Asset == New->Asset;
        New->Material.Reset(Reuse ? (*Old)->Material.Get() : UMaterialInstanceDynamic::Create(Parent, World));
        const bool Upload = D.mask_count && (!Old || (*Old)->Revision != New->Revision || (*Old)->Mask != New->Mask);
        Prepared.Add({Id, Component, New, Upload, D.amount, D.roughness, D.flags});
    }
    TSet<FString> Kept;
    for (const auto& P : Prepared) Kept.Add(P.Id);
    for (const auto& Old : WetBindings)
        if (!Kept.Contains(Old.Key)) {
            if (auto* Actor = Actors.FindRef(Old.Key); IsValid(Actor)) {
                if (auto* Component = Actor->FindComponentByClass<UMeshComponent>())
                    if (auto* Base = MeshMaterial(Old.Key); Base && Component->GetMaterial(0) != Base)
                        Component->SetMaterial(0, Base);
                if (auto* Dynamic = Actor->FindComponentByClass<UDynamicMeshComponent>())
                    RestoreColors(*Dynamic, Old.Value->Colors,
                                  Old.Value->Revision == GeometryRevisions.FindRef(Old.Key));
            }
        }
    WetBindings.Empty();
    for (auto& P : Prepared) {
        auto* Instance = P.Wet->Material.Get();
        Instance->ClearParameterValues();
        // Reuse the MID and render proxy for parameter/mask edits. Rebinding
        // a new MID would rebuild the dynamic mesh's position buffers too.
        if (auto* BaseInstance = Cast<UMaterialInstance>(P.Wet->Base)) Instance->CopyInterpParameters(BaseInstance);
        if (P.Flags & HDUE_SURFACE_GRAPH)
            Instance->SetScalarParameterValue(TEXT("HdUE_PrimvarMaskEnabled"), P.Wet->Mask.IsEmpty() ? 0.f : 1.f);
        else {
            Instance->SetScalarParameterValue(TEXT("HdUE_WetAmount"), FMath::Clamp(P.Amount, 0.f, 1.f));
            Instance->SetScalarParameterValue(TEXT("HdUE_WetRoughness"), FMath::Clamp(P.Roughness, 0.f, 1.f));
            Instance->SetScalarParameterValue(TEXT("HdUE_WetUseMask"), P.Wet->Mask.IsEmpty() ? 0.f : 1.f);
        }
        if (P.Wet->Mask.IsEmpty()) RestoreColors(*P.Component, P.Wet->Colors, true);  // no mask any more
        if (P.UploadMask) {
            using namespace UE::Geometry;
            const auto& Mesh = P.Component->GetDynamicMesh()->GetMeshRef();
            if (P.Wet->Colors.IsEmpty()) P.Wet->Colors = CornerColors(Mesh);
            const FNonManifoldMappingSupport Sources(Mesh);
            SetCornerColors(*P.Component, [&](int32 T, int C) {
                return FVector4f(P.Wet->Mask[Sources.GetOriginalNonManifoldVertexID(Mesh.GetTriangle(T)[C])], 0.f, 0.f,
                                 1.f);
            });
            ++MaskUploads;
            MaskBytes += P.Wet->Mask.Num() * sizeof(float);
        }
        if (P.Component->GetMaterial(0) != P.Wet->Material.Get()) P.Component->SetMaterial(0, P.Wet->Material.Get());
        WetBindings.Add(P.Id, P.Wet);
    }
    ++SurfaceUpdates;
    SurfaceFence.BeginFence();
    return HDUE_OK;
}
