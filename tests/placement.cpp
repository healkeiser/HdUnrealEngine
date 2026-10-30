#include "placement.h"
#include "pxr/base/gf/rotation.h"
#include <iostream>
PXR_NAMESPACE_USING_DIRECTIVE
// HdUEShears decides which meshes are uploaded in local space and moved by their UE actor's transform (rotation, axis
// scale, translation) instead of re-uploaded with baked vertices.
int main() {
    const GfMatrix4d rotate = GfMatrix4d(1).SetRotate(GfRotation(GfVec3d(1, 2, 3), 37)),
                     scale = GfMatrix4d(1).SetScale(GfVec3d(2, -.5, 3)), move = GfMatrix4d(1).SetTranslate({4, 5, 6});
    // The USD (Y up) -> UE basis: a signed axis permutation, as HdUEState::Converter applies it.
    GfMatrix4d basis(0.0);
    basis[0][1] = basis[1][2] = basis[3][3] = 1;
    basis[2][0] = -1;
    struct Case {
        GfMatrix4d m;
        bool shears;
        const char* what;
    } cases[] = {
        {GfMatrix4d(1), false, "identity"},
        {scale * rotate * move, false, "scale, then rotation and translation (mirrored)"},
        {rotate * scale, true, "non-uniform scale above a rotation"},
        {rotate * GfMatrix4d(1).SetScale(3.0) * rotate, false, "uniform scale between rotations"},
        {GfMatrix4d(1, 0, 0, .1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), true, "projective"},
    };
    int failures = 0;
    for (const auto& c : cases)
        for (const auto& m : {c.m, basis.GetInverse() * c.m * basis})
            if (HdUEShears(m) != c.shears) {
                std::cerr << "HdUEShears wrong for " << c.what << '\n';
                ++failures;
            }
    return failures;
}
