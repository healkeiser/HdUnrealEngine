"""Heavy-scene viewport run (Launch-Houdini.ps1 -TestKind heavy): the loaded stage in render purpose, until the first
converged realtime frame (every background mesh build swapped in), then through HDUE_TEST_CAMERA until it converges
again. Writes heavy.json with the times and frame states; the caller measures memory (a job memory limit and a commit
timeline). See docs/architecture.md, "Memory, GPU and disk".
"""

import json
import os
import time

import hdefereval
import hdue_houdini as integration
from hdue_houdini_test import Test as ViewportTest
from hdue_houdini_test import flipbook

_test = None


class Test(ViewportTest):
    def __init__(self, host):
        # Render purpose and no proxy before the first draw, whatever the user's viewport preferences say (hdUE's
        # defaults, UsdRenderers.json defaultpurposes, are default and render).
        viewer = integration.scene_viewer()
        viewer.showRenderPurpose(True)
        viewer.showProxyPurpose(False)
        self.started = time.monotonic()
        super().__init__(host)
        self.deadline = self.started + float(
            os.environ.get("HDUE_HEAVY_TIMEOUT", "3000")
        )

    def run(self):
        report = {}
        yield from self.image("first", 0)
        report["first_converged_seconds"] = time.monotonic() - self.started
        report["first"] = self.frames[-1]
        yield from self.capture("first.png")
        # The loader frames the whole stage once it is open; the scene camera comes after it.
        camera = os.environ.get("HDUE_TEST_CAMERA")
        if camera:
            viewer = integration.scene_viewer()
            hdefereval.execute_deferred(
                lambda: viewer.curViewport().setCamera(camera)
            )
            yield from self.image(
                "camera", 0, previous=report["first"]["generation"]
            )
            report["camera_converged_seconds"] = time.monotonic() - self.started
            report["camera"] = self.frames[-1]
            yield from self.capture("camera.png")
        (self.output / "heavy.json").write_text(
            json.dumps(report, indent=2, default=str), encoding="utf-8"
        )

    def capture(self, name):
        done = yield from flipbook(self.output / name)
        while not done:
            yield


def start(host):
    global _test
    _test = Test(host)
