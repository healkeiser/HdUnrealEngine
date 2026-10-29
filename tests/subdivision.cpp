#include "subdivision.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/types.h"
#include "pxr/imaging/hd/tokens.h"
#include <iostream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
static void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
int main() {
    try {
        // Unit cube: infinitely sharp corner tags keep its corners (|x| + |y| + |z| = 1.5); without tags they round
        // off.
        const VtVec3fArray cube{{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {-.5f, .5f, -.5f}, {.5f, .5f, -.5f},
                                {-.5f, -.5f, .5f},  {.5f, -.5f, .5f},  {-.5f, .5f, .5f},  {.5f, .5f, .5f}};
        HdMeshTopology box(PxOsdOpenSubdivTokens->catmullClark, HdTokens->rightHanded, VtIntArray(6, 4),
                           VtIntArray{0, 1, 3, 2, 4, 6, 7, 5, 0, 4, 5, 1, 2, 3, 7, 6, 0, 2, 6, 4, 1, 5, 7, 3});
        const auto extent = [&](const HdMeshTopology& topology) {
            float out = 0;
            for (const auto& p : HdUESubdivision::Create(topology, 2)->refine(cube, HdInterpolationVertex))
                out = std::max(out, std::abs(p[0]) + std::abs(p[1]) + std::abs(p[2]));
            return out;
        };
        require(extent(box) < 1.3f, "smooth cube keeps its corners");
        box.SetSubdivTags(PxOsdSubdivTags(PxOsdOpenSubdivTokens->edgeAndCorner, PxOsdOpenSubdivTokens->cornersPlus1, {},
                                          {}, {}, {}, {}, VtIntArray{0, 1, 2, 3, 4, 5, 6, 7}, VtFloatArray(8, 10.f)));
        require(std::abs(extent(box) - 1.5f) < 1e-5f, "corner tags are ignored");

        // Two quads sharing an edge, with UVs equal to the points' xy, indexed like the points. Refined through their
        // indices (smooth face-varying interpolation, smooth boundaries) they match the refined points; flattened per
        // corner, each face is its own island and they do not.
        const VtVec3fArray grid{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0}};
        const VtIntArray corners{0, 1, 4, 3, 1, 2, 5, 4};
        HdMeshTopology quads(PxOsdOpenSubdivTokens->catmullClark, HdTokens->rightHanded, VtIntArray{4, 4}, corners);
        quads.SetSubdivTags(
            PxOsdSubdivTags(PxOsdOpenSubdivTokens->edgeOnly, PxOsdOpenSubdivTokens->none, {}, {}, {}, {}, {}, {}, {}));
        VtVec2fArray uv;
        for (const auto& p : grid) uv.emplace_back(p[0], p[1]);
        VtVec2fArray flat;
        for (int i : corners) flat.push_back(uv[i]);
        const auto subdivision = HdUESubdivision::Create(quads, 2, {corners});
        const int channel = subdivision->channel(corners);
        require(channel == 1, "indexed channel missing");
        const auto points = subdivision->refine(grid, HdInterpolationVertex);
        const auto indexed = subdivision->refine(uv, HdInterpolationFaceVarying, channel);
        const auto islands = subdivision->refine(flat, HdInterpolationFaceVarying);
        const auto& refined = subdivision->topology().GetFaceVertexIndices();
        require(indexed.size() == refined.size() && islands.size() == refined.size(), "face-varying count");
        float indexedError = 0, islandError = 0;
        for (size_t c = 0; c < refined.size(); ++c) {
            const auto& p = points[refined[c]];
            indexedError = std::max(indexedError, (indexed[c] - GfVec2f(p[0], p[1])).GetLength());
            islandError = std::max(islandError, (islands[c] - GfVec2f(p[0], p[1])).GetLength());
        }
        require(indexedError < 1e-5f, "indexed UVs do not follow the points");
        require(islandError > 1e-3f, "flattened UVs should be islands");
        // Unused trailing values past the largest index are ignored; too few values fail.
        VtVec2fArray extra = uv;
        extra.emplace_back(9.f, 9.f);
        require(subdivision->refine(extra, HdInterpolationFaceVarying, channel) == indexed, "trailing values");
        require(
            subdivision->refine(VtVec2fArray(uv.begin(), uv.begin() + 5), HdInterpolationFaceVarying, channel).empty(),
            "short values");
        std::cout << "subdivision tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
