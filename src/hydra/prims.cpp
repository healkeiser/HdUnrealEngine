#include "delegate.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/mesh.h"
#include "pxr/imaging/hd/points.h"
#include "pxr/imaging/hd/basisCurves.h"
#include "pxr/imaging/hd/volume.h"
#include "pxr/imaging/hd/field.h"
#include <mutex>
#include <optional>
#if defined(HDUE_HOUDINI)
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include "../houdini/volume_fields.h"
#include "pxr/base/tf/stringUtils.h"
#endif
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/extComputationUtils.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/light.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/glf/simpleLight.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/usdLux/blackbody.h"
#include <cmath>
#include <algorithm>
#include "geom_subsets.h"
#include "instancing.h"
#include "subdivision.h"
#include "mesh_normals.h"
#include "mesh_weld.h"
#include "surface_settings.h"
#include "color_management.h"
#include "wet_mask.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/usdImaging/usdImaging/usdPrimInfoSchema.h"
#include "pxr/imaging/hd/lightSchema.h"
#include "pxr/imaging/hd/materialSchema.h"
#include "pxr/imaging/hd/materialNetworkSchema.h"
#include "pxr/imaging/hd/materialNodeSchema.h"
#include "pxr/imaging/hd/materialNodeParameterSchema.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/rotation.h"

PXR_NAMESPACE_OPEN_SCOPE
std::pair<TfTokenVector, TfToken> HdUEMaterialPrimvars(HdSceneDelegate* delegate,
                                                       const SdfPath& material);  // materials.cpp
namespace {
// HDUE_TRACE_SCENE: running totals of the mesh data Hydra sync stores in the delegate's state (its own copy of the
// scene, triangulated, a vertex per distinct corner), every 250 meshes.
void traceStored(const HdUEMeshData& m) {
    if (!HdUETraceScene()) return;
    static std::mutex lock;
    static uint64_t meshes = 0, vertices = 0, triangles = 0, bytes = 0;
    std::lock_guard guard(lock);
    bytes += HdUEMeshBytes(m);
    vertices += m.points.size();
    triangles += m.indices.size() / 3;
    if (++meshes % 250 == 0)
        std::fprintf(stderr, "hdUE stored meshes: %llu, %.1fM vertices, %.1fM triangles, %.2f GB\n",
                     (unsigned long long)meshes, vertices / 1e6, triangles / 1e6, bytes / double(1ull << 30));
}
template <class T>
T value(const VtValue& v, T fallback) {
    return v.IsHolding<T>() ? v.UncheckedGet<T>() : fallback;
}
// Sync's dirty bits end clean on every path: a prim that failed is not synced (and reported) again each frame.
struct CleanBits {
    HdDirtyBits* bits;
    ~CleanBits() { *bits = HdChangeTracker::Clean; }
};
// The mesh's UV sets and colour primvar: from its own binding, else from its GeomSubsets' bindings; else st, and
// displayColor when nothing is bound (a bound material reads colours only through a primvar reader). ponytail: one list
// of UV sets and one colour per mesh; per-subset sets if assets mix them.
std::pair<TfTokenVector, TfToken> meshPrimvars(HdSceneDelegate* delegate, const SdfPath& material,
                                               const HdMeshTopology& topology) {
    auto names = HdUEMaterialPrimvars(delegate, material);
    for (const auto& subset : topology.GetGeomSubsets())
        if (names.first.empty() || names.second.IsEmpty()) {
            const auto [uv, color] = HdUEMaterialPrimvars(delegate, subset.materialId);
            if (names.first.empty()) names.first = uv;
            if (names.second.IsEmpty()) names.second = color;
        }
    if (names.first.empty()) names.first = {TfToken("st")};
    if (material.IsEmpty()) names.second = HdTokens->displayColor;
    return names;
}
// ExtComputation primvars (UsdSkel skinning through a scene delegate), evaluated on the CPU as HdEmbree does.
HdExtComputationUtils::ValueStore computedPrimvars(HdSceneDelegate* delegate, const SdfPath& id) {
    const auto descriptors = delegate->GetExtComputationPrimvarDescriptors(id, HdInterpolationVertex);
    return descriptors.empty() ? HdExtComputationUtils::ValueStore()
                               : HdExtComputationUtils::GetComputedPrimvarValues(descriptors, delegate);
}
VtValue primvar(HdSceneDelegate* delegate, const SdfPath& id, const TfToken& name,
                const HdExtComputationUtils::ValueStore& computed) {
    const auto it = computed.find(name);
    return it == computed.end() ? delegate->Get(id, name) : it->second;
}
HdInterpolation interpolation(HdSceneDelegate* delegate, const SdfPath& id, const TfToken& name) {
    for (int i = 0; i < HdInterpolationCount; ++i)
        for (const auto& p : delegate->GetPrimvarDescriptors(id, HdInterpolation(i)))
            if (p.name == name) return HdInterpolation(i);
    return HdInterpolationConstant;
}
// The material of a prim without a bound one (mesh, points, curves): its displayColor, or white times its vertex
// colours, and its displayOpacity. displayColor is the base colour, as UE's USD importer draws unbound prims (its
// DisplayColor reference material, USDProjectSettings.h, feeds the vertex colour straight into Base Color); without
// one it is white, the vertex colour UE's importer gives a mesh without displayColor (USDGeomMeshConversion.cpp).
// The material's id is the prim's with ".displayColor".
// UsdLux MeshLightAPI: UsdImaging puts the light (isLight) and its MeshLight node (intensity, exposure, colour,
// colour temperature) on the mesh prim itself. UE's USD importer ignores MeshLightAPI; UE lights a scene from emissive
// surfaces (Lumen, Path Tracer), so the mesh emits that radiance from its display-colour material. Zero when the
// mesh is not a light. ponytail: normalize, materialSyncMode and the bound material's own emission are ignored.
GfVec3f HdUEMeshLightEmission(HdSceneDelegate* delegate, const SdfPath& id) {
    const auto source = delegate->GetRenderIndex().GetTerminalSceneIndex()->GetPrim(id).dataSource;
    const auto isLight =
        HdLightSchema::GetFromParent(source).GetContainer()
            ? HdBoolDataSource::Cast(HdLightSchema::GetFromParent(source).GetContainer()->Get(TfToken("isLight")))
            : nullptr;
    if (!isLight || !isLight->GetTypedValue(0)) return GfVec3f(0);
    const auto nodes = HdMaterialSchema::GetFromParent(source).GetMaterialNetwork().GetNodes();
    for (const auto& name : nodes.GetNames()) {
        const auto parameters = nodes.Get(name).GetParameters();
        const auto get = [&](const char* parameter, auto fallback) {
            using T = decltype(fallback);
            const auto value = parameters.Get(TfToken(parameter)).GetValue();
            const auto v = value ? value->GetValue(0) : VtValue();
            return v.IsHolding<T>() ? v.UncheckedGet<T>() : fallback;
        };
        if (!parameters.Get(TfToken("intensity")).GetValue() && !parameters.Get(TfToken("exposure")).GetValue())
            continue;
        auto color = HdUEFromSceneLinear(get("color", GfVec3f(1)));
        if (get("enableColorTemperature", false)) {
            const auto temperature = UsdLuxBlackbodyTemperatureAsRgb(get("colorTemperature", 6500.f));
            for (int c = 0; c < 3; ++c) color[c] *= temperature[c];
        }
        return color * get("intensity", 1.f) * std::exp2(get("exposure", 0.f));
    }
    return GfVec3f(1);  // a light without parameters: UsdLux's fallback intensity 1
}
HdUEMaterialData displayColorMaterial(HdSceneDelegate* delegate, const SdfPath& id, const HdUEMeshData& mesh) {
    HdUEMaterialData material;
    material.id = mesh.id + ".displayColor";
    const auto color = value(delegate->Get(id, HdTokens->displayColor), VtVec3fArray());
    material.vertexColor = !mesh.colors.empty() || !mesh.instanceColors.empty();
    material.color = material.vertexColor || color.empty() ? GfVec3f(1) : HdUEFromSceneLinear(color[0]);
    const auto opacity = value(delegate->Get(id, HdTokens->displayOpacity), VtFloatArray());
    if (!opacity.empty()) {
        // The generic ABI carries one opacity per material. Constant primvars (and identical values in other domains)
        // are exact.
        if (!std::all_of(opacity.begin(), opacity.end(), [&](float a) { return a == opacity[0]; }))
            TF_WARN("Varying displayOpacity is unsupported at %s; using its first value", id.GetText());
        if (std::isfinite(opacity[0])) material.opacity = std::clamp(opacity[0], 0.f, 1.f);
    }
    material.emissive = HdUEMeshLightEmission(delegate, id);
    return material;
}
// Varying colors only: a uniform displayColor stays the material's constant color.
std::vector<GfVec3f> HdUEVaryingColors(std::vector<GfVec3f> colors, size_t count) {
    if (colors.size() != count ||
        std::all_of(colors.begin(), colors.end(), [&](const GfVec3f& c) { return c == colors[0]; }))
        return {};
    return colors;
}
// A primvar on the triangulated mesh: per point (vertex, varying, constant) or per triangulated corner, 3 * triangle +
// corner (faceVarying, uniform). Empty when missing or invalid.
template <class T>
struct Triangulated {
    VtArray<T> values;
    bool perCorner = false;
    const T& at(size_t corner, size_t point) const { return values[perCorner ? corner : point]; }
    bool same(size_t a, size_t b) const { return !perCorner || values[a] == values[b]; }  // corners of one point
};
// computed: the primvar's ExtComputation value (vertex), which replaces the authored one.
template <class T>
Triangulated<T> triangulated(HdSceneDelegate* delegate, const SdfPath& id, const TfToken& name, size_t pointCount,
                             const VtVec3iArray& triangles, const VtIntArray& parameters, HdMeshUtil& util, HdType type,
                             const HdUESubdivision* subdivision = nullptr, const VtValue& computed = {}) {
    VtIntArray indices;
    const auto raw = computed.IsEmpty() ? delegate->GetIndexedPrimvar(id, name, &indices) : computed;
    if (!raw.IsHolding<VtArray<T>>()) return {};
    VtArray<T> array = raw.UncheckedGet<VtArray<T>>();
    const auto domain = computed.IsEmpty() ? interpolation(delegate, id, name) : HdInterpolationVertex;
    // Indexed primvars (primvars:x:indices) arrive unflattened. A subdivision refines face-varying ones through their
    // indices, so values shared across faces stay connected; others are expanded first.
    const int channel =
        subdivision && domain == HdInterpolationFaceVarying && !indices.empty() ? subdivision->channel(indices) : 0;
    if (!indices.empty()) {
        if (std::any_of(indices.begin(), indices.end(), [&](int i) { return i < 0 || size_t(i) >= array.size(); }))
            return {};
        if (!channel) {
            VtArray<T> flat(indices.size());
            for (size_t i = 0; i < indices.size(); ++i) flat[i] = array[indices[i]];
            array = std::move(flat);
        }
    }
    if (array.empty()) return {};
    if (subdivision && (array = subdivision->refine(array, domain, channel)).empty()) return {};
    Triangulated<T> out;
    if (domain == HdInterpolationConstant)
        out.values.assign(pointCount, array[0]);
    else if (domain == HdInterpolationFaceVarying) {
        VtValue output;
        const auto result = util.ComputeTriangulatedFaceVaryingPrimvar(array.cdata(), int(array.size()), type, &output);
        if (result == HdMeshComputationResult::Error) return {};
        out.values = result == HdMeshComputationResult::Unchanged ? array : value(output, VtArray<T>());
        out.perCorner = true;
        if (out.values.size() < triangles.size() * 3) return {};
    } else if (domain == HdInterpolationUniform) {
        out.values.resize(triangles.size() * 3);
        out.perCorner = true;
        for (size_t tri = 0; tri < triangles.size(); ++tri) {
            const size_t face = size_t(HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(parameters[tri]));
            if (face >= array.size()) return {};
            std::fill_n(out.values.begin() + tri * 3, 3, array[face]);
        }
    } else if (array.size() >= pointCount)
        out.values = std::move(array);
    return out;
}
// Motion samples cover one frame either side, enough for any shutter; Pass evaluates them at sub-frame times.
constexpr float HdUEMotionWindow = 1.f;
constexpr size_t HdUEMaxMotionSamples = 16;
// Samples of current^-1-relative factors: left = M(t) * M(0)^-1, right = M(0)^-1 * M(t).
HdUEMotionSamples HdUERelativeMotion(const float* times, const GfMatrix4d* values, size_t count,
                                     const GfMatrix4d& current, bool left) {
    HdUEMotionSamples out;
    const auto inverse = current.GetInverse();
    for (size_t i = 0; i < std::min(count, HdUEMaxMotionSamples); ++i) {
        out.times.push_back(times[i]);
        out.values.push_back(left ? values[i] * inverse : inverse * values[i]);
    }
    if (!out.moving()) out = {};
    return out;
}
HdUEMotionSamples HdUETransformMotion(HdSceneDelegate* delegate, const SdfPath& id, const GfMatrix4d& current,
                                      bool left) {
    float times[HdUEMaxMotionSamples];
    GfMatrix4d values[HdUEMaxMotionSamples];
    const size_t count =
        delegate->SampleTransform(id, -HdUEMotionWindow, HdUEMotionWindow, HdUEMaxMotionSamples, times, values);
    return HdUERelativeMotion(times, values, count, current, left);
}
// UsdGeomMotionAPI motion:blurScale (inherited), which UsdImaging publishes as blurScale.
float HdUEBlurScale(HdSceneDelegate* delegate, const SdfPath& id) {
    const auto scale = delegate->Get(id, HdTokens->blurScale);
    return scale.IsHolding<float>() ? std::max(0.f, scale.UncheckedGet<float>()) : 1.f;
}
// USD velocities are per second; motion blur times are in frames.
double HdUEFramesPerSecond(const HdUEState& state) {
    std::lock_guard lock(state.settingsMutex);
    const auto it = state.settings.find(TfToken("houdini:fps"));
    const double fps = it == state.settings.end()       ? 0
                       : it->second.IsHolding<double>() ? it->second.UncheckedGet<double>()
                       : it->second.IsHolding<float>()  ? it->second.UncheckedGet<float>()
                                                        : 0;
    return fps > 0 ? fps : state.setting<double>("framesPerSecond", 24);
}
// Instance transforms as hdEmbree composes them (HdUEInstanceTransform), then nested instancers apply after (child *
// parent). Masked instances (invisibleIds, inactiveIds) are already absent from GetInstanceIndices.
class Instancer final : public HdInstancer {
    std::map<TfToken, VtValue> primvars_;
    bool visible_ = true;
    VtValue primvar(const TfToken& name) const {
        const auto it = primvars_.find(name);
        return it == primvars_.end() ? VtValue() : it->second;
    }
    const Instancer* parent() const {
        return GetParentId().IsEmpty()
                   ? nullptr
                   : dynamic_cast<const Instancer*>(GetDelegate()->GetRenderIndex().GetInstancer(GetParentId()));
    }
    // A per-instance vector primvar (velocities, accelerations; instancer space) in world space for each flattened
    // instance, plus its parent instance's own; empty when no level authors it.
    std::vector<GfVec3f> ComputeInstanceVectors(const SdfPath& prototype, const TfToken& name) const {
        if (!visible_) return {};
        const auto values = primvar(name);
        const auto* up = parent();
        const auto inherited = up ? up->ComputeInstanceVectors(GetId(), name) : std::vector<GfVec3f>();
        if (values.IsEmpty() && inherited.empty()) return {};
        const auto parents = up ? up->ComputeInstanceTransforms(GetId()) : VtMatrix4dArray(1, GfMatrix4d(1));
        const auto instancer = GetDelegate()->GetInstancerTransform(GetId());
        const auto indices = GetDelegate()->GetInstanceIndices(GetId(), prototype);
        std::vector<GfVec3f> out;
        out.reserve(parents.size() * indices.size());
        for (size_t i = 0; i < parents.size(); ++i)
            for (const int index : indices) {
                GfVec3d v(0);
                HdUEInstanceVector(values, size_t(index), v);
                v = (instancer * parents[i]).TransformDir(v);
                out.emplace_back(i < inherited.size() ? v + GfVec3d(inherited[i]) : v);
            }
        return out;
    }

public:
    Instancer(HdSceneDelegate* delegate, const SdfPath& id) : HdInstancer(delegate, id) {}
    void Sync(HdSceneDelegate* delegate, HdRenderParam*, HdDirtyBits* bits) override {
        _UpdateInstancer(delegate, bits);
        if (*bits & HdChangeTracker::DirtyVisibility) visible_ = delegate->GetVisible(GetId());
        if (HdChangeTracker::IsAnyPrimvarDirty(*bits, GetId())) {
            std::map<TfToken, VtValue> current;  // a primvar removed from the instancer stops applying
            for (const auto& primvar : delegate->GetPrimvarDescriptors(GetId(), HdInterpolationInstance))
                current[primvar.name] =
                    HdChangeTracker::IsPrimvarDirty(*bits, GetId(), primvar.name) || !primvars_.count(primvar.name)
                        ? delegate->Get(GetId(), primvar.name)
                        : primvars_[primvar.name];
            primvars_.swap(current);
        }
    }
    // Motion of count flattened instances of prototype: the instancer's transform samples, and USD PointInstancer
    // velocities and accelerations per frame (dropped unless there is one per instance).
    void ComputeInstanceMotion(const SdfPath& prototype, double fps, size_t count, HdUEMeshData& mesh) const {
        float times[HdUEMaxMotionSamples];
        GfMatrix4d values[HdUEMaxMotionSamples];
        const size_t samples = GetDelegate()->SampleInstancerTransform(GetId(), -HdUEMotionWindow, HdUEMotionWindow,
                                                                       HdUEMaxMotionSamples, times, values);
        mesh.motionRight =
            HdUERelativeMotion(times, values, samples, GetDelegate()->GetInstancerTransform(GetId()), false);
        const auto perFrame = [&](const TfToken& name, double seconds) {
            auto out = ComputeInstanceVectors(prototype, name);
            if (out.size() != count) out.clear();
            for (auto& v : out) v /= float(seconds);
            return out;
        };
        mesh.velocities = perFrame(HdTokens->velocities, fps);
        mesh.accelerations = perFrame(HdTokens->accelerations, fps * fps);
        mesh.angularVelocities = perFrame(HdTokens->angularVelocities, fps);
        if (mesh.velocities.empty() && mesh.angularVelocities.empty()) SampledMotion(prototype, count, mesh);
    }
    // Without velocities, time-sampled positions, orientations, scales or matrices move each instance: linearly between
    // the samples around the frame, as UsdGeomPointInstancer::ComputeInstanceTransformsAtTime interpolates them, as a
    // velocity and an angular velocity per frame about the instance's position.
    void SampledMotion(const SdfPath& prototype, size_t count, HdUEMeshData& mesh) const {
        std::map<TfToken, VtValue> open, close;
        float t0 = 0, t1 = 0;
        for (const auto& name : {HdInstancerTokens->instanceTranslations, HdInstancerTokens->instanceRotations,
                                 HdInstancerTokens->instanceScales, HdInstancerTokens->instanceTransforms}) {
            float times[HdUEMaxMotionSamples];
            VtValue values[HdUEMaxMotionSamples];
            const size_t n = GetDelegate()->SamplePrimvar(GetId(), name, -HdUEMotionWindow, HdUEMotionWindow,
                                                          HdUEMaxMotionSamples, times, values);
            if (n < 2 || values[0] == values[n - 1]) continue;
            // shortcut: one linear segment from the first sample to the last; sample per sub-frame if a curved
            // path between more than two samples inside the window matters.
            open[name] = values[0];
            close[name] = values[n - 1];
            t0 = times[0];
            t1 = times[n - 1];
        }
        if (open.empty() || t1 <= t0) return;
        const auto from = ComputeInstanceTransforms(prototype, &open),
                   to = ComputeInstanceTransforms(prototype, &close);
        if (from.size() != count || to.size() != count) return;
        const float span = t1 - t0;
        mesh.velocities.resize(count);
        mesh.angularVelocities.resize(count);
        for (size_t i = 0; i < count; ++i) {
            mesh.velocities[i] = GfVec3f((to[i].ExtractTranslation() - from[i].ExtractTranslation()) / span);
            // The world-space turn between the two linear parts (row vectors: to = from * turn).
            const auto turn = (from[i].ExtractRotationMatrix().GetInverse() * to[i].ExtractRotationMatrix())
                                  .GetOrthonormalized()
                                  .ExtractRotation();
            mesh.angularVelocities[i] = GfVec3f(turn.GetAxis() * (turn.GetAngle() / span));  // degrees, as USD's
        }
    }
    // An instance-rate colour primvar (displayColor, or the one a bound material reads) for each flattened instance,
    // from the nearest instancer that authors it, as Storm resolves nested instance primvars; empty when none does.
    std::vector<GfVec3f> ComputeInstanceColors(const SdfPath& prototype, const TfToken& name) const {
        if (!visible_) return {};
        const auto colors = value(primvar(name), VtVec3fArray());
        const auto* up = parent();
        const auto inherited = up && colors.empty() ? up->ComputeInstanceColors(GetId(), name) : std::vector<GfVec3f>();
        if (colors.empty() && inherited.empty()) return {};
        const size_t parents = up ? up->ComputeInstanceTransforms(GetId()).size() : 1;
        const auto indices = GetDelegate()->GetInstanceIndices(GetId(), prototype);
        std::vector<GfVec3f> out;
        out.reserve(parents * indices.size());
        for (size_t i = 0; i < parents; ++i)
            for (const int index : indices)
                out.push_back(!colors.empty()        ? colors[size_t(index) < colors.size() ? index : 0]
                              : i < inherited.size() ? inherited[i]
                                                     : GfVec3f(1));
        return out;
    }
    // Instance placements; `sampled` replaces this instancer's instance primvars (values at another time).
    VtMatrix4dArray ComputeInstanceTransforms(const SdfPath& prototype,
                                              const std::map<TfToken, VtValue>* sampled = nullptr) const {
        if (!visible_) return {};
        const auto at = [&](const TfToken& name) {
            const auto found = sampled ? sampled->find(name) : std::map<TfToken, VtValue>::const_iterator();
            return sampled && found != sampled->end() ? found->second : primvar(name);
        };
        const auto instancer = GetDelegate()->GetInstancerTransform(GetId());
        const auto indices = GetDelegate()->GetInstanceIndices(GetId(), prototype);
        const auto translations = at(HdInstancerTokens->instanceTranslations),
                   rotations = at(HdInstancerTokens->instanceRotations);
        const auto scales = at(HdInstancerTokens->instanceScales), matrices = at(HdInstancerTokens->instanceTransforms);
        VtMatrix4dArray transforms(indices.size());
        for (size_t i = 0; i < indices.size(); ++i)
            transforms[i] =
                HdUEInstanceTransform(instancer, translations, rotations, scales, matrices, size_t(indices[i]));
        const auto* up = parent();
        if (!up) return transforms;
        const auto parents = up->ComputeInstanceTransforms(GetId());
        VtMatrix4dArray nested(parents.size() * transforms.size());
        for (size_t i = 0; i < parents.size(); ++i)
            for (size_t j = 0; j < transforms.size(); ++j)
                nested[i * transforms.size() + j] = transforms[j] * parents[i];
        return nested;
    }
};
// The rprim's instancer, synced with its parents (HdUEInstancePlacements syncs it first).
const Instancer* HdUEInstancer(HdSceneDelegate* delegate, const SdfPath& instancerId) {
    return instancerId.IsEmpty() ? nullptr
                                 : dynamic_cast<const Instancer*>(delegate->GetRenderIndex().GetInstancer(instancerId));
}
// Placements of an rprim under a Hydra instancer (its own transform first), or empty when not instanced.
std::vector<GfMatrix4d> HdUEInstancePlacements(HdSceneDelegate* delegate, const SdfPath& id, const SdfPath& instancerId,
                                               const GfMatrix4d& transform) {
    std::vector<GfMatrix4d> out;
    if (instancerId.IsEmpty()) return out;
    HdInstancer::_SyncInstancerAndParents(delegate->GetRenderIndex(), instancerId);
    if (const auto* instancer = HdUEInstancer(delegate, instancerId))
        for (const auto& instance : instancer->ComputeInstanceTransforms(id)) out.push_back(transform * instance);
    return out;
}
// Cumulative mesh Sync time (all Hydra threads), reported with each scene submit by HDUE_TRACE_TIMING.
std::atomic<int64_t> meshSyncMicroseconds{0};
std::atomic<int> meshSyncs{0};
struct MeshSyncTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~MeshSyncTimer() {
        meshSyncMicroseconds +=
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        ++meshSyncs;
    }
};
class Mesh final : public HdMesh {
    std::set<std::string> emitted_;

    struct Shape {
        VtVec3fArray points;
        HdMeshTopology topology;
        std::unique_ptr<HdUESubdivision> subdivision;
        VtVec3iArray triangles;
        VtIntArray parameters;  // source face per triangle
        TfTokenVector uvNames;  // the material's UV primvars, in order
        TfToken colorName;
    };
    // The mesh as hdUE draws it: catmullClark and loop meshes refined uniformly (unreal:subdivisionLevel), with their
    // subdiv tags and the indices of their face-varying UVs and colours (authored normals do not apply to them), then
    // triangulated. Sync and the wet-mask refresh compute it from the scene delegate: kept per prim for the refresh, it
    // held about 16 bytes per triangle of every mesh whether or not a mask was ever used.
    Shape triangulate(HdSceneDelegate* delegate, const HdUEState& state, bool asset, VtVec3fArray points) const {
        Shape shape;
        shape.topology = delegate->GetMeshTopology(GetId());
        std::tie(shape.uvNames, shape.colorName) =
            meshPrimvars(delegate, delegate->GetMaterialId(GetId()), shape.topology);
        if (const int level = asset ? 0 : state.setting<int>("subdivisionLevel", 0);
            level > 0 && (shape.topology.GetScheme() == PxOsdOpenSubdivTokens->catmullClark ||
                          shape.topology.GetScheme() == PxOsdOpenSubdivTokens->loop)) {
            shape.topology.SetSubdivTags(delegate->GetSubdivTags(GetId()));
            std::vector<VtIntArray> fvarIndices;
            auto names = shape.uvNames;
            names.push_back(shape.colorName);
            for (const auto& name : names)
                if (!name.IsEmpty() && interpolation(delegate, GetId(), name) == HdInterpolationFaceVarying) {
                    VtIntArray indices;
                    delegate->GetIndexedPrimvar(GetId(), name, &indices);
                    if (!indices.empty()) fvarIndices.push_back(indices);
                }
            shape.subdivision = HdUESubdivision::Create(shape.topology, level, std::move(fvarIndices));
        }
        if (shape.subdivision) {
            points = shape.subdivision->refine(points, HdInterpolationVertex);
            shape.topology = shape.subdivision->topology();
        }
        HdMeshUtil(&shape.topology, GetId()).ComputeTriangleIndices(&shape.triangles, &shape.parameters);
        shape.points = std::move(points);
        return shape;
    }

public:
    explicit Mesh(const SdfPath& id) : HdMesh(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        const MeshSyncTimer timer;
        auto* state = static_cast<HdUEState*>(param);
        // A transform, visibility or purpose edit of a mesh without instancer keeps its geometry.
        if (!emitted_.empty() && GetInstancerId().IsEmpty() &&
            !(*bits & ~(HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
                        HdChangeTracker::DirtyRenderTag))) {
            const auto transform = delegate->GetTransform(GetId());
            const bool visible = delegate->GetVisible(GetId());
            const auto renderTag = delegate->GetRenderTag(GetId());
            const auto motion = HdUETransformMotion(delegate, GetId(), transform, true);
            std::lock_guard lock(state->mutex);
            if (std::all_of(emitted_.begin(), emitted_.end(),
                            [&](const std::string& id) { return state->meshes.count(id); })) {
                for (const auto& id : emitted_) {
                    auto& part = state->meshes[id];
                    const bool shown = part.visible != visible, moved = !shown && HdUEPlacedOnly(part, transform);
                    const bool changed = part.transform != transform || shown;
                    part.transform = transform;
                    part.visible = visible;
                    part.renderTag = renderTag;
                    part.motionLeft = motion;
                    if (changed) {
                        (moved ? state->movedMeshes : state->dirtyMeshes).insert(id);
                        ++state->revision;
                    }
                }
                *bits = HdChangeTracker::Clean;
                return;
            }
        }
        HdUEMeshData mesh;
        mesh.id = GetId().GetString();
        mesh.sourceId = mesh.id;
        mesh.scenePath = delegate->GetScenePrimPath(GetId(), -1).GetString();
        mesh.bindingPath = mesh.scenePath;
        mesh.material = delegate->GetMaterialId(GetId()).GetString();
        mesh.transform = delegate->GetTransform(GetId());
        mesh.visible = delegate->GetVisible(GetId());
        mesh.renderTag = delegate->GetRenderTag(GetId());
        _UpdateInstancer(delegate, bits);  // sets GetInstancerId
        const CleanBits clean{bits};
        mesh.instanced = !GetInstancerId().IsEmpty();
        mesh.instances = HdUEInstancePlacements(delegate, GetId(), GetInstancerId(), mesh.transform);
        const auto* instancer = HdUEInstancer(delegate, GetInstancerId());
        if (instancer)
            instancer->ComputeInstanceMotion(GetId(), HdUEFramesPerSecond(*state), mesh.instances.size(), mesh);
        mesh.motionLeft = HdUETransformMotion(delegate, GetId(), mesh.transform, true);
        mesh.blurScale = HdUEBlurScale(delegate, GetId());
        mesh.primId = GetPrimId();
        mesh.categories = delegate->GetCategories(GetId());
        mesh.doubleSided = delegate->GetDoubleSided(GetId());
        mesh.sceneAsset = value(delegate->Get(GetId(), TfToken("unreal:asset")), std::string());
        if (!mesh.sceneAsset.empty() && SdfPath(mesh.scenePath).GetName() == "__HdUEAssetProxy")
            mesh.scenePath = SdfPath(mesh.scenePath).GetParentPath().GetString();
        const auto computed = computedPrimvars(delegate, GetId());
        auto shape = triangulate(delegate, *state, !mesh.sceneAsset.empty(),
                                 value(primvar(delegate, GetId(), HdTokens->points, computed), VtVec3fArray()));
        auto& points = shape.points;
        auto& topology = shape.topology;
        const auto& subdivision = shape.subdivision;
        const auto& triangles = shape.triangles;
        const auto& parameters = shape.parameters;
        const auto& uvNames = shape.uvNames;
        const auto& colorName = shape.colorName;
        HdMeshUtil util(&topology, GetId());
        // Authored normals (subdivision surfaces ignore them, and so do computed points without computed normals); zero
        // or missing ones are generated below.
        const auto computedNormals = computed.find(HdTokens->normals);
        const auto normals =
            subdivision || (computed.count(HdTokens->points) && computedNormals == computed.end())
                ? Triangulated<GfVec3f>{}
                : triangulated<GfVec3f>(delegate, GetId(), HdTokens->normals, points.size(), triangles, parameters,
                                        util, HdTypeFloatVec3, nullptr,
                                        computedNormals == computed.end() ? VtValue() : computedNormals->second);
        // UV sets in the material's order; a set the mesh lacks reads (0, 0), unless it has none.
        std::vector<Triangulated<GfVec2f>> uvSets;
        for (const auto& name : uvNames)
            uvSets.push_back(triangulated<GfVec2f>(delegate, GetId(), name, points.size(), triangles, parameters, util,
                                                   HdTypeFloatVec2, subdivision.get()));
        if (std::all_of(uvSets.begin(), uvSets.end(), [](const auto& set) { return set.values.empty(); }))
            uvSets.clear();
        const auto uvSame = [&](size_t a, size_t b) {
            return std::all_of(uvSets.begin(), uvSets.end(), [&](const auto& set) { return set.same(a, b); });
        };
        // Vertex colours: an unbound mesh's displayColor when it varies (a uniform one stays the material's constant
        // color), or the primvar a bound material's base colour reads. An instancer's instance-rate primvar of that
        // name replaces the mesh's own, as per-instance colours.
        if (instancer && !colorName.IsEmpty() && mesh.sceneAsset.empty())
            mesh.instanceColors = instancer->ComputeInstanceColors(GetId(), colorName);
        if (mesh.instanceColors.size() != mesh.instances.size()) mesh.instanceColors.clear();
        Triangulated<GfVec3f> colors;
        if (!colorName.IsEmpty() && mesh.sceneAsset.empty() && mesh.instanceColors.empty()) {
            colors = triangulated<GfVec3f>(delegate, GetId(), colorName, points.size(), triangles, parameters, util,
                                           HdTypeFloatVec3, subdivision.get());
            if (mesh.material.empty() && std::all_of(colors.values.begin(), colors.values.end(),
                                                     [&](const GfVec3f& c) { return c == colors.values[0]; }))
                colors = {};
        }
        std::shared_ptr<const VtDictionary> selectedBindings;
        {
            std::lock_guard lock(state->mutex);
            selectedBindings = state->wetBindings;
        }
        const auto& wetBindings = *selectedBindings;
        std::set<std::string> maskNames;
        const auto needMask = [&](const std::string& path) {
            HdUEMeshData target;
            target.bindingPath = path;
            const auto name = HdUEWetValue<std::string>(HdUEWetSettings(wetBindings, target), "maskPrimvar", "");
            if (!name.empty()) maskNames.insert(name);
        };
        needMask(mesh.scenePath);
        for (const auto& subset : topology.GetGeomSubsets())
            needMask(SdfPath(mesh.scenePath).AppendChild(subset.id.GetNameToken()).GetString());
        std::map<std::string, std::vector<float>> masks;  // per corner
        for (const auto& name : maskNames) {
            const TfToken token(name);
            const auto raw = delegate->Get(GetId(), token);
            const auto domain = interpolation(delegate, GetId(), token);
            try {
                masks[name] = HdUEExpandWetMask(raw, domain, points.size(), topology, triangles, parameters, util);
            } catch (const std::invalid_argument& error) {
                mesh.maskErrors[name] = name + ": " + error.what();
            }
        }
        // Indexed vertices: corners of a point weld unless a face-varying attribute splits them (UV seams, hard edges).
        // The USD -> UE basis already reverses handedness. UE's clockwise
        // front faces must not receive a second index reversal.
        try {
            mesh.sourceCorners = HdUEWeldCorners(
                triangles, points.size(),
                [&](size_t a, size_t b) {
                    return normals.same(a, b) && uvSame(a, b) && colors.same(a, b) &&
                           std::all_of(masks.begin(), masks.end(),
                                       [&](const auto& mask) { return mask.second[a] == mask.second[b]; });
                },
                mesh.indices);
        } catch (const std::invalid_argument& error) {
            TF_RUNTIME_ERROR("%s at %s", error.what(), GetId().GetText());
            return;
        }
        const size_t vertices = mesh.sourceCorners.size();
        std::vector<uint32_t> vertexPoints(vertices);
        for (size_t v = 0; v < vertices; ++v)
            vertexPoints[v] = uint32_t(triangles[mesh.sourceCorners[v] / 3][int(mesh.sourceCorners[v] % 3)]);
        std::vector<GfVec3f> smooth;
        if (normals.values.empty() || std::any_of(normals.values.begin(), normals.values.end(),
                                                  [](const auto& n) { return n.GetLengthSq() < 1e-12f; }))
            try {
                smooth = HdUEComputeSmoothNormals(points, triangles);
            } catch (const std::invalid_argument& error) {
                TF_RUNTIME_ERROR("%s at %s", error.what(), GetId().GetText());
                return;
            }
        mesh.points.reserve(vertices);
        if (!uvSets.empty()) mesh.uvSets.resize(uvSets.size() - 1);
        mesh.normals.reserve(vertices);
        for (size_t v = 0; v < vertices; ++v) {
            const size_t corner = mesh.sourceCorners[v], point = vertexPoints[v];
            mesh.points.push_back(points[point]);
            const auto normal = normals.values.empty() ? GfVec3f(0) : normals.at(corner, point);
            mesh.normals.push_back(normal.GetLengthSq() < 1e-12f ? smooth[point] : normal);
            for (size_t k = 0; k < uvSets.size(); ++k) {
                const auto uv = uvSets[k].values.empty() ? GfVec2f(0) : uvSets[k].at(corner, point);
                k ? mesh.uvSets[k - 1].push_back(uv) : mesh.uv.push_back(uv);
            }
            if (!colors.values.empty()) mesh.colors.push_back(colors.at(corner, point));
            for (const auto& [name, values] : masks) mesh.masks[name].push_back(values[corner]);
        }
        {
            // Deformation blur from time-sampled points of the same count, else from per-point velocities.
            float times[HdUEMaxMotionSamples];
            VtValue samples[HdUEMaxMotionSamples];
            const size_t count = delegate->SamplePrimvar(GetId(), HdTokens->points, -HdUEMotionWindow, HdUEMotionWindow,
                                                         HdUEMaxMotionSamples, times, samples);
            bool moves = false;
            for (size_t k = 0; k < count && count > 1; ++k) {
                auto sample = value(samples[k], VtVec3fArray());
                if (subdivision) sample = subdivision->refine(sample, HdInterpolationVertex);
                if (sample.size() != points.size()) {
                    mesh.deformTimes.clear();
                    mesh.deformOffsets.clear();
                    moves = false;
                    break;
                }
                auto& offsets = mesh.deformOffsets.emplace_back();
                offsets.reserve(vertices);
                for (const uint32_t point : vertexPoints) {
                    offsets.push_back(sample[point] - points[point]);
                    moves = moves || offsets.back() != GfVec3f(0);
                }
                mesh.deformTimes.push_back(times[k]);
            }
            if (!moves) {
                mesh.deformTimes.clear();
                mesh.deformOffsets.clear();
                auto velocities = value(delegate->Get(GetId(), HdTokens->velocities), VtVec3fArray());
                if (subdivision) velocities = subdivision->refine(velocities, HdInterpolationVertex);
                if (velocities.size() == points.size() &&
                    std::any_of(velocities.begin(), velocities.end(), [](const auto& v) { return v != GfVec3f(0); })) {
                    const float fps = float(HdUEFramesPerSecond(*state));
                    mesh.deformTimes = {-1.f, 1.f};
                    mesh.deformOffsets.resize(2);
                    for (const uint32_t point : vertexPoints) {
                        const auto v = velocities[point] / fps;
                        mesh.deformOffsets[0].push_back(-v);
                        mesh.deformOffsets[1].push_back(v);
                    }
                }
            }
        }
        if (HdUETraceScene())
            TF_STATUS("hdUE wet sync %s scene=%s requested=%zu masks=%zu", GetId().GetText(), mesh.scenePath.c_str(),
                      maskNames.size(), mesh.masks.size());
        // Geometry and the unbound display-color material are prepared before taking the state lock, which the UE
        // thread's scene upload and every other Sync wait on.
        std::optional<HdUEMaterialData> displayColor;
        if ((mesh.material.empty() || HdUEMeshLightEmission(delegate, GetId()) != GfVec3f(0)) &&
            mesh.sceneAsset.empty())
            mesh.material = displayColor.emplace(displayColorMaterial(delegate, GetId(), mesh)).id;
        std::vector<HdUEMeshData> parts;
        try {
            parts = HdUEPartitionSubsets(std::move(mesh), topology, parameters);
        } catch (const std::invalid_argument& error) {
            TF_RUNTIME_ERROR("%s at %s", error.what(), GetId().GetText());
            return;
        }
        std::unique_lock lock(state->mutex);
        if (displayColor) {
            const auto previous = state->materials.find(displayColor->id);
            if (previous == state->materials.end() || previous->second.color != displayColor->color ||
                previous->second.opacity != displayColor->opacity ||
                previous->second.emissive != displayColor->emissive ||
                previous->second.vertexColor != displayColor->vertexColor) {
                state->materials[displayColor->id] = *displayColor;
                state->dirtyMaterials.insert(displayColor->id);
            }
        }
        std::set<std::string> current;
        for (auto& part : parts) {
            // A prim deleted and re-created with the same data (Houdini rebuilds the Hydra scene when its LOP chain
            // changes) is compared with its last data, so an unchanged mesh is neither removed nor re-uploaded.
            auto previous = state->meshes.find(part.id);
            const bool revived = previous == state->meshes.end() && state->erasedMeshes.count(part.id);
            if (revived) previous = state->erasedMeshes.find(part.id);
            const auto missing = revived ? state->erasedMeshes.end() : state->meshes.end();
            part.geometryHash = HdUEGeometryHash(part);
            if (previous != missing && previous->second.geometryHash == part.geometryHash)
                part.pickMesh = previous->second.pickMesh;
            // Pick structures are built on the first pick (HdUERenderDelegate pick): building a BVH for every mesh
            // here, under the state mutex, serialized scene loads (tens of seconds on Sponza).
            const bool onlyNativeBounds =
                previous != missing && !part.sceneAsset.empty() && previous->second.sceneAsset == part.sceneAsset &&
                previous->second.transform == part.transform && previous->second.visible == part.visible;
            const bool moved = previous != missing && HdUEPlacedOnly(previous->second, part.transform);
            // A mesh UE asked for in full (fullUploads) uploads even when nothing changed since it was released.
            const bool meshChanged =
                !onlyNativeBounds &&
                (previous == missing || state->fullUploads.count(part.id) ||
                 previous->second.geometryHash != part.geometryHash ||
                 (previous->second.transform != part.transform && !moved) ||
                 previous->second.material != part.material || previous->second.visible != part.visible ||
                 previous->second.sceneAsset != part.sceneAsset || previous->second.instanced != part.instanced ||
                 previous->second.instances != part.instances ||
                 previous->second.instanceColors != part.instanceColors ||
                 previous->second.doubleSided != part.doubleSided);
            const auto id = part.id;
            // Link categories change no geometry: the next upload recomputes the light-link table (prepareLinks).
            const bool maskChanged = previous == missing || previous->second.masks != part.masks ||
                                     previous->second.maskErrors != part.maskErrors ||
                                     previous->second.categories != part.categories;
            // A released mesh Hydra resent with the same geometry stays released: UE has it, and update_meshes (or a
            // move) carries what changed, unless UE asked for it in full (fullUploads).
            if (!revived && previous != missing && previous->second.released && !state->fullUploads.count(id) &&
                previous->second.geometryHash == part.geometryHash && part.sceneAsset.empty() &&
                previous->second.sceneAsset.empty() && previous->second.instanced == part.instanced &&
                previous->second.instances == part.instances &&
                previous->second.instanceColors == part.instanceColors &&
                (previous->second.transform == part.transform || moved) && HdUEReleasable(*state, part))
                HdUEReleaseGeometry(part);
            // A released mesh that keeps its arrays now (made wet, displaced, deforming) uploads: UE may hold it in a
            // form that no longer fits (a static mesh where wet needs a dynamic one).
            const bool unreleased = !revived && previous != missing && previous->second.released && !part.released;
            traceStored(part);
            state->meshes[id] = std::move(part);
            state->fullUploads.erase(id);
            state->removed.erase(id);
            state->repull.erase(state->meshes[id].sourceId);
            state->erasedMeshes.erase(id);
            if (meshChanged || unreleased) {
                if (state->dirtyMeshes.insert(id).second) state->pendingBytes += HdUEMeshBytes(state->meshes[id]);
                ++state->revision;
            } else if (moved || maskChanged) {
                if (moved) state->movedMeshes.insert(id);
                ++state->revision;
            }
            current.insert(id);
        }
        for (const auto& id : emitted_)
            if (!current.count(id)) {
                state->meshes.erase(id);
                state->dirtyMeshes.erase(id);
                state->movedMeshes.erase(id);
                state->removed.insert(id);
                ++state->revision;
            }
        emitted_ = std::move(current);
        lock.unlock();
        HdUEStreamMeshes(*state);
    }
    void RefreshSelectedMasks(HdSceneDelegate* delegate, HdUEState& state) const {
        // A renderer-setting edit does not change a scene-index data source.
        // Some hosts therefore do not run Rprim Sync for a new mask selector.
        // Fetch only that primvar from the synchronized Hydra delegate, triangulated as Sync triangulated the mesh
        // and mapped through the parts' source corners.
        std::lock_guard lock(state.mutex);
        const auto& bindings = *state.wetBindings;
        std::set<std::string> needed;
        for (const auto& id : emitted_) {
            const auto part = state.meshes.find(id);
            if (part == state.meshes.end()) continue;
            const auto name = HdUEWetValue<std::string>(HdUEWetSettings(bindings, part->second), "maskPrimvar", "");
            if (name.empty() || part->second.masks.count(name) || part->second.maskErrors.count(name)) continue;
            if (part->second.released) {
                // No arrays to map the mask onto: the render pass re-pulls the mesh, whose next Sync reads its masks
                // and uploads it in full.
                state.fullUploads.insert(id);
                state.repull.insert(part->second.sourceId);
                ++state.revision;
            } else
                needed.insert(name);
        }
        if (needed.empty()) return;
        const auto first = state.meshes.find(*emitted_.begin());
        const bool asset = first != state.meshes.end() && !first->second.sceneAsset.empty();
        const auto shape = triangulate(
            delegate, state, asset,
            value(primvar(delegate, GetId(), HdTokens->points, computedPrimvars(delegate, GetId())), VtVec3fArray()));
        HdMeshUtil util(&shape.topology, GetId());
        for (const auto& name : needed) {
            std::vector<float> values;
            std::string error;
            try {
                values = HdUEExpandWetMask(delegate->Get(GetId(), TfToken(name)),
                                           interpolation(delegate, GetId(), TfToken(name)), shape.points.size(),
                                           shape.topology, shape.triangles, shape.parameters, util);
            } catch (const std::invalid_argument& e) {
                error = name + ": " + e.what();
            }
            for (const auto& id : emitted_) {
                auto part = state.meshes.find(id);
                if (part == state.meshes.end()) continue;
                if (!error.empty())
                    part->second.maskErrors[name] = error;
                else {
                    auto& mask = part->second.masks[name];
                    mask.clear();
                    mask.reserve(part->second.points.size());
                    for (size_t i = 0; i < part->second.points.size(); ++i)
                        mask.push_back(values.at(part->second.sourceCorners[i]));
                }
            }
        }
    }
    void Finalize(HdRenderParam* param) override {
        auto* state = static_cast<HdUEState*>(param);
        for (const auto& id : emitted_) state->erase(id);
        state->erase(GetId().GetString() + ".displayColor");
    }

protected:
    void _InitRepr(const TfToken& token, HdDirtyBits*) override {
        _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr()));
    }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
};
// Points and curves become triangle meshes: one shared sphere instanced per point, and a capped tube
// per curve. They keep one material (bound, or constant displayColor/displayOpacity).
void HdUEStoreShape(HdSceneDelegate* delegate, const SdfPath& id, HdUEState& state, HdUEMeshData mesh) {
    std::lock_guard lock(state.mutex);
    if (mesh.material.empty()) {
        const auto material = displayColorMaterial(delegate, id, mesh);
        mesh.material = material.id;
        const auto previous = state.materials.find(material.id);
        if (previous == state.materials.end() || previous->second.color != material.color ||
            previous->second.opacity != material.opacity || previous->second.vertexColor != material.vertexColor) {
            state.materials[material.id] = material;
            state.dirtyMaterials.insert(material.id);
        }
    }
    const auto previous = state.meshes.find(mesh.id);
    mesh.geometryHash = HdUEGeometryHash(mesh);
    const bool sameGeometry = previous != state.meshes.end() && previous->second.geometryHash == mesh.geometryHash;
    const bool changed = !sameGeometry || state.fullUploads.count(mesh.id) ||
                         previous->second.transform != mesh.transform || previous->second.material != mesh.material ||
                         previous->second.visible != mesh.visible || previous->second.instanced != mesh.instanced ||
                         previous->second.instances != mesh.instances ||
                         previous->second.instanceColors != mesh.instanceColors;
    mesh.pickMesh = sameGeometry ? previous->second.pickMesh : nullptr;  // built on first pick
    const bool relinked = previous != state.meshes.end() && previous->second.categories != mesh.categories;
    const auto key = mesh.id;
    state.meshes[key] = std::move(mesh);
    state.fullUploads.erase(key);
    state.removed.erase(key);
    state.repull.erase(state.meshes[key].sourceId);
    if (changed) {
        state.dirtyMeshes.insert(key);
        ++state.revision;
    } else if (relinked)
        ++state.revision;
}
HdUEMeshData HdUEShapeBase(HdRprim& prim, HdSceneDelegate* delegate) {
    HdUEMeshData mesh;
    mesh.id = mesh.sourceId = prim.GetId().GetString();
    mesh.scenePath = mesh.bindingPath = delegate->GetScenePrimPath(prim.GetId(), -1).GetString();
    mesh.material = delegate->GetMaterialId(prim.GetId()).GetString();
    mesh.transform = delegate->GetTransform(prim.GetId());
    mesh.visible = delegate->GetVisible(prim.GetId());
    mesh.renderTag = delegate->GetRenderTag(prim.GetId());
    mesh.primId = prim.GetPrimId();
    mesh.categories = delegate->GetCategories(prim.GetId());
    mesh.blurScale = HdUEBlurScale(delegate, prim.GetId());
    return mesh;
}
// Widths per vertex (or with a matching count), else the first (constant), else the fallback.
float HdUEWidth(const VtFloatArray& widths, size_t index, float fallback) {
    if (widths.empty()) return fallback;
    return widths.size() > index ? widths[index] : widths[0];
}
// Unit sphere (8 x 6 UV sphere), indexed, with radial normals.
void HdUEAppendSphere(HdUEMeshData& mesh) {
    constexpr int Slices = 8, Stacks = 6;
    const auto base = uint32_t(mesh.points.size());
    const auto vertex = [&](int stack, int slice) { return base + uint32_t(stack * (Slices + 1) + slice); };
    for (int stack = 0; stack <= Stacks; ++stack)
        for (int slice = 0; slice <= Slices; ++slice) {
            const double theta = 3.14159265358979323846 * stack / Stacks,
                         phi = 2 * 3.14159265358979323846 * slice / Slices;
            const GfVec3f point(float(std::sin(theta) * std::cos(phi)), float(std::cos(theta)),
                                float(std::sin(theta) * std::sin(phi)));
            mesh.points.push_back(point);
            mesh.normals.push_back(point);
            mesh.uv.emplace_back(0.f, 0.f);
        }
    for (int stack = 0; stack < Stacks; ++stack)
        for (int slice = 0; slice < Slices; ++slice) {
            const uint32_t a = vertex(stack, slice), b = vertex(stack + 1, slice), c = vertex(stack + 1, slice + 1),
                           d = vertex(stack, slice + 1);
            // Counter-clockwise seen from outside, as USD front faces are.
            mesh.indices.insert(mesh.indices.end(), {a, c, b, a, d, c});
        }
}
class Points final : public HdPoints {
public:
    explicit Points(const SdfPath& id) : HdPoints(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        auto mesh = HdUEShapeBase(*this, delegate);
        _UpdateInstancer(delegate, bits);
        const auto points =
            value(primvar(delegate, GetId(), HdTokens->points, computedPrimvars(delegate, GetId())), VtVec3fArray());
        const auto widths = value(delegate->Get(GetId(), HdTokens->widths), VtFloatArray());
        HdUEAppendSphere(mesh);
        mesh.instanced = true;
        auto placements = HdUEInstancePlacements(delegate, GetId(), GetInstancerId(), mesh.transform);
        const auto* instancer = HdUEInstancer(delegate, GetInstancerId());
        if (!instancer) placements = {mesh.transform};
        const float fps = float(HdUEFramesPerSecond(*static_cast<HdUEState*>(param)));
        // Points move with their prim (or instancer) transform, their velocities and their instances'.
        HdUEMeshData placed;
        if (instancer)
            instancer->ComputeInstanceMotion(GetId(), fps, placements.size(), placed);
        else
            placed.motionRight = HdUETransformMotion(delegate, GetId(), mesh.transform, false);
        mesh.motionRight = placed.motionRight;
        // Per point displayColor when it varies, else the instances' (which replace the points' own).
        std::vector<GfVec3f> colors;
        if (mesh.material.empty()) {
            const auto authored = value(delegate->Get(GetId(), HdTokens->displayColor), VtVec3fArray());
            colors = HdUEVaryingColors(std::vector<GfVec3f>(authored.begin(), authored.end()), points.size());
            if (instancer) placed.instanceColors = instancer->ComputeInstanceColors(GetId(), HdTokens->displayColor);
            if (placed.instanceColors.size() != placements.size()) placed.instanceColors.clear();
        }
        const auto velocities = value(delegate->Get(GetId(), HdTokens->velocities), VtVec3fArray());
        // Every point of every placement (nested under a Hydra instancer when there is one).
        for (size_t p = 0; p < placements.size(); ++p)
            for (size_t i = 0; i < points.size(); ++i) {
                if (velocities.size() == points.size() || !placed.velocities.empty())
                    mesh.velocities.push_back((velocities.size() == points.size()
                                                   ? GfVec3f(placements[p].TransformDir(GfVec3d(velocities[i]))) / fps
                                                   : GfVec3f(0)) +
                                              (placed.velocities.empty() ? GfVec3f(0) : placed.velocities[p]));
                if (!placed.accelerations.empty()) mesh.accelerations.push_back(placed.accelerations[p]);
                if (!placed.angularVelocities.empty()) mesh.angularVelocities.push_back(placed.angularVelocities[p]);
                const double radius = .5 * HdUEWidth(widths, i, .1f);
                mesh.instances.push_back(GfMatrix4d(1).SetScale(radius) *
                                         GfMatrix4d(1).SetTranslate(GfVec3d(points[i])) * placements[p]);
                if (!placed.instanceColors.empty())
                    mesh.instanceColors.push_back(placed.instanceColors[p]);
                else if (!colors.empty())
                    mesh.instanceColors.push_back(colors[i]);
                mesh.instanceIds.push_back(instancer ? int32_t(p) : -1);
            }
        HdUEStoreShape(delegate, GetId(), *static_cast<HdUEState*>(param), std::move(mesh));
        *bits = HdChangeTracker::Clean;
    }
    void Finalize(HdRenderParam* param) override {
        auto* state = static_cast<HdUEState*>(param);
        state->erase(GetId().GetString());
        state->erase(GetId().GetString() + ".displayColor");
    }

protected:
    void _InitRepr(const TfToken& token, HdDirtyBits*) override {
        _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr()));
    }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
};
// One primvar along one curve, by interpolation: per control vertex (blended with the basis weights), per segment end
// (varying: linear across each segment), else one value (uniform, constant, or the fallback).
template <class T>
struct CurvePrimvar {
    std::vector<T> cvs, ends;
    T value;
};
class Curves final : public HdBasisCurves {
    // Samples along one curve: cubic segments are evaluated in their basis; linear curves keep their vertices.
    struct Sample {
        GfVec3d point;
        size_t first, segment;
        double u, weights[4];
    };
    static size_t segments(size_t n, const TfToken& type, const TfToken& basis, bool periodic) {
        if (type == HdTokens->linear || n < 4) return periodic || !n ? n : n - 1;
        if (basis == HdTokens->bezier) return periodic ? n / 3 : (n - 1) / 3;
        return periodic ? n : n - 3;
    }
    static std::vector<Sample> samples(const std::vector<GfVec3d>& cv, const TfToken& type, const TfToken& basis,
                                       bool periodic) {
        const size_t n = cv.size();
        std::vector<Sample> out;
        if (type == HdTokens->linear || n < 4) {
            for (size_t i = 0; i < n; ++i) out.push_back({cv[i], i, i, 0, {1, 0, 0, 0}});
            if (periodic && n > 2) {
                out.push_back(out.front());
                out.back().segment = n - 1;
                out.back().u = 1;
            }
            return out;
        }
        const bool bezier = basis == HdTokens->bezier;
        const size_t step = bezier ? 3 : 1;
        constexpr int Steps = 16;
        for (size_t s = 0, count = segments(n, type, basis, periodic); s < count; ++s) {
            const size_t first = s * step;
            for (int t = (s ? 1 : 0); t <= Steps; ++t) {
                const double u = double(t) / Steps, u2 = u * u, u3 = u2 * u, m = 1 - u;
                Sample sample{GfVec3d(0), first, s, u};
                double* b = sample.weights;
                if (bezier) {
                    b[0] = m * m * m;
                    b[1] = 3 * u * m * m;
                    b[2] = 3 * u2 * m;
                    b[3] = u3;
                } else if (basis == HdTokens->catmullRom) {
                    b[0] = .5 * (-u3 + 2 * u2 - u);
                    b[1] = .5 * (3 * u3 - 5 * u2 + 2);
                    b[2] = .5 * (-3 * u3 + 4 * u2 + u);
                    b[3] = .5 * (u3 - u2);
                } else {
                    b[0] = m * m * m / 6;
                    b[1] = (3 * u3 - 6 * u2 + 4) / 6;
                    b[2] = (-3 * u3 + 3 * u2 + 3 * u + 1) / 6;
                    b[3] = u3 / 6;
                }  // bspline
                for (size_t k = 0; k < 4; ++k) sample.point += b[k] * cv[(first + k) % n];
                out.push_back(sample);
            }
        }
        return out;
    }
    // vertices: the curve's points; pinned curves get USD's phantom end points (2 P0 - P1, 2 Pn-1 - Pn-2) on every
    // vertex primvar. Varying values start at varying, count per curve.
    template <class T, class A>
    static CurvePrimvar<T> curvePrimvar(const VtArray<A>& values, HdInterpolation domain,
                                        const std::vector<size_t>& vertices, bool pinned, size_t curve, size_t varying,
                                        size_t count, T fallback) {
        CurvePrimvar<T> out{{}, {}, values.empty() ? fallback : T(values[0])};
        const auto get = [&](size_t i) { return i < values.size() ? T(values[i]) : out.value; };
        if (domain == HdInterpolationVertex) {
            for (const size_t v : vertices) out.cvs.push_back(get(v));
            if (pinned && out.cvs.size() > 1) {
                const size_t n = out.cvs.size();
                const T first = 2. * out.cvs[0] - out.cvs[1], last = 2. * out.cvs[n - 1] - out.cvs[n - 2];
                out.cvs.insert(out.cvs.begin(), first);
                out.cvs.push_back(last);
            }
        } else if (domain == HdInterpolationVarying)
            for (size_t i = 0; i < count; ++i) out.ends.push_back(get(varying + i));
        else if (domain == HdInterpolationUniform)
            out.value = get(curve);
        return out;
    }
    template <class T>
    static T at(const CurvePrimvar<T>& primvar, const Sample& s) {
        if (!primvar.cvs.empty()) {
            T value(0);
            for (size_t k = 0; k < 4; ++k) value += s.weights[k] * primvar.cvs[(s.first + k) % primvar.cvs.size()];
            return value;
        }
        if (!primvar.ends.empty())
            return (1 - s.u) * primvar.ends[s.segment % primvar.ends.size()] +
                   s.u * primvar.ends[(s.segment + 1) % primvar.ends.size()];
        return primvar.value;
    }

public:
    explicit Curves(const SdfPath& id) : HdBasisCurves(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        auto mesh = HdUEShapeBase(*this, delegate);
        _UpdateInstancer(delegate, bits);
        const CleanBits clean{bits};
        const auto topology = delegate->GetBasisCurvesTopology(GetId());
        // displayColor per vertex, segment end or curve; constant colors stay on the material.
        const auto displayColor = mesh.material.empty()
                                      ? value(delegate->Get(GetId(), HdTokens->displayColor), VtVec3fArray())
                                      : VtVec3fArray();
        const auto colorDomain = interpolation(delegate, GetId(), HdTokens->displayColor);
        std::vector<GfVec3f> colors;
        const auto points =
            value(primvar(delegate, GetId(), HdTokens->points, computedPrimvars(delegate, GetId())), VtVec3fArray());
        const auto widths = value(delegate->Get(GetId(), HdTokens->widths), VtFloatArray());
        const auto widthDomain = interpolation(delegate, GetId(), HdTokens->widths);
        // Curves with normals are ribbons facing them.
        const auto normals = value(delegate->Get(GetId(), HdTokens->normals), VtVec3fArray());
        const auto normalDomain = interpolation(delegate, GetId(), HdTokens->normals);
        const bool ribbon = !normals.empty();
        const auto counts = topology.GetCurveVertexCounts();
        const auto indices = topology.GetCurveIndices();
        const auto type = topology.GetCurveType(), basis = topology.GetCurveBasis();
        const bool periodic = topology.GetCurveWrap() == HdTokens->periodic;
        const bool pinned =
            topology.GetCurveWrap() == HdTokens->pinned && type == HdTokens->cubic && basis != HdTokens->bezier;
        const int Sides = ribbon ? 2 : 6;
        size_t offset = 0, varying = 0;
        // ponytail: 6-sided tubes per sample; dense grooms produce large meshes, camera-facing ribbons if that matters.
        for (size_t curveIndex = 0; curveIndex < counts.size(); ++curveIndex) {
            const size_t count = size_t(std::max(0, counts[curveIndex]));
            std::vector<size_t> vertices;
            for (size_t i = 0; i < count; ++i) {
                const size_t vertex = indices.empty() ? offset + i : size_t(indices[offset + i]);
                if (vertex >= points.size()) {
                    TF_RUNTIME_ERROR("Invalid curve index at %s", GetId().GetText());
                    return;
                }
                vertices.push_back(vertex);
            }
            offset += count;
            const size_t cvCount = count + (pinned && count > 1 ? 2 : 0);
            const size_t varyingCount = segments(cvCount, type, basis, periodic) + (periodic ? 0 : 1);
            const auto primvar = [&](const auto& values, HdInterpolation domain, auto fallback) {
                return curvePrimvar(values, domain, vertices, pinned, curveIndex, varying, varyingCount, fallback);
            };
            const auto width = primvar(widths, widthDomain, .01);
            const auto color = primvar(displayColor, colorDomain, GfVec3d(1));
            const auto normal = primvar(normals, normalDomain, GfVec3d(0));
            varying += varyingCount;
            const auto curve = samples(primvar(points, HdInterpolationVertex, GfVec3d(0)).cvs, type, basis, periodic);
            if (curve.size() < 2) continue;
            // Parallel-transported frame along the curve; ribbons span the authored normal's binormal instead.
            GfVec3d tangent = (curve[1].point - curve[0].point).GetNormalized();
            GfVec3d frame =
                GfCross(tangent, std::abs(tangent[1]) < .9 ? GfVec3d(0, 1, 0) : GfVec3d(1, 0, 0)).GetNormalized();
            std::vector<std::vector<GfVec3f>> rings, ringNormals;
            for (size_t i = 0; i < curve.size(); ++i) {
                const auto next = curve[std::min(i + 1, curve.size() - 1)].point, prev = curve[i ? i - 1 : 0].point;
                const auto t = (next - prev).GetLength() > 1e-12 ? (next - prev).GetNormalized() : tangent;
                frame = frame - t * GfDot(frame, t);
                if (frame.GetLength() < 1e-9)
                    frame = GfCross(t, std::abs(t[1]) < .9 ? GfVec3d(0, 1, 0) : GfVec3d(1, 0, 0));
                frame.Normalize();
                if (ribbon)
                    if (const auto n = at(normal, curve[i]); GfCross(t, n).GetLength() > 1e-9)
                        frame = GfCross(GfCross(t, n), t).GetNormalized();
                const auto binormal = GfCross(t, frame);
                const double radius = .5 * std::max(0., at(width, curve[i]));
                rings.emplace_back();
                ringNormals.emplace_back();
                tangent = t;
                for (int side = 0; side < Sides; ++side) {
                    // A ribbon's two edges lie along the binormal and face the normal.
                    const double angle = 2 * 3.14159265358979323846 * side / Sides;
                    const auto dir =
                        ribbon ? (side ? 1. : -1.) * binormal : std::cos(angle) * frame + std::sin(angle) * binormal;
                    rings.back().emplace_back(curve[i].point + radius * dir);
                    ringNormals.back().emplace_back(ribbon ? frame : dir);
                }
            }
            // Ring vertices are shared by the quads around them.
            const auto vertex = [&, base = uint32_t(mesh.points.size())](size_t ring, int side) {
                return base + uint32_t(ring * Sides + size_t(side));
            };
            for (size_t ring = 0; ring < rings.size(); ++ring)
                for (int side = 0; side < Sides; ++side) {
                    mesh.points.push_back(rings[ring][side]);
                    mesh.normals.push_back(ringNormals[ring][side]);
                    colors.emplace_back(at(color, curve[ring]));
                    mesh.uv.emplace_back(float(side) / Sides, float(ring) / float(rings.size() - 1));
                }
            for (size_t i = 0; i + 1 < rings.size(); ++i)
                for (int side = 0; side < (ribbon ? 1 : Sides); ++side) {
                    const int next = (side + 1) % Sides;
                    // Counter-clockwise seen from outside (dir = cos * normal + sin * binormal turns about the
                    // tangent).
                    mesh.indices.insert(mesh.indices.end(), {vertex(i, side), vertex(i + 1, next), vertex(i + 1, side),
                                                             vertex(i, side), vertex(i, next), vertex(i + 1, next)});
                }
            if (periodic || ribbon) continue;
            // Flat caps: the end faces +tangent (ring order is counter-clockwise seen from there), the start faces
            // -tangent.
            for (const bool end : {false, true}) {
                const size_t ring = end ? rings.size() - 1 : 0;
                const auto t =
                    end ? curve.back().point - curve[curve.size() - 2].point : curve[0].point - curve[1].point;
                if (t.GetLength() < 1e-12) continue;
                const GfVec3f facing(t.GetNormalized());
                const auto center = uint32_t(mesh.points.size());  // then the ring's corners, with the cap's normal
                for (int side = -1; side < Sides; ++side) {
                    mesh.points.push_back(side < 0 ? GfVec3f(curve[ring].point) : rings[ring][side]);
                    mesh.normals.push_back(facing);
                    mesh.uv.emplace_back(0.f, float(end));
                    colors.emplace_back(at(color, curve[ring]));
                }
                for (int side = 0; side < Sides; ++side) {
                    const uint32_t a = center + 1 + uint32_t(side), b = center + 1 + uint32_t((side + 1) % Sides);
                    mesh.indices.insert(mesh.indices.end(), {center, end ? a : b, end ? b : a});
                }
            }
        }
        mesh.colors = HdUEVaryingColors(std::move(colors), mesh.points.size());
        mesh.instances = HdUEInstancePlacements(delegate, GetId(), GetInstancerId(), mesh.transform);
        mesh.instanced = !GetInstancerId().IsEmpty();
        mesh.motionLeft = HdUETransformMotion(delegate, GetId(), mesh.transform, true);
        if (const auto* instancer = HdUEInstancer(delegate, GetInstancerId())) {
            instancer->ComputeInstanceMotion(GetId(), HdUEFramesPerSecond(*static_cast<HdUEState*>(param)),
                                             mesh.instances.size(), mesh);
            // Instance-rate displayColor replaces the curves' own.
            if (mesh.material.empty())
                mesh.instanceColors = instancer->ComputeInstanceColors(GetId(), HdTokens->displayColor);
            if (mesh.instanceColors.size() != mesh.instances.size())
                mesh.instanceColors.clear();
            else
                mesh.colors.clear();
        }
        HdUEStoreShape(delegate, GetId(), *static_cast<HdUEState*>(param), std::move(mesh));
    }
    void Finalize(HdRenderParam* param) override {
        auto* state = static_cast<HdUEState*>(param);
        state->erase(GetId().GetString());
        state->erase(GetId().GetString() + ".displayColor");
    }

protected:
    void _InitRepr(const TfToken& token, HdDirtyBits*) override {
        _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr()));
    }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
};
#if defined(HDUE_HOUDINI)
// openvdbAsset fields carry filePath/fieldName; volumes read them through the scene delegate.
class Field final : public HdField {
public:
    explicit Field(const SdfPath& id) : HdField(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate*, HdRenderParam*, HdDirtyBits* bits) override { *bits = Clean; }
};
// Density from an OpenVDB float grid, densified over its active bounding box.
class Volume final : public HdVolume {
    static constexpr int MaxResolution = 512;  // per axis; larger grids are point-sampled with a stride
    std::optional<GfMatrix4d> gridToLocal_;    // the loaded grid's voxel to prim space
    std::vector<std::string> emitted_;         // the prim's id, or one id per instance under a Hydra instancer
public:
    explicit Volume(const SdfPath& id) : HdVolume(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        auto* state = static_cast<HdUEState*>(param);
        HdUEVolumeData volume;
        volume.id = GetId().GetString();
        volume.visible = delegate->GetVisible(GetId());
        volume.renderTag = delegate->GetRenderTag(GetId());
        _UpdateInstancer(delegate, bits);
        const auto transform = delegate->GetTransform(GetId());
        auto placements = HdUEInstancePlacements(delegate, GetId(), GetInstancerId(), transform);
        if (!HdUEInstancer(delegate, GetInstancerId())) placements = {transform};
        // One volume per placement, sharing the density grid: "<id>#<instance>" under an instancer.
        const auto publish = [&](const HdUEVolumeData& loaded) {
            std::vector<std::string> ids;
            for (size_t i = 0; i < placements.size(); ++i) {
                auto& out = state->volumes[ids.emplace_back(
                    GetInstancerId().IsEmpty() ? volume.id : volume.id + "#" + std::to_string(i))];
                out = loaded;
                out.id = ids.back();
                out.visible = volume.visible;
                out.renderTag = volume.renderTag;
                out.localToWorld = (gridToLocal_ ? *gridToLocal_ : loaded.localToWorld) * placements[i];
                state->dirtyVolumes.insert(out.id);
                state->removed.erase(out.id);
            }
            for (const auto& id : emitted_)
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                    state->volumes.erase(id);
                    state->dirtyVolumes.erase(id);
                    state->removed.insert(id);
                }
            emitted_ = std::move(ids);
            ++state->revision;
            *bits = HdChangeTracker::Clean;
        };
        // Visibility, purpose, transform and instance edits keep the loaded grid instead of reading the file again.
        if (gridToLocal_ && !emitted_.empty() &&
            !(*bits &
              ~(HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyRenderTag | HdChangeTracker::DirtyTransform |
                HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex))) {
            std::lock_guard lock(state->mutex);
            if (const auto it = state->volumes.find(emitted_.front()); it != state->volumes.end()) {
                const auto loaded = it->second;  // the density is shared, not copied
                publish(loaded);
                return;
            }
        }
        gridToLocal_.reset();
        const auto fields = delegate->GetVolumeFieldDescriptors(GetId());
        const HdVolumeFieldDescriptor* chosen = nullptr;
        for (const auto& field : fields)
            if (field.fieldName == TfToken("density")) chosen = &field;
        if (!chosen && !fields.empty()) chosen = &fields.front();
        try {
            if (chosen) {
                const bool vdb = chosen->fieldPrimType == TfToken("openvdbAsset");
                if (!vdb && chosen->fieldPrimType != TfToken("houdiniFieldAsset"))
                    throw std::runtime_error("only openvdbAsset and houdiniFieldAsset fields are supported, not " +
                                             chosen->fieldPrimType.GetString());
                const auto file = value(delegate->Get(chosen->fieldId, TfToken("filePath")), SdfAssetPath());
                const auto path = file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
                auto gridName = value(delegate->Get(chosen->fieldId, TfToken("fieldName")), TfToken()).GetString();
                if (gridName.empty()) gridName = chosen->fieldName.GetString();
                const auto index = delegate->Get(chosen->fieldId, TfToken("fieldIndex"));
                static std::once_flag initialized;
                std::call_once(initialized, [] { openvdb::initialize(); });
                const bool vdbFile =
                    vdb && path.rfind("op:", 0) != 0 && TfStringEndsWith(TfStringToLower(path), ".vdb");
                if (vdbFile) {
                    openvdb::io::File file(path);
                    file.open(false);
                    const auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(gridName));
                    file.close();
                    if (!grid) throw std::runtime_error("grid " + gridName + " in " + path + " is not a float grid");
                    HdUEDensifyGrid(*grid, MaxResolution, volume);
                } else  // SOP volumes behind op: paths, and .bgeo files: VDB or native Houdini volumes
                    HdUELoadHoudiniField(path, gridName, index.IsHolding<int>() ? index.UncheckedGet<int>() : 0, vdb,
                                         MaxResolution, volume);
                gridToLocal_ = volume.localToWorld;
            }
        } catch (const std::exception& error) {
            TF_RUNTIME_ERROR("hdUE volume %s: %s", GetId().GetText(), error.what());
            gridToLocal_.reset();
            volume.width = volume.height = volume.depth = 0;
            volume.density.clear();
        }
        if (HdUETracePass()) {
            const auto m = volume.localToWorld * transform;
            std::fprintf(stderr, "hdUE volume %s fields=%zu dims=%ux%ux%u origin=(%g,%g,%g) x=(%g,%g,%g)%c",
                         GetId().GetText(), fields.size(), volume.width, volume.height, volume.depth, m[3][0], m[3][1],
                         m[3][2], m[0][0], m[0][1], m[0][2], 10);
            std::fflush(stderr);
        }
        std::lock_guard lock(state->mutex);
        publish(volume);
    }
    void Finalize(HdRenderParam* param) override {
        for (const auto& id : emitted_) static_cast<HdUEState*>(param)->erase(id);
    }

protected:
    void _InitRepr(const TfToken& token, HdDirtyBits*) override {
        _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr()));
    }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
};
#endif
class Light final : public HdLight {
    bool viewerStored_ = false, viewerVisible_ = false;
    GfVec3f viewerColor_{0.f};
    uint32_t type_;

public:
    Light(const SdfPath& id, uint32_t type) : HdLight(id), type_(type) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits) override {
        HdUELightData data;
        data.id = GetId().GetString();
        data.type = type_;
        data.transform = delegate->GetTransform(GetId());
        data.visible = delegate->GetVisible(GetId());
        const auto get = [&](const char* name) { return delegate->GetLightParamValue(GetId(), TfToken(name)); };
        const auto applicationLight = get("params");
        data.viewer = applicationLight.IsHolding<GlfSimpleLight>() &&
                      !applicationLight.UncheckedGet<GlfSimpleLight>().IsDomeLight();
        data.color = HdUEFromSceneLinear(value(get("color"), data.color));
        data.intensity = value(get("intensity"), 1.f) * std::exp2(value(get("exposure"), 0.f));
        if (value(get("enableColorTemperature"), false)) {
            const auto temperature = UsdLuxBlackbodyTemperatureAsRgb(value(get("colorTemperature"), 6500.f));
            for (int c = 0; c < 3; ++c) data.color[c] *= temperature[c];
        }
        if (value(get("normalize"), false)) data.flags |= HDUE_LIGHT_NORMALIZE;
        if (!value(get("shadow:enable"), true)) data.flags &= ~HDUE_LIGHT_SHADOWS;
        if (value(get("treatAsPoint"), false)) data.flags |= HDUE_LIGHT_POINT;
        if (!value(get("unreal:visibleBackground"), true)) data.flags &= ~HDUE_LIGHT_BACKGROUND;
        if (!value(get("unreal:megaLights"), true)) data.flags &= ~HDUE_LIGHT_MEGALIGHTS;
        data.diffuse = value(get("diffuse"), 1.f);
        data.specular = value(get("specular"), 1.f);
        data.angle = value(get("angle"), .53f);
        data.coneAngle = value(get("shaping:cone:angle"), 180.f);
        data.coneSoftness = value(get("shaping:cone:softness"), 0.f);
        if (const auto ies = get("shaping:ies:file"); ies.IsHolding<SdfAssetPath>()) {
            const auto& file = ies.UncheckedGet<SdfAssetPath>();
            data.ies = file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
        }
        data.iesAngleScale = value(get("shaping:ies:angleScale"), 0.f);
        data.iesNormalize = value(get("shaping:ies:normalize"), false);
        data.lightLink = value(get("lightLink"), TfToken());
        data.shadowLink = value(get("shadowLink"), TfToken());
        // DomeLight_1 poleAxis: UsdImaging resolves it (and "scene" against the stage up axis) into domeOffset, a
        // rotation of the dome alone, applied in its local space before its transform.
        if (const auto offset = get("domeOffset"); type_ == HDUE_DOME_LIGHT && offset.IsHolding<GfMatrix4d>())
            data.transform = offset.UncheckedGet<GfMatrix4d>() * data.transform;
        // Valid UsdLux that UE cannot show: warned once per process, as Karma ignores what it does not support.
        static std::once_flag filtersWarned, shadowColorWarned;
        if (!value(get("filters"), SdfPathVector()).empty())
            std::call_once(filtersWarned,
                           [&] { TF_WARN("hdUE: light filters are not supported (%s)", GetId().GetText()); });
        if (value(get("shadow:color"), GfVec3f(0)) != GfVec3f(0))
            std::call_once(shadowColorWarned, [&] {
                TF_WARN("hdUE: shadow:color is not supported; shadows stay black (%s)", GetId().GetText());
            });
        // UE lights have no shadow length limit (UE's USD importer maps shadow:enable only, USDLightConversion.cpp).
        static std::once_flag shadowDistanceWarned;
        if (value(get("shadow:distance"), -1.f) >= 0 || value(get("shadow:falloff"), -1.f) >= 0)
            std::call_once(shadowDistanceWarned, [&] {
                TF_WARN("hdUE: shadow:distance and shadow:falloff are not supported; shadows reach any distance (%s)",
                        GetId().GetText());
            });
        auto texture = get("texture:file");
        if (applicationLight.IsHolding<GlfSimpleLight>() &&
            applicationLight.UncheckedGet<GlfSimpleLight>().IsDomeLight())
            texture = VtValue(applicationLight.UncheckedGet<GlfSimpleLight>().GetDomeLightTextureFile());
        if (texture.IsHolding<SdfAssetPath>()) {
            const auto& file = texture.UncheckedGet<SdfAssetPath>();
            data.texture = file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
        }
        const auto format = value(get("texture:format"), TfToken("automatic"));
        data.textureFormat = format == TfToken("automatic") ? HDUE_ENVIRONMENT_AUTOMATIC
                             : format == TfToken("latlong") ? HDUE_ENVIRONMENT_LATLONG
                                                            : 99;
        if (data.viewer) {
            // Hdx uses 15000 for its camera light when the renderer is not
            // Storm. Treat this application light separately from USD lights.
            const auto diffuse = applicationLight.UncheckedGet<GlfSimpleLight>().GetDiffuse();
            data.color = GfVec3f(diffuse[0], diffuse[1], diffuse[2]);
            data.intensity = 1;
            data.transform = GfMatrix4d(1);
            data.type = HDUE_CAMERA_DISTANT_LIGHT;
        }
        const bool radial = type_ == HDUE_SPHERE_LIGHT || type_ == HDUE_DISK_LIGHT || type_ == HDUE_CYLINDER_LIGHT;
        data.width = value(get(radial ? "radius" : "width"), radial ? .5f : 1.f);
        data.height = type_ == HDUE_DISK_LIGHT ? data.width : value(get("height"), 1.f);
        data.length = value(get("length"), 1.f);
        auto* state = static_cast<HdUEState*>(param);
        // Hdx moves the camera light with every camera move; the backend follows the camera itself, so an unchanged
        // viewer light needs no state lock (a scene upload holds it for seconds and Houdini's UI would wait).
        if (data.viewer && viewerStored_ && viewerColor_ == data.color && viewerVisible_ == data.visible) {
            *bits = Clean;
            return;
        }
        std::lock_guard lock(state->mutex);
        if (data.viewer) {
            viewerStored_ = true;
            viewerColor_ = data.color;
            viewerVisible_ = data.visible;
        }
        const auto old = state->lights.find(data.id);
        if (old != state->lights.end() && old->second.transform == data.transform &&
            old->second.texture == data.texture && old->second.color == data.color &&
            old->second.intensity == data.intensity && old->second.visible == data.visible &&
            old->second.type == data.type && old->second.width == data.width && old->second.height == data.height &&
            old->second.length == data.length && old->second.flags == data.flags &&
            old->second.diffuse == data.diffuse && old->second.specular == data.specular &&
            old->second.angle == data.angle && old->second.coneAngle == data.coneAngle &&
            old->second.coneSoftness == data.coneSoftness && old->second.textureFormat == data.textureFormat &&
            old->second.ies == data.ies && old->second.iesAngleScale == data.iesAngleScale &&
            old->second.iesNormalize == data.iesNormalize && old->second.lightLink == data.lightLink &&
            old->second.shadowLink == data.shadowLink) {
            *bits = Clean;
            return;
        }
        if (data.viewer && old != state->lights.end() && old->second.viewer && old->second.color == data.color &&
            old->second.visible == data.visible) {
            // The backend follows the render camera at request time. Moving
            // Hdx's camera light must not submit/cancel the entire scene.
            *bits = Clean;
            return;
        }
        state->lights[data.id] = data;
        state->dirtyLights.insert(data.id);
        ++state->revision;
        *bits = Clean;
    }
    void Finalize(HdRenderParam* param) override { static_cast<HdUEState*>(param)->erase(GetId().GetString()); }
};
}  // namespace
// HdCamera that also records its transform samples for camera motion blur.
class Camera final : public HdCamera {
public:
    explicit Camera(const SdfPath& id) : HdCamera(id) {}
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits) override {
        const bool transformDirty = *bits & DirtyTransform;
        HdCamera::Sync(delegate, param, bits);
        if (!transformDirty || GetId().IsEmpty()) return;
        float times[HdUEMaxMotionSamples];
        GfMatrix4d values[HdUEMaxMotionSamples];
        const size_t count = delegate->SampleTransform(GetId(), -HdUEMotionWindow, HdUEMotionWindow,
                                                       HdUEMaxMotionSamples, times, values);
        HdUEMotionSamples motion;
        for (size_t i = 0; i < std::min(count, HdUEMaxMotionSamples); ++i) {
            motion.times.push_back(times[i]);
            motion.values.push_back(values[i]);
        }
        auto* state = static_cast<HdUEState*>(param);
        std::lock_guard lock(state->cameraMutex);
        if (motion.moving())
            state->cameraMotion[GetId().GetString()] = motion;
        else
            state->cameraMotion.erase(GetId().GetString());
    }
};
HdSprim* HdUECreateCamera(const SdfPath& id) { return new Camera(id); }
HdRprim* HdUECreateMesh(const SdfPath& id) { return new Mesh(id); }
HdRprim* HdUECreatePoints(const SdfPath& id) { return new Points(id); }
HdRprim* HdUECreateCurves(const SdfPath& id) { return new Curves(id); }
#if defined(HDUE_HOUDINI)
HdRprim* HdUECreateVolume(const SdfPath& id) { return new Volume(id); }
HdBprim* HdUECreateField(const SdfPath& id) { return new Field(id); }
#endif
HdInstancer* HdUECreateInstancer(HdSceneDelegate* delegate, const SdfPath& id) { return new Instancer(delegate, id); }
void HdUERefreshWetMasks(HdUEState& state, HdRenderIndex* index) {
    std::set<SdfPath> sources;
    {
        std::lock_guard lock(state.mutex);
        for (const auto& [id, part] : state.meshes) {
            const auto name = HdUEWetValue<std::string>(HdUEWetSettings(*state.wetBindings, part), "maskPrimvar", "");
            if (!name.empty() && !part.masks.count(name) && !part.maskErrors.count(name))
                sources.insert(SdfPath(part.sourceId));
        }
    }
    for (const auto& id : sources)
        if (const auto* mesh = dynamic_cast<const Mesh*>(index->GetRprim(id)))
            if (auto* delegate = index->GetSceneDelegateForRprim(id)) mesh->RefreshSelectedMasks(delegate, state);
}
// Re-reads released meshes from their scene delegate by calling their Sync with the geometry bits, as Hydra would for
// prims marked dirty. Houdini's viewport never syncs an Rprim the render pass marks dirty (its Sync runs only for
// stage edits), so the pass syncs them itself, on its own thread, which drives Hydra's Sync.
void HdUERepullMeshes(HdUEState& state, HdRenderIndex* index) {
    const auto ids = state.locked([&] { return state.repull; });
    for (const auto& id : ids) {
        const SdfPath path(id);
        // GetRprim hands out const prims; Hydra's own Sync calls them non-const.
        auto* rprim = const_cast<HdRprim*>(index->GetRprim(path));
        auto* delegate = rprim ? index->GetSceneDelegateForRprim(path) : nullptr;
        if (!delegate) {  // deleted meanwhile: nothing to wait for
            state.locked([&] { state.repull.erase(id); });
            continue;
        }
        HdDirtyBits bits = HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
                           HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyNormals;
        rprim->Sync(delegate, &state, &bits, HdReprTokens->smoothHull);
    }
}
HdSprim* HdUECreateLight(const SdfPath& id, uint32_t type) { return new Light(id, type); }
// Mesh, plugin and portal lights render nothing in hdUE. Hydra drops Sprim types a delegate does not support, silently;
// accepting them says so once per type.
HdSprim* HdUECreateUnsupportedLight(const SdfPath& id, const TfToken& type) {
    struct Unsupported final : HdLight {
        TfToken type;
        Unsupported(const SdfPath& id, const TfToken& type) : HdLight(id), type(type) {}
        HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
        void Sync(HdSceneDelegate* delegate, HdRenderParam*, HdDirtyBits* bits) override {
            // The generic light Sprim (GeometryLight, PortalLight) is named by its USD type.
            auto name = type;
            if (type == HdPrimTypeTokens->light && !GetId().IsEmpty())
                if (const auto typeName =
                        UsdImagingUsdPrimInfoSchema::GetFromParent(
                            delegate->GetRenderIndex().GetTerminalSceneIndex()->GetPrim(GetId()).dataSource)
                            .GetTypeName())
                    name = typeName->GetTypedValue(0);
            static std::mutex mutex;
            static std::set<TfToken> warned;
            if (std::lock_guard lock(mutex); !GetId().IsEmpty() && warned.insert(name).second)
                TF_WARN("hdUE: %s is not supported and renders nothing (%s)", name.GetText(), GetId().GetText());
            *bits = Clean;
        }
    };
    return new Unsupported(id, type);
}
// UnrealLocalFogVolume (HdUE_LocalFogVolumeAdapter): its transform, visibility and the unreal:localFogVolume dictionary
// of property values, uploaded when one of them changes.
HdSprim* HdUECreateLocalFog(const SdfPath& id) {
    struct LocalFog final : HdSprim {
        explicit LocalFog(const SdfPath& id) : HdSprim(id) {}
        HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
        void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits) override {
            *bits = HdChangeTracker::Clean;
            if (GetId().IsEmpty()) return;
            HdUELocalFogData data;
            data.id = GetId().GetString();
            data.localToWorld = delegate->GetTransform(GetId());
            data.visible = delegate->GetVisible(GetId());
            const auto values = delegate->Get(GetId(), TfToken("unreal:localFogVolume"));
            if (values.IsHolding<VtDictionary>())
                for (const auto& [name, value] : values.UncheckedGet<VtDictionary>())  // sorted by name
                    data.values.emplace_back(name, value.IsHolding<bool>()
                                                       ? (value.UncheckedGet<bool>() ? "True" : "False")
                                                       : TfStringify(value));
            auto* state = static_cast<HdUEState*>(param);
            std::lock_guard lock(state->mutex);
            const auto old = state->localFogs.find(data.id);
            if (old != state->localFogs.end() && old->second == data) return;
            state->localFogs[data.id] = std::move(data);
            state->dirtyLocalFogs.insert(GetId().GetString());
            ++state->revision;
        }
        void Finalize(HdRenderParam* param) override { static_cast<HdUEState*>(param)->erase(GetId().GetString()); }
    };
    return new LocalFog(id);
}
// Mesh Sync totals since the last call (HDUE_TRACE_TIMING).
void HdUETakeMeshSyncStats(double& milliseconds, int& count) {
    milliseconds = meshSyncMicroseconds.exchange(0) / 1000.0;
    count = meshSyncs.exchange(0);
}
PXR_NAMESPACE_CLOSE_SCOPE
