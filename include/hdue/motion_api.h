#ifndef HDUE_MOTION_API_H
#define HDUE_MOTION_API_H
#include "hdue/bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HDUE_MOTION_ABI_VERSION 1u
/* Moves already-submitted meshes without resending their geometry, for motion-blur sub-frames.
 * matrices holds count row-major UE world matrices (16 doubles each) in the convention of
 * HdueMeshDesc: count 1 replaces transform, count == instance_count replaces instance_transforms.
 * Unknown ids and mismatched counts are ignored. Owner thread; applies to the next request. */
typedef struct HdueMeshTransform {
    const char* id;
    uint32_t count;
    const double* matrices;
} HdueMeshTransform;
typedef struct HdueMotionApi {
    uint32_t struct_size, abi_version;
    HdueResult(HDUE_CALL* set_transforms)(HdueSession session, const HdueMeshTransform* meshes, uint32_t count);
} HdueMotionApi;
typedef HdueResult(HDUE_CALL* HdueGetMotionApiFn)(uint32_t version, uint32_t capacity, HdueMotionApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_motion_api(uint32_t version, uint32_t capacity, HdueMotionApi* api);
#ifdef __cplusplus
}
#endif
#endif
