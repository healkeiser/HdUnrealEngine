#pragma once
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include <algorithm>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
// The render tags a pass draws, narrowed by UsdRenderSettings includedPurposes (purpose default is Hydra's geometry
// tag). Empty tags draw every tag, so a pass left with none gets one that matches no prim.
inline std::vector<TfToken> HdUEIncludedTags(std::vector<TfToken> tags, const VtArray<TfToken>& purposes) {
    if (purposes.empty()) return tags;
    std::vector<TfToken> allowed;
    for (const auto& purpose : purposes)
        allowed.push_back(purpose == TfToken("default") ? TfToken("geometry") : purpose);
    if (tags.empty()) tags = allowed;
    std::erase_if(tags,
                  [&](const TfToken& tag) { return std::find(allowed.begin(), allowed.end(), tag) == allowed.end(); });
    if (tags.empty()) tags.push_back(TfToken("hdUE:none"));
    return tags;
}
PXR_NAMESPACE_CLOSE_SCOPE
