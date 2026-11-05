#pragma once
// OpenColorIO at hdUE's boundaries. UE renders in linear Rec.709; Hydra hosts expect the beauty in the OCIO config's
// scene_linear role (ACEScg in ACES pipelines) and author colours in it. Identity when scene_linear is linear Rec.709
// (Houdini's default config), when OCIO cannot relate the config to its built-in linear Rec.709 (no interchange role
// and no space its heuristics know), or with HDUE_COLOR_MANAGEMENT=0.
#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
// Whether converting to the scene_linear colour space is needed.
bool HdUESceneLinearIsRec709();
// Linear Rec.709 RGBA pixels (UE's output) to scene_linear, in place.
void HdUEToSceneLinear(std::vector<float>& rgba);
// Whether the OCIO config's default display and view is a display transform of its own (ACES, AgX), not an sRGB
// encode of scene_linear. UE turns its tone curve off when OCIO does the display (the OCIO viewport display, Movie
// Render Queue's Color Output), so hdUE does too.
bool HdUEViewTransformsDisplay();
// A scene_linear colour (authored in USD) to linear Rec.709 for UE.
GfVec3f HdUEFromSceneLinear(const GfVec3f& color);
// Texture colour-space names (UsdUVTexture sourceColorSpace, MaterialX colorspace): raw data, and sRGB-encoded
// (sRGB, srgb_texture, "sRGB - Texture"; not lin_srgb), which UE decodes to linear Rec.709 itself.
bool HdUERawColorSpace(const std::string& space);
bool HdUESRGBColorSpace(const std::string& space);
// A colour texture as linear Rec.709 for UE: a cached EXR converted through OCIO from space, its authored colour
// space; empty or auto means scene_linear, and only for float images (EXR, HDR). raw, sRGB-encoded and linear
// Rec.709 files, spaces the config does not know, and builds without OCIO return the path unchanged. A UDIM pattern
// (<UDIM> in the name) has each tile converted and returns the converted pattern. Converted files live in
// %TEMP%/hdue_ocio; entries unused for a week are removed.
std::string HdUELinearRec709Texture(const std::string& file, const std::string& space = {});
PXR_NAMESPACE_CLOSE_SCOPE
