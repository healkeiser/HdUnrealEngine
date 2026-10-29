#include "instancing.h"
#include "pxr/base/gf/rotation.h"
#include <iostream>
#include <stdexcept>
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
bool near(const GfVec3d& a, const GfVec3d& b) { return (a - b).GetLength() < 1e-3; }
GfVec3d place(const VtValue& translations, const VtValue& rotations, const VtValue& scales, size_t index,
              const GfVec3d& point, const VtValue& matrices = {}) {
    const GfMatrix4d instancer = GfMatrix4d(1).SetTranslate(GfVec3d(10, 0, 0));
    return HdUEInstanceTransform(instancer, translations, rotations, scales, matrices, index).Transform(point);
}
}  // namespace
int main() {
    try {
        const GfQuatd z90 = GfRotation(GfVec3d(0, 0, 1), 90).GetQuat();
        const VtValue translations(VtVec3fArray{{0, 0, 0}, {1, 2, 3}});
        const VtValue scales(VtVec3fArray{{1, 1, 1}, {2, 1, 1}});
        const auto halfQuat = GfQuath(GfHalf(z90.GetReal()), GfVec3h(z90.GetImaginary()));
        // scale, then rotate, then translate, then the instancer: (1,0,0) -> (2,0,0) -> (0,2,0) -> (1,4,3) -> (11,4,3).
        check(near(place(translations, VtValue(VtQuathArray{GfQuath(1), halfQuat}), scales, 1, GfVec3d(1, 0, 0)),
                   GfVec3d(11, 4, 3)),
              "orientations (quath) composition order");
        check(near(place(translations, VtValue(VtQuatfArray{GfQuatf(1), GfQuatf(z90)}), scales, 1, GfVec3d(1, 0, 0)),
                   GfVec3d(11, 4, 3)),
              "orientationsf (quatf)");
        check(near(place(translations,
                         VtValue(VtVec4fArray{{1, 0, 0, 0},
                                              GfVec4f(float(z90.GetReal()), 0, 0, float(z90.GetImaginary()[2]))}),
                         scales, 1, GfVec3d(1, 0, 0)),
                   GfVec3d(11, 4, 3)),
              "vec4f <real, i, j, k> rotations");
        // A non-unit quaternion rotates without scaling, as UsdGeomPointInstancer computes it; a zero one is identity.
        check(near(place({}, VtValue(VtQuatfArray{GfQuatf(0, 0, 0, 2)}), {}, 0, GfVec3d(1, 0, 0)), GfVec3d(9, 0, 0)),
              "non-unit orientation was not normalized");
        check(near(place({}, VtValue(VtQuatfArray{GfQuatf(0, 0, 0, 0)}), {}, 0, GfVec3d(1, 0, 0)), GfVec3d(11, 0, 0)),
              "zero orientation is not identity");
        check(near(place(VtValue(VtVec3hArray{GfVec3h(1, 2, 3)}), {}, {}, 0, GfVec3d(0)), GfVec3d(11, 2, 3)),
              "half positions");
        check(near(place(translations, {}, {}, 5, GfVec3d(0)), GfVec3d(10, 0, 0)),
              "an index past the primvar must leave the instancer transform");
        const GfMatrix4d matrix = GfMatrix4d(1).SetTranslate(GfVec3d(0, 0, 5));
        check(near(place(translations, {}, scales, 1, GfVec3d(1, 0, 0), VtValue(VtMatrix4dArray{matrix, matrix})),
                   GfVec3d(13, 2, 8)),
              "instanceTransforms apply first");
        const GfMatrix4d rotate = GfMatrix4d(1).SetRotate(GfRotation(GfVec3d(0, 0, 1), 45));
        const GfMatrix4d stretch = GfMatrix4d(1).SetScale(GfVec3d(2, 1, 1));
        check(!HdUEHasShear(stretch * rotate * GfMatrix4d(1).SetTranslate(GfVec3d(1, 2, 3))),
              "scale then rotation reported as shear");
        check(!HdUEHasShear(GfMatrix4d(1).SetScale(GfVec3d(-1, 2, 3))), "negative scale reported as shear");
        check(HdUEHasShear(rotate * stretch), "rotated prototype under a non-uniform scale is sheared");
        std::cout << "PASS: instance composition, quath/quatf/vec4f orientations, normalization, shear detection\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
