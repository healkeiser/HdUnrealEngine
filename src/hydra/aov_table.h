#pragma once
#include "hdue/bridge.h"
#include <cstdint>
#include <string_view>

// The AOVs hdUE renders besides the beauty, depth and Cryptomatte (cryptomatte.h), in one table: a render var selects a
// row by its name or one of its aliases (Karma's render var names and light path expressions, spaces removed).
// scripts/build_render_settings_hda.py reads this table for the Houdini render var assets.
//
// Buffer rows are UE's buffer visualizations ([Engine.BufferVisualizationMaterials]); all of a frame's render in one
// raster pass (MRQ's buffer visualization pipes). Lighting rows are the beauty restricted to UE's lighting components
// (the Path Tracer's Lighting Components settings; realtime show flags), one render each. Id rows are hdUE's ID passes.
enum class HdUEAovKind : uint8_t { Buffer, Lighting, Id };
enum class HdUEAovFormat : uint8_t {
    Float,   // channel R
    Color,   // RGB
    Vector,  // a world direction in UE axes (buffers: encoded 0.5 + 0.5 * d), published in USD axes, normalized
    Int,     // the ID passes' encoding
    Beauty,  // RGBA radiance, converted to scene_linear and exposed like the beauty
};
enum : uint8_t { HdUERealtime = 1, HdUEPathTraced = 2, HdUEBothModes = 3 };
struct HdUEAovInfo {
    const char* name;
    const char* aliases;
    HdUEAovKind kind;
    const char* buffer;  // Buffer rows: the buffer visualization
    uint32_t aov;        // the HDUE_AOV_* request: Lighting rows with their HDUE_LIGHTING_* components
    HdUEAovFormat format;
    uint8_t modes;
    uint32_t pathTraced;  // path traced: this request instead (the Path Tracer's own albedo or normal); 0 for none
    const char* doc;
};
constexpr uint32_t HdUELighting(uint32_t components) { return HDUE_AOV_LIGHTING | components << 8; }
inline constexpr HdUEAovInfo HdUEAovTable[] = {
    {"albedo", "baseColor CDA", HdUEAovKind::Buffer, "BaseColor", HDUE_AOV_BUFFERS, HdUEAovFormat::Color, HdUEBothModes,
     HDUE_AOV_ALBEDO, "Base colour (GBuffer); path traced, the Path Tracer's albedo, averaged over its samples."},
    {"normal", "N ray:hitN", HdUEAovKind::Buffer, "WorldNormal", HDUE_AOV_BUFFERS, HdUEAovFormat::Vector, HdUEBothModes,
     HDUE_AOV_NORMAL,
     "World-space shading normal in USD axes, normalized; path traced, the Path Tracer's, averaged over its samples."},
    {"roughness", "", HdUEAovKind::Buffer, "Roughness", HDUE_AOV_BUFFERS, HdUEAovFormat::Float, HdUEBothModes, 0,
     "Roughness."},
    {"metallic", "", HdUEAovKind::Buffer, "Metallic", HDUE_AOV_BUFFERS, HdUEAovFormat::Float, HdUEBothModes, 0,
     "Metallic."},
    {"specular", "", HdUEAovKind::Buffer, "Specular", HDUE_AOV_BUFFERS, HdUEAovFormat::Float, HdUEBothModes, 0,
     "Specular (UE's F0 scale, 0.5 for 4% reflectance)."},
    {"occlusion", "", HdUEAovKind::Buffer, "MaterialAO", HDUE_AOV_BUFFERS, HdUEAovFormat::Float, HdUEBothModes, 0,
     "Material ambient occlusion."},
    {"ambientOcclusion", "AO", HdUEAovKind::Buffer, "AmbientOcclusion", HDUE_AOV_BUFFERS, HdUEAovFormat::Float,
     HdUEBothModes, 0, "Screen-space ambient occlusion of the raster pass (1 unoccluded)."},
    {"opacity", "", HdUEAovKind::Buffer, "Opacity", HDUE_AOV_BUFFERS, HdUEAovFormat::Float, HdUEBothModes, 0,
     "UE's GBuffer opacity (subsurface and two-sided foliage data; 0 for other surfaces)."},
    {"subsurfaceColor", "", HdUEAovKind::Buffer, "SubsurfaceColor", HDUE_AOV_BUFFERS, HdUEAovFormat::Color,
     HdUEBothModes, 0, "Subsurface colour of subsurface shading models, else black."},
    {"anisotropy", "", HdUEAovKind::Buffer, "Anisotropy", HDUE_AOV_BUFFERS, HdUEAovFormat::Float, HdUEBothModes, 0,
     "Anisotropy."},
    {"tangent", "", HdUEAovKind::Buffer, "WorldTangent", HDUE_AOV_BUFFERS, HdUEAovFormat::Vector, HdUEBothModes, 0,
     "World-space tangent in USD axes, normalized."},
    {"shadingModel", "", HdUEAovKind::Buffer, "ShadingModel", HDUE_AOV_BUFFERS, HdUEAovFormat::Color, HdUEBothModes, 0,
     "UE's shading model, as the colour code of its Shading Model view."},
    {"customDepth", "", HdUEAovKind::Buffer, "CustomDepthWorldUnits", HDUE_AOV_BUFFERS, HdUEAovFormat::Float,
     HdUEBothModes, 0, "UE's custom depth (cm); hdUE meshes do not render into it, so it is the far value."},
    {"customStencil", "", HdUEAovKind::Buffer, "CustomStencil", HDUE_AOV_BUFFERS, HdUEAovFormat::Float, HdUEBothModes,
     0, "UE's custom stencil; hdUE meshes do not write it, so it is 0."},
    {"velocity", "", HdUEAovKind::Buffer, "Velocity", HDUE_AOV_BUFFERS, HdUEAovFormat::Color, HdUEBothModes, 0,
     "UE's encoded screen velocity (0.5 still); the AOV pass renders one time, so it is 0.5."},
    {"combineddiffuse", "diffuse C<RD>.*L C<RD>.*[LO]", HdUEAovKind::Lighting, nullptr,
     HdUELighting(HDUE_LIGHTING_DIFFUSE | HDUE_LIGHTING_INDIRECT_DIFFUSE), HdUEAovFormat::Beauty, HdUEBothModes, 0,
     "Diffuse reflection, direct and indirect."},
    {"directdiffuse", "C<RD>L", HdUEAovKind::Lighting, nullptr, HdUELighting(HDUE_LIGHTING_DIFFUSE),
     HdUEAovFormat::Beauty, HdUEBothModes, 0, "Diffuse reflection of direct light."},
    {"indirectdiffuse", "C<RD>.+L", HdUEAovKind::Lighting, nullptr, HdUELighting(HDUE_LIGHTING_INDIRECT_DIFFUSE),
     HdUEAovFormat::Beauty, HdUEBothModes, 0, "Diffuse reflection of indirect light (Lumen global illumination)."},
    {"combinedglossyreflection", "glossy C<RG>.*L C<RG>.*[LO]", HdUEAovKind::Lighting, nullptr,
     HdUELighting(HDUE_LIGHTING_SPECULAR | HDUE_LIGHTING_INDIRECT_SPECULAR), HdUEAovFormat::Beauty, HdUEBothModes, 0,
     "Specular reflection (and transmission), direct and indirect."},
    {"directglossyreflection", "C<RG>L", HdUEAovKind::Lighting, nullptr, HdUELighting(HDUE_LIGHTING_SPECULAR),
     HdUEAovFormat::Beauty, HdUEBothModes, 0, "Specular reflection of direct light."},
    {"indirectglossyreflection", "C<RG>.+L", HdUEAovKind::Lighting, nullptr,
     HdUELighting(HDUE_LIGHTING_INDIRECT_SPECULAR), HdUEAovFormat::Beauty, HdUEBothModes, 0,
     "Specular reflection of indirect light (Lumen reflections)."},
    {"directemission", "emission CO", HdUEAovKind::Lighting, nullptr, HdUELighting(HDUE_LIGHTING_EMISSIVE),
     HdUEAovFormat::Beauty, HdUEBothModes, 0, "Emission seen directly."},
    {"combinedvolume", "volume CV.*L", HdUEAovKind::Lighting, nullptr,
     HdUELighting(HDUE_LIGHTING_VOLUME | HDUE_LIGHTING_INDIRECT_VOLUME), HdUEAovFormat::Beauty, HdUEPathTraced, 0,
     "Volume scattering, direct and indirect (path traced)."},
    {"directvolume", "CVL", HdUEAovKind::Lighting, nullptr, HdUELighting(HDUE_LIGHTING_VOLUME), HdUEAovFormat::Beauty,
     HdUEPathTraced, 0, "Volume scattering of direct light (path traced)."},
    {"indirectvolume", "CV.+L", HdUEAovKind::Lighting, nullptr, HdUELighting(HDUE_LIGHTING_INDIRECT_VOLUME),
     HdUEAovFormat::Beauty, HdUEPathTraced, 0, "Volume scattering of indirect light (path traced)."},
    {"primId", "", HdUEAovKind::Id, nullptr, HDUE_AOV_PRIM_ID, HdUEAovFormat::Int, HdUEBothModes, 0,
     "Hydra prim ID of the visible mesh, -1 for background."},
    {"instanceId", "", HdUEAovKind::Id, nullptr, HDUE_AOV_INSTANCE_ID, HdUEAovFormat::Int, HdUEBothModes, 0,
     "Hydra instance index of an instanced prototype, -1 elsewhere."},
};

// The row a render var name or light path expression selects; nullptr for none.
inline const HdUEAovInfo* HdUEFindAov(std::string_view name) {
    if (name.empty()) return nullptr;
    for (const auto& row : HdUEAovTable) {
        if (name == row.name) return &row;
        for (std::string_view aliases = row.aliases; !aliases.empty();) {
            const size_t end = aliases.find(' ');
            if (aliases.substr(0, end) == name) return &row;
            aliases = end == std::string_view::npos ? std::string_view() : aliases.substr(end + 1);
        }
    }
    return nullptr;
}
