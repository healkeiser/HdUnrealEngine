#ifndef HDUE_SUBDIVISION_H
#define HDUE_SUBDIVISION_H
// Uniform OpenSubdiv refinement of catmullClark and loop meshes. Primvars of every interpolation follow the refined
// topology, which hdUE then triangulates like any polygon mesh; GeomSubsets move to their child faces.
#include "pxr/imaging/hd/enums.h"
#include "pxr/imaging/hd/meshTopology.h"
#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <algorithm>
#include <memory>
#include <numeric>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdUESubdivision {
public:
    // Null unless the scheme subdivides and level > 0. The coarse topology carries its subdiv tags (creases, corners,
    // boundary and face-varying rules). fvarIndices are authored face-varying primvar indices, refined as channels 1..n.
    static std::unique_ptr<HdUESubdivision> Create(const HdMeshTopology& coarse, int level, std::vector<VtIntArray> fvarIndices = {}) {
        const auto scheme = coarse.GetScheme();
        if (level <= 0 || (scheme != PxOsdOpenSubdivTokens->catmullClark && scheme != PxOsdOpenSubdivTokens->loop)) return nullptr;
        // Channel 0 is the identity, for face-varying primvars flattened to one value per face corner.
        const size_t cornerCount = coarse.GetFaceVertexIndices().size();
        VtIntArray corners(cornerCount);
        std::iota(corners.begin(), corners.end(), 0);
        fvarIndices.erase(std::remove_if(fvarIndices.begin(), fvarIndices.end(), [&](const VtIntArray& indices) {
            return indices.size() != cornerCount || std::any_of(indices.begin(), indices.end(), [](int i) { return i < 0; });
        }), fvarIndices.end());
        fvarIndices.insert(fvarIndices.begin(), corners);
        auto refiner = PxOsdRefinerFactory::Create(coarse.GetPxOsdMeshTopology(), fvarIndices);
        if (!refiner) return nullptr;
        OpenSubdiv::Far::TopologyRefiner::UniformOptions options(level);
        options.fullTopologyInLastLevel = true;
        refiner->RefineUniform(options);
        auto result = std::unique_ptr<HdUESubdivision>(new HdUESubdivision);
        result->refiner_ = refiner; result->level_ = level; result->fvarIndices_ = std::move(fvarIndices);
        const auto& last = refiner->GetLevel(level);
        VtIntArray counts(last.GetNumFaces()), indices, holes;
        for (int f = 0; f < last.GetNumFaces(); ++f) {
            const auto vertices = last.GetFaceVertices(f);
            counts[f] = vertices.size();
            for (int i = 0; i < vertices.size(); ++i) indices.push_back(vertices[i]);
            if (last.IsFaceHole(f)) holes.push_back(f);
        }
        result->topology_ = HdMeshTopology(PxOsdOpenSubdivTokens->none, coarse.GetOrientation(), counts, indices, holes);
        // Each coarse face's descendants in the last level, for GeomSubsets.
        std::vector<std::vector<int>> children(coarse.GetFaceVertexCounts().size());
        for (size_t f = 0; f < children.size(); ++f) {
            std::vector<int> faces{int(f)};
            for (int l = 0; l < level; ++l) {
                std::vector<int> next;
                for (int face : faces) for (int child : refiner->GetLevel(l).GetFaceChildFaces(face)) next.push_back(child);
                faces = std::move(next);
            }
            children[f] = std::move(faces);
        }
        HdGeomSubsets subsets;
        for (const auto& subset : coarse.GetGeomSubsets()) {
            auto refined = subset; refined.indices.clear();
            for (int face : subset.indices)
                if (face >= 0 && size_t(face) < children.size()) for (int child : children[face]) refined.indices.push_back(child);
            subsets.push_back(refined);
        }
        result->topology_.SetGeomSubsets(subsets);
        return result;
    }
    const HdMeshTopology& topology() const { return topology_; }
    // The face-varying channel refining values with these authored indices; 0 (flattened values) when none was built.
    int channel(const VtIntArray& indices) const {
        for (size_t c = 1; c < fvarIndices_.size(); ++c) if (fvarIndices_[c] == indices) return int(c);
        return 0;
    }
    // Values for the refined topology in the same interpolation; empty when the count does not match the coarse mesh.
    // Face-varying values come back one per refined face corner, whichever channel indexes them.
    template<class T> VtArray<T> refine(const VtArray<T>& values, HdInterpolation domain, int channel = 0) const {
        if (domain == HdInterpolationConstant || domain == HdInterpolationInstance) return values;
        const auto expected = [&](int l) {
            const auto& level = refiner_->GetLevel(l);
            return domain == HdInterpolationUniform ? level.GetNumFaces() : domain == HdInterpolationFaceVarying ? level.GetNumFVarValues(channel) : level.GetNumVertices();
        };
        // An indexed channel counts values up to its largest index; authored values past it are unused.
        if (channel ? int(values.size()) < expected(0) : int(values.size()) != expected(0)) return {};
        std::vector<Element<T>> src(values.begin(), values.begin() + expected(0)), dst;
        OpenSubdiv::Far::PrimvarRefiner refiner(*refiner_);
        for (int l = 1; l <= level_; ++l) {
            dst.assign(expected(l), Element<T>{});
            const Element<T>* in = src.data();
            Element<T>* out = dst.data();
            if (domain == HdInterpolationVertex) refiner.Interpolate(l, in, out);
            else if (domain == HdInterpolationVarying) refiner.InterpolateVarying(l, in, out);
            else if (domain == HdInterpolationUniform) refiner.InterpolateFaceUniform(l, in, out);
            else refiner.InterpolateFaceVarying(l, in, out, channel);
            src.swap(dst);
        }
        VtArray<T> result;
        if (domain == HdInterpolationFaceVarying) {
            // Back to one value per refined face corner, in the order of the refined topology.
            const auto& last = refiner_->GetLevel(level_);
            for (int f = 0; f < last.GetNumFaces(); ++f) for (int value : last.GetFaceFVarValues(f, channel)) result.push_back(src[value].value);
        } else {
            result.reserve(src.size());
            for (const auto& element : src) result.push_back(element.value);
        }
        return result;
    }
private:
    template<class T> struct Element {
        T value = T(0); // Gf vectors leave default-constructed values uninitialized
        Element() = default;
        Element(const T& v) : value(v) {}
        void Clear(void* = nullptr) { value = T(0); }
        void AddWithWeight(const Element& source, float weight) { value += source.value * weight; }
    };
    HdUESubdivision() = default;
    PxOsdTopologyRefinerSharedPtr refiner_;
    int level_ = 0;
    std::vector<VtIntArray> fvarIndices_;
    HdMeshTopology topology_;
};

PXR_NAMESPACE_CLOSE_SCOPE
#endif
