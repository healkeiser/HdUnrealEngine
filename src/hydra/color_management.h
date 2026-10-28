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
// A linear colour texture (EXR, HDR) authored in scene_linear, as a cached copy in linear Rec.709 for UE; other files
// (8-bit sRGB textures, data) and identity configs return the path unchanged.
std::string HdUELinearRec709Texture(const std::string& file);
PXR_NAMESPACE_CLOSE_SCOPE
