#ifndef HDUE_NURBS_H
#define HDUE_NURBS_H
// NURBS evaluation for NurbsCurves and NurbsPatch: Hydra's HdsiNurbsApproximatingSceneIndex draws the control hull, so
// hdUE first replaces the control points with points sampled on the curve or surface (order 2, a polyline or grid).
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/types.h"
#include <algorithm>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Samples per knot span.
constexpr int HdUENurbsSamplesPerSpan = 16;

// The `order` basis functions non-zero at t (The NURBS Book A2.2), for control points first..first + order - 1;
// knots hold count + order values.
inline std::vector<double> HdUENurbsBasis(int count, int order, const double* knots, double t, int& first) {
    int span = order - 1;
    while (span < count - 1 && t >= knots[span + 1]) ++span;
    first = span - order + 1;
    std::vector<double> n(order, 0.), left(order, 0.), right(order, 0.);
    n[0] = 1;
    for (int j = 1; j < order; ++j) {
        left[j] = t - knots[span + 1 - j];
        right[j] = knots[span + j] - t;
        double saved = 0;
        for (int r = 0; r < j; ++r) {
            const double d = right[r + 1] + left[j - r], q = d != 0 ? n[r] / d : 0;
            n[r] = saved + right[r + 1] * q;
            saved = left[j - r] * q;
        }
        n[j] = saved;
    }
    return n;
}
// Parameters sampling [lo, hi] (the knot domain when the range is empty or reversed), HdUENurbsSamplesPerSpan per span.
inline std::vector<double> HdUENurbsParameters(int count, int order, const double* knots, double lo, double hi) {
    if (!(lo < hi)) {
        lo = knots[order - 1];
        hi = knots[count];
    }
    const int samples = std::max(1, count - order + 1) * HdUENurbsSamplesPerSpan + 1;
    std::vector<double> out(samples);
    for (int i = 0; i < samples; ++i) out[i] = lo + (hi - lo) * i / (samples - 1);
    return out;
}
// One curve: count control points (with weights when not empty) of the given order and knots, sampled; empty when the
// knots do not match.
inline VtVec3fArray HdUENurbsCurve(const GfVec3f* points, const double* weights, int count, int order,
                                   const double* knots, size_t knotCount, double lo, double hi) {
    if (order < 2 || count < order || knotCount != size_t(count + order)) return {};
    VtVec3fArray out;
    for (const double t : HdUENurbsParameters(count, order, knots, lo, hi)) {
        int first = 0;
        const auto n = HdUENurbsBasis(count, order, knots, t, first);
        GfVec3d p(0);
        double w = 0;
        for (int i = 0; i < order; ++i) {
            const double b = n[i] * (weights ? weights[first + i] : 1.);
            p += b * GfVec3d(points[first + i]);
            w += b;
        }
        out.push_back(GfVec3f(w != 0 ? p / w : p));
    }
    return out;
}
// A patch of uCount x vCount control points (u varies fastest), sampled into a grid of uSamples x vSamples points in
// the same order; empty when the knots do not match.
inline VtVec3fArray HdUENurbsPatch(const VtVec3fArray& points, const VtDoubleArray& weights, int uCount, int vCount,
                                   int uOrder, int vOrder, const VtDoubleArray& uKnots, const VtDoubleArray& vKnots,
                                   GfVec2d uRange, GfVec2d vRange, int& uSamples, int& vSamples) {
    if (uOrder < 2 || vOrder < 2 || uCount < uOrder || vCount < vOrder || points.size() != size_t(uCount * vCount) ||
        uKnots.size() != size_t(uCount + uOrder) || vKnots.size() != size_t(vCount + vOrder) ||
        (!weights.empty() && weights.size() != points.size()))
        return {};
    const auto us = HdUENurbsParameters(uCount, uOrder, uKnots.data(), uRange[0], uRange[1]);
    const auto vs = HdUENurbsParameters(vCount, vOrder, vKnots.data(), vRange[0], vRange[1]);
    uSamples = int(us.size());
    vSamples = int(vs.size());
    VtVec3fArray out;
    out.reserve(us.size() * vs.size());
    for (const double v : vs) {
        int vFirst = 0;
        const auto nv = HdUENurbsBasis(vCount, vOrder, vKnots.data(), v, vFirst);
        for (const double u : us) {
            int uFirst = 0;
            const auto nu = HdUENurbsBasis(uCount, uOrder, uKnots.data(), u, uFirst);
            GfVec3d p(0);
            double w = 0;
            for (int j = 0; j < vOrder; ++j)
                for (int i = 0; i < uOrder; ++i) {
                    const size_t c = size_t(vFirst + j) * uCount + size_t(uFirst + i);
                    const double b = nu[i] * nv[j] * (weights.empty() ? 1. : weights[c]);
                    p += b * GfVec3d(points[c]);
                    w += b;
                }
            out.push_back(GfVec3f(w != 0 ? p / w : p));
        }
    }
    return out;
}

PXR_NAMESPACE_CLOSE_SCOPE
#endif
