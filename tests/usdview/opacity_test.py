"""Exercise review meshes at opacity .7 / 1 / 0 in both renderers."""

import hashlib
import time

import numpy as np
from material_api import MaterialApi
from pxr import Gf, Usd, UsdGeom, Vt
from scene_check import SceneCheck


class OpacityTest(SceneCheck):
    def __init__(self, *args):
        super().__init__(*args)
        self.deadline = time.monotonic() + 900
        self.material_checks = []
        self.comparisons = []

    def image(self, label, samples):
        previous = self.frames[-1]["generation"] if self.frames else 0
        return (yield from self.wait_image(label, previous, samples=samples))

    def materials(self, targets):
        """The meshes drawn translucent are the targets whose displayOpacity is below 1, and the displayColor
        materials in use (one per look, shared by the meshes that have it) are blended exactly when some are.
        """
        expected = sum(
            1
            for prim in targets
            if any(
                float(x) < 1
                for x in UsdGeom.Gprim(prim).GetDisplayOpacityPrimvar().Get()
                or [1]
            )
        )
        stats = self.stats()
        if stats["translucentMeshes"] != expected:
            raise AssertionError(
                f"{stats['translucentMeshes']} translucent meshes, expected {expected}"
            )
        looks = {
            look: self.api.material_stats(stats["sessionHandle"], look)
            for look in stats["displayColorMaterials"]
        }
        blended = [
            look for look, info in looks.items() if info["blend_mode"] == 2
        ]
        if bool(blended) != bool(expected) or not all(
            info["substrate_enabled"] for info in looks.values()
        ):
            raise AssertionError(f"Wrong opacity materials: {looks}")
        self.material_checks.append(
            dict(translucent=expected, looks=len(looks), blended=len(blended))
        )

    def run(self):
        model = self.controller._dataModel
        stage = model.stage
        root, session = stage.GetRootLayer(), stage.GetSessionLayer()
        digest = hashlib.sha256(open(root.realPath, "rb").read()).hexdigest()
        original = session.ExportToString()
        edit_target = stage.GetEditTarget()
        meshes = [p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)]
        targets = [
            p
            for p in meshes
            if (
                UsdGeom.Gprim(p).GetDisplayOpacityPrimvar().Get() is not None
                and any(
                    float(x) < 1
                    for x in UsdGeom.Gprim(p).GetDisplayOpacityPrimvar().Get()
                )
            )
        ]
        if len(targets) != 13:
            raise AssertionError(
                f"Expected 13 Review scene opacity meshes, got {len(targets)}"
            )
        cache = UsdGeom.BBoxCache(
            Usd.TimeCode.Default(), [UsdGeom.Tokens.default_]
        )
        bounds = {
            str(p.GetPath()): cache.ComputeWorldBound(p).ComputeAlignedBox()
            for p in targets
        }
        model.viewSettings.showHUD = False
        model.viewSettings.showBBoxes = False
        self.controller._mainWindow.resize(1600, 1000)
        self.api = MaterialApi(self.launcher.runtime)
        stage.SetEditTarget(session)
        try:
            for mode, samples in (("realtime", 0), ("pathtraced", 64)):
                model.viewSettings.cameraPrim = None
                session.ImportFromString(original)
                self.view.setFixedSize(1280, 720)
                if mode == "pathtraced":
                    self.launcher.select_mode(mode)
                yield
                baseline = yield from self.image(mode + "-scene-07", samples)
                if self.stats()["translucentMeshes"] != 13:
                    raise AssertionError(
                        "Review scene opacity primvars were not imported"
                    )
                self.materials(targets)
                submitted = self.stats()["submittedMeshes"]
                for prim in targets:
                    UsdGeom.Gprim(prim).GetDisplayOpacityPrimvar().Set(
                        Vt.FloatArray([1])
                    )
                yield from self.image(mode + "-scene-opaque", samples)
                if self.stats()["translucentMeshes"]:
                    raise AssertionError(
                        "Opaque edits did not select opaque parents"
                    )
                self.materials(targets)
                for prim in targets:
                    UsdGeom.Gprim(
                        prim
                    ).GetDisplayOpacityPrimvar().GetAttr().Clear()
                restored = yield from self.image(
                    mode + "-scene-restored", samples
                )
                if self.stats()["submittedMeshes"] != submitted:
                    raise AssertionError("Opacity edits resubmitted geometry")
                # Inspect each real source mesh against an authored checker backdrop.
                # All temporary visibility, camera and opacity edits live in the
                # anonymous session layer, leaving the supplied asset untouched.
                for prim in meshes:
                    UsdGeom.Imageable(prim).CreateVisibilityAttr().Set(
                        UsdGeom.Tokens.invisible
                    )
                self.view.setFixedSize(640, 480)
                camera = UsdGeom.Camera.Define(
                    stage, "/HdUEOpacityReview/Camera"
                )
                camera.CreateHorizontalApertureAttr(36)
                camera.CreateVerticalApertureAttr(27)
                camera.CreateFocalLengthAttr(38)
                camera.CreateClippingRangeAttr(Gf.Vec2f(0.01, 100000))
                camera_op = camera.AddTransformOp()
                model.viewSettings.cameraPrim = camera.GetPrim()
                panels = []
                for tile in range(4):
                    mesh = UsdGeom.Mesh.Define(
                        stage, f"/HdUEOpacityReview/Backdrop{tile}"
                    )
                    mesh.CreateFaceVertexCountsAttr([4])
                    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
                    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
                    mesh.CreateDisplayColorAttr(
                        [
                            (
                                Gf.Vec3f(0.5, 0.08, 0.035)
                                if tile in (0, 3)
                                else Gf.Vec3f(0.04, 0.1, 0.5)
                            )
                        ]
                    )
                    panels.append(mesh)
                previous_target = None
                for index, prim in enumerate(targets):
                    if previous_target:
                        UsdGeom.Imageable(
                            previous_target
                        ).GetVisibilityAttr().Set(UsdGeom.Tokens.invisible)
                    UsdGeom.Imageable(prim).GetVisibilityAttr().Set(
                        UsdGeom.Tokens.inherited
                    )
                    box = bounds[str(prim.GetPath())]
                    center = box.GetMidpoint()
                    size = box.GetSize()
                    radius = max(max(size), 0.1) * 0.8
                    distance = radius * 3.5
                    camera_op.Set(
                        Gf.Matrix4d()
                        .SetLookAt(
                            center + Gf.Vec3d(0, -distance, 0),
                            center,
                            Gf.Vec3d(0, 0, 1),
                        )
                        .GetInverse()
                    )
                    y = box.GetMax()[1] + radius * 0.3
                    for tile, panel in enumerate(panels):
                        x0 = (
                            center[0]
                            + (-1 if tile % 2 == 0 else 0) * radius * 2
                        )
                        z0 = (
                            center[2]
                            + (-1 if tile // 2 == 0 else 0) * radius * 2
                        )
                        panel.CreatePointsAttr(
                            [
                                Gf.Vec3f(x0, y, z0),
                                Gf.Vec3f(x0 + radius * 2, y, z0),
                                Gf.Vec3f(x0 + radius * 2, y, z0 + radius * 2),
                                Gf.Vec3f(x0, y, z0 + radius * 2),
                            ]
                        )
                    pictures = {}
                    for opacity, label in (
                        (0.7, "07"),
                        (1, "opaque"),
                        (0, "clear"),
                    ):
                        UsdGeom.Gprim(prim).GetDisplayOpacityPrimvar().Set(
                            Vt.FloatArray([opacity])
                        )
                        pictures[label] = yield from self.image(
                            f"{mode}-{index:02}-{label}", samples
                        )
                        self.materials(targets)
                    opaque = pictures["opaque"].astype(float)
                    clear = pictures["clear"].astype(float)
                    partial = pictures["07"].astype(float)
                    mask = np.mean(np.abs(opaque - clear), axis=2) > 5
                    changed = int(mask.sum())
                    to_opaque = (
                        float(np.mean(np.abs(partial[mask] - opaque[mask])))
                        if changed
                        else 0
                    )
                    to_clear = (
                        float(np.mean(np.abs(partial[mask] - clear[mask])))
                        if changed
                        else 0
                    )
                    if changed < 100 or min(to_clear, to_opaque) < 0.8:
                        raise AssertionError(
                            f"Opacity has no visible intermediate result: {prim.GetPath()}, {changed}, {to_opaque}, {to_clear}"
                        )
                    self.comparisons.append(
                        dict(
                            mode=mode,
                            prim=str(prim.GetPath()),
                            pixels=changed,
                            partial_to_opaque=to_opaque,
                            partial_to_clear=to_clear,
                        )
                    )
                    previous_target = prim
        finally:
            model.viewSettings.cameraPrim = None
            session.ImportFromString(original)
            stage.SetEditTarget(edit_target)
        if (
            hashlib.sha256(open(root.realPath, "rb").read()).hexdigest()
            != digest
        ):
            raise AssertionError("Source USD changed")
        self.launcher.test_result = dict(
            opacity_verified=True,
            targets=[str(p.GetPath()) for p in targets],
            frames=self.frames,
            material_checks=self.material_checks,
            comparisons=self.comparisons,
            usd_unchanged=True,
            source_sha256=digest,
            module_handles=self.launcher.runtime.module_handles(),
            same_process=True,
            opacity_edit_mesh_resubmissions=0,
        )
