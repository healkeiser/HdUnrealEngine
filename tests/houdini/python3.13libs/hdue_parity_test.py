"""Viewport/husk parity capture: the loaded stage through HDUE_TEST_CAMERA in the real Solaris viewport, tonemapper on.
Writes the converged beauty as the delegate holds it (viewport.pfm, what husk writes for the same stage) and Houdini's
flipbook of the viewport with its OCIO display on (viewport.png) and off (viewport-noocio.png). See docs/rendering.md,
"Viewport and husk".
"""

import os
import time

import hdefereval
import hdue_houdini as integration
from hdue_houdini_test import Test as ViewportTest
from hdue_houdini_test import flipbook
from hdue_viewer import write_pfm

_test = None


class Test(ViewportTest):
    def run(self):
        viewer = integration.scene_viewer()
        yield from self.wait(1.5)
        hdefereval.execute_deferred(
            lambda: viewer.curViewport().setCamera(
                os.environ["HDUE_TEST_CAMERA"]
            )
        )
        # The loader's hdUE Render Settings LOP is the stage's render settings; author what the scene's own prim does.
        integration.set_render_setting("xn__unrealtonemapper", 1)
        yield from self.wait(1)
        yield from self.image("parity", 0)
        # Lumen keeps converging after the first converged frame; take the frame after it has been still.
        yield from self.wait(5)
        rgb = yield from self.image("parity-settled", 0)
        write_pfm(self.output / "viewport.pfm", rgb)
        integration._report(
            f"parity display: {viewer.getOCIODisplay()} / {viewer.getOCIOView()}"
        )
        yield from self.capture("viewport.png")
        viewer.setUsingOCIO(False)
        yield from self.wait(2)
        yield from self.capture("viewport-noocio.png")
        viewer.setUsingOCIO(True)

    def wait(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            yield

    def capture(self, name):
        done = yield from flipbook(self.output / name)
        self.update()
        while not done:
            yield


def start(host):
    global _test
    _test = Test(host)
