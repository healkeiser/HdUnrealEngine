#ifndef HDUE_BRIDGE_H
#define HDUE_BRIDGE_H

#include <stdint.h>

#if defined(_WIN32)
#define HDUE_CALL __cdecl
#if defined(HDUE_BRIDGE_EXPORTS)
#define HDUE_EXPORT __declspec(dllexport)
#else
#define HDUE_EXPORT
#endif
#else
#define HDUE_CALL
#define HDUE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HDUE_ABI_VERSION 16u
#define HDUE_ENGINE_MAJOR 5u
#define HDUE_ENGINE_MINOR 8u
#define HDUE_ENGINE_PATCH 4u

typedef int32_t HdueResult;
typedef uint64_t HdueSession;
enum {
    HDUE_OK = 0,
    HDUE_PENDING = 1,
    HDUE_INVALID_ARGUMENT = -1,
    HDUE_ABI_MISMATCH = -2,
    HDUE_WRONG_THREAD = -3,
    HDUE_INVALID_STATE = -4,
    HDUE_NOT_SUPPORTED = -5,
    HDUE_BUFFER_TOO_SMALL = -6,
    HDUE_STALE_FRAME = -7,
    HDUE_ENGINE_ERROR = -8
};
enum {
    HDUE_STATE_COLD = 0,
    HDUE_STATE_STARTING = 1,
    HDUE_STATE_READY = 2,
    HDUE_STATE_FAILED = 3,
    HDUE_STATE_STOPPED = 4
};
enum { HDUE_REALTIME = 0, HDUE_PATH_TRACED = 1 };
enum {
    HDUE_RGBA32_FLOAT = 1,
    HDUE_SCENE_LINEAR_SRGB = 1,
    HDUE_MESH_REQUIRE_NANITE = 1,
    HDUE_MESH_EDITABLE = 2,
    HDUE_MESH_SINGLE_SIDED = 4
}; /* EDITABLE: geometry edited after upload (surface effects); others build static meshes. SINGLE_SIDED:
      UsdGeomGprim doubleSided = 0 under unreal:cullBackface: its generated surface material culls back faces (older
      backends ignore it). */

/* Every descriptor is caller-owned. Set struct_size to sizeof(the descriptor).
 * Strings are UTF-8 and are copied during the call. No UE/pxr/STL type, native
 * allocation or exception crosses this ABI. All calls except get_api execute
 * on the thread that called initialize. One engine lifetime per process. */
/* HdueInitDesc.flags. INTERACTIVE: a viewport (Houdini's UI) that renders while the user works: meshes are drawn
 * first and their Lumen cards and Nanite data built in the background, frames are paced for navigation and do not
 * wait for PSOs or texture streaming. Without it, the offline profile (husk): everything is built before the frame. */
enum { HDUE_INIT_INTERACTIVE = 1 };
typedef struct HdueInitDesc {
    uint32_t struct_size;
    uint32_t flags; /* HDUE_INIT_* */
    uint64_t instance_handle;
    uint64_t window_handle;
    const char* project_path;
    const char* map_path;
    const char* extra_arguments;
} HdueInitDesc;

typedef struct HdueStatus {
    uint32_t struct_size;
    uint32_t state;
    uint32_t world_ready;
    uint32_t rhi_ready;
    uint32_t engine_major;
    uint32_t engine_minor;
    uint32_t engine_patch;
    uint32_t render_mode_mask;
    uint64_t dedicated_video_memory_bytes;
    char message[512];
} HdueStatus;

typedef struct HdueSessionDesc {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
} HdueSessionDesc;

typedef struct HdueRenderRequest {
    uint32_t struct_size;
    uint32_t mode;
    uint64_t generation;
    uint32_t samples_per_pixel;
    uint32_t warmup_frames;
    double time_seconds;
    /* UE coordinates in cm, +X forward, +Z up. Hydra performs conversion. */
    double eye[3];
    double target[3];
    float horizontal_fov_degrees;
    float near_clip_cm;
    uint32_t lumen_enabled;
    uint32_t aov; /* HDUE_AOV_*; other than COLOR, a rasterized GBuffer channel, except in HDUE_PATH_TRACED mode (ABI
                     11): ALBEDO and NORMAL are the Path Tracer's own buffers of the converged beauty request with the
                     same view (raster when there is none, e.g. motion-blurred sub-frames) */
    double up[3];
    /* Projection (ABI 7) in view space (cm, +X right, +Y up, +Z forward): NDC = scale * xy / z + offset
       for perspective, scale * xy + offset for orthographic. A zero scale derives it from the FOV. */
    double projection_scale[2];
    double projection_offset[2];
    uint32_t orthographic;
    float far_clip_cm; /* orthographic depth range [near_clip_cm, far_clip_cm] */
    float fstop;       /* depth of field when fstop and focus_distance_cm are > 0 */
    float focus_distance_cm;
    float focal_length_cm;
    /* ABI 15: UE's spherical lens distortion (CameraCalibrationCore's Brown-Conrady model, unitless): K1, K2, K3, P1,
       P2, and the principal point in normalized image coordinates (0.5, 0.5 is the centre). All zero: no distortion. */
    float lens_distortion[5];
    float lens_principal_point[2];
    uint32_t reserved;
} HdueRenderRequest;
/* Non-color AOVs are rasterized GBuffer channels (Substrate first BSDF), in RGB. */
enum {
    HDUE_AOV_COLOR = 0,
    HDUE_AOV_ALBEDO = 1,
    HDUE_AOV_NORMAL = 2,
    HDUE_AOV_ROUGHNESS = 3,
    HDUE_AOV_METALLIC = 4,
    HDUE_AOV_SPECULAR = 5,
    HDUE_AOV_OCCLUSION = 6,
    /* Unlit ID pass: R = id / 2048, G = id % 2048, B = 1 where a mesh with prim_id >= 0 is visible. */
    HDUE_AOV_PRIM_ID = 7,
    /* Same encoding as PRIM_ID with the Hydra instance index of instanced meshes; -1 elsewhere. */
    HDUE_AOV_INSTANCE_ID = 8,
    /* ABI 11: one raster render of the buffer visualizations set with set_aov_buffers. */
    HDUE_AOV_BUFFERS = 9,
    /* ABI 11: the beauty (same mode, samples, warmup, Lumen, exposure) restricted to the lighting components in bits
       8-15 (HDUE_AOV_LIGHTING | HDUE_LIGHTING_* << 8): UE's Path Tracer Lighting Components post-process settings;
       in realtime its Diffuse, Specular, Direct Lighting and Global Illumination show flags (no volume split). */
    HDUE_AOV_LIGHTING = 10,
    HDUE_AOV_COUNT = 11
};
enum {
    HDUE_LIGHTING_EMISSIVE = 1,
    HDUE_LIGHTING_DIFFUSE = 2,
    HDUE_LIGHTING_INDIRECT_DIFFUSE = 4,
    HDUE_LIGHTING_SPECULAR = 8,
    HDUE_LIGHTING_INDIRECT_SPECULAR = 16,
    HDUE_LIGHTING_VOLUME = 32,
    HDUE_LIGHTING_INDIRECT_VOLUME = 64
};

typedef struct HdueFrameInfo {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t color_space;
    uint32_t row_stride_bytes;
    uint64_t generation;
    uint64_t frame_id;
    uint64_t required_bytes;
    uint32_t completed_samples;
    uint32_t converged;
    double request_to_frame_ms;
    double readback_ms;
} HdueFrameInfo;

enum { HDUE_DISTANT_LIGHT = 0, HDUE_SPHERE_LIGHT = 1, HDUE_RECT_LIGHT = 2, HDUE_CAMERA_DISTANT_LIGHT = 3 };
/* Material inputs that can be driven by a texture (UsdUVTexture, MaterialX image). */
enum {
    HDUE_TEXTURE_BASE_COLOR = 0,
    HDUE_TEXTURE_EMISSIVE = 1,
    HDUE_TEXTURE_METALLIC = 2,
    HDUE_TEXTURE_ROUGHNESS = 3,
    HDUE_TEXTURE_OPACITY = 4,
    HDUE_TEXTURE_NORMAL = 5,
    HDUE_TEXTURE_DISPLACEMENT = 6,
    HDUE_TEXTURE_OCCLUSION = 7,
    HDUE_TEXTURE_CLEARCOAT = 8,
    HDUE_TEXTURE_CLEARCOAT_ROUGHNESS = 9,
    HDUE_TEXTURE_SPECULAR_COLOR = 10,
    HDUE_TEXTURE_SLOT_COUNT = 11
};
/* Colour slots hold (sRGB-encoded, for 8-bit files) colours; the others hold linear data. With
 * HDUE_TEXTURE_ENCODING_AUTO, this decides whether an 8-bit texture is sRGB-decoded. */
static inline int hdue_texture_is_color(uint32_t slot) {
    return slot == HDUE_TEXTURE_BASE_COLOR || slot == HDUE_TEXTURE_EMISSIVE || slot == HDUE_TEXTURE_SPECULAR_COLOR;
}
/* ABI 9: UsdUVTexture wrapS / wrapT. BLACK reads (0,0,0,0) outside [0,1], so the value is the slot's bias. */
enum { HDUE_WRAP_REPEAT = 0, HDUE_WRAP_CLAMP = 1, HDUE_WRAP_MIRROR = 2, HDUE_WRAP_BLACK = 3 };
/* ABI 9: how 8-bit texels decode (UsdUVTexture sourceColorSpace). AUTO follows hdue_texture_is_color; float
 * images are always linear. */
enum { HDUE_TEXTURE_ENCODING_AUTO = 0, HDUE_TEXTURE_ENCODING_SRGB = 1, HDUE_TEXTURE_ENCODING_LINEAR = 2 };
typedef struct HdueTextureDesc {
    const char* file; /* Null or empty: the slot uses its constant. */
    /* value = texel * scale + bias. Scalar slots first reduce the texel with channel_mask. */
    float scale[4];
    float bias[4];
    float channel_mask[4];
    uint32_t wrap[2];  /* HDUE_WRAP_* for u (wrapS) and v (wrapT) */
    uint32_t encoding; /* HDUE_TEXTURE_ENCODING_* */
    uint32_t uv_set;   /* ABI 10: the mesh UV set (HdueMeshDesc uv) the texture reads */
} HdueTextureDesc;
typedef struct HdueMaterialDesc {
    const char* id;
    const char* native_asset;
    float base_color[4]; /* rgb diffuseColor, a opacity */
    float metallic;
    float roughness;
    float emissive_color[3];
    float opacity_threshold; /* > 0 selects cutout (masked) coverage */
    /* UE UV (top-left origin) affine: u' = m[0]u + m[1]v + m[2], v' = m[3]u + m[4]v + m[5]. */
    float uv_transform[6];
    HdueTextureDesc textures[HDUE_TEXTURE_SLOT_COUNT];
    uint32_t flags; /* HDUE_MATERIAL_* */
    /* UsdPreviewSurface displacement, in cm along the vertex normal (texture scale/bias are in cm too). */
    float displacement;
    /* UsdPreviewSurface: occlusion, clear coat, index of refraction, and specularColor for the specular workflow. */
    float occlusion, clearcoat, clearcoat_roughness, ior;
    float specular_color[3];
    /* ABI 8, MaterialX standard_surface / OpenPBR; zero for UsdPreviewSurface. subsurface or sheen > 0 selects the
     * layered Standard Surface parent. Colours are linear Rec.709; subsurface_radius is the mean free path in cm. */
    float subsurface, subsurface_color[3], subsurface_radius[3];
    float sheen, sheen_color[3], sheen_roughness;
    float coat_color[3];
    /* ABI 16: transmission > 0 refracts, as UE's Interchange imports a transmissive standard_surface / OpenPBR (the
       translucent Standard Surface parent); transmission_depth in cm, 0 for UE's default. */
    float transmission, transmission_color[3], transmission_depth;
} HdueMaterialDesc;
/* useSpecularWorkflow: diffuse and F0 come from diffuseColor and specularColor instead of metallic and ior. */
#define HDUE_MATERIAL_SPECULAR_WORKFLOW 2u
/* thin_walled: a two-sided thin shell (cloth, leaves) that transmits its subsurface colour. */
#define HDUE_MATERIAL_THIN_WALLED 4u
/* subsurface_color / sheen_color are multiplied by the base color texture (the input reads the base color's image). */
#define HDUE_MATERIAL_SUBSURFACE_BASE_TEXTURE 8u
#define HDUE_MATERIAL_SHEEN_BASE_TEXTURE 16u
/* Multiply the base color by the mesh's colors / instance_colors (displayColor primvars). */
#define HDUE_MATERIAL_VERTEX_COLOR 1u
typedef struct HdueMeshDesc {
    const char* id;
    const char* material_id;
    const char* native_asset;
    const float* positions;
    const float* normals;
    const float* uv;
    const uint32_t* indices;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t visible;
    uint32_t flags;
    /* Row-major UE world transform. Dynamic vertices are in local UE cm. */
    double transform[16];
    int32_t prim_id; /* Hydra prim ID written by the HDUE_AOV_PRIM_ID pass; -1 for none */
    /* With instance_transforms, vertices are prototype-local UE cm and the mesh draws once per
     * row-major UE world matrix (16 doubles each); transform is ignored. NULL: not instanced. */
    uint32_t instance_count;
    const double* instance_transforms;
    /* Linear RGB multiplied into the surface's base color: per vertex (vertex_count * 3) and, with
     * instance_transforms, per instance (instance_count * 3). NULL: white. */
    const float* colors;
    const float* instance_colors;
    /* Hydra instanceId per instance for the HDUE_AOV_INSTANCE_ID pass (instance_count entries,
     * -1 for none). NULL: the instance's index. */
    const int32_t* instance_ids;
    /* ABI 10: UV sets in uv, vertex_count * 2 floats each, one after another (0 counts as 1). The backend keeps 4. */
    uint32_t uv_set_count;
} HdueMeshDesc;
/* ABI 13: an uploaded mesh's material, visibility, sidedness and prim ID, without its geometry (update_meshes).
 * Fields as in HdueMeshDesc. */
typedef struct HdueMeshUpdate {
    const char* id;
    const char* material_id;
    uint32_t visible;
    uint32_t flags;
    int32_t prim_id;
    /* Out: 1 when the change needs the geometry again (an unknown or native mesh, a static/editable switch, a
     * Nanite mesh given a translucent material or the reverse); apply_scene the mesh in full then. */
    uint32_t needs_upload;
} HdueMeshUpdate;
typedef struct HdueLightDesc {
    const char* id;
    uint32_t type;
    uint32_t visible;
    double position[3];
    /* World direction, or (forward,right,up) relative to the render camera
       for HDUE_CAMERA_DISTANT_LIGHT. The latter updates with request_render. */
    double direction[3];
    float color[3];
    float intensity;
    float width_cm;
    float height_cm;
} HdueLightDesc;
typedef struct HdueSceneBatch {
    uint32_t struct_size;
    uint32_t reset_scene;
    const HdueMaterialDesc* materials;
    uint32_t material_count;
    uint32_t mesh_count;
    const HdueMeshDesc* meshes;
    const HdueLightDesc* lights;
    uint32_t light_count;
    uint32_t removed_count;
    const char* const* removed_ids;
} HdueSceneBatch;

enum { HDUE_NATIVE_PICK_LOD0 = 1, HDUE_NATIVE_PICK_NANITE_FALLBACK = 2 };
enum { HDUE_PICK_MATERIAL_OPAQUE = 1, HDUE_PICK_MATERIAL_TWO_SIDED = 2, HDUE_PICK_MATERIAL_DEFORMED = 4 };
typedef struct HdueNativeRay {
    uint32_t struct_size;
    uint32_t reserved;
    /* UE world cm. Direction is normalized by the backend. */
    double origin[3];
    double direction[3];
    double minimum_cm;
    double maximum_cm;
} HdueNativeRay;
typedef struct HdueNativeHit {
    uint32_t struct_size;
    uint32_t hit;
    uint32_t flags;
    uint32_t required_id_bytes;
    double distance_cm;
    double point[3];
    double normal[3];
} HdueNativeHit;
typedef struct HduePickMaterialInfo {
    uint32_t struct_size;
    uint32_t flags;
} HduePickMaterialInfo;
enum { HDUE_PARAMETER_SCALAR = 0, HDUE_PARAMETER_VECTOR = 1, HDUE_PARAMETER_TEXTURE = 2 };
typedef struct HdueRendererInfo {
    uint32_t struct_size;
    uint32_t substrate_enabled;
    uint32_t adaptive_gbuffer;
    uint32_t project_closures_per_pixel;
    uint32_t bytes_per_pixel;
} HdueRendererInfo;
typedef struct HdueMaterialParameter {
    const char* name;
    uint32_t type;
    uint32_t reserved;
    float value[4];
    /* A cooked UE texture path for HDUE_PARAMETER_TEXTURE. Caller owned UTF-8. */
    const char* texture_asset;
} HdueMaterialParameter;
typedef struct HdueMaterialUpdate {
    uint32_t struct_size;
    uint32_t parameter_count;
    const char* material_id;
    const HdueMaterialParameter* parameters;
} HdueMaterialUpdate;
typedef struct HdueMaterialStats {
    uint32_t struct_size;
    uint32_t substrate_enabled;
    uint32_t closure_count;
    uint32_t bytes_per_pixel;
    uint32_t scalar_parameters;
    uint32_t vector_parameters;
    uint32_t texture_parameters;
    uint32_t blend_mode;
} HdueMaterialStats;
enum { HDUE_RESIDENCY_IDLE, HDUE_RESIDENCY_PREPARING, HDUE_RESIDENCY_READY, HDUE_RESIDENCY_FAILED };
typedef struct HdueResidencyOptions {
    uint32_t struct_size;
    uint32_t maximum_pages; /* 0: use physical pool capacity; otherwise also enforce this total budget. */
    double timeout_seconds; /* Positive, preparation only. Default 120 seconds. */
} HdueResidencyOptions;
typedef struct HdueResidencyStatus {
    uint32_t struct_size;
    uint32_t phase;
    uint64_t generation;
    uint64_t required_pages;
    uint64_t resident_pages;
    uint64_t locked_tiles;
    uint64_t required_mappings;
    uint64_t ready_mappings;
    uint32_t textures;
    uint32_t preparation_frames;
    double preparation_ms;
    char message[512];
} HdueResidencyStatus;
enum {
    HDUE_ASSET_UNKNOWN = 0,
    HDUE_ASSET_STATIC_MESH = 1,
    HDUE_ASSET_SKELETAL_MESH = 2,
    HDUE_ASSET_ACTOR = 3,
    HDUE_ASSET_LEVEL = 4,
    HDUE_ASSET_RESOURCE = 5,
    HDUE_ASSET_SPARSE_VOLUME = 6 /* A cooked Sparse Volume Texture, rendered as a Heterogeneous Volume. */
};
enum { HDUE_ASSET_LOADING = 0, HDUE_ASSET_READY = 1, HDUE_ASSET_FAILED = 2 };
typedef struct HdueAssetDesc {
    uint32_t struct_size, visible;
    const char* id;
    const char* asset_path; /* Cooked UE object path; input copied during the call. */
    double transform[16];   /* Native asset cm -> UE world cm, row-major. */
} HdueAssetDesc;
typedef struct HdueAssetStatus {
    uint32_t struct_size, kind, state, actors, meshes, lights;
    double bounds_min[3], bounds_max[3]; /* UE world cm. */
    char message[512];                   /* Caller owned; no allocation crosses the DLL boundary. */
} HdueAssetStatus;
typedef struct HdueAssetSceneStatus {
    uint32_t struct_size, instances, loading, failed, actors, meshes, lights, reserved;
    char message[512];
} HdueAssetSceneStatus;
typedef struct HdueMeshTransform {
    const char* id;
    uint32_t count;
    const double* matrices;
} HdueMeshTransform;
/* set_post_process: overrides for the beauty (never for AOVs). names are FPostProcessSettings property
 * names (BloomIntensity, ColorSaturation, ...) and values their text: numbers, True/False, enum
 * names, object paths, or whitespace/comma-separated numbers filling a struct's fields in order
 * (FVector4 X Y Z W, FLinearColor R G B A). Each listed property also sets its bOverride_ flag.
 * materials are cooked post-process material paths added as blendables with weight 1.
 * tonemapper 0 keeps the beauty linear (default); 1 runs UE's tonemapper, bloom, grading, vignette,
 * grain and lens effects, and the beauty becomes UE's tone-mapped image as display-linear Rec.709 (what UE's sRGB
 * output decodes to). exposure_stops (the host camera's exposure) adds to AutoExposureBias.
 * Names containing a dot set that console variable instead (process-wide, e.g. r.ScreenPercentage, sg.ShadowQuality;
 * True/False become 1/0); a variable dropped from a later set gets back the value it had before hdUE set it.
 * HeightFog True places one UE Exponential Height Fog in the scene (beauty and AOVs alike; absent or False removes it),
 * and HeightFog:<UExponentialHeightFogComponent property> sets its properties as above (RelativeLocation "0 0 Z" its
 * height in UE centimeters; FColor properties take linear R G B[ A] stored as sRGB). The fog is rebuilt only when
 * these change, so volumetric fog keeps its history.
 * Unknown names and unparsable values are skipped with a warning in the UE log. Replaces the
 * previous set; applies to the next request. */
typedef struct HduePostProcessDesc {
    uint32_t struct_size;
    const char* const* names;
    const char* const* values;
    uint32_t count;
    const char* const* materials;
    uint32_t material_count;
    int32_t tonemapper;
    float exposure_stops;
} HduePostProcessDesc;
/* upsert_volume: dense density volumes rendered as UE heterogeneous volumes. density holds
 * width * height * depth floats, x fastest; voxel (i, j, k) covers the unit cube
 * [i, i+1] x [j, j+1] x [k, k+1] of the volume's local space, which transform
 * (row-major, row vectors) maps to UE world centimeters. Extinction per cm is
 * density * extinction_scale. The descriptor and its arrays are caller-owned and
 * copied. */
typedef struct HdueVolumeDesc {
    uint32_t struct_size;
    uint32_t visible;
    const char* id;
    uint32_t width, height, depth;
    uint32_t reserved;
    const float* density;
    double transform[16];
    float albedo[3];
    float extinction_scale;
    /* ABI 16: emission from another field on the same voxels (null: none), shown as UE's BlackBody of
       value * temperature_scale (kelvin) times emission_intensity. */
    const float* emission;
    float emission_intensity;
    float temperature_scale;
} HdueVolumeDesc;
/* upsert_local_fog_volume (ABI 14): one UE Local Fog Volume, as the actor a level places. transform (row-major, row
 * vectors, UE world centimeters) places UE's base volume, a sphere of radius 500 cm. names and values set
 * ULocalFogVolumeComponent properties as set_post_process sets FPostProcessSettings ones; unknown ones are skipped
 * with a warning. remove_asset removes it. Realtime only: UE's Path Tracer does not render local fog volumes. */
typedef struct HdueLocalFogVolumeDesc {
    uint32_t struct_size;
    uint32_t visible;
    const char* id;
    double transform[16];
    const char* const* names;
    const char* const* values;
    uint32_t count;
} HdueLocalFogVolumeDesc;
/* upsert_groom (ABI 16): curves as UE hair strands, a UGroomAsset built at runtime and drawn by a groom component (UE's
 * default hair material). points: point_count xyz in UE world centimeters; widths_cm per point; curve_point_counts per
 * curve, summing to point_count. Guides are generated from every strand (no simulation). apply_scene's removed_ids
 * removes it. */
typedef struct HdueGroomDesc {
    uint32_t struct_size;
    uint32_t visible;
    const char* id;
    uint32_t point_count, curve_count;
    const float* points;
    const float* widths_cm;
    const uint32_t* curve_point_counts;
} HdueGroomDesc;
enum { HDUE_DOME_LIGHT = 4, HDUE_DISK_LIGHT = 5, HDUE_CYLINDER_LIGHT = 6 };
enum {
    HDUE_LIGHT_SHADOWS = 1,
    HDUE_LIGHT_NORMALIZE = 2,
    HDUE_LIGHT_POINT = 4,
    HDUE_LIGHT_BACKGROUND = 8,
    HDUE_LIGHT_MEGALIGHTS = 16
};
enum { HDUE_ENVIRONMENT_AUTOMATIC = 0, HDUE_ENVIRONMENT_LATLONG = 1 };
typedef struct HdueLight {
    uint32_t struct_size, type, visible, flags;
    const char* id;
    const char* texture_file;
    double position_cm[3];
    /* World-space UE vectors for the light's local USD +X,+Y,+Z axes. Dome maps use local +Y as the pole and +Z at
       longitude zero; the delegate folds DomeLight_1 poleAxis (Hydra's domeOffset) into these axes. */
    double axes[9];
    float color[3], intensity;
    float width_cm, height_cm, length_cm, angle_degrees;
    float diffuse, specular, cone_angle, cone_softness;
    uint32_t cube_resolution, texture_format;
    /* UsdLux ShapingAPI IES profile (point, sphere, spot, rect and disk lights); empty for none. */
    const char* ies_file;
    float ies_angle_scale; /* UsdLux theta remap: > 0 theta / s, < 0 (theta - pi) / -s + pi, 0 none */
    uint32_t ies_normalize;
    /* UE lighting channels (bit per channel) of a linked light; 0 for every channel (unlinked). */
    uint32_t lighting_channels;
    /* UE light component properties (unreal:light:<Property>) set by name before the light is registered, after the
       UsdLux translation, so they can override it; values are text as for set_post_process (USD booleans True/False,
       struct fields as bare numbers, objects as cooked paths). Unknown names and bad values are skipped with a warning
       in the UE log. */
    uint32_t property_count;
    const char* const* property_names;
    const char* const* property_values;
    /* ABI 16: an image (unreal:lightFunction:texture) shown as UE's light function on a local or distant light;
       empty for none. */
    const char* light_function_file;
} HdueLight;
/* Light and shadow linking per object (mesh ID): lighting channels in bits 0-2, and NO_SHADOW when the object is
   outside every shadow-casting light's shadowLink. Objects not listed use every channel and cast shadows. */
enum { HDUE_LINK_CHANNELS = 7, HDUE_LINK_NO_SHADOW = 8 };
typedef struct HdueLightBatch {
    uint32_t struct_size, light_count;
    const HdueLight* lights;
    /* With HDUE_LIGHT_BATCH_LINKS, object_ids/object_links replace the whole object table. */
    uint32_t flags, object_count;
    const char* const* object_ids;
    const uint32_t* object_links;
} HdueLightBatch;
enum { HDUE_LIGHT_BATCH_LINKS = 1 };
typedef struct HdueLightingStatus {
    uint32_t struct_size, light_count, dome_count, environment_pending;
    uint32_t mega_lights_project, mega_lights_allowed, mega_lights_active, cube_resolution;
    uint64_t frame_id, generation;
    float environment_max_radiance;
    uint32_t reserved;
} HdueLightingStatus;
enum { HDUE_WET_MASK_PREVIEW = 1u, HDUE_SURFACE_GRAPH = 2u };
/* apply_surfaces: a full snapshot of enabled bindings. An empty snapshot restores dry bindings.
 * geometry_revision starts at 1 and increments on every base mesh upsert in
 * this session, resetting when removed. Masks address expanded mesh vertices.
 * count=0 selects uniform coverage. All data is copied before return. */
typedef struct HdueWetBinding {
    uint32_t struct_size, flags;
    const char* object_id;
    const char* variant_asset;
    const char* preview_asset;
    uint64_t geometry_revision;
    float amount, roughness;
    uint32_t mask_count, reserved;
    const float* mask;
} HdueWetBinding;
typedef struct HdueSurfaceBatch {
    uint32_t struct_size, binding_count;
    const HdueWetBinding* bindings;
} HdueSurfaceBatch;
typedef struct HdueSurfaceStatus {
    uint32_t struct_size, binding_count, upload_pending, reserved;
    uint64_t mask_uploads, mask_bytes, geometry_uploads, updates;
} HdueSurfaceStatus;
#define HDUE_GPU_SLOTS 3u
/* Zero-copy frames for a host that composites with its own graphics API (Houdini's GL
 * viewport), D3D12 RHI only. While enabled, every beauty draw is also written into a ring of
 * HDUE_GPU_SLOTS shared textures: RGBA16F colour (RGB times color_scale, straight alpha) and,
 * with depth, R32F linear axial depth times depth_scale (centimetres to the host's units;
 * FLT_MAX where nothing was hit). Rows are bottom-up (first row = bottom of the image), the
 * layout of a GL texture. After writing a frame, UE's queue signals the shared ready fence with
 * the frame's value. The colour readback (poll_frame/copy_frame/copy_depth) then runs only for
 * converged frames, so CPU consumers still get every converged image.
 *
 * configure_gpu_frames and poll_gpu_frame use the runtime owner thread. acquire_gpu_frame may be called
 * from any thread (the host's graphics thread): it returns the newest frame and keeps UE from
 * writing its slot until the host signals the shared release fence with frame.frame_id, which
 * the host must do right after reading. Pass the last seen frame.ring; when the ring changed
 * (first call, resize, depth toggled), the handle fields hold new NT handles (textures and
 * fences) that the caller owns and must close, else they are null. */
typedef struct HdueGpuOptions {
    uint32_t struct_size;
    uint32_t enabled;
    uint32_t depth;
    float color_scale;
    float depth_scale;
} HdueGpuOptions;
typedef struct HdueGpuFrame {
    uint32_t struct_size;
    uint32_t slot;     /* texture of this frame in the ring */
    uint64_t ring;     /* in: last ring seen; out: current ring */
    uint64_t frame_id; /* ready fence value once the frame is written; increasing */
    uint64_t generation;
    uint32_t width, height, converged, has_depth;
    uint64_t color_bytes, depth_bytes; /* allocation size of each texture, for the import */
    uint64_t color[HDUE_GPU_SLOTS], depth[HDUE_GPU_SLOTS];
    uint64_t ready_fence, release_fence;
    uint8_t adapter_luid[8]; /* LUID of UE's adapter: the importer must use the same GPU */
} HdueGpuFrame;

/* The function table. The delegate and the backend come from the same build: hdue_get_api fills every entry of the
 * table for HDUE_ABI_VERSION or fails (HDUE_ABI_MISMATCH for another version, HDUE_BUFFER_TOO_SMALL when capacity is
 * smaller than sizeof(HdueApi)). Every call runs on the thread that called initialize unless noted. */
typedef struct HdueApi {
    uint32_t struct_size;
    uint32_t abi_version;
    HdueResult(HDUE_CALL* initialize)(const HdueInitDesc* desc);
    HdueResult(HDUE_CALL* get_status)(HdueStatus* status);
    HdueResult(HDUE_CALL* tick)(void);
    HdueResult(HDUE_CALL* create_session)(const HdueSessionDesc* desc, HdueSession* session);
    HdueResult(HDUE_CALL* destroy_session)(HdueSession session);
    /* Resizes the session's render target, keeping its scene. */
    HdueResult(HDUE_CALL* resize_session)(HdueSession session, uint32_t width, uint32_t height);
    HdueResult(HDUE_CALL* request_render)(HdueSession session, const HdueRenderRequest* request);
    HdueResult(HDUE_CALL* poll_frame)(HdueSession session, HdueFrameInfo* frame);
    HdueResult(HDUE_CALL* copy_frame)(HdueSession session, uint64_t frame_id, void* buffer, uint64_t capacity);
    HdueResult(HDUE_CALL* window_message)(uint64_t hwnd, uint32_t message, uint64_t wparam, int64_t lparam,
                                          int64_t* result);
    HdueResult(HDUE_CALL* shutdown)(void);
    HdueResult(HDUE_CALL* apply_scene)(HdueSession session, const HdueSceneBatch* batch);

    /* No Tick/render wait. On a hit, object_id includes a trailing NUL; BUFFER_TOO_SMALL fills hit.required_id_bytes
       without touching object_id. StaticMesh uses CPU-readable resident LOD0, Nanite uses fallback LOD0. */
    HdueResult(HDUE_CALL* pick_native)(HdueSession session, const HdueNativeRay* ray, HdueNativeHit* hit,
                                       char* object_id, uint32_t id_capacity);
    HdueResult(HDUE_CALL* get_pick_material_info)(const char* asset_path, HduePickMaterialInfo* info);

    HdueResult(HDUE_CALL* get_renderer_info)(HdueRendererInfo* info);
    /* Validates the complete batch before changing any value. Successful edits cancel the current render; request a
       newer generation. */
    HdueResult(HDUE_CALL* set_material_parameters)(HdueSession session, const HdueMaterialUpdate* update);
    HdueResult(HDUE_CALL* get_material_stats)(HdueSession session, const char* material_id, HdueMaterialStats* stats);

    /* Configure before request_render; changes cancel the current request. */
    HdueResult(HDUE_CALL* configure_residency)(HdueSession session, const HdueResidencyOptions* options);
    HdueResult(HDUE_CALL* get_residency_status)(HdueSession session, HdueResidencyStatus* status);

    HdueResult(HDUE_CALL* upsert_asset)(HdueSession session, const HdueAssetDesc* desc);
    HdueResult(HDUE_CALL* remove_asset)(HdueSession session, const char* id);
    HdueResult(HDUE_CALL* get_asset_status)(HdueSession session, const char* id, HdueAssetStatus* status);
    HdueResult(HDUE_CALL* get_asset_scene_status)(HdueSession session, HdueAssetSceneStatus* status);

    /* Depth readback. Enable before requesting a generation. copy_depth uses the color frame_id and caller-owned
     * float32 storage: top-down axial camera distance in centimeters, zero for background. A stale color/depth pair
     * must be discarded and polled again. */
    HdueResult(HDUE_CALL* set_depth_enabled)(HdueSession session, uint32_t enabled);
    HdueResult(HDUE_CALL* copy_depth)(HdueSession session, uint64_t frame_id, float* output, uint64_t capacity_bytes);

    /* AOV requests (HdueRenderRequest.aov other than HDUE_AOV_COLOR) render on their own view state and publish into
     * their own frame slot, so the beauty's history, poll_frame, copy_frame and copy_depth are unaffected. poll_aov
     * reports the newest frame of the newest AOV request; copy_frame copies it by its frame_id. AOV generations are
     * independent of beauty generations. */
    HdueResult(HDUE_CALL* poll_aov)(HdueSession session, HdueFrameInfo* frame);
    /* The buffers HDUE_AOV_BUFFERS requests render, all in one raster render (Movie Render Queue's buffer
     * visualization pipes): keys of UE's [Engine.BufferVisualizationMaterials] (BaseColor, WorldNormal, ...) or
     * post-process material paths. Unknown names fail with HDUE_INVALID_ARGUMENT. */
    HdueResult(HDUE_CALL* set_aov_buffers)(HdueSession session, const char* const* names, uint32_t count);
    /* Buffer index (of set_aov_buffers) of the BUFFERS frame frame_id: RGBA float32, top row first, the frame's
     * required_bytes. */
    HdueResult(HDUE_CALL* copy_aov_buffer)(HdueSession session, uint64_t frame_id, uint32_t index, void* pixels,
                                           uint64_t capacity);

    /* Moves already-submitted meshes without resending their geometry, for motion-blur sub-frames.
     * matrices holds count row-major UE world matrices (16 doubles each) in the convention of
     * HdueMeshDesc: count 1 replaces transform, count == instance_count replaces instance_transforms.
     * Unknown ids and mismatched counts are ignored. Applies to the next request. */
    HdueResult(HDUE_CALL* set_transforms)(HdueSession session, const HdueMeshTransform* meshes, uint32_t count);

    HdueResult(HDUE_CALL* set_post_process)(HdueSession session, const HduePostProcessDesc* desc);

    HdueResult(HDUE_CALL* upsert_volume)(HdueSession session, const HdueVolumeDesc* volume);
    HdueResult(HDUE_CALL* remove_volume)(HdueSession session, const char* id);

    HdueResult(HDUE_CALL* upsert_lights)(HdueSession session, const HdueLightBatch* batch);
    HdueResult(HDUE_CALL* get_lighting_status)(HdueSession session, HdueLightingStatus* status);

    HdueResult(HDUE_CALL* apply_surfaces)(HdueSession session, const HdueSurfaceBatch* batch);
    HdueResult(HDUE_CALL* get_surface_status)(HdueSession session, HdueSurfaceStatus* status);

    HdueResult(HDUE_CALL* configure_gpu_frames)(HdueSession session, const HdueGpuOptions* options);
    /* The newest shared frame (frame_id, generation, size, converged) without holding it. */
    HdueResult(HDUE_CALL* poll_gpu_frame)(HdueSession session, HdueFrameInfo* frame);
    /* Any thread (see HdueGpuFrame). */
    HdueResult(HDUE_CALL* acquire_gpu_frame)(HdueSession session, HdueGpuFrame* frame);

    /* ABI 13: changes meshes apply_scene uploaded without resending their geometry; sets each needs_upload. Call after
     * the apply_scene that sent their materials. */
    HdueResult(HDUE_CALL* update_meshes)(HdueSession session, HdueMeshUpdate* meshes, uint32_t count);

    HdueResult(HDUE_CALL* upsert_local_fog_volume)(HdueSession session, const HdueLocalFogVolumeDesc* volume);

    HdueResult(HDUE_CALL* upsert_groom)(HdueSession session, const HdueGroomDesc* groom);
} HdueApi;

/* The backend's one export. Does not initialize the engine. The DLL must remain loaded until process exit. */
typedef HdueResult(HDUE_CALL* HdueGetApiFn)(uint32_t version, uint32_t capacity, HdueApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_api(uint32_t version, uint32_t capacity, HdueApi* api);

#ifdef __cplusplus
}
#endif
#endif
