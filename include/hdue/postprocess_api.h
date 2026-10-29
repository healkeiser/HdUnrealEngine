#ifndef HDUE_POSTPROCESS_API_H
#define HDUE_POSTPROCESS_API_H
#include "hdue/bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HDUE_POSTPROCESS_ABI_VERSION 1u
/* Post-process overrides for the beauty (never for AOVs). names are FPostProcessSettings property
 * names (BloomIntensity, ColorSaturation, ...) and values their text: numbers, True/False, enum
 * names, object paths, or whitespace/comma-separated numbers filling a struct's fields in order
 * (FVector4 X Y Z W, FLinearColor R G B A). Each listed property also sets its bOverride_ flag.
 * materials are cooked post-process material paths added as blendables with weight 1.
 * tonemapper 0 keeps the beauty linear (default); 1 runs UE's tonemapper, bloom, grading, vignette,
 * grain and lens effects and makes the beauty display-referred.
 * Names containing a dot set that console variable instead (process-wide, e.g. r.ScreenPercentage, sg.ShadowQuality;
 * True/False become 1/0); a variable dropped from a later set gets back the value it had before hdUE set it.
 * Unknown names and unparsable values are skipped with a warning in the UE log. Replaces the
 * previous set; owner thread; applies to the next request. */
typedef struct HduePostProcessDesc {
    uint32_t struct_size;
    const char* const* names;
    const char* const* values;
    uint32_t count;
    const char* const* materials;
    uint32_t material_count;
    int32_t tonemapper;
} HduePostProcessDesc;
typedef struct HduePostProcessApi {
    uint32_t struct_size, abi_version;
    HdueResult (HDUE_CALL *set_post_process)(HdueSession session, const HduePostProcessDesc* desc);
} HduePostProcessApi;
typedef HdueResult (HDUE_CALL *HdueGetPostProcessApiFn)(uint32_t version, uint32_t capacity, HduePostProcessApi* api);
HDUE_EXPORT HdueResult HDUE_CALL hdue_get_postprocess_api(uint32_t version, uint32_t capacity, HduePostProcessApi* api);
#ifdef __cplusplus
}
#endif
#endif
