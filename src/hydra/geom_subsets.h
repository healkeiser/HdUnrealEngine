#pragma once
#include "delegate.h"
#include "pxr/imaging/hd/meshTopology.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/base/tf/diagnostic.h"
#include <stdexcept>
#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE
// The bridge has one material per mesh. Partition indexed triangles by their coarse USD face; each part keeps
// its own copy of the vertices it uses (shared vertices are duplicated across parts), and the original pick path.
inline std::vector<HdUEMeshData> HdUEPartitionSubsets(HdUEMeshData mesh, const HdMeshTopology& topology,
                                                      const VtIntArray& faceParams) {
    if (topology.GetGeomSubsets().empty() || !mesh.sceneAsset.empty()) {
        std::vector<HdUEMeshData> result;
        result.emplace_back(std::move(mesh));
        return result;
    }
    const size_t vertices = mesh.points.size();
    const auto sized = [&](size_t size) { return size == vertices; };
    if (mesh.indices.size() != faceParams.size() * 3 || !sized(mesh.normals.size()) ||
        !sized(mesh.sourceCorners.size()) || (!mesh.uv.empty() && !sized(mesh.uv.size())) ||
        !std::all_of(mesh.uvSets.begin(), mesh.uvSets.end(), [&](const auto& set) { return sized(set.size()); }) ||
        (!mesh.colors.empty() && !sized(mesh.colors.size())) ||
        !std::all_of(mesh.indices.begin(), mesh.indices.end(), [&](uint32_t index) { return index < vertices; }) ||
        !std::all_of(mesh.deformOffsets.begin(), mesh.deformOffsets.end(),
                     [&](const auto& offsets) { return sized(offsets.size()); }) ||
        !std::all_of(mesh.masks.begin(), mesh.masks.end(), [&](const auto& mask) { return sized(mask.second.size()); }))
        throw std::invalid_argument("Invalid indexed subset attributes");
    std::vector<int> owners(topology.GetFaceVertexCounts().size(), 0);
    HdUEMeshData prototype;
    prototype.id = mesh.id;
    prototype.material = mesh.material;
    prototype.scenePath = mesh.scenePath;
    prototype.sceneAsset = mesh.sceneAsset;
    prototype.transform = mesh.transform;
    prototype.visible = mesh.visible;
    prototype.renderTag = mesh.renderTag;  // a part without its purpose was hidden by the render-tag filter
    prototype.bindingPath = mesh.bindingPath;
    prototype.maskErrors = mesh.maskErrors;
    prototype.sourceId = mesh.sourceId;
    prototype.primId = mesh.primId;
    prototype.categories = mesh.categories;
    prototype.doubleSided = mesh.doubleSided;
    prototype.instanced = mesh.instanced;
    prototype.instances = mesh.instances;
    prototype.instanceColors = mesh.instanceColors;
    prototype.instanceIds = mesh.instanceIds;
    prototype.motionLeft = mesh.motionLeft;
    prototype.motionRight = mesh.motionRight;
    prototype.velocities = mesh.velocities;
    prototype.accelerations = mesh.accelerations;
    prototype.angularVelocities = mesh.angularVelocities;
    prototype.blurScale = mesh.blurScale;
    prototype.deformTimes = mesh.deformTimes;
    prototype.deformOffsets.resize(mesh.deformOffsets.size());
    prototype.uvSets.resize(mesh.uvSets.size());
    std::vector<HdUEMeshData> parts{prototype};
    bool invalid = false;
    for (const auto& subset : topology.GetGeomSubsets()) {
        if (subset.type != HdGeomSubset::TypeFaceSet || subset.materialId.IsEmpty()) continue;
        const int owner = int(parts.size());
        parts.push_back(prototype);
        parts.back().id = subset.id.GetString();
        parts.back().material = subset.materialId.GetString();
        parts.back().bindingPath = SdfPath(mesh.scenePath).AppendChild(subset.id.GetNameToken()).GetString();
        // materialBind subsets are nonOverlapping; a face listed again keeps its first subset, as Storm draws it.
        for (int face : subset.indices) {
            if (face < 0 || size_t(face) >= owners.size() || (owners[face] != 0 && owners[face] != owner)) {
                invalid = true;
                continue;
            }
            owners[face] = owner;
        }
    }
    if (invalid)
        TF_WARN("%s: materialBind subsets overlap or list missing faces; each face keeps its first subset",
                mesh.id.c_str());
    // Each part's local index of a mesh vertex, valid where stamp holds the part.
    std::vector<uint32_t> local(vertices);
    std::vector<int> stamp(vertices, -1);
    std::vector<std::vector<uint32_t>> triangles(parts.size());
    for (size_t triangle = 0; triangle < faceParams.size(); ++triangle) {
        const int face = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(faceParams[triangle]);
        if (face < 0 || size_t(face) >= owners.size()) throw std::invalid_argument("Invalid subset coarse face");
        triangles[size_t(owners[face])].push_back(uint32_t(triangle));
    }
    for (size_t owner = 0; owner < parts.size(); ++owner) {
        auto& part = parts[owner];
        part.indices.reserve(triangles[owner].size() * 3);
        for (const uint32_t triangle : triangles[owner])
            for (size_t corner = triangle * 3; corner < triangle * 3 + 3; ++corner) {
                const uint32_t vertex = mesh.indices[corner];
                if (stamp[vertex] != int(owner)) {
                    stamp[vertex] = int(owner);
                    local[vertex] = uint32_t(part.points.size());
                    part.points.push_back(mesh.points[vertex]);
                    part.normals.push_back(mesh.normals[vertex]);
                    if (!mesh.uv.empty()) part.uv.push_back(mesh.uv[vertex]);
                    for (size_t k = 0; k < mesh.uvSets.size(); ++k) part.uvSets[k].push_back(mesh.uvSets[k][vertex]);
                    if (!mesh.colors.empty()) part.colors.push_back(mesh.colors[vertex]);
                    part.sourceCorners.push_back(mesh.sourceCorners[vertex]);
                    for (size_t k = 0; k < mesh.deformOffsets.size(); ++k)
                        part.deformOffsets[k].push_back(mesh.deformOffsets[k][vertex]);
                    for (const auto& [name, values] : mesh.masks) part.masks[name].push_back(values[vertex]);
                }
                part.indices.push_back(local[vertex]);
            }
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const auto& part) { return part.indices.empty(); }),
                parts.end());
    return parts;
}
PXR_NAMESPACE_CLOSE_SCOPE
