"""A MaterialX graph hdUE cannot translate at render time, prepared live in the real Solaris viewport
(samples/materialx/procedural.usda, Launch-Houdini.ps1 -TestKind materialx-live): after an edit of a value UE's import
bakes in (the checkers' frequency, new each run so no earlier preparation applies), the viewport keeps rendering the
translation without an error while UE's MaterialX importer prepares the graph in the background, then shows the
imported red and grey checkers.
"""

import time

import hdefereval
import hdue_houdini as integration
from hdue_houdini_test import Test as ViewportTest

_test = None
MATERIAL = "/World/Looks/Checkers"


def red_fraction(rgb):
    return float(
        (
            (rgb[..., 0] > 3 * rgb[..., 1])
            & (rgb[..., 0] > 0.05 * rgb[..., 0].max())
        ).mean()
    )


class Test(ViewportTest):
    def run(self):
        viewer = integration.scene_viewer()
        for _ in range(30):
            yield
        # Through the sample's camera, the card fills the middle of the frame (a free view shows it small).
        hdefereval.execute_deferred(
            lambda: viewer.curViewport().setCamera("/Camera")
        )
        while not self.host.prepared_materials.applied:
            if self.host.material_error:
                raise ValueError(self.host.material_error)
            yield
        display = viewer.pwd().displayNode()
        edit = display.parent().createNode("pythonscript", "new_frequency")
        edit.setInput(0, display)
        frequency = 3 + (int(time.time()) % 1000) / 1000
        edit.parm("python").set(
            "from pxr import Gf\nhou.pwd().editableStage().GetAttributeAtPath("
            f"'{MATERIAL}/Checks.inputs:freq').Set(Gf.Vec2f({frequency}, {frequency}))"
        )
        edit.setDisplayFlag(True)
        live = self.host.live_preparation
        started = live.started
        deadline = time.monotonic() + 600
        prepared = self.host.prepared_materials
        while True:
            if self.host.material_error:
                raise AssertionError(
                    "The viewport reported an error while the graph prepared: "
                    + self.host.material_error
                )
            if live.phase == "failed":
                raise AssertionError(live.message)
            asset = prepared.settings.get("nativeMaterialAssets", {}).get(
                MATERIAL, ""
            )
            if live.started > started and "/MX/" in asset and prepared.applied:
                break
            if time.monotonic() > deadline:
                raise AssertionError(
                    f"The edited MaterialX graph was not prepared: {live.status()}"
                )
            yield
        image = yield from self.image(
            "01-imported",
            0,
            self.frames[-1]["generation"] if self.frames else 0,
        )
        yield from self.capture("01-imported-solaris.png")
        fraction = red_fraction(image)
        self.stats = dict(
            asset=asset,
            frequency=frequency,
            red=fraction,
            jobs=live.started - started,
        )
        if not 0.15 < fraction < 0.45:
            raise AssertionError(
                f"The imported checkers do not show: {fraction} of the frame red"
            )


def start(host):
    global _test
    _test = Test(host)
