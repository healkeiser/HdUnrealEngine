#include "render_tags.h"
#include <iostream>
PXR_NAMESPACE_USING_DIRECTIVE
// HdUEIncludedTags applies UsdRenderSettings includedPurposes to a render pass's tags.
int main() {
    const TfToken geometry("geometry"), render("render"), proxy("proxy");
    struct Case {
        std::vector<TfToken> tags;
        VtArray<TfToken> purposes;
        std::vector<TfToken> expected;
        const char* what;
    } cases[] = {
        {{geometry, render}, {}, {geometry, render}, "no includedPurposes keeps the pass's tags"},
        {{geometry, render}, {TfToken("default")}, {geometry}, "default is the geometry tag"},
        {{}, {TfToken("default"), render}, {geometry, render}, "a pass drawing every tag gets the purposes"},
        {{geometry, render}, {proxy}, {TfToken("unreal:none")}, "no tag left draws nothing, not everything"},
    };
    int failures = 0;
    for (const auto& c : cases)
        if (HdUEIncludedTags(c.tags, c.purposes) != c.expected) {
            std::cerr << "HdUEIncludedTags wrong: " << c.what << '\n';
            ++failures;
        }
    return failures;
}
