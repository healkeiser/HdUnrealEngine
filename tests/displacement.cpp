#include "displacement.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
static void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
int main() {
    try {
        // A unit quad in XY (two triangles, +Z normals) with a UV seam: its second triangle has its own copies of the
        // diagonal's points, as a welded mesh keeps a UV split.
        HdUEDicedMesh quad;
        quad.points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        quad.normals.assign(6, GfVec3f(0, 0, 1));
        quad.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}};
        quad.indices = {0, 1, 2, 3, 4, 5};
        require(HdUEDiceLevels(2, 1024 * 1024) == 9 && HdUEDiceLevels(2, 8) == 0, "levels follow the texels");
        require(HdUEDiceLevels(2, 1u << 30, 1000) == 4, "levels stay within the triangle budget");
        HdUEDice(quad, 2);
        // Each triangle: 4^2 children and 15 points (6 + 9 midpoints); edge midpoints are shared.
        require(quad.indices.size() == 32 * 3 && quad.points.size() == 30, "two levels");
        require(quad.uvs[0].size() == 30 && quad.normals.size() == 30, "attributes follow the points");
        for (size_t i = 0; i < quad.points.size(); ++i)
            require(quad.points[i][0] == quad.uvs[0][i][0] && quad.points[i][1] == quad.uvs[0][i][1],
                    "UVs interpolate with the points");
        // A ramp: height = u, so the displaced surface tilts and its normals follow it on both sides of the seam.
        HdUEDisplace(quad, [&](size_t i) { return quad.uvs[0][i][0]; });
        const auto tilted = GfVec3f(-1, 0, 1).GetNormalized();
        for (size_t i = 0; i < quad.points.size(); ++i)
            require(quad.points[i][2] == quad.points[i][0] && GfIsClose(quad.normals[i], tilted, 1e-5),
                    "points move along the normal and normals follow the displaced surface");
        // Sampling: bilinear at UE UVs (top-left origin), wraps, then dot(texel * scale + bias, mask).
        HdUEImage image{2, 2, 1, {0, 1, 2, 3}};  // rows top to bottom
        const uint32_t repeat[2] = {HDUE_WRAP_REPEAT, HDUE_WRAP_REPEAT}, clamp[2] = {HDUE_WRAP_CLAMP, HDUE_WRAP_CLAMP},
                       black[2] = {HDUE_WRAP_BLACK, HDUE_WRAP_BLACK}, mirror[2] = {HDUE_WRAP_MIRROR, HDUE_WRAP_MIRROR};
        const GfVec4f one(1), zero(0), red(1, 0, 0, 0);
        require(image.sample({.25f, .25f}, repeat, one, zero, red) == 0, "top-left texel");
        require(image.sample({.75f, .75f}, repeat, one, zero, red) == 3, "bottom-right texel");
        require(image.sample({.5f, .25f}, repeat, one, zero, red) == .5f, "bilinear");
        require(image.sample({1.25f, .25f}, repeat, one, zero, red) == 0, "repeat");
        require(image.sample({0, .25f}, repeat, one, zero, red) == .5f, "repeat blends across the edge");
        require(image.sample({0, .25f}, clamp, one, zero, red) == 0, "clamp");
        require(image.sample({0, .25f}, mirror, one, zero, red) == 0, "mirror");
        require(image.sample({-.75f, .25f}, mirror, one, zero, red) == 1, "mirror reflects");
        require(image.sample({-.5f, .25f}, black, GfVec4f(2), GfVec4f(.5f), red) == .5f, "black reads the bias");
        require(image.sample({.75f, .25f}, repeat, GfVec4f(2), GfVec4f(.5f), GfVec4f(0, 0, 0, 1)) == 2.5f,
                "a missing alpha reads 1");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "displacement contracts passed\n";
    return 0;
}
