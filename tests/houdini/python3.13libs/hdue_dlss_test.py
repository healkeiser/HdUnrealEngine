"""NVIDIA DLSS in the real Solaris viewport (a runtime built with NVIDIA's DLSS plugin, docs/building.md): DLSS and
then Ray Reconstruction on render about the TSR image, NVIDIA's runtime creates the DLSS feature (Ray Reconstruction's
with DenoiseMode=DLSSRR), and both off render the TSR image again.
"""

import json
import os
import time
from pathlib import Path

import hdue_houdini as integration
import numpy as np
from hdue_houdini_test import Test as ViewportTest

_test = None
DLSS = "xn__unrealcvarrNGXDLSSEnable"  # set_render_setting takes the encoded name without its suffix
RAY_RECONSTRUCTION = "xn__unrealcvarrHdUEDLSSRayReconstruction"


def ue_logs_since(start):
    """The runtime project's UE logs written since start (other hosts may share the folder)."""
    profile = json.loads(Path(os.environ["HDUE_RUNTIME_CONFIG"]).read_text())
    logs = Path(profile["runtime_project"]).parent / "Saved" / "Logs"
    return "".join(
        log.read_text(encoding="utf-8", errors="replace")
        for log in logs.glob("*.log")
        if log.stat().st_mtime >= start
    )


class Test(ViewportTest):
    def run(self):
        import hdefereval

        start = time.time()
        viewer = integration.scene_viewer()
        for _ in range(24):
            yield
        hdefereval.execute_deferred(viewer.curViewport().frameAll)
        for _ in range(8):
            yield

        def render(label):
            previous = self.frames[-1]["generation"] if self.frames else 0
            rgb = yield from self.image(label, 0, previous)
            mean = rgb.reshape(-1, 3).mean(axis=0)
            self.frames[-1]["mean_rgb"] = mean.tolist()
            return mean

        base = yield from render("01-tsr")
        integration.set_render_setting(DLSS, 1)
        dlss = yield from render("02-dlss")
        integration.set_render_setting(RAY_RECONSTRUCTION, 1)
        ray_reconstruction = yield from render("03-dlss-rr")
        integration.set_render_setting(RAY_RECONSTRUCTION, 0)
        integration.set_render_setting(DLSS, 0)
        off = yield from render("04-tsr-again")
        for label, mean, tolerance in (
            ("DLSS", dlss, 0.02),
            ("Ray Reconstruction", ray_reconstruction, 0.03),
            ("DLSS off", off, 0.005),
        ):
            if np.abs(mean - base).max() > tolerance:
                raise AssertionError(f"{label} differs from TSR: {base} {mean}")
        log = ue_logs_since(start)
        if "Creating   NGX DLSS Feature" not in log:
            raise AssertionError("No DLSS feature was created (UE log)")
        if "DenoiseMode=DLSSRR" not in log:
            raise AssertionError(
                "No Ray Reconstruction feature was created (UE log)"
            )


def start(host):
    global _test
    _test = Test(host)
