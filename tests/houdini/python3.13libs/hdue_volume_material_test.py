"""A prepared Substrate Builder volume material on a Volume prim in the real Solaris viewport
(samples/volume_material/volume_material.usda, Launch-Houdini.ps1 -TestKind volume-material -SubstrateLibrary ...): with
every input unconnected it renders as hdUE's default volume material (M_HdUEVolume) does, and its albedo, a generated
material parameter, edits live.
"""

import hdue_houdini as integration
import numpy as np
from hdue_houdini_test import Test as ViewportTest

_test = None


class Test(ViewportTest):
    def run(self):
        import hdefereval

        viewer = integration.scene_viewer()
        for _ in range(30):
            yield
        hdefereval.execute_deferred(viewer.curViewport().frameAll)
        while not self.host.prepared_materials.applied:
            if self.host.material_error:
                raise ValueError(self.host.material_error)
            yield
        for _ in range(20):
            yield

        def render(label):
            previous = self.frames[-1]["generation"] if self.frames else 0
            rgb = yield from self.image(label, 0, previous)
            mean = rgb.reshape(-1, 3).mean(axis=0)
            self.frames[-1]["mean_rgb"] = mean.tolist()
            return mean

        def edit(name, code):
            """A Python Script LOP after the displayed node, displayed."""
            display = viewer.pwd().displayNode()
            node = display.parent().createNode("pythonscript", name)
            node.setInput(0, display)
            node.parm("python").set(
                "from pxr import Gf, UsdShade\nstage = hou.pwd().editableStage()\n"
                + code
            )
            node.setDisplayFlag(True)
            return node

        bound = yield from render("01-graph-material")
        unbind = edit(
            "unbind",
            "UsdShade.MaterialBindingAPI(stage.GetPrimAtPath('/Fog')).UnbindAllBindings()",
        )
        default = yield from render("02-default-volume-material")
        # The graph's defaults are M_HdUEVolume's: the same image (to the realtime renderer's noise).
        if np.abs(bound - default).max() > 0.02 * max(default.max(), 1e-6):
            raise AssertionError(
                f"The default volume graph differs from M_HdUEVolume: {bound} {default}"
            )
        display = unbind.inputs()[0]
        unbind.destroy()
        display.setDisplayFlag(True)
        yield from render("03-graph-material-again")
        edit(
            "red_albedo",
            "stage.GetPrimAtPath('/Looks/Smoke/Graph/volume').GetAttribute('inputs:albedo').Set(Gf.Vec3f(0.8, 0.05, 0.05))",
        )
        red = yield from render("04-red-albedo")
        # The wall fills most of the frame and the fog is thin: a red albedo lowers green against red by a few
        # percent of the frame's mean, against about 0.1% from frame to frame.
        if not red[1] / red[0] < 0.98 * bound[1] / bound[0]:
            raise AssertionError(
                f"The volume's albedo did not turn red: {bound} {red}"
            )
        yield from self.capture("04-red-albedo-solaris.png")
        self.stats = dict(
            bound=bound.tolist(), default=default.tolist(), red=red.tolist()
        )


def start(host):
    global _test
    _test = Test(host)
