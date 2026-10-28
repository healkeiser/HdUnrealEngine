#include "delegate.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/mesh.h"
#include "pxr/imaging/hd/points.h"
#include "pxr/imaging/hd/basisCurves.h"
#include "pxr/imaging/hd/volume.h"
#include "pxr/imaging/hd/field.h"
#include <mutex>
#if defined(HDUE_HOUDINI)
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include "../houdini/volume_fields.h"
#include "pxr/base/tf/stringUtils.h"
#endif
#include "pxr/imaging/hd/meshUtil.h"
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
#include "subdivision.h"
#include "mesh_normals.h"
#include "surface_settings.h"
#include "wet_mask.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/matrix4d.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace {
template<class T> T value(const VtValue& v, T fallback) { return v.IsHolding<T>() ? v.UncheckedGet<T>() : fallback; }
// UsdTransform2d feeding a texture's st, as an affine in UE's flipped-V UV space.
// Returns false when st is unconnected, so a later texture may still supply one.
bool uvTransform(const HdMaterialNetwork2& network, const HdMaterialNode2& texture, std::array<float, 6>& out) {
    const auto st = texture.inputConnections.find(TfToken("st"));
    if (st == texture.inputConnections.end() || st->second.empty()) return false;
    const auto node = network.nodes.find(st->second.front().upstreamNode);
    if (node == network.nodes.end() || node->second.nodeTypeId != TfToken("UsdTransform2d")) return true;
    const auto param = [&](const char* name) { const auto it = node->second.parameters.find(TfToken(name)); return it == node->second.parameters.end() ? VtValue() : it->second; };
    const GfVec2f scale = value(param("scale"), GfVec2f(1)), translation = value(param("translation"), GfVec2f(0));
    const double radians = value(param("rotation"), 0.f) * 3.14159265358979323846 / 180;
    // USD order: scale, then rotate, then translate.
    const auto usd = [&](GfVec2f p) {
        p = GfVec2f(p[0] * scale[0], p[1] * scale[1]);
        return GfVec2f(float(std::cos(radians) * p[0] - std::sin(radians) * p[1]), float(std::sin(radians) * p[0] + std::cos(radians) * p[1])) + translation;
    };
    const auto ue = [&](float u, float v) { const auto p = usd(GfVec2f(u, 1 - v)); return GfVec2f(p[0], 1 - p[1]); };
    const auto origin = ue(0, 0), du = ue(1, 0) - origin, dv = ue(0, 1) - origin;
    out = {du[0], dv[0], origin[0], du[1], dv[1], origin[1]};
    return true;
}
HdInterpolation interpolation(HdSceneDelegate* delegate, const SdfPath& id, const TfToken& name) {
    for (int i = 0; i < HdInterpolationCount; ++i)
        for (const auto& p : delegate->GetPrimvarDescriptors(id, HdInterpolation(i))) if (p.name == name) return HdInterpolation(i);
    return HdInterpolationConstant;
}
// Varying colors only: a uniform displayColor stays the material's constant color.
std::vector<GfVec3f> HdUEVaryingColors(std::vector<GfVec3f> colors, size_t count) {
    if (colors.size() != count || std::all_of(colors.begin(), colors.end(), [&](const GfVec3f& c) { return c == colors[0]; })) return {};
    return colors;
}
template<class T> std::vector<T> expanded(HdSceneDelegate* delegate, const SdfPath& id, const TfToken& name,
    const VtVec3iArray& triangles, const VtIntArray& parameters, HdMeshUtil& util, HdType type, const HdUESubdivision* subdivision = nullptr) {
    // Indexed primvars (primvars:x:indices) arrive unflattened; expand them first.
    VtIntArray indices;
    const auto raw = delegate->GetIndexedPrimvar(id, name, &indices);
    if (!raw.IsHolding<VtArray<T>>()) return {};
    VtArray<T> array = raw.UncheckedGet<VtArray<T>>();
    if (!indices.empty()) {
        VtArray<T> flat(indices.size());
        for (size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] < 0 || size_t(indices[i]) >= array.size()) return {};
            flat[i] = array[indices[i]];
        }
        array = std::move(flat);
    }
    if (array.empty()) return {};
    const auto domain = interpolation(delegate, id, name);
    if (subdivision && (array = subdivision->refine(array, domain)).empty()) return {};
    VtArray<T> faceValues;
    if (domain == HdInterpolationFaceVarying) {
        VtValue output;
        const auto result = util.ComputeTriangulatedFaceVaryingPrimvar(array.cdata(), int(array.size()), type, &output);
        if (result == HdMeshComputationResult::Error) return {};
        faceValues = result == HdMeshComputationResult::Unchanged ? array : value(output, VtArray<T>());
    }
    std::vector<T> out;
    out.reserve(triangles.size() * 3);
    for (size_t tri = 0; tri < triangles.size(); ++tri) for (int corner : {0, 1, 2}) {
        const size_t index = domain == HdInterpolationFaceVarying ? tri * 3 + corner :
            domain == HdInterpolationConstant ? 0 : domain == HdInterpolationUniform ?
            size_t(HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(parameters[tri])) : size_t(triangles[tri][corner]);
        const auto& source = domain == HdInterpolationFaceVarying ? faceValues : array;
        if (index >= source.size()) return {};
        out.push_back(source[index]);
    }
    return out;
}
// Motion samples cover one frame either side, enough for any shutter; Pass evaluates them at sub-frame times.
constexpr float HdUEMotionWindow = 1.f;
constexpr size_t HdUEMaxMotionSamples = 16;
// Samples of current^-1-relative factors: left = M(t) * M(0)^-1, right = M(0)^-1 * M(t).
HdUEMotionSamples HdUERelativeMotion(const float* times, const GfMatrix4d* values, size_t count, const GfMatrix4d& current, bool left) {
    HdUEMotionSamples out;
    const auto inverse = current.GetInverse();
    for (size_t i = 0; i < std::min(count, HdUEMaxMotionSamples); ++i) {
        out.times.push_back(times[i]); out.values.push_back(left ? values[i] * inverse : inverse * values[i]);
    }
    if (!out.moving()) out = {};
    return out;
}
HdUEMotionSamples HdUETransformMotion(HdSceneDelegate* delegate, const SdfPath& id, const GfMatrix4d& current, bool left) {
    float times[HdUEMaxMotionSamples]; GfMatrix4d values[HdUEMaxMotionSamples];
    const size_t count = delegate->SampleTransform(id, -HdUEMotionWindow, HdUEMotionWindow, HdUEMaxMotionSamples, times, values);
    return HdUERelativeMotion(times, values, count, current, left);
}
// USD velocities are per second; motion blur times are in frames.
double HdUEFramesPerSecond(const HdUEState& state) {
    const auto it = state.settings.find(TfToken("houdini:fps"));
    const double fps = it == state.settings.end() ? 0 : it->second.IsHolding<double>() ? it->second.UncheckedGet<double>()
        : it->second.IsHolding<float>() ? it->second.UncheckedGet<float>() : 0;
    return fps > 0 ? fps : state.setting<double>("framesPerSecond", 24);
}
// Instance transforms as hdEmbree composes them: for each index,
// instanceTransform * scale * rotation * translation * instancerTransform, then nested
// instancers apply after (child * parent).
class Instancer final : public HdInstancer {
    std::map<TfToken, VtValue> primvars_;
    bool visible_ = true;
    template<class V> static bool vec3(const VtValue& v, size_t i, GfVec3d& out) {
        if (!v.IsHolding<VtArray<V>>() || i >= v.UncheckedGet<VtArray<V>>().size()) return false;
        const auto& x = v.UncheckedGet<VtArray<V>>()[i]; out = GfVec3d(x[0], x[1], x[2]); return true;
    }
    static bool vector3(const VtValue& v, size_t i, GfVec3d& out) {
        return vec3<GfVec3f>(v, i, out) || vec3<GfVec3d>(v, i, out) || vec3<GfVec3h>(v, i, out);
    }
    template<class Q> static bool quat(const VtValue& v, size_t i, GfQuatd& out) {
        if (!v.IsHolding<VtArray<Q>>() || i >= v.UncheckedGet<VtArray<Q>>().size()) return false;
        const auto& q = v.UncheckedGet<VtArray<Q>>()[i]; const auto im = q.GetImaginary();
        out = GfQuatd(q.GetReal(), GfVec3d(im[0], im[1], im[2])); return true;
    }
    static bool rotation(const VtValue& v, size_t i, GfQuatd& out) {
        if (quat<GfQuath>(v, i, out) || quat<GfQuatf>(v, i, out) || quat<GfQuatd>(v, i, out)) return true;
        if (!v.IsHolding<VtVec4fArray>() || i >= v.UncheckedGet<VtVec4fArray>().size()) return false;
        const auto& q = v.UncheckedGet<VtVec4fArray>()[i]; out = GfQuatd(q[0], GfVec3d(q[1], q[2], q[3])); return true; // <real, i, j, k>
    }
    VtValue primvar(const TfToken& name) const { const auto it = primvars_.find(name); return it == primvars_.end() ? VtValue() : it->second; }
public:
    Instancer(HdSceneDelegate* delegate, const SdfPath& id) : HdInstancer(delegate, id) {}
    void Sync(HdSceneDelegate* delegate, HdRenderParam*, HdDirtyBits* bits) override {
        _UpdateInstancer(delegate, bits);
        if (*bits & HdChangeTracker::DirtyVisibility) visible_ = delegate->GetVisible(GetId());
        if (HdChangeTracker::IsAnyPrimvarDirty(*bits, GetId()))
            for (const auto& primvar : delegate->GetPrimvarDescriptors(GetId(), HdInterpolationInstance))
                if (HdChangeTracker::IsPrimvarDirty(*bits, GetId(), primvar.name)) primvars_[primvar.name] = delegate->Get(GetId(), primvar.name);
    }
    // Per-instance velocities in world units per second (top-level instancers only; nested ones return none).
    std::vector<GfVec3f> ComputeInstanceVelocities(const SdfPath& prototype) const {
        const auto velocities = primvar(HdTokens->velocities);
        if (!visible_ || !GetParentId().IsEmpty() || velocities.IsEmpty()) return {};
        const auto instancer = GetDelegate()->GetInstancerTransform(GetId());
        std::vector<GfVec3f> out;
        for (const int index : GetDelegate()->GetInstanceIndices(GetId(), prototype)) {
            GfVec3d v(0); vector3(velocities, size_t(index), v);
            out.emplace_back(instancer.TransformDir(v));
        }
        return out;
    }
    HdUEMotionSamples ComputeMotion() const {
        float times[HdUEMaxMotionSamples]; GfMatrix4d values[HdUEMaxMotionSamples];
        const size_t count = GetDelegate()->SampleInstancerTransform(GetId(), -HdUEMotionWindow, HdUEMotionWindow, HdUEMaxMotionSamples, times, values);
        return HdUERelativeMotion(times, values, count, GetDelegate()->GetInstancerTransform(GetId()), false);
    }
    // Per-instance displayColor (instance interpolation), repeated for each parent instance like the transforms.
    std::vector<GfVec3f> ComputeInstanceColors(const SdfPath& prototype) const {
        const auto colors = value(primvar(HdTokens->displayColor), VtVec3fArray());
        if (colors.empty() || !visible_) return {};
        std::vector<GfVec3f> out;
        for (const int index : GetDelegate()->GetInstanceIndices(GetId(), prototype)) out.push_back(size_t(index) < colors.size() ? colors[index] : colors[0]);
        if (GetParentId().IsEmpty()) return out;
        const auto* parent = dynamic_cast<const Instancer*>(GetDelegate()->GetRenderIndex().GetInstancer(GetParentId()));
        const size_t parents = parent ? parent->ComputeInstanceTransforms(GetId()).size() : 0;
        std::vector<GfVec3f> nested; nested.reserve(parents * out.size());
        for (size_t i = 0; i < parents; ++i) nested.insert(nested.end(), out.begin(), out.end());
        return nested;
    }
    VtMatrix4dArray ComputeInstanceTransforms(const SdfPath& prototype) const {
        if (!visible_) return {};
        const auto instancer = GetDelegate()->GetInstancerTransform(GetId());
        const auto indices = GetDelegate()->GetInstanceIndices(GetId(), prototype);
        const auto translations = primvar(HdInstancerTokens->instanceTranslations), rotations = primvar(HdInstancerTokens->instanceRotations);
        const auto scales = primvar(HdInstancerTokens->instanceScales), matrices = primvar(HdInstancerTokens->instanceTransforms);
        VtMatrix4dArray transforms(indices.size());
        for (size_t i = 0; i < indices.size(); ++i) {
            const size_t index = size_t(indices[i]);
            GfMatrix4d m = instancer, step(1);
            GfVec3d v; GfQuatd q;
            if (vector3(translations, index, v)) m = step.SetTranslate(v) * m;
            if (rotation(rotations, index, q)) m = GfMatrix4d(1).SetRotate(q) * m;
            if (vector3(scales, index, v)) m = GfMatrix4d(1).SetScale(v) * m;
            if (matrices.IsHolding<VtMatrix4dArray>() && index < matrices.UncheckedGet<VtMatrix4dArray>().size())
                m = matrices.UncheckedGet<VtMatrix4dArray>()[index] * m;
            transforms[i] = m;
        }
        if (GetParentId().IsEmpty()) return transforms;
        const auto* parent = dynamic_cast<const Instancer*>(GetDelegate()->GetRenderIndex().GetInstancer(GetParentId()));
        if (!parent) return transforms;
        const auto parents = parent->ComputeInstanceTransforms(GetId());
        VtMatrix4dArray nested(parents.size() * transforms.size());
        for (size_t i = 0; i < parents.size(); ++i)
            for (size_t j = 0; j < transforms.size(); ++j) nested[i * transforms.size() + j] = transforms[j] * parents[i];
        return nested;
    }
};
class Mesh final : public HdMesh {
    std::set<std::string> emitted_;
    HdMeshTopology maskTopology_;
    VtVec3iArray maskTriangles_;
    VtIntArray maskParameters_;
    size_t pointCount_=0;
public:
    explicit Mesh(const SdfPath& id) : HdMesh(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        auto* state = static_cast<HdUEState*>(param);
        HdUEMeshData mesh;
        mesh.id = GetId().GetString();
        mesh.sourceId = mesh.id;
        mesh.scenePath = delegate->GetScenePrimPath(GetId(), -1).GetString();
        mesh.bindingPath = mesh.scenePath;
        mesh.material = delegate->GetMaterialId(GetId()).GetString();
        mesh.transform = delegate->GetTransform(GetId());
        mesh.visible = delegate->GetVisible(GetId());
        _UpdateInstancer(delegate, bits); // sets GetInstancerId
        if (!GetInstancerId().IsEmpty()) {
            HdInstancer::_SyncInstancerAndParents(delegate->GetRenderIndex(), GetInstancerId());
            mesh.instanced = true;
            if (const auto* instancer = dynamic_cast<const Instancer*>(delegate->GetRenderIndex().GetInstancer(GetInstancerId()))) {
                for (const auto& instance : instancer->ComputeInstanceTransforms(GetId())) mesh.instances.push_back(mesh.transform * instance);
                if (mesh.material.empty()) mesh.instanceColors = HdUEVaryingColors(instancer->ComputeInstanceColors(GetId()), mesh.instances.size());
                mesh.motionRight = instancer->ComputeMotion();
                mesh.velocities = instancer->ComputeInstanceVelocities(GetId());
                if (mesh.velocities.size() != mesh.instances.size()) mesh.velocities.clear();
                for (auto& v : mesh.velocities) v /= float(HdUEFramesPerSecond(*state));
            }
        }
        mesh.motionLeft = HdUETransformMotion(delegate, GetId(), mesh.transform, true);
        mesh.primId = GetPrimId();
        mesh.sceneAsset = value(delegate->Get(GetId(), TfToken("hdue:asset")), std::string());
        if (!mesh.sceneAsset.empty() && SdfPath(mesh.scenePath).GetName() == "__HdUEAssetProxy")
            mesh.scenePath = SdfPath(mesh.scenePath).GetParentPath().GetString();
        auto points = value(delegate->Get(GetId(), HdTokens->points), VtVec3fArray());
        auto topology = delegate->GetMeshTopology(GetId());
        // catmullClark and loop meshes are refined uniformly (hdUE:subdivisionLevel); authored normals do not apply to them.
        const auto subdivision = mesh.sceneAsset.empty() ? HdUESubdivision::Create(topology, state->setting<int>("subdivisionLevel", 2)) : nullptr;
        if (subdivision) { points = subdivision->refine(points, HdInterpolationVertex); topology = subdivision->topology(); }
        HdMeshUtil util(&topology, GetId());
        VtVec3iArray triangles;
        VtIntArray parameters;
        util.ComputeTriangleIndices(&triangles, &parameters);
        maskTopology_=topology; maskTriangles_=triangles; maskParameters_=parameters; pointCount_=points.size();
        // The USD -> UE basis already reverses handedness. UE's clockwise
        // front faces must not receive a second index reversal.
        for (const auto& triangle : triangles) for (int corner : {0, 1, 2}) {
            if (triangle[corner] < 0 || size_t(triangle[corner]) >= points.size()) { TF_RUNTIME_ERROR("Invalid mesh index at %s", GetId().GetText()); return; }
            mesh.points.push_back(points[triangle[corner]]);
        }
        {
            // Deformation blur from time-sampled points of the same count, else from per-point velocities.
            float times[HdUEMaxMotionSamples]; VtValue samples[HdUEMaxMotionSamples];
            const size_t count = delegate->SamplePrimvar(GetId(), HdTokens->points, -HdUEMotionWindow, HdUEMotionWindow, HdUEMaxMotionSamples, times, samples);
            bool moves = false;
            for (size_t k = 0; k < count && count > 1; ++k) {
                auto sample = value(samples[k], VtVec3fArray());
                if (subdivision) sample = subdivision->refine(sample, HdInterpolationVertex);
                if (sample.size() != points.size()) { mesh.deformTimes.clear(); mesh.deformOffsets.clear(); moves = false; break; }
                auto& offsets = mesh.deformOffsets.emplace_back(); offsets.reserve(mesh.points.size());
                for (const auto& triangle : triangles) for (int corner : {0, 1, 2}) {
                    offsets.push_back(sample[triangle[corner]] - points[triangle[corner]]);
                    moves = moves || offsets.back() != GfVec3f(0);
                }
                mesh.deformTimes.push_back(times[k]);
            }
            if (!moves) {
                mesh.deformTimes.clear(); mesh.deformOffsets.clear();
                auto velocities = value(delegate->Get(GetId(), HdTokens->velocities), VtVec3fArray());
                if (subdivision) velocities = subdivision->refine(velocities, HdInterpolationVertex);
                if (velocities.size() == points.size() && std::any_of(velocities.begin(), velocities.end(), [](const auto& v) { return v != GfVec3f(0); })) {
                    const float fps = float(HdUEFramesPerSecond(*state));
                    mesh.deformTimes = {-1.f, 1.f}; mesh.deformOffsets.resize(2);
                    for (const auto& triangle : triangles) for (int corner : {0, 1, 2}) {
                        const auto v = velocities[triangle[corner]] / fps;
                        mesh.deformOffsets[0].push_back(-v); mesh.deformOffsets[1].push_back(v);
                    }
                }
            }
        }
        if (!subdivision) mesh.normals = expanded<GfVec3f>(delegate, GetId(), HdTokens->normals, triangles, parameters, util, HdTypeFloatVec3);
        if (mesh.normals.size() != mesh.points.size()) mesh.normals.resize(mesh.points.size(), GfVec3f(0));
        if (std::any_of(mesh.normals.begin(),mesh.normals.end(),[](const auto& normal) {return normal.GetLengthSq()<1e-12f;})) {
            try {
                const auto smooth=HdUEComputeSmoothNormals(points,triangles);
                for (size_t i=0;i<mesh.normals.size();++i)
                    if (mesh.normals[i].GetLengthSq()<1e-12f) mesh.normals[i]=smooth[i];
            } catch (const std::invalid_argument& error) { TF_RUNTIME_ERROR("%s at %s",error.what(),GetId().GetText()); return; }
        }
        mesh.uv = expanded<GfVec2f>(delegate, GetId(), TfToken("st"), triangles, parameters, util, HdTypeFloatVec2, subdivision.get());
        if (mesh.material.empty() && mesh.sceneAsset.empty())
            mesh.colors = HdUEVaryingColors(expanded<GfVec3f>(delegate, GetId(), HdTokens->displayColor, triangles, parameters, util, HdTypeFloatVec3, subdivision.get()), mesh.points.size());
        std::shared_ptr<const VtDictionary> selectedBindings;
        { std::lock_guard lock(state->mutex); selectedBindings=state->wetBindings; }
        const auto& wetBindings=*selectedBindings;
        std::set<std::string> maskNames;
        const auto needMask = [&](const std::string& path) {
            HdUEMeshData target; target.bindingPath=path;
            const auto name=HdUEWetValue<std::string>(HdUEWetSettings(wetBindings,target),"maskPrimvar","");
            if (!name.empty()) maskNames.insert(name);
        };
        needMask(mesh.scenePath);
        for (const auto& subset : topology.GetGeomSubsets()) needMask(SdfPath(mesh.scenePath).AppendChild(subset.id.GetNameToken()).GetString());
        for (const auto& name : maskNames) {
            const TfToken token(name);
            const auto raw=delegate->Get(GetId(),token);
            const auto domain=interpolation(delegate,GetId(),token);
            try { mesh.masks[name]=HdUEExpandWetMask(raw,domain,points.size(),topology,triangles,parameters,util); }
            catch (const std::invalid_argument& error) { mesh.maskErrors[name]=name+": "+error.what(); }
        }
        if (TfGetenvBool("HDUE_TRACE_SCENE",false)) TF_STATUS("hdUE wet sync %s scene=%s requested=%zu masks=%zu",GetId().GetText(),mesh.scenePath.c_str(),maskNames.size(),mesh.masks.size());
        std::lock_guard lock(state->mutex);
        if (mesh.material.empty() && mesh.sceneAsset.empty()) {
            HdUEMaterialData material;
            material.id = mesh.id + ".displayColor";
            const auto color = value(delegate->Get(GetId(), HdTokens->displayColor), VtVec3fArray());
            if (!color.empty()) material.color = color[0];
            material.vertexColor = !mesh.colors.empty() || !mesh.instanceColors.empty();
            if (material.vertexColor) material.color = GfVec3f(1);
            const auto opacity = value(delegate->Get(GetId(), HdTokens->displayOpacity), VtFloatArray());
            if (!opacity.empty()) {
                // The generic ABI carries one opacity per material. Constant
                // primvars (and identical values in other domains) are exact.
                if (!std::all_of(opacity.begin(), opacity.end(), [&](float a) { return a == opacity[0]; }))
                    TF_WARN("Varying displayOpacity is unsupported at %s; using its first value", GetId().GetText());
                if (std::isfinite(opacity[0])) material.opacity = std::clamp(opacity[0], 0.f, 1.f);
            }
            mesh.material = material.id;
            const auto previous = state->materials.find(material.id);
            if (previous == state->materials.end() || previous->second.color != material.color || previous->second.opacity != material.opacity ||
                previous->second.vertexColor != material.vertexColor) {
                state->materials[material.id] = material;
                state->dirtyMaterials.insert(material.id);
            }
        }
        std::vector<HdUEMeshData> parts;
        try { parts = HdUEPartitionSubsets(std::move(mesh), topology, parameters); }
        catch (const std::invalid_argument& error) { TF_RUNTIME_ERROR("%s at %s", error.what(), GetId().GetText()); return; }
        std::set<std::string> current;
        for (auto& part : parts) {
            const auto previous = state->meshes.find(part.id);
            if (previous != state->meshes.end() && previous->second.points == part.points) part.pickMesh = previous->second.pickMesh;
            if (!part.pickMesh) part.pickMesh = std::make_shared<HdUEPickMesh>(part.points);
            const bool onlyNativeBounds = previous != state->meshes.end() && !part.sceneAsset.empty() &&
                previous->second.sceneAsset==part.sceneAsset && previous->second.transform==part.transform && previous->second.visible==part.visible;
            const bool meshChanged = !onlyNativeBounds && (previous == state->meshes.end() || previous->second.points != part.points ||
                previous->second.normals != part.normals || previous->second.uv != part.uv || previous->second.transform != part.transform ||
                previous->second.material != part.material || previous->second.visible != part.visible || previous->second.sceneAsset != part.sceneAsset ||
                previous->second.instanced != part.instanced || previous->second.instances != part.instances ||
                previous->second.colors != part.colors || previous->second.instanceColors != part.instanceColors);
            const auto id = part.id;
            const bool maskChanged=previous==state->meshes.end() || previous->second.masks!=part.masks || previous->second.maskErrors!=part.maskErrors;
            state->meshes[id] = std::move(part);
            state->removed.erase(id);
            if (meshChanged) { state->dirtyMeshes.insert(id); ++state->revision; }
            else if (maskChanged) ++state->revision;
            current.insert(id);
        }
        for (const auto& id : emitted_) if (!current.count(id)) {
            state->meshes.erase(id); state->dirtyMeshes.erase(id); state->removed.insert(id); ++state->revision;
        }
        emitted_ = std::move(current);
        *bits = HdChangeTracker::Clean;
    }
    void RefreshSelectedMasks(HdSceneDelegate* delegate, HdUEState& state) const {
        // A renderer-setting edit does not change a scene-index data source.
        // Some hosts therefore do not run Rprim Sync for a new mask selector.
        // Fetch only that primvar from the synchronized Hydra delegate, using
        // the cached triangulation and subset corner mapping.
        std::lock_guard lock(state.mutex);
        const auto& bindings=*state.wetBindings;
        std::set<std::string> needed;
        for (const auto& id:emitted_) {
            const auto part=state.meshes.find(id); if (part==state.meshes.end()) continue;
            const auto name=HdUEWetValue<std::string>(HdUEWetSettings(bindings,part->second),"maskPrimvar","");
            if (!name.empty() && !part->second.masks.count(name) && !part->second.maskErrors.count(name)) needed.insert(name);
        }
        for (const auto& name:needed) {
            std::vector<float> values; std::string error;
            HdMeshUtil util(&maskTopology_,GetId());
            try { values=HdUEExpandWetMask(delegate->Get(GetId(),TfToken(name)),interpolation(delegate,GetId(),TfToken(name)),pointCount_,
                maskTopology_,maskTriangles_,maskParameters_,util); }
            catch (const std::invalid_argument& e) {error=name+": "+e.what();}
            for (const auto& id:emitted_) {
                auto part=state.meshes.find(id); if (part==state.meshes.end()) continue;
                if (!error.empty()) part->second.maskErrors[name]=error;
                else {
                    auto& mask=part->second.masks[name]; mask.clear(); mask.reserve(part->second.points.size());
                    for (size_t i=0;i<part->second.points.size();++i) mask.push_back(values.at(part->second.sourceCorners.empty()?i:part->second.sourceCorners[i]));
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
    void _InitRepr(const TfToken& token, HdDirtyBits*) override { _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr())); }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
};
// Points and curves become triangle meshes: one shared sphere instanced per point, and a capped tube
// per curve. They keep one material (bound, or constant displayColor/displayOpacity).
void HdUEStoreShape(HdSceneDelegate* delegate, const SdfPath& id, HdUEState& state, HdUEMeshData mesh) {
    std::lock_guard lock(state.mutex);
    if (mesh.material.empty()) {
        HdUEMaterialData material; material.id = mesh.id + ".displayColor";
        const auto color = value(delegate->Get(id, HdTokens->displayColor), VtVec3fArray());
        if (!color.empty()) material.color = color[0];
        material.vertexColor = !mesh.colors.empty() || !mesh.instanceColors.empty();
        if (material.vertexColor) material.color = GfVec3f(1);
        const auto opacity = value(delegate->Get(id, HdTokens->displayOpacity), VtFloatArray());
        if (!opacity.empty() && std::isfinite(opacity[0])) material.opacity = std::clamp(opacity[0], 0.f, 1.f);
        mesh.material = material.id;
        const auto previous = state.materials.find(material.id);
        if (previous == state.materials.end() || previous->second.color != material.color || previous->second.opacity != material.opacity ||
            previous->second.vertexColor != material.vertexColor) {
            state.materials[material.id] = material; state.dirtyMaterials.insert(material.id);
        }
    }
    const auto previous = state.meshes.find(mesh.id);
    const bool changed = previous == state.meshes.end() || previous->second.points != mesh.points || previous->second.normals != mesh.normals ||
        previous->second.uv != mesh.uv || previous->second.transform != mesh.transform || previous->second.material != mesh.material ||
        previous->second.visible != mesh.visible || previous->second.instanced != mesh.instanced || previous->second.instances != mesh.instances ||
        previous->second.colors != mesh.colors || previous->second.instanceColors != mesh.instanceColors;
    mesh.pickMesh = previous != state.meshes.end() && previous->second.points == mesh.points ? previous->second.pickMesh : std::make_shared<HdUEPickMesh>(mesh.points);
    const auto key = mesh.id;
    state.meshes[key] = std::move(mesh); state.removed.erase(key);
    if (changed) { state.dirtyMeshes.insert(key); ++state.revision; }
}
HdUEMeshData HdUEShapeBase(HdRprim& prim, HdSceneDelegate* delegate) {
    HdUEMeshData mesh;
    mesh.id = mesh.sourceId = prim.GetId().GetString();
    mesh.scenePath = mesh.bindingPath = delegate->GetScenePrimPath(prim.GetId(), -1).GetString();
    mesh.material = delegate->GetMaterialId(prim.GetId()).GetString();
    mesh.transform = delegate->GetTransform(prim.GetId());
    mesh.visible = delegate->GetVisible(prim.GetId());
    mesh.primId = prim.GetPrimId();
    return mesh;
}
// Placements of an rprim under a Hydra instancer (its own transform first), or empty when not instanced.
std::vector<GfMatrix4d> HdUEInstancePlacements(HdSceneDelegate* delegate, const SdfPath& id, const SdfPath& instancerId, const GfMatrix4d& transform) {
    std::vector<GfMatrix4d> out;
    if (instancerId.IsEmpty()) return out;
    HdInstancer::_SyncInstancerAndParents(delegate->GetRenderIndex(), instancerId);
    if (const auto* instancer = dynamic_cast<const Instancer*>(delegate->GetRenderIndex().GetInstancer(instancerId)))
        for (const auto& instance : instancer->ComputeInstanceTransforms(id)) out.push_back(transform * instance);
    return out;
}
// Widths per vertex (or with a matching count), else the first (constant), else the fallback.
float HdUEWidth(const VtFloatArray& widths, size_t index, float fallback) {
    if (widths.empty()) return fallback;
    return widths.size() > index ? widths[index] : widths[0];
}
// Unit sphere (8 x 6 UV sphere) as a triangle soup with radial normals.
void HdUEAppendSphere(HdUEMeshData& mesh) {
    constexpr int Slices = 8, Stacks = 6;
    const auto at = [](int stack, int slice) {
        const double theta = 3.14159265358979323846 * stack / Stacks, phi = 2 * 3.14159265358979323846 * slice / Slices;
        return GfVec3f(float(std::sin(theta) * std::cos(phi)), float(std::cos(theta)), float(std::sin(theta) * std::sin(phi)));
    };
    for (int stack = 0; stack < Stacks; ++stack) for (int slice = 0; slice < Slices; ++slice) {
        const GfVec3f a = at(stack, slice), b = at(stack + 1, slice), c = at(stack + 1, slice + 1), d = at(stack, slice + 1);
        // Counter-clockwise seen from outside, as USD front faces are.
        for (const auto& corner : {a, c, b, a, d, c}) { mesh.points.push_back(corner); mesh.normals.push_back(corner); mesh.uv.emplace_back(0.f, 0.f); }
    }
}
class Points final : public HdPoints {
public:
    explicit Points(const SdfPath& id) : HdPoints(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        auto mesh = HdUEShapeBase(*this, delegate);
        _UpdateInstancer(delegate, bits);
        const auto points = value(delegate->Get(GetId(), HdTokens->points), VtVec3fArray());
        const auto widths = value(delegate->Get(GetId(), HdTokens->widths), VtFloatArray());
        HdUEAppendSphere(mesh);
        mesh.instanced = true;
        auto placements = HdUEInstancePlacements(delegate, GetId(), GetInstancerId(), mesh.transform);
        if (placements.empty()) placements.push_back(mesh.transform);
        std::vector<GfVec3f> colors;
        if (mesh.material.empty()) {
            const auto authored = value(delegate->Get(GetId(), HdTokens->displayColor), VtVec3fArray());
            colors = HdUEVaryingColors(std::vector<GfVec3f>(authored.begin(), authored.end()), points.size());
        }
        // Points move with their prim (or instancer) transform and their velocities.
        mesh.motionRight = GetInstancerId().IsEmpty() ? HdUETransformMotion(delegate, GetId(), mesh.transform, false) : HdUEMotionSamples{};
        const auto velocities = value(delegate->Get(GetId(), HdTokens->velocities), VtVec3fArray());
        const float fps = float(HdUEFramesPerSecond(*static_cast<HdUEState*>(param)));
        // Every point of every placement (nested under a Hydra instancer when there is one).
        for (const auto& placement : placements) for (size_t i = 0; i < points.size(); ++i) {
            if (velocities.size() == points.size()) mesh.velocities.emplace_back(GfVec3f(placement.TransformDir(GfVec3d(velocities[i]))) / fps);
            const double radius = .5 * HdUEWidth(widths, i, .1f);
            mesh.instances.push_back(GfMatrix4d(1).SetScale(radius) * GfMatrix4d(1).SetTranslate(GfVec3d(points[i])) * placement);
            if (!colors.empty()) mesh.instanceColors.push_back(colors[i]);
            mesh.instanceIds.push_back(GetInstancerId().IsEmpty() ? -1 : int32_t(&placement - placements.data()));
        }
        HdUEStoreShape(delegate, GetId(), *static_cast<HdUEState*>(param), std::move(mesh));
        *bits = HdChangeTracker::Clean;
    }
    void Finalize(HdRenderParam* param) override {
        auto* state = static_cast<HdUEState*>(param);
        state->erase(GetId().GetString()); state->erase(GetId().GetString() + ".displayColor");
    }
protected:
    void _InitRepr(const TfToken& token, HdDirtyBits*) override { _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr())); }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
};
class Curves final : public HdBasisCurves {
    // Samples along one curve: cubic segments are evaluated in their basis; linear curves keep their vertices.
    struct Sample { GfVec3d first; double second; GfVec3d color; };
    static std::vector<Sample> samples(const std::vector<GfVec3d>& cv, const std::vector<double>& width, const std::vector<GfVec3d>& color,
        const TfToken& type, const TfToken& basis, bool periodic) {
        const size_t n = cv.size();
        std::vector<Sample> out;
        if (type == HdTokens->linear || n < 4) {
            for (size_t i = 0; i < n; ++i) out.push_back({cv[i], width[i], color[i]});
            if (periodic && n > 2) out.push_back(out.front());
            return out;
        }
        const bool bezier = basis == HdTokens->bezier;
        const size_t step = bezier ? 3 : 1;
        const size_t segments = bezier ? (periodic ? n / 3 : (n - 1) / 3) : (periodic ? n : n - 3);
        constexpr int Steps = 16;
        for (size_t s = 0; s < segments; ++s) {
            const size_t first = s * step;
            const auto P = [&](size_t k) { return cv[(first + k) % n]; };
            const auto W = [&](size_t k) { return width[(first + k) % n]; };
            const auto C = [&](size_t k) { return color[(first + k) % n]; };
            for (int t = (s ? 1 : 0); t <= Steps; ++t) {
                const double u = double(t) / Steps, u2 = u * u, u3 = u2 * u, m = 1 - u;
                double b[4];
                if (bezier) { b[0] = m * m * m; b[1] = 3 * u * m * m; b[2] = 3 * u2 * m; b[3] = u3; }
                else if (basis == HdTokens->catmullRom) { b[0] = .5 * (-u3 + 2 * u2 - u); b[1] = .5 * (3 * u3 - 5 * u2 + 2); b[2] = .5 * (-3 * u3 + 4 * u2 + u); b[3] = .5 * (u3 - u2); }
                else { b[0] = m * m * m / 6; b[1] = (3 * u3 - 6 * u2 + 4) / 6; b[2] = (-3 * u3 + 3 * u2 + 3 * u + 1) / 6; b[3] = u3 / 6; } // bspline
                GfVec3d point(0), c(0); double w = 0;
                for (size_t k = 0; k < 4; ++k) { point += b[k] * P(k); w += b[k] * W(k); c += b[k] * C(k); }
                out.push_back({point, w, c});
            }
        }
        return out;
    }
public:
    explicit Curves(const SdfPath& id) : HdBasisCurves(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        auto mesh = HdUEShapeBase(*this, delegate);
        _UpdateInstancer(delegate, bits);
        const auto topology = delegate->GetBasisCurvesTopology(GetId());
        // displayColor per vertex or per curve; constant colors stay on the material.
        const auto displayColor = mesh.material.empty() ? value(delegate->Get(GetId(), HdTokens->displayColor), VtVec3fArray()) : VtVec3fArray();
        std::vector<GfVec3f> colors;
        const auto points = value(delegate->Get(GetId(), HdTokens->points), VtVec3fArray());
        const auto widths = value(delegate->Get(GetId(), HdTokens->widths), VtFloatArray());
        const auto counts = topology.GetCurveVertexCounts();
        const auto indices = topology.GetCurveIndices();
        const bool periodic = topology.GetCurveWrap() == HdTokens->periodic;
        constexpr int Sides = 6;
        size_t offset = 0;
        // ponytail: 6-sided tubes per sample; dense grooms produce large meshes, camera-facing ribbons if that matters.
        for (size_t curveIndex = 0; curveIndex < counts.size(); ++curveIndex) {
            const int count = counts[curveIndex];
            std::vector<GfVec3d> cv, color; std::vector<double> width;
            for (int i = 0; i < count; ++i) {
                const size_t vertex = indices.empty() ? offset + i : size_t(indices[offset + i]);
                if (vertex >= points.size()) { TF_RUNTIME_ERROR("Invalid curve index at %s", GetId().GetText()); return; }
                cv.emplace_back(points[vertex]);
                width.push_back(HdUEWidth(widths, vertex, .01f));
                color.emplace_back(displayColor.size() == points.size() ? displayColor[vertex]
                    : displayColor.size() == counts.size() ? displayColor[curveIndex] : GfVec3f(1));
            }
            offset += size_t(count);
            const auto curve = samples(cv, width, color, topology.GetCurveType(), topology.GetCurveBasis(), periodic);
            if (curve.size() < 2) continue;
            // Parallel-transported frame along the curve.
            GfVec3d tangent = (curve[1].first - curve[0].first).GetNormalized();
            GfVec3d normal = GfCross(tangent, std::abs(tangent[1]) < .9 ? GfVec3d(0, 1, 0) : GfVec3d(1, 0, 0)).GetNormalized();
            std::vector<std::vector<GfVec3f>> rings, ringNormals;
            for (size_t i = 0; i < curve.size(); ++i) {
                const auto next = curve[std::min(i + 1, curve.size() - 1)].first, prev = curve[i ? i - 1 : 0].first;
                const auto t = (next - prev).GetLength() > 1e-12 ? (next - prev).GetNormalized() : tangent;
                normal = normal - t * GfDot(normal, t);
                if (normal.GetLength() < 1e-9) normal = GfCross(t, std::abs(t[1]) < .9 ? GfVec3d(0, 1, 0) : GfVec3d(1, 0, 0));
                normal.Normalize();
                const auto binormal = GfCross(t, normal);
                rings.emplace_back(); ringNormals.emplace_back(); tangent = t;
                for (int side = 0; side < Sides; ++side) {
                    const double angle = 2 * 3.14159265358979323846 * side / Sides;
                    const auto dir = std::cos(angle) * normal + std::sin(angle) * binormal;
                    rings.back().emplace_back(curve[i].first + .5 * curve[i].second * dir);
                    ringNormals.back().emplace_back(dir);
                }
            }
            for (size_t i = 0; i + 1 < rings.size(); ++i) for (int side = 0; side < Sides; ++side) {
                const int next = (side + 1) % Sides;
                // Counter-clockwise seen from outside (dir = cos * normal + sin * binormal turns about the tangent).
                const std::pair<size_t, int> quad[6] = {{i, side}, {i + 1, next}, {i + 1, side}, {i, side}, {i, next}, {i + 1, next}};
                for (const auto& [ring, corner] : quad) {
                    mesh.points.push_back(rings[ring][corner]); mesh.normals.push_back(ringNormals[ring][corner]); colors.emplace_back(curve[ring].color);
                    mesh.uv.emplace_back(float(corner) / Sides, float(ring) / float(rings.size() - 1));
                }
            }
            if (periodic) continue;
            // Flat caps: the end faces +tangent (ring order is counter-clockwise seen from there), the start faces -tangent.
            for (const bool end : {false, true}) {
                const size_t ring = end ? rings.size() - 1 : 0;
                const auto t = end ? curve.back().first - curve[curve.size() - 2].first : curve[0].first - curve[1].first;
                if (t.GetLength() < 1e-12) continue;
                const GfVec3f center(curve[ring].first), facing(t.GetNormalized());
                for (int side = 0; side < Sides; ++side) {
                    const int next = (side + 1) % Sides;
                    for (const auto& corner : {center, rings[ring][end ? side : next], rings[ring][end ? next : side]}) {
                        mesh.points.push_back(corner); mesh.normals.push_back(facing); mesh.uv.emplace_back(0.f, float(end)); colors.emplace_back(curve[ring].color);
                    }
                }
            }
        }
        mesh.colors = HdUEVaryingColors(std::move(colors), mesh.points.size());
        mesh.instances = HdUEInstancePlacements(delegate, GetId(), GetInstancerId(), mesh.transform);
        mesh.instanced = !GetInstancerId().IsEmpty();
        mesh.motionLeft = HdUETransformMotion(delegate, GetId(), mesh.transform, true);
        if (mesh.instanced)
            if (const auto* instancer = dynamic_cast<const Instancer*>(delegate->GetRenderIndex().GetInstancer(GetInstancerId())))
                mesh.motionRight = instancer->ComputeMotion();
        HdUEStoreShape(delegate, GetId(), *static_cast<HdUEState*>(param), std::move(mesh));
        *bits = HdChangeTracker::Clean;
    }
    void Finalize(HdRenderParam* param) override {
        auto* state = static_cast<HdUEState*>(param);
        state->erase(GetId().GetString()); state->erase(GetId().GetString() + ".displayColor");
    }
protected:
    void _InitRepr(const TfToken& token, HdDirtyBits*) override { _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr())); }
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
    static constexpr int MaxResolution = 512; // per axis; larger grids are point-sampled with a stride
public:
    explicit Volume(const SdfPath& id) : HdVolume(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdChangeTracker::AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits, const TfToken&) override {
        auto* state = static_cast<HdUEState*>(param);
        HdUEVolumeData volume; volume.id = GetId().GetString(); volume.visible = delegate->GetVisible(GetId());
        const auto fields = delegate->GetVolumeFieldDescriptors(GetId());
        const HdVolumeFieldDescriptor* chosen = nullptr;
        for (const auto& field : fields) if (field.fieldName == TfToken("density")) chosen = &field;
        if (!chosen && !fields.empty()) chosen = &fields.front();
        try {
            if (chosen) {
                const bool vdb = chosen->fieldPrimType == TfToken("openvdbAsset");
                if (!vdb && chosen->fieldPrimType != TfToken("houdiniFieldAsset"))
                    throw std::runtime_error("only openvdbAsset and houdiniFieldAsset fields are supported, not " + chosen->fieldPrimType.GetString());
                const auto file = value(delegate->Get(chosen->fieldId, TfToken("filePath")), SdfAssetPath());
                const auto path = file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
                auto gridName = value(delegate->Get(chosen->fieldId, TfToken("fieldName")), TfToken()).GetString();
                if (gridName.empty()) gridName = chosen->fieldName.GetString();
                const auto index = delegate->Get(chosen->fieldId, TfToken("fieldIndex"));
                static std::once_flag initialized; std::call_once(initialized, [] { openvdb::initialize(); });
                const bool vdbFile = vdb && path.rfind("op:", 0) != 0 && TfStringEndsWith(TfStringToLower(path), ".vdb");
                if (vdbFile) {
                    openvdb::io::File file(path); file.open(false);
                    const auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(gridName));
                    file.close();
                    if (!grid) throw std::runtime_error("grid " + gridName + " in " + path + " is not a float grid");
                    HdUEDensifyGrid(*grid, MaxResolution, volume);
                } else // SOP volumes behind op: paths, and .bgeo files: VDB or native Houdini volumes
                    HdUELoadHoudiniField(path, gridName, index.IsHolding<int>() ? index.UncheckedGet<int>() : 0, vdb, MaxResolution, volume);
                volume.localToWorld = volume.localToWorld * delegate->GetTransform(GetId());
            }
        } catch (const std::exception& error) {
            TF_RUNTIME_ERROR("hdUE volume %s: %s", GetId().GetText(), error.what());
            volume.width = volume.height = volume.depth = 0; volume.density.clear();
        }
        if (TfGetenvBool("HDUE_TRACE_PASS", false)) {
            const auto& m = volume.localToWorld;
            std::fprintf(stderr, "hdUE volume %s fields=%zu dims=%ux%ux%u origin=(%g,%g,%g) x=(%g,%g,%g)%c", GetId().GetText(), fields.size(),
                volume.width, volume.height, volume.depth, m[3][0], m[3][1], m[3][2], m[0][0], m[0][1], m[0][2], 10);
            std::fflush(stderr);
        }
        std::lock_guard lock(state->mutex);
        state->volumes[volume.id] = std::move(volume);
        state->dirtyVolumes.insert(GetId().GetString()); state->removed.erase(GetId().GetString()); ++state->revision;
        *bits = HdChangeTracker::Clean;
    }
    void Finalize(HdRenderParam* param) override { static_cast<HdUEState*>(param)->erase(GetId().GetString()); }
protected:
    void _InitRepr(const TfToken& token, HdDirtyBits*) override { _reprs.emplace_back(token, HdReprSharedPtr(new HdRepr())); }
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
};
#endif
class Material final : public HdMaterial {
public:
    explicit Material(const SdfPath& id) : HdMaterial(id) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits) override {
        HdUEMaterialData data; data.id = GetId().GetString();
        const auto resource = delegate->GetMaterialResource(GetId());
        HdMaterialNetwork2 network;
        if (resource.IsHolding<HdMaterialNetwork2>()) network = resource.UncheckedGet<HdMaterialNetwork2>();
        else if (resource.IsHolding<HdMaterialNetworkMap>()) network = HdConvertToHdMaterialNetwork2(resource.UncheckedGet<HdMaterialNetworkMap>());
        for (const auto& [path, node] : network.nodes) {
            if (node.nodeTypeId != TfToken("UsdPreviewSurface")) continue;
            const auto get = [&](const char* name) { const auto it = node.parameters.find(TfToken(name)); return it == node.parameters.end() ? VtValue() : it->second; };
            data.color = value(get("diffuseColor"), data.color);
            data.emissive = value(get("emissiveColor"), data.emissive);
            data.metallic = value(get("metallic"), data.metallic);
            data.roughness = value(get("roughness"), data.roughness);
            data.opacity = value(get("opacity"), data.opacity);
            data.opacityThreshold = value(get("opacityThreshold"), data.opacityThreshold);
            static const std::pair<const char*, int> inputs[] = {{"diffuseColor", HDUE_TEXTURE_BASE_COLOR},
                {"emissiveColor", HDUE_TEXTURE_EMISSIVE}, {"metallic", HDUE_TEXTURE_METALLIC}, {"roughness", HDUE_TEXTURE_ROUGHNESS},
                {"opacity", HDUE_TEXTURE_OPACITY}, {"normal", HDUE_TEXTURE_NORMAL}};
            bool uvSet = false;
            for (const auto& [input, slot] : inputs) {
                const auto connection = node.inputConnections.find(TfToken(input));
                if (connection == node.inputConnections.end() || connection->second.empty()) continue;
                const auto& link = connection->second.front();
                const auto upstream = network.nodes.find(link.upstreamNode);
                if (upstream == network.nodes.end() || upstream->second.nodeTypeId != TfToken("UsdUVTexture")) continue;
                const auto& texture = upstream->second;
                const auto param = [&](const char* name) { const auto it = texture.parameters.find(TfToken(name)); return it == texture.parameters.end() ? VtValue() : it->second; };
                const auto file = param("file");
                if (!file.IsHolding<SdfAssetPath>()) continue;
                const auto& asset = file.UncheckedGet<SdfAssetPath>();
                auto& out = data.textures[slot];
                out.file = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
                out.scale = value(param("scale"), out.scale);
                out.bias = value(param("bias"), out.bias);
                // A single-channel output (r, g, b, a) selects that channel; rgb keeps all.
                const auto& channel = link.upstreamOutputName.GetString();
                if (channel.size() == 1 && std::string("rgba").find(channel[0]) != std::string::npos) {
                    out.mask = GfVec4f(0); out.mask[int(std::string("rgba").find(channel[0]))] = 1;
                }
                // ponytail: one UV transform per material, from the first textured input; per-texture transforms if assets mix them.
                if (!uvSet) uvSet = uvTransform(network, texture, data.uvTransform);
            }
        }
        auto* state = static_cast<HdUEState*>(param);
        std::lock_guard lock(state->mutex);
        state->materials[data.id] = data; state->dirtyMaterials.insert(data.id); ++state->revision;
        *bits = Clean;
    }
    void Finalize(HdRenderParam* param) override { static_cast<HdUEState*>(param)->erase(GetId().GetString()); }
};
class Light final : public HdLight {
    uint32_t type_;
public:
    Light(const SdfPath& id, uint32_t type) : HdLight(id), type_(type) {}
    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits) override {
        HdUELightData data; data.id = GetId().GetString(); data.type = type_;
        data.transform = delegate->GetTransform(GetId()); data.visible = delegate->GetVisible(GetId());
        const auto get = [&](const char* name) { return delegate->GetLightParamValue(GetId(), TfToken(name)); };
        const auto applicationLight = get("params");
        data.viewer = applicationLight.IsHolding<GlfSimpleLight>() && !applicationLight.UncheckedGet<GlfSimpleLight>().IsDomeLight();
        data.color = value(get("color"), data.color);
        data.intensity = value(get("intensity"), 1.f) * std::exp2(value(get("exposure"), 0.f));
        if(value(get("enableColorTemperature"),false)) {
            const auto temperature=UsdLuxBlackbodyTemperatureAsRgb(value(get("colorTemperature"),6500.f));
            for(int c=0;c<3;++c) data.color[c]*=temperature[c];
        }
        if(value(get("normalize"),false)) data.flags|=HDUE_LIGHT_NORMALIZE;
        if(!value(get("shadow:enable"),true)) data.flags&=~HDUE_LIGHT_SHADOWS;
        if(value(get("treatAsPoint"),false)) data.flags|=HDUE_LIGHT_POINT;
        if(!value(get("hdue:visibleBackground"),true)) data.flags&=~HDUE_LIGHT_BACKGROUND;
        if(!value(get("hdue:megaLights"),true)) data.flags&=~HDUE_LIGHT_MEGALIGHTS;
        data.diffuse=value(get("diffuse"),1.f); data.specular=value(get("specular"),1.f);
        data.angle=value(get("angle"),.53f);
        data.coneAngle=value(get("shaping:cone:angle"),180.f); data.coneSoftness=value(get("shaping:cone:softness"),0.f);
        if(const auto ies=get("shaping:ies:file"); ies.IsHolding<SdfAssetPath>()) {
            const auto& file=ies.UncheckedGet<SdfAssetPath>();
            data.ies=file.GetResolvedPath().empty()?file.GetAssetPath():file.GetResolvedPath();
        }
        data.iesAngleScale=value(get("shaping:ies:angleScale"),0.f); data.iesNormalize=value(get("shaping:ies:normalize"),false);
        auto texture=get("texture:file");
        if(applicationLight.IsHolding<GlfSimpleLight>() && applicationLight.UncheckedGet<GlfSimpleLight>().IsDomeLight())
            texture=VtValue(applicationLight.UncheckedGet<GlfSimpleLight>().GetDomeLightTextureFile());
        if(texture.IsHolding<SdfAssetPath>()) {
            const auto& file=texture.UncheckedGet<SdfAssetPath>();
            data.texture=file.GetResolvedPath().empty()?file.GetAssetPath():file.GetResolvedPath();
        }
        const auto format=value(get("texture:format"),TfToken("automatic"));
        data.textureFormat=format==TfToken("automatic")?HDUE_ENVIRONMENT_AUTOMATIC:format==TfToken("latlong")?HDUE_ENVIRONMENT_LATLONG:99;
        if (data.viewer) {
            // Hdx uses 15000 for its camera light when the renderer is not
            // Storm. Treat this application light separately from USD lights.
            const auto diffuse = applicationLight.UncheckedGet<GlfSimpleLight>().GetDiffuse();
            data.color = GfVec3f(diffuse[0], diffuse[1], diffuse[2]);
            data.intensity = 1;
            data.transform = GfMatrix4d(1);
            data.type = HDUE_CAMERA_DISTANT_LIGHT;
        }
        const bool radial=type_==HDUE_SPHERE_LIGHT || type_==HDUE_DISK_LIGHT || type_==HDUE_CYLINDER_LIGHT;
        data.width=value(get(radial?"radius":"width"),radial?.5f:1.f);
        data.height=type_==HDUE_DISK_LIGHT?data.width:value(get("height"),1.f);
        data.length=value(get("length"),1.f);
        auto* state = static_cast<HdUEState*>(param);
        std::lock_guard lock(state->mutex);
        const auto old = state->lights.find(data.id);
        if(old!=state->lights.end() && old->second.transform==data.transform && old->second.texture==data.texture &&
            old->second.color==data.color && old->second.intensity==data.intensity && old->second.visible==data.visible &&
            old->second.type==data.type && old->second.width==data.width && old->second.height==data.height && old->second.length==data.length &&
            old->second.flags==data.flags && old->second.diffuse==data.diffuse && old->second.specular==data.specular &&
            old->second.angle==data.angle && old->second.coneAngle==data.coneAngle && old->second.coneSoftness==data.coneSoftness &&
            old->second.textureFormat==data.textureFormat && old->second.ies==data.ies && old->second.iesAngleScale==data.iesAngleScale &&
            old->second.iesNormalize==data.iesNormalize) {*bits=Clean; return;}
        if (data.viewer && old != state->lights.end() && old->second.viewer && old->second.color == data.color && old->second.visible == data.visible) {
            // The backend follows the render camera at request time. Moving
            // Hdx's camera light must not submit/cancel the entire scene.
            *bits = Clean;
            return;
        }
        state->lights[data.id] = data; state->dirtyLights.insert(data.id); ++state->revision;
        *bits = Clean;
    }
    void Finalize(HdRenderParam* param) override { static_cast<HdUEState*>(param)->erase(GetId().GetString()); }
};
}
// HdCamera that also records its transform samples for camera motion blur.
class Camera final : public HdCamera {
public:
    explicit Camera(const SdfPath& id) : HdCamera(id) {}
    void Sync(HdSceneDelegate* delegate, HdRenderParam* param, HdDirtyBits* bits) override {
        const bool transformDirty = *bits & DirtyTransform;
        HdCamera::Sync(delegate, param, bits);
        if (!transformDirty || GetId().IsEmpty()) return;
        float times[HdUEMaxMotionSamples]; GfMatrix4d values[HdUEMaxMotionSamples];
        const size_t count = delegate->SampleTransform(GetId(), -HdUEMotionWindow, HdUEMotionWindow, HdUEMaxMotionSamples, times, values);
        HdUEMotionSamples motion;
        for (size_t i = 0; i < std::min(count, HdUEMaxMotionSamples); ++i) { motion.times.push_back(times[i]); motion.values.push_back(values[i]); }
        auto* state = static_cast<HdUEState*>(param);
        std::lock_guard lock(state->mutex);
        if (motion.moving()) state->cameraMotion[GetId().GetString()] = motion; else state->cameraMotion.erase(GetId().GetString());
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
    { std::lock_guard lock(state.mutex);
      for (const auto& [id,part]:state.meshes) {
        const auto name=HdUEWetValue<std::string>(HdUEWetSettings(*state.wetBindings,part),"maskPrimvar","");
        if (!name.empty() && !part.masks.count(name) && !part.maskErrors.count(name)) sources.insert(SdfPath(part.sourceId));
      }
    }
    for (const auto& id:sources)
        if (const auto* mesh=dynamic_cast<const Mesh*>(index->GetRprim(id)))
            if (auto* delegate=index->GetSceneDelegateForRprim(id)) mesh->RefreshSelectedMasks(delegate,state);
}
HdSprim* HdUECreateMaterial(const SdfPath& id) { return new Material(id); }
HdSprim* HdUECreateLight(const SdfPath& id, uint32_t type) { return new Light(id, type); }
PXR_NAMESPACE_CLOSE_SCOPE
