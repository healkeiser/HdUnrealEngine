"""An UnrealDecal with a prepared Substrate Builder decal material in the real Solaris viewport
(samples/decal/decal.usda, Launch-Houdini.ps1 -TestKind decal -SubstrateLibrary ...): it paints the middle of the wall
red, and hiding the prim removes it.
"""

import hdue_houdini as integration
from hdue_houdini_test import Test as ViewportTest

_test = None


def red(rgb):
    """Pixels the red decal paints: red at least three times green, on a lit surface. Independent of the framing (a
    portrait viewport puts the wall anywhere).
    """
    return (rgb[..., 0] > 3 * rgb[..., 1]) & (
        rgb[..., 0] > 0.05 * rgb[..., 0].max()
    )


class Test(ViewportTest):
    def run(self):
        viewer = integration.scene_viewer()
        for _ in range(30):
            yield
        while not self.host.prepared_materials.applied:
            if self.host.material_error:
                raise ValueError(self.host.material_error)
            yield
        for _ in range(20):
            yield
        decal = yield from self.image(
            "01-decal", 0, self.frames[-1]["generation"] if self.frames else 0
        )
        # With the decal: the capture checks for red pixels.
        yield from self.capture("01-decal-solaris.png")
        # A live edit of the decal graph's Slab color (a generated material parameter) repaints it without recompiling.
        display = viewer.pwd().displayNode()
        green = display.parent().createNode("pythonscript", "green_paint")
        green.setInput(0, display)
        green.parm("python").set(
            "from pxr import Gf\nhou.pwd().editableStage().GetPrimAtPath('/Looks/Paint/Graph/paint')"
            ".GetAttribute('inputs:diffuse_albedo').Set(Gf.Vec3f(0.05, 0.8, 0.05))"
        )
        green.setDisplayFlag(True)
        recolored = yield from self.image(
            "02-decal-green", 0, self.frames[-1]["generation"]
        )
        painted_green = float(
            (recolored[..., 1] > 3 * recolored[..., 0]).mean()
        )
        self.frames[-1]["green"] = painted_green
        if not (
            painted_green > 0.002 and float(red(recolored).mean()) < 0.0005
        ):
            raise AssertionError(
                f"The decal's live color edit did not repaint it: {painted_green} green"
            )
        green.destroy()
        display.setDisplayFlag(True)
        yield from self.image(
            "03-decal-red-again", 0, self.frames[-1]["generation"]
        )
        hide = display.parent().createNode("pythonscript", "hide_decal")
        hide.setInput(0, display)
        hide.parm("python").set(
            "from pxr import UsdGeom\nUsdGeom.Imageable(hou.pwd().editableStage().GetPrimAtPath('/Decal')).MakeInvisible()"
        )
        hide.setDisplayFlag(True)
        plain = yield from self.image(
            "04-no-decal", 0, self.frames[-1]["generation"]
        )
        painted, bare = float(red(decal).mean()), float(red(plain).mean())
        # Elsewhere the two frames match: the decal stays inside its box.
        outside = ~red(decal)
        drift = float(
            abs(decal[outside].mean() - plain[outside].mean())
            / max(plain[outside].mean(), 1e-6)
        )
        self.frames[-1]["decal"] = dict(painted=painted, bare=bare, drift=drift)
        if not (painted > 0.002 and bare < painted / 20):
            raise AssertionError(
                f"The decal did not paint the wall red: {painted} of the frame against {bare}"
            )
        if drift > 0.02:
            raise AssertionError(
                f"The decal changed the frame outside its box: {drift}"
            )
        self.stats = dict(painted=painted, bare=bare, drift=drift)


def start(host):
    global _test
    _test = Test(host)
