"""Solaris selection on hdUE (Launch-Houdini -TestKind selection): the native highlight and click picking, with
hdUE:viewportIdAovs at its default.

HDUE_TEST_CAMERA: a camera to look through (default: frame all). HDUE_TEST_SELECT: comma-separated prims to select;
each one is also clicked at the centre of its bounds and must be what the click selects.
"""

import json
import os
import time
import traceback

import hdefereval
import hdue_houdini as integration
import hou
import numpy as np
from hdue_houdini_test import flipbook
from PySide6 import QtCore, QtGui, QtWidgets

_test = None


def _read(path):
    image = QtGui.QImage(str(path)).convertToFormat(
        QtGui.QImage.Format_RGBA8888
    )
    return (
        np.frombuffer(bytes(image.constBits()), dtype=np.uint8)
        .reshape(image.height(), image.bytesPerLine())[:, : image.width() * 4]
        .reshape(image.height(), image.width(), 4)[..., :3]
        .astype(float)
    )


class Selection:
    def __init__(self, host):
        self.host = host
        self.out = host.log_dir
        self.report = dict(passed=False, steps=[])
        self.deadline = time.monotonic() + 240
        self.timer = QtCore.QTimer()
        self.timer.timeout.connect(self.advance)
        self.steps = self.run()
        self.timer.start(50)

    def advance(self):
        try:
            if integration._failure:
                raise RuntimeError(integration._failure)
            if time.monotonic() > self.deadline:
                raise TimeoutError(str(self.host.api.status()))
            next(self.steps)
        except StopIteration:
            self.report["passed"] = True
            self.finish()
        except BaseException:
            (self.out / "test-error.txt").write_text(
                traceback.format_exc(), encoding="utf-8"
            )
            self.finish()

    def finish(self):
        self.timer.stop()
        (self.out / "selection.json").write_text(
            json.dumps(self.report, indent=2, default=str), encoding="utf-8"
        )
        self.host.close()
        hdefereval.execute_deferred(lambda: hou.hscript("quit -f"))

    def log(self, **entry):
        self.report["steps"].append(entry)
        integration._report("selection test: " + repr(entry))

    def wait(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            yield

    def capture(self, name):
        path = self.out / name
        done = yield from flipbook(path)
        viewer = integration.scene_viewer()
        viewer.setRendererPaused(True)
        viewer.setRendererPaused(False)
        while not done:
            yield
        yield from self.wait(0.5)
        return _read(path)

    def run(self):
        from pxr import Usd, UsdGeom

        viewer = integration.scene_viewer()
        yield from self.wait(1.5)
        camera = os.environ.get("HDUE_TEST_CAMERA")
        if camera:
            hdefereval.execute_deferred(
                lambda: viewer.curViewport().setCamera(camera)
            )
        else:
            hdefereval.execute_deferred(viewer.curViewport().frameAll)
        while True:
            s = self.host.api.status()
            if s["error"]:
                raise RuntimeError(s["error"])
            if (
                s["active"]
                and s["frame"]["frame_id"]
                and s["frame"]["converged"]
            ):
                break
            yield
        # The ID passes start once the view has been still for 1 s.
        yield from self.wait(4)
        stage = viewer.stage()
        targets = tuple(os.environ["HDUE_TEST_SELECT"].split(","))
        self.report.update(
            up_axis=str(UsdGeom.GetStageUpAxis(stage)),
            meters_per_unit=UsdGeom.GetStageMetersPerUnit(stage),
            camera=camera,
            targets=targets,
        )
        stable = self.host.api.status()["generation"]
        base = yield from self.capture("selection-before.png")
        viewer.setCurrentSceneGraphSelection(targets)
        viewer.setSceneGraphHighlight(targets)
        yield from self.wait(3)
        selected = yield from self.capture("selection-after.png")
        # Houdini tints the selected prims' primId pixels and outlines them.
        changed = int((np.abs(base - selected).sum(axis=2) > 20).sum())
        self.log(
            step="highlight",
            changed=changed,
            pixels=int(base.shape[0] * base.shape[1]),
        )
        if changed < base.shape[0] * base.shape[1] * 0.001:
            raise AssertionError(
                f"Selecting {targets} drew no highlight ({changed} pixels changed)"
            )
        if self.host.api.status()["generation"] != stable:
            raise AssertionError("Selecting restarted the render")
        viewer.setCurrentSceneGraphSelection(())
        viewer.setSceneGraphHighlight(())
        # Click picking: a left click in the selection state, sent through Qt (no desktop input).
        viewer.setCurrentState("scenegraphselect")
        yield from self.wait(0.5)
        viewport = viewer.curViewport()
        height = viewport.size()[-1]
        widget = QtWidgets.QApplication.widgetAt(
            viewer.qtScreenGeometry().center()
        )
        bounds = UsdGeom.BBoxCache(
            Usd.TimeCode.Default(), ["default", "render"]
        )
        for target in targets:
            centre = bounds.ComputeWorldBound(
                stage.GetPrimAtPath(target)
            ).ComputeCentroid()
            x, y = viewport.mapToScreen(hou.Vector3(*centre))
            viewer.setCurrentSceneGraphSelection(())
            yield from self.wait(0.3)
            pos = QtCore.QPointF(x, height - y)
            for kind, buttons in (
                (QtCore.QEvent.MouseButtonPress, QtCore.Qt.LeftButton),
                (QtCore.QEvent.MouseButtonRelease, QtCore.Qt.NoButton),
            ):
                QtWidgets.QApplication.sendEvent(
                    widget,
                    QtGui.QMouseEvent(
                        kind,
                        pos,
                        widget.mapToGlobal(pos),
                        QtCore.Qt.LeftButton,
                        buttons,
                        QtCore.Qt.NoModifier,
                    ),
                )
            yield from self.wait(1)
            picked = viewer.currentSceneGraphSelection()
            self.log(step="click", target=target, at=(x, y), picked=picked)
            if not picked or not picked[0].startswith(target):
                raise AssertionError(f"Clicking {target} selected {picked}")
        viewer.setCurrentSceneGraphSelection(())
        viewer.enterViewState()


def start(host):
    global _test
    _test = Selection(host)
