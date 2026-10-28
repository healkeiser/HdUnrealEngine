#pragma once
#include "ImageCore.h"
#include "MeshDescription.h"
#include "CoreMinimal.h"
#include "hdue/bridge.h"
#include "hdue/material_api.h"
#include "hdue/surface_api.h"
#include "hdue/volume_api.h"
#include "RenderCommandFence.h"
#include "UObject/StrongObjectPtr.h"
#include "NativeAssets.h"
class UWorld;
class AActor;
class UMaterialInterface;
class UTexture2D;
class UMaterialInstanceDynamic;
struct FNativePickGeometry;
class FHdueLighting;

class FHdueScene {
public:
    explicit FHdueScene(UWorld* InWorld);
    ~FHdueScene();
    bool Apply(const HdueSceneBatch& Batch, FString& Error);
    void TickConversions();
    void UpdateCameraLights(const FVector& Eye, const FVector& Target, const FVector& Up);
    HdueResult SetMaterialParameters(const HdueMaterialUpdate& Update, FString& Error);
    HdueResult GetMaterialStats(const char* Id, HdueMaterialStats& Stats, FString& Error);
    HdueResult ApplySurface(const HdueSurfaceBatch& Batch, FString& Error);
    HdueSurfaceStatus SurfaceStatus() const;
    bool SurfaceReady() const;
    TArray<TStrongObjectPtr<UTexture2D>> GatherVirtualTextures() const;
    bool HasRefractiveMaterials() const;
    HdueResult PickNative(const HdueNativeRay& Ray, HdueNativeHit& Hit, FString& ObjectId, FString& Error);
    // Swaps every mesh to M_HdUEId (its prim ID in custom primitive data) for the ID pass, or back.
    bool SetIdMode(int32 Mode, FString& Error); // 0 off, 1 prim IDs, 2 instance IDs
    // Dense density grids as heterogeneous volumes (hdue/volume_api.h).
    bool UpsertVolume(const HdueVolumeDesc& Desc, FString& Error);
    void RemoveVolume(const FString& Id);
    void SetTransforms(const FString& Id, uint32 Count, const double* Matrices); // hdue/motion_api.h
    FHdueNativeAssets Assets;
    TUniquePtr<FHdueLighting> Lighting;
private:
    UWorld* World;
    TMap<FString, AActor*> Actors;
    TMap<FString, FVector> CameraLightDirections;
    TMap<FString, FString> MeshMaterials;
    TMap<FString, TStrongObjectPtr<UMaterialInterface>> Materials;
    TMap<FString, int32> PrimIds;
    struct FVolume {
        AActor* Actor = nullptr;
        TStrongObjectPtr<class UVolumeTexture> Texture;
        TStrongObjectPtr<UMaterialInstanceDynamic> Material;
    };
    TMap<FString, FVolume> Volumes;
    int32 IdMode = 0;
    // ponytail: keyed by path and colour space, released only on scene reset; key on file time if live texture edits matter.
    TMap<FString, TStrongObjectPtr<UTexture2D>> PreviewTextures;
    double TextureSeconds = 0; int32 TextureImports = 0; // per Apply, for its timing log
    TArray<TStrongObjectPtr<UMaterialInterface>> RetiredMaterials; // replaced last Apply; released next Apply
    TMap<FString, FImage> DecodedImages;
    TMap<FString, FMeshDescription> PendingStatic; // dynamic meshes TickConversions turns into static meshes // decoded in parallel at the start of Apply, consumed by PreviewTexture
    UTexture2D* PreviewTexture(const FString& File, bool SRGB, FString& Error);
    struct FWet {
        FString Asset;
        UMaterialInterface* Base = nullptr;
        TStrongObjectPtr<UMaterialInstanceDynamic> Material;
        TArray<float> Mask;
        uint64 Revision = 0;
    };
    TMap<FString, TSharedPtr<FWet>> WetBindings;
    TMap<FString, uint64> GeometryRevisions;
    FRenderCommandFence SurfaceFence;
    uint64 MaskUploads = 0, MaskBytes = 0, GeometryUploads = 0, SurfaceUpdates = 0;
    TMap<FString, TSharedPtr<FNativePickGeometry>> NativePickGeometry;
    void Remove(const FString& Id);
    void BindMaterials();
};
