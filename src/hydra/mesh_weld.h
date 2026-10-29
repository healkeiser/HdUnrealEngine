#pragma once
#include "pxr/pxr.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/vt/array.h"
#include <cstdint>
#include <stdexcept>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
// Welds triangulated corners (3 * triangle + corner) into vertices: corners of one point share a vertex unless
// same(vertexCorner, corner) finds a per-corner attribute (face-varying normal, UV...) that differs. Fills one
// vertex index per corner and returns each vertex's first corner. Points never referenced get no vertex.
template <class Same>
std::vector<uint32_t> HdUEWeldCorners(const VtVec3iArray& triangles, size_t pointCount, Same&& same, std::vector<uint32_t>& indices) {
    constexpr uint32_t None = UINT32_MAX;
    if (triangles.size() >= None / 3) throw std::invalid_argument("Mesh exceeds 32-bit corner indices");
    // Per point its first vertex, per vertex the next vertex of the same point: chains are as long as a point's
    // distinct face-varying values (1 for smooth, connected attributes).
    std::vector<uint32_t> first(pointCount, None), next, corners;
    indices.resize(triangles.size() * 3);
    for (size_t corner = 0; corner < indices.size(); ++corner) {
        const int point = triangles[corner / 3][int(corner % 3)];
        if (point < 0 || size_t(point) >= pointCount) throw std::invalid_argument("Invalid mesh index");
        uint32_t* link = &first[size_t(point)];
        while (*link != None && !same(size_t(corners[*link]), corner)) link = &next[*link];
        uint32_t vertex = *link;
        if (vertex == None) { // link is not used once next grows
            vertex = *link = uint32_t(corners.size());
            corners.push_back(uint32_t(corner)); next.push_back(None);
        }
        indices[corner] = vertex;
    }
    return corners;
}
PXR_NAMESPACE_CLOSE_SCOPE
