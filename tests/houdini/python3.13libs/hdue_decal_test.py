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
        yield from self.capture(
            "01-decal-solaris.png"
        )  # with the decal: the capture checks for red pixels
        display = viewer.pwd().displayNode()
        hide = display.parent().createNode("pythonscript", "hide_decal")
        hide.setInput(0, display)
        hide.parm("python").set(
            "from pxr import UsdGeom\nUsdGeom.Imageable(hou.pwd().editableStage().GetPrimAtPath('/Decal')).MakeInvisible()"
        )
        hide.setDisplayFlag(True)
        plain = yield from self.image(
            "02-no-decal", 0, self.frames[-1]["generation"]
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
