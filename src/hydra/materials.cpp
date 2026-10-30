// Material sprims: UsdPreviewSurface and MaterialX surfaces (standard_surface, OpenPBR) read into HdUEMaterialData.
#include "delegate.h"
#include "color_management.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/stringUtils.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <tuple>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
// Numeric types convert (an int thin_walled, a double ior); a colour on a scalar input uses its mean (opacity).
template <class T>
T value(const VtValue& v, T fallback) {
    if constexpr (std::is_same_v<T, float>)
        if (v.IsHolding<GfVec3f>()) {
            const auto c = v.UncheckedGet<GfVec3f>();
            return (c[0] + c[1] + c[2]) / 3;
        }
    if (v.IsHolding<T>()) return v.UncheckedGet<T>();
    return v.CanCast<T>() ? VtValue::Cast<T>(v).template UncheckedGet<T>() : fallback;
}
TfToken token(const VtValue& v) {
    if (v.IsHolding<TfToken>()) return v.UncheckedGet<TfToken>();
    return v.IsHolding<std::string>() ? TfToken(v.UncheckedGet<std::string>()) : TfToken();
}
VtValue param(const HdMaterialNode2& node, const char* name) {
    const auto it = node.parameters.find(TfToken(name));
    return it == node.parameters.end() ? VtValue() : it->second;
}
const HdMaterialConnection2* connection(const HdMaterialNode2& node, const char* input) {
    const auto it = node.inputConnections.find(TfToken(input));
    return it == node.inputConnections.end() || it->second.empty() ? nullptr : &it->second.front();
}
// A scalar broadcasts to all four channels; a vector3 gets w = 1.
GfVec4f vec4(const VtValue& v) {
    if (v.IsHolding<GfVec3f>()) {
        const auto c = v.UncheckedGet<GfVec3f>();
        return GfVec4f(c[0], c[1], c[2], 1);
    }
    return v.IsHolding<GfVec4f>() ? v.UncheckedGet<GfVec4f>() : GfVec4f(value(v, 1.f));
}
GfVec4f mul(const GfVec4f& a, const GfVec4f& b) { return GfCompMult(a, b); }
void warnOnce(const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string> warned;
    std::lock_guard lock(mutex);
    if (warned.insert(message).second) TF_WARN("hdUE: %s", message.c_str());
}
HdMaterialNetwork2 network(HdSceneDelegate* delegate, const SdfPath& id) {
    const auto resource = delegate->GetMaterialResource(id);
    if (resource.IsHolding<HdMaterialNetwork2>()) return resource.UncheckedGet<HdMaterialNetwork2>();
    if (resource.IsHolding<HdMaterialNetworkMap>())
        return HdConvertToHdMaterialNetwork2(resource.UncheckedGet<HdMaterialNetworkMap>());
    return {};
}
// USD/MaterialX texture-space scale, rotation (degrees) and translation, as an affine in UE's flipped-V UV space.
std::array<float, 6> uvAffine(GfVec2f scale, float degrees, GfVec2f translation) {
    const double radians = degrees * 3.14159265358979323846 / 180;
    // USD order: scale, then rotate, then translate.
    const auto usd = [&](GfVec2f p) {
        p = GfVec2f(p[0] * scale[0], p[1] * scale[1]);
        return GfVec2f(float(std::cos(radians) * p[0] - std::sin(radians) * p[1]),
                       float(std::sin(radians) * p[0] + std::cos(radians) * p[1])) +
               translation;
    };
    const auto ue = [&](float u, float v) {
        const auto p = usd(GfVec2f(u, 1 - v));
        return GfVec2f(p[0], 1 - p[1]);
    };
    const auto origin = ue(0, 0), du = ue(1, 0) - origin, dv = ue(0, 1) - origin;
    return {du[0], dv[0], origin[0], du[1], dv[1], origin[1]};
}

// What drives a shader input: an image, with the nodes between them folded into value = texel * scale + bias (and a
// channel), or a constant, transformed the same way.
struct Source {
    const HdMaterialNode2* image = nullptr;
    VtValue constant;
    HdUETextureData texture;
    int channel = -1;
    bool primvar = false;  // a vec3 primvar reader: the mesh's vertex colours
};
// The vec3 primvar a reader node reads (UsdPrimvarReader_float3 varname, MaterialX geompropvalue geomprop); empty
// otherwise.
TfToken colorPrimvar(const HdMaterialNode2& node) {
    const auto& type = node.nodeTypeId.GetString();
    if (type == "UsdPrimvarReader_float3") return token(param(node, "varname"));
    return TfStringStartsWith(type, "ND_geompropvalue_color3") || TfStringStartsWith(type, "ND_geompropvalue_vector3")
               ? token(param(node, "geomprop"))
               : TfToken();
}
// The UV primvar a coordinate reader reads (UsdPrimvarReader_float2 varname, MaterialX geompropvalue geomprop:
// exporters name it map1, UVMap, ...); empty otherwise.
TfToken uvPrimvar(const HdMaterialNode2& node) {
    if (node.nodeTypeId == TfToken("UsdPrimvarReader_float2")) return token(param(node, "varname"));
    return node.nodeTypeId == TfToken("ND_geompropvalue_vector2") ? token(param(node, "geomprop")) : TfToken();
}
// The UV primvars a material reads, in node order: its meshes' UV sets 0, 1, ...
TfTokenVector uvPrimvars(const HdMaterialNetwork2& network) {
    TfTokenVector names;
    for (const auto& [path, node] : network.nodes)
        if (const auto name = uvPrimvar(node);
            !name.IsEmpty() && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    return names;
}
class SurfaceReader {
public:
    SurfaceReader(const HdMaterialNetwork2& network, const HdMaterialNode2& shader, HdUEMaterialData& data)
        : network_(network), shader_(shader), data_(data), uvNames_(uvPrimvars(network)) {}
    // Reads an input's constant into out (which holds the fallback). With texture, an image driving the input fills it
    // and returns true; without, a textured input keeps its constant, with a warning. Colour images are converted
    // from their authored colour space (OCIO) to linear Rec.709.
    template <class T>
    bool read(const char* input, T& out, HdUETextureData* texture = nullptr, bool color = false) {
        out = value(param(shader_, input), out);
        const auto* link = connection(shader_, input);
        if (!link) return false;
        Source source;
        trace(*link, source);
        if (source.primvar) {
            // The base colour multiplies the mesh's vertex colours (HDUE_MATERIAL_VERTEX_COLOR), which carry the
            // primvar.
            if constexpr (std::is_same_v<T, GfVec3f>)
                if (texture == &data_.textures[HDUE_TEXTURE_BASE_COLOR]) {
                    data_.vertexColor = true;
                    out = transformed(GfVec3f(1), source.texture);
                    return false;
                }
            warnOnce(TfStringPrintf("primvar readers drive only the base colour; %s uses its constant", input));
            return false;
        }
        if (!source.image) {
            if (!source.constant.IsEmpty()) out = transformed(value(source.constant, out), source.texture);
            return false;
        }
        if (!texture) {
            warnOnce(TfStringPrintf("textured %s inputs are not translated; %s uses its constant", input,
                                    shader_.nodeTypeId.GetText()));
            return false;
        }
        *texture = source.texture;
        texture->uvSet = uvSet(*source.image);
        // A host image (op:) exists only once the host exported it: converted when the scene is packed.
        if (color) {
            texture->convert = true;
            if (!HdUEHostTexture(texture->file))
                texture->file = HdUELinearRec709Texture(texture->file, texture->colorSpace);
        }
        if (source.channel >= 0) {
            texture->mask = GfVec4f(0);
            texture->mask[source.channel % 4] = 1;
        }
        // ponytail: one UV transform per material, from the first textured input; per-texture transforms if assets mix
        // them.
        if (!uvSet_) uvSet_ = uvTransform(*source.image, data_.uvTransform);
        return true;
    }
    template <class T>
    bool read(const char* input, T& out, int slot) {
        return read(input, out, &data_.textures[slot]);
    }
    // Colours: authored in the OCIO scene_linear role, while UE works in linear Rec.709.
    bool color(const char* input, GfVec3f& out, HdUETextureData* texture = nullptr) {
        const bool textured = read(input, out, texture, true);
        out = HdUEFromSceneLinear(out);
        return textured;
    }
    bool color(const char* input, GfVec3f& out, int slot) { return color(input, out, &data_.textures[slot]); }

private:
    static float transformed(float v, const HdUETextureData& t) { return v * t.scale[0] + t.bias[0]; }
    static GfVec3f transformed(const GfVec3f& v, const HdUETextureData& t) {
        return GfVec3f(v[0] * t.scale[0] + t.bias[0], v[1] * t.scale[1] + t.bias[1], v[2] * t.scale[2] + t.bias[2]);
    }
    template <class T>
    static T transformed(const T& v, const HdUETextureData&) {
        return v;
    }
    // Walks upstream through the nodes that only rescale or reselect a value. Value so far = x * scale + bias, where x
    // is the upstream node's output: each node replaces x with its own affine of its input.
    void trace(const HdMaterialConnection2& link, Source& source) {
        const auto it = network_.nodes.find(link.upstreamNode);
        if (it == network_.nodes.end()) return;
        const auto& node = it->second;
        const auto& type = node.nodeTypeId.GetString();
        auto& t = source.texture;
        const auto affine = [&](const GfVec4f& scale, const GfVec4f& bias) {
            t.bias += mul(t.scale, bias);
            t.scale = mul(t.scale, scale);
        };
        const auto upstream = [&](const char* input) {
            if (const auto* next = connection(node, input))
                trace(*next, source);
            else
                source.constant = param(node, input);
        };
        const auto starts = [&](const char* prefix) { return TfStringStartsWith(type, prefix); };
        if (type == "UsdUVTexture" || starts("ND_image_") || starts("ND_tiledimage_")) {
            const auto file = param(node, "file");
            if (!file.IsHolding<SdfAssetPath>()) return;
            const auto& asset = file.UncheckedGet<SdfAssetPath>();
            t.file = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
            if (t.file.empty()) return;
            source.image = &node;
            const bool usd = type == "UsdUVTexture";
            // UsdUVTexture's sourceColorSpace (raw, sRGB, auto), else the file's colorSpace metadata, which Hydra
            // passes as colorSpace:file, as it does MaterialX's colorspace attribute (srgb_texture, lin_rec709,
            // acescg, ...). It selects sRGB decoding of 8-bit texels (auto: by slot) and, for colour inputs, the OCIO
            // conversion.
            t.colorSpace = usd ? token(param(node, "sourceColorSpace")).GetString() : "";
            if (t.colorSpace.empty() || TfStringToLower(t.colorSpace) == "auto")
                t.colorSpace = token(param(node, "colorSpace:file")).GetString();
            const auto space = TfStringToLower(t.colorSpace);
            t.encoding = HdUESRGBColorSpace(space)          ? HDUE_TEXTURE_ENCODING_SRGB
                         : space.empty() || space == "auto" ? HDUE_TEXTURE_ENCODING_AUTO
                                                            : HDUE_TEXTURE_ENCODING_LINEAR;
            // wrapS/wrapT; MaterialX u/vaddressmode (constant reads its default, 0 unless authored: black).
            // ponytail: useMetadata (UsdUVTexture's default) does not read the file's wrap metadata; it falls back
            // to black, as the spec says for files without it.
            const auto wrap = [&](const char* input, const char* fallback) {
                auto mode = token(param(node, input)).GetString();
                if (mode.empty()) mode = fallback;
                return mode == "repeat" || mode == "periodic" ? HDUE_WRAP_REPEAT
                       : mode == "clamp"                      ? HDUE_WRAP_CLAMP
                       : mode == "mirror"                     ? HDUE_WRAP_MIRROR
                                                              : HDUE_WRAP_BLACK;
            };
            if (!starts("ND_tiledimage_")) {
                t.wrap[0] = wrap(usd ? "wrapS" : "uaddressmode", usd ? "useMetadata" : "periodic");
                t.wrap[1] = wrap(usd ? "wrapT" : "vaddressmode", usd ? "useMetadata" : "periodic");
            }
            if (usd) {
                affine(value(param(node, "scale"), GfVec4f(1)), value(param(node, "bias"), GfVec4f(0)));
                // A single-channel output (r, g, b, a) selects that channel; rgb keeps all.
                const auto& output = link.upstreamOutputName.GetString();
                if (output.size() == 1 && std::string("rgba").find(output[0]) != std::string::npos)
                    source.channel = int(std::string("rgba").find(output[0]));
            }
            return;
        }
        if (!colorPrimvar(node).IsEmpty())
            source.primvar = true;
        else if (starts("ND_constant_"))
            source.constant = param(node, "value");
        else if (starts("ND_convert_") || starts("ND_dot_"))
            upstream("in");
        else if (starts("ND_extract_")) {
            source.channel = value(param(node, "index"), 0);
            upstream("in");
        } else if (starts("ND_normalmap")) {
            // Tangent-space decode, xy scaled by the node's scale: n = (2 texel - 1) * (k, k, 1).
            const float k = value(param(node, "scale"), 1.f);
            affine(GfVec4f(2 * k, 2 * k, 2, 1), GfVec4f(-k, -k, -1, 0));
            upstream("in");
        } else if (starts("ND_multiply_") && (!connection(node, "in1") || !connection(node, "in2"))) {
            const char* driven = connection(node, "in1") ? "in1" : "in2";
            affine(vec4(param(node, driven[2] == '1' ? "in2" : "in1")), GfVec4f(0));
            upstream(driven);
        } else
            warnOnce(TfStringPrintf("MaterialX node %s is not translated; inputs it drives use their constant",
                                    type.c_str()));
    }
    // The texture coordinate transform an image applies; false when it reads no coordinates, so a later image may.
    bool uvTransform(const HdMaterialNode2& image, std::array<float, 6>& out) const {
        if (TfStringStartsWith(image.nodeTypeId.GetString(), "ND_tiledimage_")) {
            out = uvAffine(value(param(image, "uvtiling"), GfVec2f(1)), 0, value(param(image, "uvoffset"), GfVec2f(0)));
            return true;
        }
        const auto* st = connection(image, image.nodeTypeId == TfToken("UsdUVTexture") ? "st" : "texcoord");
        if (!st) return false;
        const auto node = network_.nodes.find(st->upstreamNode);
        if (node == network_.nodes.end()) return true;
        if (node->second.nodeTypeId == TfToken("UsdTransform2d"))
            out = uvAffine(value(param(node->second, "scale"), GfVec2f(1)), value(param(node->second, "rotation"), 0.f),
                           value(param(node->second, "translation"), GfVec2f(0)));
        else if (TfStringStartsWith(node->second.nodeTypeId.GetString(), "ND_place2d"))
            warnOnce("MaterialX place2d is not translated; textures use their mesh UVs");
        return true;
    }
    // The UV set an image reads: its st / texcoord reader's primvar, also through a UsdTransform2d; 0 unconnected.
    uint32_t uvSet(const HdMaterialNode2& image) const {
        const auto* st = connection(image, image.nodeTypeId == TfToken("UsdUVTexture") ? "st" : "texcoord");
        for (int hop = 0; st && hop < 2; ++hop) {
            const auto node = network_.nodes.find(st->upstreamNode);
            if (node == network_.nodes.end()) break;
            if (const auto name = uvPrimvar(node->second); !name.IsEmpty()) {
                const auto set = size_t(std::find(uvNames_.begin(), uvNames_.end(), name) - uvNames_.begin());
                if (set < 4) return uint32_t(set);
                warnOnce("materials read up to 4 UV primvars; textures reading others use the first");
                return 0;
            }
            st = connection(node->second, "in");  // UsdTransform2d
        }
        return 0;
    }
    const HdMaterialNetwork2& network_;
    const HdMaterialNode2& shader_;
    HdUEMaterialData& data_;
    const TfTokenVector uvNames_;
    bool uvSet_ = false;
};

void readPreviewSurface(SurfaceReader& r, HdUEMaterialData& data) {
    r.color("diffuseColor", data.color, HDUE_TEXTURE_BASE_COLOR);
    r.color("emissiveColor", data.emissive, HDUE_TEXTURE_EMISSIVE);
    r.read("metallic", data.metallic, HDUE_TEXTURE_METALLIC);
    r.read("roughness", data.roughness, HDUE_TEXTURE_ROUGHNESS);
    r.read("opacity", data.opacity, HDUE_TEXTURE_OPACITY);
    r.read("opacityThreshold", data.opacityThreshold);
    GfVec3f normal(0, 0, 1);
    r.read("normal", normal, HDUE_TEXTURE_NORMAL);
    r.read("displacement", data.displacement, HDUE_TEXTURE_DISPLACEMENT);
    r.read("occlusion", data.occlusion, HDUE_TEXTURE_OCCLUSION);
    r.read("clearcoat", data.clearcoat, HDUE_TEXTURE_CLEARCOAT);
    r.read("clearcoatRoughness", data.clearcoatRoughness, HDUE_TEXTURE_CLEARCOAT_ROUGHNESS);
    r.read("ior", data.ior);
    r.color("specularColor", data.specularColor, HDUE_TEXTURE_SPECULAR_COLOR);
    int specularWorkflow = 0;
    r.read("useSpecularWorkflow", specularWorkflow);
    data.specularWorkflow = specularWorkflow != 0;
    // The metalness workflow ignores specularColor. A clear coat renders through the layered Standard Surface parent,
    // whose specular_color tints the base's specular: white.
    if (!data.specularWorkflow) {
        data.specularColor = GfVec3f(1);
        data.textures[HDUE_TEXTURE_SPECULAR_COLOR] = {};
    }
}

// standard_surface and OpenPBR share their model; only input names and defaults differ.
template <class T>
struct Input {
    const char* name;
    T fallback;
};
struct MaterialXSurface {
    Input<float> base;
    Input<GfVec3f> baseColor;
    Input<float> metalness, specular;
    Input<GfVec3f> specularColor;
    Input<float> specularRoughness, ior, coat;
    Input<GfVec3f> coatColor;
    Input<float> coatRoughness, sheen;
    Input<GfVec3f> sheenColor;
    Input<float> sheenRoughness, subsurface;
    Input<GfVec3f> subsurfaceColor, subsurfaceRadius;
    Input<float> subsurfaceScale, transmission, emission;
    Input<GfVec3f> emissionColor;
    Input<float> opacity;
    Input<bool> thinWalled;
    const char* normal;
};
const MaterialXSurface standardSurface{{"base", .8f},
                                       {"base_color", GfVec3f(1)},
                                       {"metalness", 0},
                                       {"specular", 1},
                                       {"specular_color", GfVec3f(1)},
                                       {"specular_roughness", .2f},
                                       {"specular_IOR", 1.5f},
                                       {"coat", 0},
                                       {"coat_color", GfVec3f(1)},
                                       {"coat_roughness", .1f},
                                       {"sheen", 0},
                                       {"sheen_color", GfVec3f(1)},
                                       {"sheen_roughness", .3f},
                                       {"subsurface", 0},
                                       {"subsurface_color", GfVec3f(1)},
                                       {"subsurface_radius", GfVec3f(1)},
                                       {"subsurface_scale", 1},
                                       {"transmission", 0},
                                       {"emission", 0},
                                       {"emission_color", GfVec3f(1)},
                                       {"opacity", 1},
                                       {"thin_walled", false},
                                       "normal"};
// OpenPBR's subsurface_radius is the scalar distance and subsurface_radius_scale the per-channel colour.
const MaterialXSurface openPbr{{"base_weight", 1},
                               {"base_color", GfVec3f(.8f)},
                               {"base_metalness", 0},
                               {"specular_weight", 1},
                               {"specular_color", GfVec3f(1)},
                               {"specular_roughness", .3f},
                               {"specular_ior", 1.5f},
                               {"coat_weight", 0},
                               {"coat_color", GfVec3f(1)},
                               {"coat_roughness", 0},
                               {"fuzz_weight", 0},
                               {"fuzz_color", GfVec3f(1)},
                               {"fuzz_roughness", .5f},
                               {"subsurface_weight", 0},
                               {"subsurface_color", GfVec3f(.8f)},
                               {"subsurface_radius_scale", GfVec3f(1, .5f, .25f)},
                               {"subsurface_radius", 1},
                               {"transmission_weight", 0},
                               {"emission_luminance", 0},
                               {"emission_color", GfVec3f(1)},
                               {"geometry_opacity", 1},
                               {"geometry_thin_walled", false},
                               "geometry_normal"};

// Multiplies a slot's constant and texture by a weight (base, specular and emission only scale their colours).
void weight(HdUEMaterialData& data, int slot, GfVec3f& constant, float weight) {
    constant *= weight;
    auto& t = data.textures[slot];
    for (int i = 0; i < 3; ++i) {
        t.scale[i] *= weight;
        t.bias[i] *= weight;
    }
}
// Subsurface and sheen colours reuse the base colour texture when they read the same image (the usual authoring);
// their tint is then relative to the base colour slot. Other images fall back to the input's constant.
bool baseTextureTint(const HdUEMaterialData& data, const HdUETextureData& texture, GfVec3f& tint, const char* input) {
    const auto& base = data.textures[HDUE_TEXTURE_BASE_COLOR];
    if (texture.file != base.file) {
        warnOnce(TfStringPrintf("%s reads an image other than base_color's; using its constant", input));
        return false;
    }
    for (int i = 0; i < 3; ++i) tint[i] = base.scale[i] != 0 ? texture.scale[i] / base.scale[i] : 1;
    return true;
}
void readMaterialX(SurfaceReader& r, HdUEMaterialData& data, const MaterialXSurface& s) {
    const auto read = [&](const auto& input, auto& out, auto... slot) {
        out = input.fallback;
        return r.read(input.name, out, slot...);
    };
    const auto color = [&](const Input<GfVec3f>& input, GfVec3f& out, auto... slot) {
        out = input.fallback;
        return r.color(input.name, out, slot...);
    };
    float base, specular, subsurfaceScale, transmission, emission;
    read(s.base, base);
    color(s.baseColor, data.color, HDUE_TEXTURE_BASE_COLOR);
    weight(data, HDUE_TEXTURE_BASE_COLOR, data.color, base);
    read(s.metalness, data.metallic, HDUE_TEXTURE_METALLIC);
    read(s.specular, specular);
    color(s.specularColor, data.specularColor, HDUE_TEXTURE_SPECULAR_COLOR);
    weight(data, HDUE_TEXTURE_SPECULAR_COLOR, data.specularColor, specular);
    read(s.specularRoughness, data.roughness, HDUE_TEXTURE_ROUGHNESS);
    read(s.ior, data.ior);
    read(s.coat, data.clearcoat, HDUE_TEXTURE_CLEARCOAT);
    color(s.coatColor, data.coatColor);
    read(s.coatRoughness, data.clearcoatRoughness, HDUE_TEXTURE_CLEARCOAT_ROUGHNESS);
    HdUETextureData texture;
    read(s.sheen, data.sheen);
    if (color(s.sheenColor, data.sheenColor, &texture))
        data.sheenBaseTexture = baseTextureTint(data, texture, data.sheenColor, s.sheenColor.name);
    read(s.sheenRoughness, data.sheenRoughness);
    read(s.subsurface, data.subsurface);
    if (color(s.subsurfaceColor, data.subsurfaceColor, &texture))
        data.subsurfaceBaseTexture = baseTextureTint(data, texture, data.subsurfaceColor, s.subsurfaceColor.name);
    read(s.subsurfaceRadius, data.subsurfaceRadius);
    read(s.subsurfaceScale, subsurfaceScale);
    data.subsurfaceRadius *= subsurfaceScale;
    read(s.emission, emission);
    color(s.emissionColor, data.emissive, HDUE_TEXTURE_EMISSIVE);
    weight(data, HDUE_TEXTURE_EMISSIVE, data.emissive, emission);
    // MaterialX opacity is coverage without a threshold: an image cuts out at 0.5 (as UE's Interchange imports it),
    // a constant below 1 is see-through. Transmission is approximated as see-through coverage.
    if (read(s.opacity, data.opacity, HDUE_TEXTURE_OPACITY)) data.opacityThreshold = .5f;
    read(s.transmission, transmission);
    data.opacity *= 1 - std::clamp(transmission, 0.f, 1.f);
    read(s.thinWalled, data.thinWalled);
    GfVec3f normal(0, 0, 1);
    r.read(s.normal, normal, HDUE_TEXTURE_NORMAL);
}
// The shader behind the surface terminal; else, for networks without terminals, the first known surface.
const HdMaterialNode2* surface(const HdMaterialNetwork2& network) {
    const auto terminal = network.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal != network.terminals.end()) {
        const auto node = network.nodes.find(terminal->second.upstreamNode);
        return node == network.nodes.end() ? nullptr : &node->second;
    }
    for (const auto& [path, node] : network.nodes)
        if (node.nodeTypeId == TfToken("UsdPreviewSurface")) return &node;
    return nullptr;
}
// MaterialX displacement (ND_displacement_float): displacement * scale along the normal. UsdPreviewSurface reads its
// own.
void readDisplacement(const HdMaterialNetwork2& network, HdUEMaterialData& data) {
    const auto terminal = network.terminals.find(HdMaterialTerminalTokens->displacement);
    const auto node =
        terminal == network.terminals.end() ? network.nodes.end() : network.nodes.find(terminal->second.upstreamNode);
    if (node == network.nodes.end() || !TfStringStartsWith(node->second.nodeTypeId.GetString(), "ND_displacement_"))
        return;
    if (node->second.nodeTypeId != TfToken("ND_displacement_float")) {
        warnOnce("MaterialX vector displacement is not translated");
        return;
    }
    SurfaceReader(network, node->second, data).read("displacement", data.displacement, HDUE_TEXTURE_DISPLACEMENT);
    const float scale = value(param(node->second, "scale"), 1.f);
    auto& texture = data.textures[HDUE_TEXTURE_DISPLACEMENT];
    data.displacement *= scale;
    texture.scale *= scale;
    texture.bias *= scale;
}
void readSurface(const HdMaterialNetwork2& network, const HdMaterialNode2& shader, HdUEMaterialData& data) {
    SurfaceReader reader(network, shader, data);
    const auto& type = shader.nodeTypeId.GetString();
    if (type == "UsdPreviewSurface" || type == "ND_UsdPreviewSurface_surfaceshader")
        readPreviewSurface(reader, data);
    else if (TfStringStartsWith(type, "ND_standard_surface_surfaceshader"))
        readMaterialX(reader, data, standardSurface);
    else if (TfStringStartsWith(type, "ND_open_pbr_surface_surfaceshader"))
        readMaterialX(reader, data, openPbr);
    else
        warnOnce(TfStringPrintf("surface shader %s is not translated; its materials render with the default surface",
                                type.c_str()));
}

class Material final : public HdMaterial {
public:
    explicit Material(const SdfPath& id) : HdMaterial(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits) override {
        HdUEMaterialData data;
        data.id = GetId().GetString();
        const auto net = network(delegate, GetId());
        readDisplacement(net, data);
        if (const auto* shader = surface(net)) readSurface(net, *shader, data);
        auto* state = static_cast<HdUEState*>(param);
        std::lock_guard lock(state->mutex);
        // Displacement moves vertices when meshes are packed: a change re-packs the meshes bound to the material.
        const auto displacement = [](const HdUEMaterialData& m) {
            const auto& t = m.textures[HDUE_TEXTURE_DISPLACEMENT];
            return std::tuple(m.displacement, t.file, t.scale, t.bias, t.mask, t.uvSet, t.wrap[0], t.wrap[1],
                              m.uvTransform);
        };
        const auto previous = state->materials.find(data.id);
        if (displacement(previous == state->materials.end() ? HdUEMaterialData() : previous->second) !=
            displacement(data))
            for (const auto& [id, mesh] : state->meshes)
                if (mesh.material == data.id) state->dirtyMeshes.insert(id);
        state->materials[data.id] = data;
        state->dirtyMaterials.insert(data.id);
        ++state->revision;
        *bits = Clean;
    }
    void Finalize(HdRenderParam* param) override { static_cast<HdUEState*>(param)->erase(GetId().GetString()); }
};
}  // namespace

// The UV primvars a material reads (its meshes' UV sets, uvPrimvars) and the vec3 primvar it reads as vertex colours
// (displayColor, Cd, ...); empty when none.
std::pair<TfTokenVector, TfToken> HdUEMaterialPrimvars(HdSceneDelegate* delegate, const SdfPath& material) {
    std::pair<TfTokenVector, TfToken> names;
    if (material.IsEmpty()) return names;
    const auto net = network(delegate, material);
    names.first = uvPrimvars(net);
    for (const auto& [path, node] : net.nodes)
        if (names.second.IsEmpty()) names.second = colorPrimvar(node);
    return names;
}
HdSprim* HdUECreateMaterial(const SdfPath& id) { return new Material(id); }
PXR_NAMESPACE_CLOSE_SCOPE
