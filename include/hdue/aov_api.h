#ifndef HDUE_AOV_API_H
#define HDUE_AOV_API_H
#include "hdue/bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HDUE_AOV_ABI_VERSION 2u
/* AOV requests (HdueRenderRequest.aov other than HDUE_AOV_COLOR) render on their own view
 * state and publish into their own frame slot, so the beauty's history, poll_frame,
 * copy_frame and copy_depth are unaffected. poll_aov reports the newest frame of the newest
 * AOV request; copy_frame copies it by its frame_id. AOV generations are independent of
 * beauty generations. All calls use the runtime owner thread. */
typedef struct HdueAovApi {
    uint32_t struct_size, abi_version;
    HdueResult(HDUE_CALL* poll_aov)(HdueSession session, HdueFrameInfo* frame);
    /* ABI 2. The buffers HDUE_AOV_BUFFERS requests render, all in one raster render (Movie Render Queue's buffer
     * visualization pipes): keys of UE's [Engine.BufferVisualizationMaterials] (BaseColor, WorldNormal, ...) or
     * post-process material paths. Unknown names fail with HDUE_INVALID_ARGUMENT. */
    HdueResult(HDUE_CALL* set_buffers)(HdueSession session, const char* const* names, uint32_t count);
    /* ABI 2. Buffer index (of set_buffers) of the BUFFERS frame frame_id: RGBA float32, top row first, the frame's
     * required_bytes. */
    HdueResult(HDUE_CALL* copy_buffer)(HdueSession session, uint64_t frame_id, uint32_t index, void* pixels,
                                       uint64_t capacity);
} HdueAovApi;
typedef HdueResult(HDUE_CALL* HdueGetAovApiFn)(uint32_t version, uint32_t capacity, HdueAovApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_aov_api(uint32_t version, uint32_t capacity, HdueAovApi* api);
#ifdef __cplusplus
}
#endif
#endif
