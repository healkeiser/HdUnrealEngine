#ifndef HDUE_VIEWPORT_API_H
#define HDUE_VIEWPORT_API_H
#include "hdue/bridge.h"
#include "hdue/material_api.h"
#ifdef HDUE_VIEWPORT_EXPORTS
#define HDUE_VIEWPORT_EXPORT __declspec(dllexport)
#else
#define HDUE_VIEWPORT_EXPORT __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Host-side viewport extension, exported by the Houdini delegate DLL.
 * Call on the Houdini UI thread. No ownership crosses this boundary. */
typedef struct HdueViewportStatus {
    uint32_t struct_size, active, create_thread, execute_thread;
    HdueSession session;
    uint64_t generation, published_frames, scene_submissions;
    uint32_t mode, meshes, materials, lights;
    HdueFrameInfo frame;
    char error[512];
} HdueViewportStatus;
HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_get_viewport_status(HdueViewportStatus* status);
/* The viewed stage's up axis (0 Y, 1 Z), which Houdini does not pass to the viewport's delegate. Render mode,
 * samples, units and time come through Hydra (the render settings prim, stageMetersPerUnit, houdini:frame). */
HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_up_axis(uint32_t z_up);
/* Called by the host on the thread that owns UE, after each tick: runs UE work that delegates executing on
 * other threads (Live Render) queued, and records this thread as the owner. */
HDUE_VIEWPORT_EXPORT void HDUE_CALL hdue_run_ui_tasks(void);
/* Runs task(context) on the thread that owns UE and returns when it has run; HDUE_INVALID_STATE if that thread did
 * not run it. Hosts call owner-only backend functions through it while hdUE runs UE on its own thread (Houdini's
 * UI). Any thread except one the UE thread waits for; on the UE thread itself it runs inline. */
HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_run_on_ue_thread(void(HDUE_CALL* task)(void*), void* context);
typedef struct HdueViewportMaterial {
    const char* material_id;
    const char* native_asset;
    uint32_t parameter_count, reserved;
    const HdueMaterialParameter* parameters;
} HdueViewportMaterial;
typedef struct HdueViewportMaterials {
    uint32_t struct_size, abi_version;
    HdueSession session;
    uint32_t material_count, reserved;
    const HdueViewportMaterial* materials;
} HdueViewportMaterials;
/* Version 1. Replace the complete prepared-material binding set atomically.
 * Stale sessions and invalid batches leave existing settings untouched. */
HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_materials(const HdueViewportMaterials* materials);
typedef struct HdueViewportWet {
    const char* prim_path;
    const char* variant_asset;
    const char* preview_asset;
    const char* mask_primvar;
    float amount, roughness;
    uint32_t flags, reserved;
} HdueViewportWet;
typedef struct HdueViewportSurfaces {
    uint32_t struct_size, abi_version;
    HdueSession session;
    uint32_t binding_count, mask_preview;
    const HdueViewportWet* bindings;
    const char* validation_error;
} HdueViewportSurfaces;
/* An error blocks new wet convergence until fixed, preserving the last image. */
HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_surfaces(const HdueViewportSurfaces* surfaces);
typedef struct HdueViewportTexture {
    const char* uri;  /* op: or opdef: path as authored in USD */
    const char* file; /* image the host exported for it */
} HdueViewportTexture;
typedef struct HdueViewportTextures {
    uint32_t struct_size, abi_version;
    HdueSession session;
    uint32_t texture_count, reserved;
    const HdueViewportTexture* textures;
} HdueViewportTextures;
/* Version 1. Replace the complete set of host-exported images for op:/opdef: texture paths (Houdini COPs and
 * embedded HDA images), which only the host can evaluate. Materials and lights re-read their textures. */
HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_textures(const HdueViewportTextures* textures);
#ifdef __cplusplus
}
#endif
#endif
