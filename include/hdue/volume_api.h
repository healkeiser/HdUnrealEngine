#ifndef HDUE_VOLUME_API_H
#define HDUE_VOLUME_API_H
#include "hdue/bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HDUE_VOLUME_ABI_VERSION 1u
/* Dense density volumes rendered as UE heterogeneous volumes. density holds
 * width * height * depth floats, x fastest; voxel (i, j, k) covers the unit cube
 * [i, i+1] x [j, j+1] x [k, k+1] of the volume's local space, which transform
 * (row-major, row vectors) maps to UE world centimeters. Extinction per cm is
 * density * extinction_scale. The descriptor and its arrays are caller-owned and
 * copied. All calls use the runtime owner thread. */
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
} HdueVolumeDesc;
typedef struct HdueVolumeApi {
    uint32_t struct_size, abi_version;
    HdueResult(HDUE_CALL* upsert)(HdueSession session, const HdueVolumeDesc* volume);
    HdueResult(HDUE_CALL* remove)(HdueSession session, const char* id);
} HdueVolumeApi;
typedef HdueResult(HDUE_CALL* HdueGetVolumeApiFn)(uint32_t version, uint32_t capacity, HdueVolumeApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_volume_api(uint32_t version, uint32_t capacity, HdueVolumeApi* api);
#ifdef __cplusplus
}
#endif
#endif
