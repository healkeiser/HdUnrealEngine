"""Integration test driven by Houdini's real Qt/Solaris event loop."""

import ctypes as C
import json
import math
import time
import traceback

import hdue_houdini as integration
import hou
import numpy as np
from hdue_viewer import write_pfm
from PySide6 import QtCore

_test = None


def flipbook(path):
    """Capture the Solaris compositor to path with a native flipbook (QWidget.grab must not re-enter Houdini's
    Vulkan/GL viewport painting); yields until the file exists, then returns a list that fills when the flipbook ends.
    """
    import hdefereval

    viewer = integration.scene_viewer()
    done = []

    def capture():
        settings = viewer.flipbookSettings().stash()
        settings.frameRange((hou.frame(), hou.frame()))
        settings.outputToMPlay(False)
        settings.beautyPassOnly(False)
        settings.useResolution(False)
        settings.output(str(path))
        viewer.flipbook(viewer.curViewport(), settings, open_dialog=False)
        done.append(True)

    hdefereval.execute_deferred(capture)
    while not path.is_file():
        yield
    return done


class Test:
    def __init__(self, host):
        self.host = host
        self.output = host.log_dir
        self.frames = []
        self.capture_errors = []
        self.deadline = time.monotonic() + 300
        self.captures = []
        self.selection_overlay = False
        self.api = host.api
        self.timer = QtCore.QTimer()
        self.timer.timeout.connect(self.advance)
        self.steps = self.run()
        self.timer.start(25)
        self.last_status = 0
        self.verified = None

    def advance(self):
        try:
            if time.monotonic() > self.deadline:
                raise TimeoutError(
                    "Solaris test timed out: " + repr(self.api.status())
                )
            if integration._failure:
                raise RuntimeError(integration._failure)
            current = integration.scene_viewer().currentNode()
            if current and current.errors():
                raise RuntimeError(repr(current.errors()))
            if time.monotonic() - self.last_status > 2:
                self.last_status = time.monotonic()
                (self.output / "viewport-state.json").write_text(
                    json.dumps(self.api.status(), indent=2), encoding="utf-8"
                )
                self.host.write_state()
            next(self.steps)
        except StopIteration:
            self.timer.stop()
            self.verified = dict(
                passed=not self.capture_errors,
                presentation="native Solaris viewport",
                native_solaris_compositor_verified=True,
                native_selection_overlay_verified=self.selection_overlay,
                up_axis="Z",
                meters_per_unit=0.01,
                captures=self.captures,
                capture_errors=self.capture_errors,
                frames=self.frames,
                pid=__import__("os").getpid(),
                houdini=hou.applicationVersionString(),
                delegate_module=self.api.library._handle,
                backend_module=self.host.runtime.library._handle,
            )
            self.finish()
        except BaseException:
            self.timer.stop()
            error = traceback.format_exc()
            (self.output / "test-error.txt").write_text(error, encoding="utf-8")
            integration._report(error)
            self.finish()

    def finish(self):
        hou.hipFile.save(str(self.output / "hdue-test.hip"))
        self.host.close()
        if self.verified:
            if self.host.runtime.status().state != 4:
                raise AssertionError("UE shutdown incomplete")
            (self.output / "verified.json").write_text(
                json.dumps(self.verified, indent=2), encoding="utf-8"
            )
        # Let Houdini's own event dispatcher process the exit. SystemExit
        # raised directly inside a PySide signal does not leave Houdini.
        import hdefereval

        hdefereval.execute_deferred(lambda: hou.hscript("quit -f"))

    def image(self, label, mode, previous=0, minimum_meshes=0):
        while True:
            s = self.api.status()
            if s["error"]:
                raise RuntimeError(s["error"])
            if (
                s["active"]
                and s["mode"] == mode
                and s["meshes"] >= minimum_meshes
                and s["generation"] > previous
                and s["frame"]["frame_id"]
                and s["frame"]["converged"]
            ):
                data = self.api.pixels(s)
                if data:
                    frame, pixels = data
                    rgb = (
                        np.ctypeslib.as_array(pixels)
                        .reshape(frame.height, frame.width, 4)[..., :3]
                        .copy()
                    )
                    if not np.isfinite(rgb).all():
                        raise AssertionError("Non-finite image")
                    if rgb.max() > 0.01:
                        break
            yield
        kernel = C.WinDLL("kernel32")
        kernel.GetCurrentThreadId.restype = C.c_uint32
        owner = kernel.GetCurrentThreadId()
        # The host talks to hdUE from Houdini's UI thread; UE itself runs on hdUE's own thread, never on the UI's.
        if s["create_thread"] != owner or s["execute_thread"] == owner:
            raise AssertionError("UE work ran on the UI thread: " + repr(s))
        frame, pixels = data
        alpha = np.ctypeslib.as_array(pixels)[3::4]
        s["backend_alpha_min"] = float(alpha.min())
        s["backend_alpha_max"] = float(alpha.max())
        if mode == 1 and frame.completed_samples < 64:
            raise AssertionError("PT converged early")
        depth = self.api.depth_pixels(s)
        if depth is None:
            raise AssertionError("Converged color has no matching depth")
        depth = np.ctypeslib.as_array(depth).reshape(frame.height, frame.width)
        hits = depth[depth > 0]
        if (
            not np.isfinite(depth).all()
            or not len(hits)
            or hits.max() - hits.min() < 1
        ):
            raise AssertionError("Missing or constant UE scene depth")
        s["depth_cm_min"] = float(hits.min())
        s["depth_cm_max"] = float(hits.max())
        s["depth_hits"] = int(len(hits))
        s["depth_background"] = int(depth.size - len(hits))
        write_pfm(self.output / (label + "-depth.pfm"), depth)
        write_pfm(self.output / (label + ".pfm"), rgb)
        self.frames.append(dict(label=label, **s))
        integration._report("Verified " + label + ": " + repr(s))
        return rgb

    def run(self):
        viewer = integration.scene_viewer()
        import hdefereval
        from pxr import UsdGeom

        for _ in range(24):
            yield
        # The selection highlight below relies on hdUE:viewportIdAovs at its default (on).
        hdefereval.execute_deferred(viewer.curViewport().frameAll)
        for _ in range(8):
            yield
        first = yield from self.image("01-lumen", 0)
        # The stage reaches hdUE as authored (smoke.usda: Z-up centimetres); the delegate converts it.
        if (
            UsdGeom.GetStageUpAxis(viewer.stage()) != "Z"
            or UsdGeom.GetStageMetersPerUnit(viewer.stage()) != 0.01
        ):
            raise AssertionError("Solaris output is not the input stage space")
        import threading

        result = []
        thread = threading.Thread(
            target=lambda: result.append(self.api.set_up_axis(1))
        )
        thread.start()
        thread.join()
        if result != [-3]:
            raise AssertionError(
                "Viewport up axis did not reject a worker thread: "
                + repr(result)
            )
        self.depth_contracts()
        yield from self.capture("01-lumen-solaris.png")
        self.navigate("pan", -60, 20)
        moved = yield from self.image(
            "02-camera-translation", 0, self.frames[-1]["generation"]
        )
        if np.abs(first - moved).mean() < 0.001:
            raise AssertionError("Camera did not change the image")
        node = viewer.currentNode()
        integration._report("Settings node: " + str(node))
        # The smoke cube is animated and the viewport camera has a shutter; this test checks
        # Path Tracer accumulation of whole frames, so keep motion-blur sub-frames out of it.
        integration.set_render_setting("xn__hdUEmotionBlur", 0)
        integration.set_mode("pathtraced")
        # Houdini passes the render settings prim's hdUE:renderMode to the delegate on its next update.
        for _ in range(200):
            if self.api.status()["mode"] == 1:
                break
            yield
        else:
            raise AssertionError("hdUE:renderMode did not reach the viewport")
        if self.api.status()["frame"]["frame_id"]:
            raise AssertionError(
                "The previous Lumen image was exposed as a pending Path Tracer result"
            )
        traced = yield from self.image(
            "03-pathtraced", 1, self.frames[-1]["generation"]
        )
        yield from self.capture("03-pathtraced-solaris.png")
        edit = node.parent().createNode("pythonscript", "hdUE_transform_test")
        edit.setInput(0, node)
        edit.parm("python").set(
            'from pxr import UsdGeom, Gf\nstage=hou.pwd().editableStage()\nUsdGeom.Xformable(stage.GetPrimAtPath("/World/Cube")).AddTranslateOp(opSuffix="hdueTest").Set(Gf.Vec3d(60,0,0))'
        )
        edit.setDisplayFlag(True)
        edit.setCurrent(True, clear_all_selected=True)
        transformed = yield from self.image(
            "04-transform-edit", 1, self.frames[-1]["generation"]
        )
        if np.abs(traced - transformed).mean() < 0.001:
            raise AssertionError("Transform did not change the image")
        native = edit.parent().createNode("pythonscript", "hdUE_native_test")
        native.setInput(0, edit)
        native.parm("python").set(
            """from pxr import UsdGeom,Sdf,Gf
s=hou.pwd().editableStage()
p=UsdGeom.Xform.Define(s,"/Native").GetPrim()
p.CreateAttribute("hdue:asset",Sdf.ValueTypeNames.String).Set("/Game/HdUE/SM_Nanite.SM_Nanite")
UsdGeom.Xformable(p).AddTranslateOp().Set(Gf.Vec3d(0,2,0))
parent=UsdGeom.Xform.Define(s,"/NativeParent")
parent.AddTranslateOp().Set(Gf.Vec3d(-2,0,0)); parent.AddScaleOp().Set(Gf.Vec3f(1.5))
p=UsdGeom.Mesh.Define(s,"/NativeParent/MeshAsset").GetPrim()
p.CreateAttribute("hdue:asset",Sdf.ValueTypeNames.String).Set("/Game/HdUE/SM_Nanite.SM_Nanite")
UsdGeom.Xformable(p).AddTranslateOp().Set(Gf.Vec3d(0,1,0))
"""
        )
        prepared = integration.settings_node(native)
        prepared.setDisplayFlag(True)
        prepared.setCurrent(True, clear_all_selected=True)
        # Render settings flow downstream, so this chain inherits Path Tracer from step 03.
        integration.set_mode("realtime")
        yield from self.image(
            "05-native-lumen",
            0,
            self.frames[-1]["generation"],
            minimum_meshes=4,
        )
        from asset_runtime import Assets, AssetSceneStatus

        api = Assets(self.host.runtime)
        assets = AssetSceneStatus()
        assets.struct_size = C.sizeof(assets)
        self.host.runtime.check(
            api.get_scene(self.frames[-1]["session"], C.byref(assets)),
            "native scene status",
        )
        if (assets.instances, assets.loading, assets.failed, assets.meshes) != (
            2,
            0,
            0,
            2,
        ):
            raise AssertionError("Native meshes were not ready")
        self.frames[-1]["native_assets"] = {
            n: getattr(assets, n) for n, _ in assets._fields_ if n != "message"
        }
        integration.set_mode("pathtraced")
        prepared.cook(force=True)
        yield from self.image(
            "06-native-pathtraced", 1, self.frames[-1]["generation"]
        )
        native_image = yield from self.capture(
            "06-native-pathtraced-solaris.png"
        )
        # Standard Solaris scene-graph selection must draw an overlay without
        # restarting a completed Path Tracer accumulation.
        stable = self.api.status()
        viewer.setCurrentSceneGraphSelection(("/World/Cube",))
        viewer.setSceneGraphHighlight(("/World/Cube",))
        for _ in range(12):
            yield
        selected_image = yield from self.capture(
            "06-selection-solaris.png", selected=True
        )
        if self.api.status()["generation"] != stable["generation"]:
            raise AssertionError("Selection reset the Path Tracer")
        if "/World/Cube" not in viewer.currentSceneGraphSelection():
            raise AssertionError("Scene graph selection was not retained")
        self.selection_overlay = bool(
            np.mean(
                np.abs(
                    native_image.astype(float) - selected_image.astype(float)
                )
            )
            >= 0.001
        )
        # The delegate's primId AOV drives Houdini's native selection highlight.
        if not self.selection_overlay:
            raise AssertionError(
                "Selecting /World/Cube drew no native highlight"
            )
        viewer.setCurrentSceneGraphSelection(())
        viewer.setSceneGraphHighlight(())
        viewer.enterViewState()
        old = self.frames[-1]["session"]
        viewer.setHydraRenderer("Houdini VK")
        for _ in range(8):
            yield
        integration.activate()
        # Houdini pauses a delegate it switches away from and resumes it on return; a restart replaces it.
        viewer.restartRenderer()
        while self.api.status()["session"] in (0, old):
            yield
        before = yield from self.image("07-delegate-recreated", 1, 0)
        if (
            self.frames[-1]["session"] == old
            or self.host.runtime.initializations != 1
        ):
            raise AssertionError("Incorrect UE/delegate lifetime")
        self.navigate("orbit", 35, 15)
        orbited = yield from self.image(
            "08-camera-orbit", 1, self.frames[-1]["generation"]
        )
        if np.abs(before - orbited).mean() < 0.001:
            raise AssertionError("Orbit did not change the image")
        self.navigate("dolly", 0, -35)
        dolly = yield from self.image(
            "09-camera-dolly", 1, self.frames[-1]["generation"]
        )
        if np.abs(orbited - dolly).mean() < 0.001:
            raise AssertionError("Dolly did not change the image")
        yield from self.capture("09-native-pathtraced-solaris.png")
        previous = self.frames[-1]["generation"]
        old_size = (
            self.frames[-1]["frame"]["width"],
            self.frames[-1]["frame"]["height"],
        )
        hou.qt.mainWindow().showNormal()
        hou.qt.mainWindow().resize(1800, 1100)
        for _ in range(12):
            yield
        yield from self.image("10-resize", 1, previous)
        new_size = (
            self.frames[-1]["frame"]["width"],
            self.frames[-1]["frame"]["height"],
        )
        if old_size == new_size:
            raise AssertionError("Resize did not reallocate the AOVs")
        yield from self.capture("10-resize-solaris.png")

    def capture(self, name, selected=False):
        from PySide6 import QtGui

        path = self.output / name
        done = yield from flipbook(path)
        # The flipbook's nested event loop ends on the renderer's next update; the test must not go on (or quit) inside it.
        self.update()
        while not done:
            yield
        image = QtGui.QImage(str(path)).convertToFormat(
            QtGui.QImage.Format_RGBA8888
        )
        while image.isNull():
            yield
            image = QtGui.QImage(str(path)).convertToFormat(
                QtGui.QImage.Format_RGBA8888
            )
        pixels = (
            np.frombuffer(bytes(image.constBits()), dtype=np.uint8)
            .reshape(image.height(), image.bytesPerLine())[
                :, : image.width() * 4
            ]
            .reshape(image.height(), image.width(), 4)
            .copy()
        )
        r, g, b = np.moveaxis(
            pixels[
                50 : image.height() - 30 : 2, 30 : image.width() - 30 : 2, :3
            ].astype(float),
            -1,
            0,
        )
        lit = int((r + g + b > 45).sum())
        red = int(((r > 20) & (r > g * 1.5) & (r > b * 1.5)).sum())
        blue = int(((b > 30) & (b > r * 1.5) & (b > g * 1.3)).sum())
        # Houdini's selection highlight tints the selected red cube.
        if lit < image.width() * image.height() * 0.005 or (
            not selected and red < image.width() * image.height() * 0.0001
        ):
            raise AssertionError(
                f"Solaris did not display the red cube: {name}, lit={lit}, red={red}"
            )
        if (
            self.frames[-1]["meshes"] >= 4
            and blue < image.width() * image.height() * 0.0001
        ):
            raise AssertionError(
                "Solaris did not display the native blue meshes: " + name
            )
        self.captures.append(
            dict(
                file=name,
                width=image.width(),
                height=image.height(),
                lit_pixels=lit,
                red_pixels=red,
                blue_pixels=blue,
            )
        )
        return pixels

    def navigate(self, action, dx, dy):
        viewport = integration.scene_viewer().curViewport()
        camera = viewport.defaultCamera()
        position = hou.Vector3(camera.translation())
        if action == "pan":
            camera.setTranslation(
                position + hou.Vector3(-dx, dy, 0) * abs(position[2]) / 500.0
            )
        elif action == "orbit":
            camera.setRotation(
                hou.hmath.buildRotate(
                    hou.Vector3(-dy * 0.3, 0, 0)
                ).extractRotationMatrix3()
                * camera.rotation()
                * hou.hmath.buildRotate(
                    hou.Vector3(0, -dx * 0.3, 0)
                ).extractRotationMatrix3()
            )
        elif action == "dolly":
            camera.setTranslation(
                hou.Vector3(
                    position[0], position[1], position[2] * math.exp(dy * 0.008)
                )
            )
        self.update()

    def update(self):
        # Houdini runs a converged renderer's next update on viewport interaction only, not on viewport.draw(): a
        # scripted camera edit or a finished flipbook waits for it. Pausing and resuming runs one.
        viewer = integration.scene_viewer()
        viewer.setRendererPaused(True)
        viewer.setRendererPaused(False)

    def depth_contracts(self):
        import threading

        from hdue_viewport import DepthApi, Frame

        api = DepthApi()
        if self.api.get_depth(99, C.sizeof(api), C.byref(api)) != -2:
            raise AssertionError("Depth ABI version accepted")
        if self.api.get_depth(1, C.sizeof(api) - 1, C.byref(api)) != -6:
            raise AssertionError("Short depth table accepted")
        # Depth belongs to UE's newest frame, by UE's frame id (the viewport status numbers the frames Houdini shows).
        state = self.api.status()
        frame = Frame()
        frame.struct_size = C.sizeof(frame)
        self.host.runtime.check(
            self.api.poll(state["session"], C.byref(frame)), "poll image"
        )
        guard = np.full(frame.width * frame.height, -123, dtype=np.float32)
        pointer = guard.ctypes.data_as(C.c_void_p)
        if (
            self.api.copy_depth(
                state["session"], frame.frame_id, pointer, guard.nbytes - 1
            )
            != -6
            or not (guard == -123).all()
        ):
            raise AssertionError("Short depth buffer contract failed")
        if (
            self.api.copy_depth(state["session"], 0, pointer, guard.nbytes)
            != -7
        ):
            raise AssertionError("Stale depth frame accepted")
        result = []
        thread = threading.Thread(
            target=lambda: result.append(
                self.api.set_depth.direct(state["session"], 1)
            )
        )
        thread.start()
        thread.join()
        if result != [-3]:
            raise AssertionError("Depth API did not reject a worker thread")


def start(host):
    global _test
    _test = Test(host)
