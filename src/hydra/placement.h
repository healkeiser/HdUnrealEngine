#pragma once
#include "pxr/base/gf/matrix4d.h"
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE
// Generic meshes are uploaded in local space and placed by their UE actor's transform, which holds a rotation, a scale
// along its own axes (negative included) and a translation, but no shear: a matrix whose rows are not orthogonal (a
// non-uniform scale above a rotation) is baked into the vertices instead. The USD -> UE basis is a signed axis
// permutation, so the test holds in either space. A projective matrix counts as shearing.
inline bool HdUEShears(const GfMatrix4d& m) {
    if (m[0][3] != 0 || m[1][3] != 0 || m[2][3] != 0) return true;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const GfVec3d u(m[a][0], m[a][1], m[a][2]), v(m[b][0], m[b][1], m[b][2]);
            if (std::abs(GfDot(u, v)) > 1e-5 * u.GetLength() * v.GetLength()) return true;
        }
    return false;
}
PXR_NAMESPACE_CLOSE_SCOPE
