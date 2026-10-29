#pragma once
#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3h.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE
// Element i of a vec3 array primvar (float, double or half), false when missing.
template <class V>
bool HdUEInstanceVec3(const VtValue& v, size_t i, GfVec3d& out) {
    if (!v.IsHolding<VtArray<V>>() || i >= v.UncheckedGet<VtArray<V>>().size()) return false;
    const auto& x = v.UncheckedGet<VtArray<V>>()[i];
    out = GfVec3d(x[0], x[1], x[2]);
    return true;
}
inline bool HdUEInstanceVector(const VtValue& v, size_t i, GfVec3d& out) {
    return HdUEInstanceVec3<GfVec3f>(v, i, out) || HdUEInstanceVec3<GfVec3d>(v, i, out) ||
           HdUEInstanceVec3<GfVec3h>(v, i, out);
}
template <class Q>
bool HdUEInstanceQuat(const VtValue& v, size_t i, GfQuatd& out) {
    if (!v.IsHolding<VtArray<Q>>() || i >= v.UncheckedGet<VtArray<Q>>().size()) return false;
    const auto& q = v.UncheckedGet<VtArray<Q>>()[i];
    const auto im = q.GetImaginary();
    out = GfQuatd(q.GetReal(), GfVec3d(im[0], im[1], im[2]));
    return true;
}
// Element i of instanceRotations: PointInstancer orientations (quath) or orientationsf (quatf), quatd, or <real, i, j,
// k> vec4f. Normalized, as UsdGeomPointInstancer::ComputeInstanceTransformsAtTime does (half quaternions are not unit).
inline bool HdUEInstanceRotation(const VtValue& v, size_t i, GfQuatd& out) {
    if (!HdUEInstanceQuat<GfQuath>(v, i, out) && !HdUEInstanceQuat<GfQuatf>(v, i, out) &&
        !HdUEInstanceQuat<GfQuatd>(v, i, out)) {
        if (!v.IsHolding<VtVec4fArray>() || i >= v.UncheckedGet<VtVec4fArray>().size()) return false;
        const auto& q = v.UncheckedGet<VtVec4fArray>()[i];
        out = GfQuatd(q[0], GfVec3d(q[1], q[2], q[3]));
    }
    out.Normalize();
    return true;
}
// Instance index's transform as hdEmbree composes it: instanceTransform * scale * rotation * translation * instancer.
inline GfMatrix4d HdUEInstanceTransform(const GfMatrix4d& instancer, const VtValue& translations,
                                        const VtValue& rotations, const VtValue& scales, const VtValue& matrices,
                                        size_t index) {
    GfMatrix4d m = instancer;
    GfVec3d v;
    GfQuatd q;
    if (HdUEInstanceVector(translations, index, v)) m = GfMatrix4d(1).SetTranslate(v) * m;
    if (HdUEInstanceRotation(rotations, index, q)) m = GfMatrix4d(1).SetRotate(q) * m;
    if (HdUEInstanceVector(scales, index, v)) m = GfMatrix4d(1).SetScale(v) * m;
    if (matrices.IsHolding<VtMatrix4dArray>() && index < matrices.UncheckedGet<VtMatrix4dArray>().size())
        m = matrices.UncheckedGet<VtMatrix4dArray>()[index] * m;
    return m;
}
// True when m's linear part is not an axis scale times a rotation (its rows are not orthogonal). UE instances are
// FTransforms, which cannot hold that shear (a rotated prototype under a non-uniform instance scale, for instance).
inline bool HdUEHasShear(const GfMatrix4d& m) {
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const GfVec3d x(m[a][0], m[a][1], m[a][2]), y(m[b][0], m[b][1], m[b][2]);
            if (std::abs(GfDot(x, y)) > 1e-6 * x.GetLength() * y.GetLength()) return true;
        }
    return false;
}
PXR_NAMESPACE_CLOSE_SCOPE
