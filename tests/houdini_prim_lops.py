"""hdUE's create/edit prim LOPs in hython: python tests/houdini_prim_lops.py (restarts itself in hython with the
integration and the delegate's schemas registered, as scripts/build_render_settings_hda.py does).
"""

import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

if not os.environ.get("HDUE_BUILD_HDA"):
    import build_render_settings_hda

    build_render_settings_hda.__file__ = (
        __file__  # relaunch this test, with the builder's environment
    )
    build_render_settings_hda.relaunch()

import hou
import loputils
from pxr import Gf, UsdGeom

FOG = "unreal:localFogVolume:"


def encode(attribute):
    return hou.text.encode(attribute)


class LocalFogVolume(unittest.TestCase):
    def setUp(self):
        hou.hipFile.clear(suppress_save_prompt=True)
        self.create = hou.node("/stage").createNode(
            "hdue::hduelocalfogvolume::2.0", "fog"
        )

    def test_create(self):
        node = self.create
        node.parmTuple("t").set((1, 2, 3))
        node.parm("scale").set(5)
        node.parm(encode(FOG + "RadialFogExtinction")).set(0.5)
        prim = node.stage().GetPrimAtPath("/fog/fog")
        self.assertEqual(prim.GetTypeName(), "UnrealLocalFogVolume")
        self.assertAlmostEqual(
            prim.GetAttribute(FOG + "RadialFogExtinction").Get(), 0.5
        )
        # Every property is authored in Create mode (the viewport is not told about attributes added later).
        self.assertTrue(
            prim.GetAttribute(FOG + "HeightFogExtinction").IsAuthored()
        )
        matrix = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(0)
        self.assertEqual(matrix.ExtractTranslation(), Gf.Vec3d(1, 2, 3))
        self.assertAlmostEqual(
            matrix.GetRow3(0).GetLength(), 5.0
        )  # Uniform Scale is the radius
        guide = UsdGeom.Sphere(node.stage().GetPrimAtPath("/fog/fog/guide"))
        self.assertEqual(guide.GetPurposeAttr().Get(), UsdGeom.Tokens.guide)
        self.assertFalse(node.errors())

    def test_edit(self):
        edit = hou.node("/stage").createNode(
            "hdue::hduelocalfogvolume::2.0", "edit"
        )
        edit.setInput(0, self.create)
        edit.parm("createprims").set(0)  # Edit
        edit.parm("primpattern").set("/fog/fog")
        self.create.parm(encode(FOG + "RadialFogExtinction")).set(0.25)
        # Initialize Parameters, as its menu runs it: every control Do Nothing, then the values read from the prim.
        loputils.initializeParameters(edit, "setdonothing")
        loputils.initializeParameters(edit, "initdonothingfromprim")
        self.assertEqual(
            edit.parm(encode(FOG + "RadialFogExtinction_control")).eval(),
            "none",
        )
        self.assertAlmostEqual(
            edit.parm(encode(FOG + "RadialFogExtinction")).eval(), 0.25
        )
        edit.parm(encode(FOG + "HeightFogExtinction_control")).set("set")
        edit.parm(encode(FOG + "HeightFogExtinction")).set(0.75)
        stage = edit.stage()
        prim = stage.GetPrimAtPath("/fog/fog")
        self.assertAlmostEqual(
            prim.GetAttribute(FOG + "HeightFogExtinction").Get(), 0.75
        )
        self.assertAlmostEqual(
            prim.GetAttribute(FOG + "RadialFogExtinction").Get(), 0.25
        )
        # Edit mode adds no prim and leaves the transform alone.
        self.assertEqual(
            UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(0),
            UsdGeom.Xformable(
                self.create.stage().GetPrimAtPath("/fog/fog")
            ).ComputeLocalToWorldTransform(0),
        )
        self.assertFalse(edit.errors())

    def test_default_edit_keeps_transform(self):
        """An Edit node left at its defaults (no Initialize Parameters) must not move or grow the prim."""
        self.create.parmTuple("t").set((3, 0, 0))
        self.create.parm("scale").set(5)
        previous = self.create
        for i in range(2):
            edit = hou.node("/stage").createNode(
                "hdue::hduelocalfogvolume::2.0", f"edit{i}"
            )
            edit.setInput(0, previous)
            edit.parm("createprims").set(0)
            edit.parm("primpattern").set("/fog/fog")
            previous = edit
        matrix = lambda node: UsdGeom.Xformable(  # noqa: E731
            node.stage().GetPrimAtPath("/fog/fog")
        ).ComputeLocalToWorldTransform(0)
        self.assertTrue(Gf.IsClose(matrix(previous), matrix(self.create), 1e-6))
        self.assertFalse(previous.stage().GetPrimAtPath("/fog/fog/guide/guide"))

    def test_force_edit(self):
        """Force Edit edits a prim marked houdini:editable = false and creates nothing (no guide, no missing prim)."""
        lock = hou.node("/stage").createNode("configureprimitive", "lock")
        lock.setInput(0, self.create)
        lock.parm("primpattern").set("/fog/fog")
        lock.parm("seteditable").set(True)
        lock.parm("editable").set(False)
        force = hou.node("/stage").createNode(
            "hdue::hduelocalfogvolume::2.0", "force"
        )
        force.setInput(0, lock)
        force.parm("createprims").set(2)  # Force Edit
        force.parm("primpattern").set("/fog/fog /fog/missing")
        force.parm(encode(FOG + "RadialFogExtinction")).set(0.125)
        stage = force.stage()
        self.assertAlmostEqual(
            stage.GetPrimAtPath("/fog/fog")
            .GetAttribute(FOG + "RadialFogExtinction")
            .Get(),
            0.125,
        )
        self.assertFalse(stage.GetPrimAtPath("/fog/missing"))

    def test_handles(self):
        """Sphere's transform handles are bound to the asset's transform parameters."""
        script = (
            hou.nodeType(
                hou.lopNodeTypeCategory(), "hdue::hduelocalfogvolume::2.0"
            )
            .definition()
            .sections()["DialogScript"]
            .contents()
        )
        self.assertIn('bindhandle tx lopxform "Transformer" tx ""', script)
        self.assertIn(
            'bindhandle scale lopxform "Transformer" uniform_scale ""', script
        )

    def test_edit_skips_other_types(self):
        cube = hou.node("/stage").createNode("cube", "cube")
        edit = hou.node("/stage").createNode(
            "hdue::hduelocalfogvolume::2.0", "edit"
        )
        edit.setInput(0, cube)
        edit.parm("createprims").set(0)
        edit.parm("primpattern").set("/cube")
        edit.cook(force=True)
        self.assertFalse(
            edit.stage()
            .GetPrimAtPath("/cube")
            .HasAttribute(FOG + "RadialFogExtinction")
        )

    def test_legacy_hidden(self):
        """OPcustomize keeps 1.0 out of the TAB menu."""
        self.assertTrue(
            hou.nodeType(
                hou.lopNodeTypeCategory(), "hdue::hduelocalfogvolume::1.0"
            ).hidden()
        )
        self.assertFalse(
            hou.nodeType(
                hou.lopNodeTypeCategory(), "hdue::hduelocalfogvolume::2.0"
            ).hidden()
        )

    def test_same_prim_as_legacy(self):
        """2.0 authors what 1.0 did for the same inputs: type, every UE property, and the world matrix."""
        legacy = hou.node("/stage").createNode(
            "hdue::hduelocalfogvolume::1.0", "old"
        )
        legacy.parmTuple("t").set((1, 2, 3))
        legacy.parmTuple("r").set((10, 20, 30))
        legacy.parm("radius").set(4)
        self.create.parm("primpath").set("/fog/localfogvolume")
        self.create.parmTuple("t").set((1, 2, 3))
        self.create.parmTuple("r").set((10, 20, 30))
        self.create.parm("scale").set(4)
        old = legacy.stage().GetPrimAtPath("/fog/localfogvolume")
        new = self.create.stage().GetPrimAtPath("/fog/localfogvolume")
        self.assertEqual(old.GetTypeName(), new.GetTypeName())
        names = [
            a.GetName()
            for a in old.GetAttributes()
            if a.GetName().startswith(FOG)
        ]
        self.assertTrue(names)
        for name in names:
            self.assertEqual(
                old.GetAttribute(name).Get(), new.GetAttribute(name).Get(), name
            )
        self.assertTrue(
            Gf.IsClose(
                UsdGeom.Xformable(old).ComputeLocalToWorldTransform(0),
                UsdGeom.Xformable(new).ComputeLocalToWorldTransform(0),
                1e-5,
            )
        )


class PostProcessVolume(unittest.TestCase):
    TYPE = "hdue::hduepostprocessvolume::1.0"
    BIAS = "unreal:postProcess:AutoExposureBias"

    def setUp(self):
        hou.hipFile.clear(suppress_save_prompt=True)
        self.create = hou.node("/stage").createNode(self.TYPE, "volume")

    def test_create(self):
        node = self.create
        node.parm(encode(self.BIAS + "_control")).set("set")
        node.parm(encode(self.BIAS)).set(-1)
        node.parm(encode("unreal:postProcessVolume:Priority")).set(2)
        prim = node.stage().GetPrimAtPath("/postprocess/volume")
        self.assertEqual(prim.GetTypeName(), "UnrealPostProcessVolume")
        self.assertAlmostEqual(prim.GetAttribute(self.BIAS).Get(), -1)
        self.assertAlmostEqual(
            prim.GetAttribute("unreal:postProcessVolume:Priority").Get(), 2
        )
        # APostProcessVolume's defaults are authored, and are UE's.
        self.assertFalse(
            prim.GetAttribute("unreal:postProcessVolume:bUnbound").Get()
        )
        self.assertAlmostEqual(
            prim.GetAttribute("unreal:postProcessVolume:BlendRadius").Get(), 100
        )
        # Post-process settings start at Do Nothing: only the ones turned on override, as UE's bOverride_ flags.
        authored = [
            a.GetName()
            for a in prim.GetAttributes()
            if a.GetName().startswith("unreal:postProcess:") and a.IsAuthored()
        ]
        self.assertEqual(authored, [self.BIAS])
        guide = UsdGeom.Cube(
            node.stage().GetPrimAtPath("/postprocess/volume/guide")
        )
        self.assertEqual(guide.GetSizeAttr().Get(), 2)
        self.assertEqual(guide.GetPurposeAttr().Get(), UsdGeom.Tokens.guide)
        self.assertFalse(node.errors())

    def test_no_render_settings_conditions(self):
        """The render settings' Essential | Advanced and render-mode parameters do not exist here."""
        for template in self.create.parmTemplateGroup().entriesWithoutFolders():
            for condition in template.conditionals().values():
                for word in (
                    "hdue_show",
                    "xn__unrealrenderMode",
                    "xn__unrealtonemapper",
                ):
                    self.assertNotIn(word, condition, template.name())

    def test_edit(self):
        edit = hou.node("/stage").createNode(self.TYPE, "edit")
        edit.setInput(0, self.create)
        edit.parm("createprims").set(0)
        edit.parm("primpattern").set("/postprocess/volume")
        edit.parm(encode(self.BIAS + "_control")).set("set")
        edit.parm(encode(self.BIAS)).set(1)
        prim = edit.stage().GetPrimAtPath("/postprocess/volume")
        self.assertAlmostEqual(prim.GetAttribute(self.BIAS).Get(), 1)
        self.assertFalse(edit.stage().GetPrimAtPath("/postprocess/edit"))


class SceneComponents(unittest.TestCase):
    """hdUE Height Fog, Sky Atmosphere and Volumetric Cloud: their prim, with UE's defaults as the schema's fallbacks."""

    NODES = (
        (
            "hdue::hdueheightfog::1.0",
            "UnrealExponentialHeightFog",
            "unreal:heightFog:FogDensity",
            0.02,
        ),
        (
            "hdue::hdueskyatmosphere::1.0",
            "UnrealSkyAtmosphere",
            "unreal:skyAtmosphere:BottomRadius",
            6360,
        ),
        (
            "hdue::hduevolumetriccloud::1.0",
            "UnrealVolumetricCloud",
            "unreal:volumetricCloud:LayerBottomAltitude",
            5,
        ),
    )

    def test_create_and_set(self):
        for node_type, prim_type, attribute, default in self.NODES:
            with self.subTest(node_type):
                hou.hipFile.clear(suppress_save_prompt=True)
                node = hou.node("/stage").createNode(node_type, "component")
                prim = node.stage().GetPrimAtPath("/environment/component")
                self.assertEqual(prim.GetTypeName(), prim_type)
                # Do Nothing by default: the schema's fallback, UE's constructor value, applies.
                self.assertFalse(prim.GetAttribute(attribute).IsAuthored())
                self.assertAlmostEqual(
                    prim.GetAttribute(attribute).Get(), default, places=5
                )
                self.assertAlmostEqual(
                    node.parm(encode(attribute)).eval(), default, places=5
                )
                node.parm(encode(attribute + "_control")).set("set")
                node.parm(encode(attribute)).set(default * 0.5)
                prim = node.stage().GetPrimAtPath("/environment/component")
                self.assertAlmostEqual(
                    prim.GetAttribute(attribute).Get(), default * 0.5, places=4
                )
                self.assertFalse(node.errors())


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() else 1)
