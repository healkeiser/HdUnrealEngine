#ifndef HDUE_LIGHTING_API_H
#define HDUE_LIGHTING_API_H
#include "hdue/bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HDUE_LIGHTING_ABI_VERSION 3u
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
    /* ABI 3: UE lighting channels (bit per channel) of a linked light; 0 for every channel (unlinked). */
    uint32_t lighting_channels, reserved;
} HdueLight;
/* Light and shadow linking per object (mesh ID): lighting channels in bits 0-2, and NO_SHADOW when the object is
   outside every shadow-casting light's shadowLink. Objects not listed use every channel and cast shadows. */
enum { HDUE_LINK_CHANNELS = 7, HDUE_LINK_NO_SHADOW = 8 };
typedef struct HdueLightBatch {
    uint32_t struct_size, light_count;
    const HdueLight* lights;
    /* ABI 3: with HDUE_LIGHT_BATCH_LINKS, object_ids/object_links replace the whole object table. */
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
typedef struct HdueLightingApi {
    uint32_t struct_size, abi_version;
    HdueResult(HDUE_CALL* upsert)(HdueSession session, const HdueLightBatch* batch);
    HdueResult(HDUE_CALL* get_status)(HdueSession session, HdueLightingStatus* status);
} HdueLightingApi;
typedef HdueResult(HDUE_CALL* HdueGetLightingApiFn)(uint32_t version, uint32_t capacity, HdueLightingApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_lighting_api(uint32_t version, uint32_t capacity, HdueLightingApi* api);
#ifdef __cplusplus
}
#endif
#endif
