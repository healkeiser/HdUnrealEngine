"""Real GPU gates: the Path Tracer's virtual texture warm-up. No samples are reported before its pages settle (the
warm-up draws are discarded), nothing is locked, a page budget fails without samples and recovers, a timeout starts the
Path Tracer with what is resident, and an edit during the warm-up starts a new one.
"""

import hashlib
import time
from pathlib import Path

from pxr import Usd, UsdShade
from residency_api import ResidencyApi
from usdview_test import PLUGIN, SmokeTest


class ResidencyTest(SmokeTest):
    def __init__(self, *args):
        super().__init__(*args)
        self.deadline = time.monotonic() + 600
        self.progress = []
        self.last_progress = None

    def stats(self):
        stats = super().stats()
        phase = stats.get("vtResidencyPhase", 0)
        if phase == 1 and stats["completedSamples"]:
            raise AssertionError(
                f"Path Tracer reported samples during the warm-up: {stats}"
            )
        if stats.get("vtLockedTiles"):
            raise AssertionError(f"Virtual texture tiles were locked: {stats}")
        value = {
            key: stats[key]
            for key in (
                "generation",
                "completedSamples",
                "vtResidencyPhase",
                "vtResidencyGeneration",
                "vtTextures",
                "vtRequiredPages",
                "vtResidentPages",
                "vtReadyMappings",
                "vtPreparationFrames",
                "vtPreparationMs",
            )
        }
        if value != self.last_progress:
            self.progress.append(value)
            self.last_progress = value
        return stats

    def wait_preparing(self, previous):
        self.view.updateGL()
        while True:
            value = self.stats()
            if (
                value["generation"] > previous
                and value["vtResidencyPhase"] == 1
            ):
                return value
            yield

    def image(self, label, previous=0, samples=64):
        return (
            yield from self.wait_frame(
                label, previous, samples, require_red=False
            )
        )

    def run(self):
        model = self.controller._dataModel
        stage = model.stage
        root, session = stage.GetRootLayer(), stage.GetSessionLayer()
        source_hash = hashlib.sha256(
            Path(root.realPath).read_bytes()
        ).hexdigest()
        original_session = session.ExportToString()
        edit_target = stage.GetEditTarget()
        model.viewSettings.showHUD = False
        model.viewSettings.showBBoxes = False
        model.viewSettings.cameraPrim = stage.GetPrimAtPath("/Camera")
        self.controller._mainWindow.resize(1600, 1000)
        self.view.setFixedSize(1280, 720)
        yield
        stage.SetEditTarget(session)
        failures = []
        try:
            self.launcher.select_mode("pathtraced")
            cold = yield from self.wait_preparing(0)
            api = ResidencyApi(self.launcher.runtime)
            # A warm-up cut short by its timeout starts the Path Tracer with the pages it has.
            api.configure(cold["sessionHandle"], timeout_seconds=0.000001)
            model.currentFrame = Usd.TimeCode(model.currentFrame.GetValue() + 1)
            timed_out = yield from self.image(
                "timeout-starts", cold["generation"]
            )

            # Scene calls after the request (a frame's ApplyScene) release the warm-up's status once it is ready, so
            # the checks read the progress recorded while it ran.
            def warm_up(generation):
                return [
                    p
                    for p in self.progress
                    if p["vtResidencyGeneration"] == generation
                    and p["vtResidencyPhase"] == 2
                ]

            first_stats = self.stats()
            if first_stats["error"] or not warm_up(first_stats["generation"]):
                raise AssertionError(
                    f"A warm-up timeout did not start the Path Tracer: {first_stats}"
                )
            api.configure(first_stats["sessionHandle"])
            model.currentFrame = Usd.TimeCode(model.currentFrame.GetValue() + 1)
            settled = yield from self.image(
                "large-udim-64", first_stats["generation"]
            )
            first = self.stats()
            ready = warm_up(first["generation"])
            if not any(
                p["vtTextures"] >= 2
                and p["vtResidentPages"]
                and p["vtPreparationFrames"] >= 5
                for p in ready
            ):
                raise AssertionError(
                    f"Fixture did not exercise the warm-up: {first} {ready}"
                )
            previous = first["generation"]
            model.currentFrame = Usd.TimeCode(model.currentFrame.GetValue() + 1)
            preparing = yield from self.wait_preparing(previous)
            cancelled = preparing["generation"]
            # Cancel during the warm-up through a real material edit; the next generation warms up again and starts
            # accumulation at zero.
            UsdShade.Shader(
                stage.GetPrimAtPath("/Looks/UDIM_Normal/Normal")
            ).GetInput("scale").Set(0.9)
            yield from self.image("edited-during-preparation", cancelled)
            before = self.stats()["generation"]
            api.configure(self.stats()["sessionHandle"], 1, 120)
            model.currentFrame = Usd.TimeCode(model.currentFrame.GetValue() + 1)
            self.view.updateGL()
            while True:
                raw = self.view._renderer.GetRenderStats()
                if raw["generation"] > before and raw["vtResidencyPhase"] == 3:
                    break
                yield
            if raw["completedSamples"] or "budget exceeded" not in raw["error"]:
                raise AssertionError(
                    f"Residency failure started PT or lost diagnostics: {raw}"
                )
            failures.append(dict(kind="budget", **raw))
            api.configure(raw["sessionHandle"])
            model.currentFrame = Usd.TimeCode(model.currentFrame.GetValue() + 1)
            yield from self.image("budget-recovered", raw["generation"])
            before = self.stats()["generation"]
            self.launcher.select_mode("realtime")
            yield from self.image("realtime-ends-warm-up", before, samples=0)
            if self.stats()["vtResidencyPhase"]:
                raise AssertionError("Realtime kept the Path Tracer's warm-up")
            self.switch("HdStormRendererPlugin")
            for _ in range(4):
                yield
            self.switch(PLUGIN)
            self.launcher.select_mode("pathtraced")
            yield from self.image("recreated-64")
        finally:
            session.ImportFromString(original_session)
            stage.SetEditTarget(edit_target)
        if (
            hashlib.sha256(Path(root.realPath).read_bytes()).hexdigest()
            != source_hash
        ):
            raise AssertionError("Source changed")
        if not any(
            p["vtResidencyPhase"] == 1 and p["completedSamples"] == 0
            for p in self.progress
        ):
            raise AssertionError("No warm-up was observed")
        self.launcher.test_result = dict(
            residency_verified=True,
            frames=self.frames,
            progress=self.progress,
            cancelled_generation=cancelled,
            failures=failures,
            timed_out_mean=float(timed_out.mean()),
            settled_mean=float(settled.mean()),
            source_unchanged=True,
            same_process=True,
            module_handles=self.launcher.runtime.module_handles(),
        )
