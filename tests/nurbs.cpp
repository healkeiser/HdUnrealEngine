#include "nurbs.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
static void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
int main() {
    try {
        // A clamped cubic with one span is the Bezier curve of its control points: ends on them, peak 1.75 at x = 0.
        const GfVec3f cvs[] = {{-4, -2, 0}, {-4.f / 3, 3, 0}, {4.f / 3, 3, 0}, {4, -2, 0}};
        const double knots[] = {0, 0, 0, 0, 1, 1, 1, 1};
        const auto curve = HdUENurbsCurve(cvs, nullptr, 4, 4, knots, 8, 0, 0);
        require(curve.size() == HdUENurbsSamplesPerSpan + 1, "one span gives samples per span + 1 points");
        require((curve.front() - cvs[0]).GetLength() < 1e-5f && (curve.back() - cvs[3]).GetLength() < 1e-5f,
                "a clamped curve ends on its end points");
        const auto& middle = curve[curve.size() / 2];
        require(std::abs(middle[0]) < 1e-5f && std::abs(middle[1] - 1.75f) < 1e-5f, "Bezier midpoint");
        // Equal weights change nothing; a heavier middle pulls the curve towards it.
        const double equal[] = {2, 2, 2, 2}, heavy[] = {1, 4, 4, 1};
        require((HdUENurbsCurve(cvs, equal, 4, 4, knots, 8, 0, 0)[8] - middle).GetLength() < 1e-5f, "uniform weights");
        require(HdUENurbsCurve(cvs, heavy, 4, 4, knots, 8, 0, 0)[8][1] > 2.5f, "rational weights");
        // A range evaluates part of the curve; mismatched knots give nothing.
        require((HdUENurbsCurve(cvs, nullptr, 4, 4, knots, 8, 0, .5).back() - middle).GetLength() < 1e-5f, "range");
        require(HdUENurbsCurve(cvs, nullptr, 4, 4, knots, 7, 0, 0).empty(), "bad knots");

        // A bicubic Bezier patch: its top edge is the Bezier curve of the top row.
        VtVec3fArray grid;
        for (int v = 0; v < 4; ++v)
            for (int u = 0; u < 4; ++u)
                grid.push_back(v < 3 ? GfVec3f(2 * u - 3, v, 0) : GfVec3f(cvs[u]) + GfVec3f(0, 5, 0));
        const VtDoubleArray patchKnots(knots, knots + 8);
        int us = 0, vs = 0;
        const auto patch = HdUENurbsPatch(grid, {}, 4, 4, 4, 4, patchKnots, patchKnots, GfVec2d(0), GfVec2d(0), us, vs);
        require(us == 17 && vs == 17 && patch.size() == 17 * 17, "patch grid");
        require(std::abs(patch[16 * 17 + 8][1] - 6.75f) < 1e-5f, "patch edge is the Bezier curve of its row");
        // Hermite: the scene's arch (tangents 12 up and 12 down) is the Bezier curve peaking at y = 1.
        VtIntArray bezierCounts;
        const auto bezier =
            HdUEHermiteToBezier({{-4, -2, 0}, {4, -2, 0}}, {{0, 12, 0}, {0, -12, 0}}, VtIntArray{2}, bezierCounts);
        require(bezierCounts == VtIntArray{4} && bezier.size() == 4, "one Hermite segment, four Bezier points");
        require(bezier[1] == GfVec3f(-4, 2, 0) && bezier[2] == GfVec3f(4, 2, 0), "tangents / 3");
        const double bezierKnots[] = {0, 0, 0, 0, 1, 1, 1, 1};
        require(std::abs(HdUENurbsCurve(bezier.data(), nullptr, 4, 4, bezierKnots, 8, 0, 0)[8][1] - 1.f) < 1e-5f,
                "Hermite arch peak");
        require(HdUEHermiteToBezier({{0, 0, 0}}, {}, VtIntArray{1}, bezierCounts).empty(), "bad tangents");
        std::cout << "nurbs: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nurbs: " << error.what() << "\n";
        return 1;
    }
}
