#ifndef HDUE_AOV_API_H
#define HDUE_AOV_API_H
#include "hdue/bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HDUE_AOV_ABI_VERSION 1u
/* AOV requests (HdueRenderRequest.aov other than HDUE_AOV_COLOR) render on their own view
 * state and publish into their own frame slot, so the beauty's history, poll_frame,
 * copy_frame and copy_depth are unaffected. poll_aov reports the newest frame of the newest
 * AOV request; copy_frame copies it by its frame_id. AOV generations are independent of
 * beauty generations. All calls use the runtime owner thread. */
typedef struct HdueAovApi {
    uint32_t struct_size, abi_version;
    HdueResult(HDUE_CALL* poll_aov)(HdueSession session, HdueFrameInfo* frame);
} HdueAovApi;
typedef HdueResult(HDUE_CALL* HdueGetAovApiFn)(uint32_t version, uint32_t capacity, HdueAovApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_aov_api(uint32_t version, uint32_t capacity, HdueAovApi* api);
#ifdef __cplusplus
}
#endif
#endif
