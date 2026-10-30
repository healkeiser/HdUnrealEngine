#pragma once
// UsdPreviewSurface displacement as geometry. UE's path tracer traces neither World Position Offset nor Nanite
// tessellation, so the delegate dices a displaced mesh (each triangle into 4 per level, edge midpoints shared) and
// moves its points along their normals before the upload: Lumen, the Path Tracer and Nanite all see the same surface.
#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/hash.h"
#include "hdue/bridge.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
struct HdUEDicedMesh {
    std::vector<GfVec3f> points, normals, colors;  // colors: empty or per point
    std::vector<std::vector<GfVec2f>> uvs;         // UV sets, per point
    std::vector<uint32_t> indices;                 // three per triangle
};
// Levels that bring the triangle count to about one triangle per two texels, within budget triangles.
inline int HdUEDiceLevels(size_t triangles, size_t texels, size_t budget = size_t(1) << 20) {
    int levels = 0;
    for (const size_t target = std::min(texels / 2, budget); triangles && triangles * 4 <= target; triangles *= 4)
        ++levels;
    return levels;
}
// Splits every triangle into 4, levels times; a new point interpolates its edge's two points (normals re-normalized).
inline void HdUEDice(HdUEDicedMesh& mesh, int levels) {
    for (int level = 0; level < levels; ++level) {
        std::unordered_map<uint64_t, uint32_t> midpoints;
        const auto midpoint = [&](uint32_t a, uint32_t b) {
            const auto [it, added] =
                midpoints.try_emplace(uint64_t(std::min(a, b)) << 32 | std::max(a, b), uint32_t(mesh.points.size()));
            if (added) {
                const auto mix = [&](auto& values) {
                    if (!values.empty()) values.push_back((values[a] + values[b]) * .5f);
                };
                mix(mesh.points);
                mix(mesh.normals);
                if (!mesh.normals.empty()) mesh.normals.back().Normalize();
                mix(mesh.colors);
                for (auto& set : mesh.uvs) mix(set);
            }
            return it->second;
        };
        std::vector<uint32_t> indices;
        indices.reserve(mesh.indices.size() * 4);
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
            const uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            indices.insert(indices.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
        }
        mesh.indices = std::move(indices);
    }
}
// Moves each point along its normal by height(point). With normals, the mesh then gets the displaced surface's normals,
// area weighted over the triangles around each undisplaced position (UV seams split points, not the surface).
template <class Height>
void HdUEDisplace(HdUEDicedMesh& mesh, Height height, bool normals = true) {
    const auto rest = mesh.points;
    for (size_t i = 0; i < mesh.points.size(); ++i) mesh.points[i] += mesh.normals[i].GetNormalized() * height(i);
    if (!normals) return;
    std::unordered_map<GfVec3f, GfVec3d, TfHash> sums;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const auto& p = mesh.points;
        const uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
        const auto area = GfCross(GfVec3d(p[b] - p[a]), GfVec3d(p[c] - p[a]));
        for (const uint32_t i : {a, b, c}) sums[rest[i]] += area;
    }
    for (size_t i = 0; i < mesh.points.size(); ++i)
        if (const auto sum = sums[rest[i]]; sum.GetLengthSq() > 1e-24) mesh.normals[i] = GfVec3f(sum.GetNormalized());
}
// A float image (rows top to bottom) sampled like UE samples a texture slot: bilinear, at UE UVs (top-left origin),
// with the slot's wraps, reduced to dot(texel * scale + bias, mask). Missing channels read the first; alpha reads 1.
struct HdUEImage {
    int width = 0, height = 0, channels = 0;
    std::vector<float> texels;
    float sample(GfVec2f uv, const uint32_t wrap[2], const GfVec4f& scale, const GfVec4f& bias,
                 const GfVec4f& mask) const {
        const auto address = [&](int i, int size, uint32_t mode) {
            if (mode == HDUE_WRAP_CLAMP) return std::clamp(i, 0, size - 1);
            if (mode == HDUE_WRAP_BLACK) return i < 0 || i >= size ? -1 : i;
            if (mode != HDUE_WRAP_MIRROR) return ((i % size) + size) % size;
            const int k = ((i % (2 * size)) + 2 * size) % (2 * size);
            return k < size ? k : 2 * size - 1 - k;
        };
        const float x = uv[0] * width - .5f, y = uv[1] * height - .5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        GfVec4f texel(0);
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const int px = address(x0 + dx, width, wrap[0]), py = address(y0 + dy, height, wrap[1]);
                if (px < 0 || py < 0) continue;  // black wrap
                const float* t = &texels[(size_t(py) * width + px) * channels];
                const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                for (int c = 0; c < 4; ++c) texel[c] += w * (c < channels ? t[c] : c == 3 ? 1.f : t[0]);
            }
        return GfDot(GfCompMult(texel, scale) + bias, mask);
    }
};
PXR_NAMESPACE_CLOSE_SCOPE
