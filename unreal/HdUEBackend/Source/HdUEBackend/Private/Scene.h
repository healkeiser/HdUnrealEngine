#pragma once
#include "ImageCore.h"
#include "Engine/Texture.h"
#include "Tasks/Task.h"
#include "Rendering/NaniteResources.h"
#include "hdue/bridge.h"
// UV sets a mesh carries (ABI 10 uv_set_count, of which UE keeps 4: Nanite's limit); 1 without UVs.
inline int32 HdueUVSets(const HdueMeshDesc& Desc) { return FMath::Clamp<int32>(int32(Desc.uv_set_count), 1, 4); }
// UV channels of static and dynamic mesh vertices: the sets, the last one repeated. The parent materials read 4, and UE
// 5.8's local vertex factory shaded them with constant UVs on meshes with fewer (Nanite clamps them correctly).
// ponytail: 12 bytes more per vertex for one-set meshes; per-material channel counts if memory matters.
constexpr int32 HdueVertexUVs = 4;
// A mesh descriptor with its own copies of the arrays, for Nanite builds that outlive Apply's batch.
struct FNaniteSource {
    TArray<float> Positions, Normals, UV, Colors;
    TArray<uint32> Indices;
    HdueMeshDesc Desc{};
    bool Nanite = false;  // build Nanite data too
    explicit FNaniteSource(const HdueMeshDesc& In) : Desc(In) {
        Positions = TArray<float>(In.positions, int32(In.vertex_count * 3));
        Desc.positions = Positions.GetData();
        if (In.normals) {
            Normals = TArray<float>(In.normals, int32(In.vertex_count * 3));
            Desc.normals = Normals.GetData();
        }
        if (In.uv) {
            UV = TArray<float>(In.uv, int32(In.vertex_count * 2) * HdueUVSets(In));
            Desc.uv_set_count = uint32(HdueUVSets(In));
            Desc.uv = UV.GetData();
        }
        if (In.colors) {
            Colors = TArray<float>(In.colors, int32(In.vertex_count * 3));
            Desc.colors = Colors.GetData();
        }
        Indices = TArray<uint32>(In.indices, int32(In.index_count));
        Desc.indices = Indices.GetData();
    }
};
#include "CoreMinimal.h"
#include "hdue/bridge.h"
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
    // A texture of the Preview Surface parents and the channels it was built for. Scalar inputs that read one or two
    // channels get them repacked into BC4/BC5: Channels holds the source channel of each stored channel (empty: as
    // read).
    struct FPreviewTexture {
        TStrongObjectPtr<UTexture2D> Texture;
        TArray<uint8, TInlineAllocator<2>> Channels;
        uint8 Read = 0;
        bool Vector = false;    // channels scalar inputs read (bit per RGBA channel); read by a colour/normal input
        bool SRGB = false;      // 8-bit texels are sRGB-decoded
        uint8 Address[2] = {};  // TextureAddress for u and v (wrapS, wrapT): sampler state, part of the key
        float Coverage = 0;     // texel value a cutout is clipped at: mips keep its alpha coverage (0: plain mips)
        TUniquePtr<FTexturePlatformData> Pending;  // built on workers by Apply; PreviewTexture gives it to Texture
    };
    FHdueScene(UWorld* InWorld, bool InInteractive);
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
    HdueResult PickNative(const HdueNativeRay& Ray, HdueNativeHit& Hit, FString& ObjectId, FString& Error);
    // Swaps every mesh to M_HdUEId (its prim ID in custom primitive data) for the ID pass, or back.
    bool SetIdMode(int32 Mode, FString& Error);  // 0 off, 1 prim IDs, 2 instance IDs
    // Dense density grids as heterogeneous volumes (upsert_volume).
    bool UpsertVolume(const HdueVolumeDesc& Desc, FString& Error);
    void RemoveVolume(const FString& Id);
    bool UpsertGroom(const HdueGroomDesc& Desc, FString& Error);
    void SetTransforms(const FString& Id, uint32 Count, const double* Matrices);  // set_transforms
    void UpdateMeshes(HdueMeshUpdate* Meshes, uint32 Count);                      // update_meshes
    void ApplyLinks();  // Lighting's object table to every mesh (after a light batch replaced it)
    FHdueNativeAssets Assets;
    TUniquePtr<FHdueLighting> Lighting;

private:
    UWorld* World;
    // HDUE_INIT_INTERACTIVE (Houdini's viewport): meshes show without Lumen cards and Nanite first, and
    // TickConversions swaps in versions built in the background. Offline hosts (husk) build both before the frame.
    bool Interactive;
    TMap<FString, AActor*> Actors;
    TMap<FString, FVector> CameraLightDirections;
    TMap<FString, FString> MeshMaterials;
    TSet<FString> SingleSided;  // meshes sent with HDUE_MESH_SINGLE_SIDED
    // By material id: Base's twin on the one-sided parent (null when Base has none).
    struct FOneSided {
        UMaterialInterface* Base = nullptr;
        TStrongObjectPtr<UMaterialInterface> Material;
    };
    TMap<FString, FOneSided> OneSided;
    TMap<FString, TStrongObjectPtr<UMaterialInterface>> Materials;
    TMap<FString, int32> PrimIds;
    struct FVolume {
        AActor* Actor = nullptr;
        TStrongObjectPtr<class UVolumeTexture> Texture, Emission;
        TStrongObjectPtr<UMaterialInstanceDynamic> Material;
    };
    TMap<FString, FVolume> Volumes;
    struct FGroom {
        AActor* Actor = nullptr;
        TStrongObjectPtr<class UGroomAsset> Asset;
    };
    TMap<FString, FGroom> Grooms;  // upsert_groom, by id
    int32 IdMode = 0;
    // ponytail: keyed by path and colour space, released only on scene reset; key on file time if live texture edits
    // matter.
    TMap<FString, FPreviewTexture> PreviewTextures;
    double TextureSeconds = 0;
    int32 TextureImports = 0;                                       // per Apply, for its timing log
    TArray<TStrongObjectPtr<UMaterialInterface>> RetiredMaterials;  // replaced last Apply; released next Apply
    // A static mesh's render data (with Lumen cards if asked), and Nanite data for the version TickConversions swaps
    // in.
    struct FConversion {
        TUniquePtr<FStaticMeshRenderData> RenderData;
        FBoxSphereBounds Bounds;
        TSharedPtr<Nanite::FResources> Nanite;
    };
    static FConversion BuildStatic(const HdueMeshDesc& Desc, bool Cards);
    // Background card and Nanite builds (interactive hosts), queued until memory allows them (TickConversions).
    struct FPendingStatic {
        TSharedPtr<FNaniteSource> Source;                // the mesh's arrays, released once its build starts
        int64 Memory = 0;                                // reserved from launch until the mesh is swapped in
        UE::Tasks::TTask<TSharedPtr<FConversion>> Task;  // invalid while queued
    };
    TMap<FString, FPendingStatic> PendingStatic;
    int64 ReservedMemory = 0;
    // Nanite root pages of the meshes that carry Nanite data (NaniteFits): every page of a runtime mesh is a root page,
    // and UE's root pool is bounded (NaniteStreamingManager.cpp, GetMaxPagePoolSizeInMB).
    TMap<FString, uint32> NaniteRootPages;
    int64 NaniteRootPagesUsed = 0;
    bool NaniteFits(const FString& Id, const Nanite::FResources* Nanite);
    const FPreviewTexture* PreviewTexture(const FString& Key, FString& Error);
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
    UMaterialInterface* MeshMaterial(const FString& MeshId);  // its material, one-sided if the mesh is
};
