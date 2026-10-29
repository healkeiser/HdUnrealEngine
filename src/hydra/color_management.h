#pragma once
// OpenColorIO at hdUE's boundaries. UE renders in linear Rec.709; Hydra hosts expect the beauty in the OCIO config's
// scene_linear role (ACEScg in ACES pipelines) and author colours in it. Identity when scene_linear is linear Rec.709
// (Houdini's default config), when the config has no linear Rec.709 space, or with HDUE_COLOR_MANAGEMENT=0.
#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
// Name of the scene_linear colour space, and whether converting to it is needed.
const std::string& HdUESceneLinearName();
bool HdUESceneLinearIsRec709();
// Linear Rec.709 RGBA pixels (UE's output) to scene_linear, in place.
void HdUEToSceneLinear(std::vector<float>& rgba);
// A scene_linear colour (authored in USD) to linear Rec.709 for UE.
GfVec3f HdUEFromSceneLinear(const GfVec3f& color);
// Texture colour-space names (UsdUVTexture sourceColorSpace, MaterialX colorspace): raw data, and sRGB-encoded
// (sRGB, srgb_texture, "sRGB - Texture"; not lin_srgb), which UE decodes to linear Rec.709 itself.
bool HdUERawColorSpace(const std::string& space);
bool HdUESRGBColorSpace(const std::string& space);
// A colour texture as linear Rec.709 for UE: a cached EXR converted through OCIO from space, its authored colour
// space; empty or auto means scene_linear, and only for float images (EXR, HDR). raw, sRGB-encoded and linear
// Rec.709 files, spaces the config does not know, and builds without OCIO return the path unchanged.
std::string HdUELinearRec709Texture(const std::string& file, const std::string& space = {});
PXR_NAMESPACE_CLOSE_SCOPE
