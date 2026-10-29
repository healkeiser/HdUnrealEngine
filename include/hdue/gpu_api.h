#ifndef HDUE_GPU_API_H
#define HDUE_GPU_API_H
#include "hdue/bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HDUE_GPU_ABI_VERSION 1u
#define HDUE_GPU_SLOTS 3u
/* Optional zero-copy frames for a host that composites with its own graphics API (Houdini's GL
 * viewport), D3D12 RHI only. While enabled, every beauty draw is also written into a ring of
 * HDUE_GPU_SLOTS shared textures: RGBA16F colour (RGB times color_scale, straight alpha) and,
 * with depth, R32F linear axial depth times depth_scale (centimetres to the host's units;
 * FLT_MAX where nothing was hit). Rows are bottom-up (first row = bottom of the image), the
 * layout of a GL texture. After writing a frame, UE's queue signals the shared ready fence with
 * the frame's value. The colour readback (poll_frame/copy_frame/copy_depth) then runs only for
 * converged frames, so CPU consumers still get every converged image.
 *
 * configure and poll_gpu_frame use the runtime owner thread. acquire_gpu_frame may be called
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
typedef struct HdueGpuApi {
    uint32_t struct_size, abi_version;
    HdueResult(HDUE_CALL* configure)(HdueSession session, const HdueGpuOptions* options);
    /* The newest shared frame (frame_id, generation, size, converged) without holding it. */
    HdueResult(HDUE_CALL* poll_gpu_frame)(HdueSession session, HdueFrameInfo* frame);
    HdueResult(HDUE_CALL* acquire_gpu_frame)(HdueSession session, HdueGpuFrame* frame);
} HdueGpuApi;
typedef HdueResult(HDUE_CALL* HdueGetGpuApiFn)(uint32_t version, uint32_t capacity, HdueGpuApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_gpu_api(uint32_t version, uint32_t capacity, HdueGpuApi* api);
#ifdef __cplusplus
}
#endif
#endif
