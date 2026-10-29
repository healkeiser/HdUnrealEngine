"""Houdini 22.0 Solaris host: housekeeping for the hdUE viewport (material preparation, op: images, asset proxies,
redraws). UE runs on the delegate's own thread; the host starts when a Scene Viewer first selects hdUE.
"""

import atexit
import json
import os
import sys
import threading
import time
import traceback
from pathlib import Path

import hou
from pxr import Plug, Sdf, Tf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade

ROOT = Path(__file__).resolve().parents[3]
# Appended: hdUE's modules must not shadow Houdini's or another package's.
if str(ROOT / "scripts") not in sys.path:
    sys.path.append(str(ROOT / "scripts"))
PLUGIN = "HdUEHoudiniRendererPlugin"
LABEL = "Unreal Engine (hdUE)"
_host = None
_failure = None
_catalog_cache = {}
_watcher = None


def log_directory():
    """HDUE_HOUDINI_LOG_DIR, else a per-process folder under Houdini's temp directory (never the install folder)."""
    directory = os.environ.get("HDUE_HOUDINI_LOG_DIR")
    if directory:
        return Path(directory)
    return (
        Path(hou.text.expandString("$HOUDINI_TEMP_DIR"))
        / "hdUE"
        / str(os.getpid())
    )


def _report(message):
    print("hdUE Houdini: " + message, flush=True)
    directory = os.environ.get("HDUE_HOUDINI_LOG_DIR")
    if directory:
        path = Path(directory)
        path.mkdir(parents=True, exist_ok=True)
        with (path / "integration.log").open("a", encoding="utf-8") as log:
            log.write(time.strftime("%H:%M:%S ") + message + "\n")


def runtime_config():
    path = Path(os.environ["HDUE_RUNTIME_CONFIG"]).resolve()
    config = json.loads(path.read_text(encoding="utf-8-sig"))
    if config.get("version") != 1:
        raise ValueError("Unsupported hdUE runtime config")
    for key in (
        "backend_dll",
        "runtime_project",
        "asset_catalog",
        "materialx_library",
        "substrate_library",
    ):
        if key in config:
            value = Path(config[key])
            config[key] = str(
                value
                if value.is_absolute()
                else (path.parent / value).resolve()
            )
    return config


class Host:
    def __init__(self):
        from hdue_runtime import Runtime
        from hdue_viewport import Viewport
        from PySide6 import QtCore, QtWidgets

        if hou.applicationVersion()[:2] != (22, 0):
            raise RuntimeError("This package requires Houdini 22.0")
        self.owner = threading.get_ident()
        self.log_dir = log_directory()
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.failures = 0
        config = runtime_config()
        os.environ["HDUE_BACKEND_DLL"] = config["backend_dll"]
        hou.putenv("HDUE_BACKEND_DLL", config["backend_dll"])
        # UE runs on hdUE's own thread (started by the first hdUE delegate), not on Houdini's UI thread.
        self.runtime = Runtime(
            config["backend_dll"],
            config["runtime_project"],
            self.log_dir / "ue.log",
            start=False,
        )
        # The delegate DLL Houdini registered (PXR_PLUGINPATH_NAME): the host's diagnostics live in the same module.
        plugin = Plug.Registry().GetPluginForType(Tf.Type.FindByName(PLUGIN))
        if plugin is None:
            raise RuntimeError(PLUGIN + " is not on PXR_PLUGINPATH_NAME")
        self.api = Viewport(self.runtime, plugin.path)
        from hdue_materialx import PreparedMaterials
        from substrate_live import LivePreparation

        self.live_substrate = LivePreparation(
            config["runtime_project"], self.log_dir
        )
        self.prepared_materials = PreparedMaterials(self.live_substrate)
        self.assets = NativeAssets()
        self.texture_files = {}
        self.pushed_textures = None
        self.material_error = None
        self.last_materials = None
        # Material and op: texture preparation walk the stage (~70 ms each on Sponza): only after a shading change, which
        # the viewer stage's change notices tell apart. Render settings, cameras and transforms (playback) do not pay it.
        self.watched_stage = None
        self.stage_listener = None
        self.materials_dirty = self.textures_dirty = True
        self.materials_animated = False
        self.viewer = None
        self.last_options = None
        self.options_key = None
        self.libraries = (None, None)
        self.z_up = False
        self.closed = False
        self.last_state = time.monotonic()
        self.last_textures = None
        self.texture_error = None
        self.running = True
        # Housekeeping only (preparation, textures, status): UE ticks on its own thread. Rescheduled after each
        # run, since a permanent due timer can starve LOP cooks.
        self.timer = QtCore.QTimer()
        self.timer.setSingleShot(True)
        self.timer.setTimerType(QtCore.Qt.PreciseTimer)
        self.timer.timeout.connect(self.tick)
        self.timer.start(16)
        QtWidgets.QApplication.instance().aboutToQuit.connect(self.close)
        atexit.register(self.close)
        self.write_state()

    def watch_stage(self, stage):
        if self.watched_stage is not None and stage == self.watched_stage:
            return
        self.watched_stage = stage
        self.materials_dirty = self.textures_dirty = True
        self.stage_listener = Tf.Notice.Register(
            Usd.Notice.ObjectsChanged, self.stage_changed, stage
        )

    def stage_changed(self, notice, stage):
        if self.materials_dirty and self.textures_dirty:
            return
        if any(
            _shading_change(stage, path, False)
            for path in notice.GetResyncedPaths()
        ) or any(
            _shading_change(stage, path, True)
            for path in notice.GetChangedInfoOnlyPaths()
        ):
            self.materials_dirty = self.textures_dirty = True

    def write_state(self, error=None):
        # Diagnostics for tests and support only (HDUE_HOUDINI_LOG_DIR): no file write every 2 s otherwise.
        if not os.environ.get("HDUE_HOUDINI_LOG_DIR"):
            return
        # UE boots on hdUE's own thread, so its status can be unavailable for a while; that must not stop the host's
        # timer (any exception in tick() does).
        try:
            runtime_state = self.runtime.status().state
        except RuntimeError as failure:
            runtime_state = str(failure)
        data = dict(
            houdini=hou.applicationVersionString(),
            usd=Usd.GetVersion(),
            pid=os.getpid(),
            owner=self.owner,
            ready=self.runtime.ready,
            closed=self.closed,
            ticks=self.runtime.ticks,
            ue_initializations=self.runtime.initializations,
            runtime_state=runtime_state,
            error=error,
            texture_error=self.texture_error,
            material_error=self.material_error,
            substrate_preparation=self.live_substrate.status(),
        )
        (self.log_dir / "runtime-state.json").write_text(
            json.dumps(data, indent=2), encoding="utf-8"
        )

    def material_problem(self, message, session):
        if session:
            self.prepared_materials.publish_error(session, self.api, message)
        if message != self.material_error:
            _report(message)
            hou.ui.setStatusMessage(
                "hdUE: " + message, hou.severityType.Warning
            )
        self.material_error = message

    def tick(self):
        global _failure
        if not self.running:
            return
        try:
            state = self.api.status()
            self.live_substrate.tick()
            # Optional UI shim (hdUE_settings subnet): mask preview and automatic Substrate preparation.
            node = current_settings_node()
            if _hdue_selected():
                if state["active"]:
                    try:
                        stage = scene_viewer().stage()
                        if not stage:
                            network = scene_viewer().pwd()
                            display = (
                                network.displayNode()
                                if isinstance(network, hou.LopNetwork)
                                else scene_viewer().currentNode()
                            )
                            if display and display.errors():
                                raise ValueError("; ".join(display.errors()))
                    except (hou.Error, ValueError, RuntimeError) as error:
                        self.material_problem(str(error), state["session"])
                        stage = None
                    if stage:
                        values = _settings_values(node) if node else ()
                        key = _stage_key(values)
                        # Render settings, as husk reads them; the stage changes only with the stage key.
                        if key != self.options_key:
                            self.options_key = key
                            self.libraries = libraries(stage, node)
                            self.z_up = UsdGeom.GetStageUpAxis(stage) == "Z"
                        self.sync_scene(
                            state["session"], key, any(self.libraries)
                        )
                        # Material preparation walks the stage and hashes embedded textures: only after a change.
                        self.watch_stage(stage)
                        # Preparation reads materials and wet opinions at the current frame: redo it per frame only if they are animated.
                        if self.materials_dirty:
                            self.materials_animated = _materials_animated(stage)
                        materials_key = (
                            state["session"],
                            values,
                            self.libraries,
                            hou.frame() if self.materials_animated else None,
                        )
                        if state["session"] and (
                            self.materials_dirty
                            or materials_key != self.last_materials
                        ):
                            self.last_materials = materials_key
                            self.materials_dirty = False
                            try:
                                preview = node and node.parm(
                                    "hdue_wet_mask_preview"
                                )
                                self.prepared_materials.settings[
                                    "wetMaskPreview"
                                ] = (bool(preview.eval()) if preview else False)
                                automatic = node and node.parm(
                                    "hdue_substrate_auto_prepare"
                                )
                                self.live_substrate.enabled = (
                                    bool(automatic.eval())
                                    if automatic
                                    else True
                                )
                                changed = self.prepared_materials.sync(
                                    stage,
                                    Usd.TimeCode(hou.frame()),
                                    self.libraries[0],
                                    state["session"],
                                    self.api,
                                    self.libraries[1],
                                )
                                if changed:
                                    scene_viewer().curViewport().draw()
                                if self.material_error:
                                    hou.ui.setStatusMessage(
                                        "hdUE: material preparation is valid; rendering resumed."
                                    )
                                self.material_error = None
                            except (ValueError, OSError, hou.Error) as error:
                                self.material_problem(
                                    str(error), state["session"]
                                )
                        # Hydra brings the render settings, units and time; Houdini sends no up axis.
                        axis = (state["session"], self.z_up)
                        if axis != self.last_options and self.api.up_axis(
                            self.z_up
                        ):
                            self.last_options = axis
                            scene_viewer().curViewport().draw()
            if self.assets.authored:
                # The bounds meshes serve hdUE only; other renderers get the scene as authored.
                viewer = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
                if viewer is None or viewer.currentHydraRenderer() not in (
                    LABEL,
                    PLUGIN,
                ):
                    try:
                        self.assets.clear()
                    except hou.Error:
                        _report(traceback.format_exc())
            if self.viewer:
                if state["error"]:
                    self.viewer.status.setText(state["error"])
                elif not state["active"]:
                    self.viewer.status.setText(
                        "Select Unreal Engine (hdUE) in Solaris to resume"
                    )
            if (
                self.viewer
                and self.viewer.isVisible()
                and state["active"]
                and state["frame"]["frame_id"]
            ):
                key = (
                    state["session"],
                    state["generation"],
                    state["frame"]["frame_id"],
                )
                if key != self.viewer.last_frame:
                    data = self.api.pixels(state, require_converged=False)
                    if data:
                        self.viewer.present(state, *data)
            if time.monotonic() - self.last_state > 2:
                self.last_state = time.monotonic()
                self.write_state()
            self.failures = 0
        except BaseException:
            # Shown in Houdini's status bar; the host retries, and stops only after repeated failures.
            _failure = traceback.format_exc()
            self.failures += 1
            if self.failures >= 5:
                self.running = False
                self.timer.stop()
            message = (
                "hdUE host "
                + ("stopped" if not self.running else "error")
                + ": "
                + _failure.splitlines()[-1]
            )
            try:
                hou.ui.setStatusMessage(message, hou.severityType.Error)
            except BaseException:
                pass
            if self.viewer:
                self.viewer.status.setText(_failure.splitlines()[-1])
            self.write_state(_failure)
            _report(_failure)
        finally:
            if self.running:
                self.timer.start(16)

    def sync_scene(self, session, key, materials=False):
        """What the delegate cannot read from USD by itself, kept out of the LOP graph: images of op:/opdef: texture
        paths (exported here, outside any cook) and bounds meshes for hdue:asset prims (viewport overrides).
        """
        from hdue_op_textures import cache, prepare_stage

        viewer = scene_viewer()
        redraw = False
        try:
            stage = viewer.stage()
            if key != self.last_textures:
                network = viewer.pwd()
                display = (
                    network.displayNode()
                    if isinstance(network, hou.LopNetwork)
                    else None
                )
                # Never cook from here: this timer also fires while Houdini cooks (a re-entrant cook fails with
                # "Infinite recursion in evaluation"). The viewed stage is already composed.
                if display and display.needsToCook():
                    return
                if display:
                    redraw = self.assets.sync(
                        network, display, stage, materials
                    )
            self.last_textures = key
            if self.textures_dirty or cache().dirty("viewport"):
                self.textures_dirty = False
                self.texture_files = prepare_stage(stage)
            if (
                session
                and (session, self.texture_files) != self.pushed_textures
                and self.api.textures(session, self.texture_files)
            ):
                self.pushed_textures = (session, dict(self.texture_files))
                redraw = True
            error = None
        except (hou.Error, ValueError, RuntimeError) as failure:
            error = str(failure)
        if error and error != self.texture_error:
            _report(error)
        self.texture_error = error
        if redraw:
            viewer.curViewport().draw()

    def close(self):
        if self.closed:
            return
        self.running = False
        self.timer.stop()
        try:
            self.assets.clear()
        except BaseException:
            _report(traceback.format_exc())
        self.live_substrate.close()
        # Switch viewers away so their sessions end before UE shuts down (deactivated hdUE delegates only pause).
        try:
            for viewer in hou.ui.paneTabs():
                if isinstance(
                    viewer, hou.SceneViewer
                ) and viewer.currentHydraRenderer() in (LABEL, PLUGIN):
                    others = [
                        name
                        for name in viewer.hydraRenderers()
                        if name not in (LABEL, PLUGIN)
                    ]
                    viewer.setHydraRenderer(
                        others[0] if others else "Houdini GL"
                    )
        except BaseException:
            _report(traceback.format_exc())
        if self.viewer:
            self.viewer.hide()
        self.runtime.close()
        # UE outlives individual viewports on hdUE's thread; end it with Houdini (one UE lifetime per process).
        shutdown = getattr(self.api.library, "hdue_shutdown_ue", None)
        if shutdown:
            shutdown.argtypes = []
            shutdown.restype = None
            shutdown()
        from hdue_op_textures import close

        close()
        self.closed = True
        self.write_state()


def _hdue_selected():
    try:
        return any(
            isinstance(tab, hou.SceneViewer)
            and tab.currentHydraRenderer() in (LABEL, PLUGIN)
            for tab in hou.ui.paneTabs()
        )
    except (hou.Error, TypeError):  # TypeError: a viewer outside LOPs
        return False


def start_when_selected():
    """uiready.py: a cheap 500 ms watch; the host (DLL loads, material preparation, its 16 ms timer) starts only when a
    Scene Viewer selects hdUE, so sessions that never use hdUE pay nothing. Launches with a scene start now.
    """
    global _watcher
    if any(
        os.environ.get(name) for name in ("HDUE_HOUDINI_INPUT", "HDUE_HOUDINI_HIP")
    ):
        return start()
    from PySide6 import QtCore

    def poll():
        if _hdue_selected():
            _watcher.stop()
            start()

    _watcher = QtCore.QTimer()
    _watcher.timeout.connect(poll)
    _watcher.start(500)


def start():
    global _host, _failure
    if _host:
        return _host
    try:
        import hdue_substrate

        hdue_substrate.install()
        _host = Host()
        _report("hdUE host started (UE runs on its own thread)")
        input_usd = os.environ.get("HDUE_HOUDINI_INPUT")
        if input_usd:
            open_usd(input_usd)
        elif os.environ.get("HDUE_HOUDINI_HIP"):
            import hdefereval
            from PySide6 import QtCore

            QtCore.QTimer.singleShot(
                500, lambda: hdefereval.execute_deferred(activate_saved_hip)
            )
    except BaseException:
        _failure = traceback.format_exc()
        _report(_failure)
        try:
            hou.ui.setStatusMessage(
                "hdUE host failed to start: " + _failure.splitlines()[-1],
                hou.severityType.Error,
            )
        except BaseException:
            pass
        path = log_directory()
        path.mkdir(parents=True, exist_ok=True)
        (path / "startup-error.txt").write_text(_failure, encoding="utf-8")
    return _host


RENDER_ROOT = Sdf.Path("/Render")
VIEWER_CAMERA = Sdf.Path("/__HoudiniFreeCamera__")
SHADING_PROPERTIES = ("material:binding", "hdue:surface:wet:")


def _shading_change(stage, path, info_only):
    """Whether a changed path can change prepared materials or op: textures. Houdini rewrites its free viewport camera
    on every camera move, and cameras and transforms change every frame of playback: only shading prims, bindings and
    wet opinions count for value changes; any structure change outside the render settings does.
    """
    if path.HasPrefix(RENDER_ROOT) or path.HasPrefix(VIEWER_CAMERA):
        return False
    if not info_only:
        return True
    if path.IsPropertyPath() and path.name.startswith(SHADING_PROPERTIES):
        return True
    prim = stage.GetPrimAtPath(path.GetPrimPath())
    return bool(prim) and (
        prim.IsA(UsdShade.Material)
        or prim.IsA(UsdShade.Shader)
        or prim.IsA(UsdShade.NodeGraph)
        or prim.HasAPI(UsdLux.LightAPI)
    )


def _materials_animated(stage):
    """Whether a material, shader or hdUE wet opinion has time samples."""
    return any(
        attr.ValueMightBeTimeVarying()
        for prim in stage.Traverse()
        for attr in prim.GetAttributes()
        if prim.IsA(UsdShade.Material)
        or prim.IsA(UsdShade.Shader)
        or attr.GetName().startswith("hdue:surface:wet:")
    )


def _settings_values(settings):
    return tuple(
        parm.eval()
        for parm in settings.parms()
        if parm.name().startswith("hdue_")
    )


def _stage_key(values):
    """Changes whenever the viewed stage or the hdUE settings change: the displayed LOP's cook count and the settings values."""
    viewer = scene_viewer()
    network = viewer.pwd()
    if not isinstance(network, hou.LopNetwork):
        return (None, None, 0, values)  # no Hydra view outside LOPs
    display = network.displayNode()
    return (
        viewer.currentHydraRenderer(),
        display.path() if display else None,
        display.cookCount() if display else 0,
        values,
    )


def scene_viewer():
    viewer = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
    if viewer is None:
        raise RuntimeError("Open a Scene Viewer pane for Solaris")
    return viewer


def activate_saved_hip():
    """Activate an already loaded HIP without adding a second source network."""
    try:
        stage_network = hou.node("/stage")
        if not stage_network or not stage_network.displayNode():
            return
        viewer = scene_viewer()
        viewer.setPwd(stage_network)
        viewer.setCurrentNode(stage_network.displayNode())
        controls = current_settings_node()
        if controls:
            configure_live_preparation(controls)
        viewer.setLinkGroup(hou.paneLinkType.Pinned)
        activate()
        stage = viewer.stage()
        world = stage.GetPrimAtPath("/World") if stage else None
        if world:
            bounds = (
                UsdGeom.BBoxCache(
                    Usd.TimeCode(hou.frame()), ["default", "render"]
                )
                .ComputeWorldBound(world)
                .ComputeAlignedRange()
            )
            if not bounds.IsEmpty():
                viewer.curViewport().frameBoundingBox(
                    hou.BoundingBox(*bounds.GetMin(), *bounds.GetMax())
                )
        builder = hou.node(stage_network.userData("hdue:startupBuilder") or "")
        if builder:
            editor = hou.ui.paneTabOfType(hou.paneTabType.NetworkEditor)
            if editor:
                editor.setLinkGroup(hou.paneLinkType.Pinned)
                editor.setPwd(builder)
                for child in builder.children():
                    child.setSelected(True)
                editor.homeToSelection()
            control = builder.node("amount") or builder.node("uv")
            if control:
                control.setCurrent(True, clear_all_selected=True)
        _report("Opened HIP: " + hou.hipFile.path())
    except (hou.Error, ValueError, RuntimeError):
        _report(traceback.format_exc())


def activate():
    if not _host:
        start()
    if not _host or _failure:
        raise RuntimeError(_failure or "UE not ready")
    viewer = scene_viewer()
    names = viewer.hydraRenderers()
    _report("Available Solaris renderers: " + repr(names))
    viewer.setHydraRenderer(LABEL if LABEL in names else PLUGIN)
    if os.environ.get("HDUE_HOUDINI_IMAGE_VIEWER") == "1":
        show_viewer()
    return viewer


def show_viewer():
    if not _host:
        start()
    if not _host or _failure:
        raise RuntimeError(_failure or "UE not ready")
    if _host.viewer is None:
        from hdue_viewer import Viewer

        _host.viewer = Viewer(_host)
    _host.viewer.show()
    return _host.viewer


def current_settings_node():
    viewer = hou.ui.paneTabOfType(hou.paneTabType.SceneViewer)
    if viewer is None:
        return None
    network = viewer.pwd()
    node = (
        network.displayNode()
        if isinstance(network, hou.LopNetwork)
        else viewer.currentNode()
    )
    # hdue_wet_mask_preview exists only on the hdUE_settings subnet.
    while node and not node.parm("hdue_wet_mask_preview"):
        inputs = node.inputs()
        node = inputs[0] if inputs else None
    return node


def configure_live_preparation(node):
    if node.parm("hdue_substrate_auto_prepare"):
        return
    group = node.parmTemplateGroup()
    group.append(
        hou.ToggleParmTemplate(
            "hdue_substrate_auto_prepare",
            "Prepare graph edits automatically",
            default_value=True,
        )
    )
    retry = hou.ButtonParmTemplate(
        "hdue_substrate_retry", "Retry material preparation"
    )
    retry.setScriptCallback(
        "import hdue_houdini; hdue_houdini._host.live_substrate.retry()"
    )
    retry.setScriptCallbackLanguage(hou.scriptLanguage.Python)
    group.append(retry)
    node.setParmTemplateGroup(group)


def settings_node(input_node):
    """HdUE's viewport-only controls on a pass-through subnet: the scene below it stays the input's USD, unchanged.

    Everything that changes the image is a render setting (hdUE Render Settings LOP), so husk renders it too: render
    mode, samples, and the cooked MaterialX and Substrate libraries. COP/HDA images and native asset bounds are the
    host's (Host.sync_scene), outside the LOP graph.
    """
    node = input_node.parent().createNode("subnet", "hdUE_settings")
    node.setInput(0, input_node)
    group = node.parmTemplateGroup()
    group.append(
        hou.ToggleParmTemplate(
            "hdue_wet_mask_preview", "Preview Primvar mask", default_value=False
        )
    )
    group.append(
        hou.ToggleParmTemplate(
            "hdue_substrate_auto_prepare",
            "Prepare graph edits automatically",
            default_value=True,
        )
    )
    retry = hou.ButtonParmTemplate(
        "hdue_substrate_retry", "Retry material preparation"
    )
    retry.setScriptCallback(
        "import hdue_houdini; hdue_houdini._host.live_substrate.retry()"
    )
    retry.setScriptCallbackLanguage(hou.scriptLanguage.Python)
    group.append(retry)
    node.setParmTemplateGroup(group)
    node.node("output0").setInput(0, node.indirectInputs()[0])
    return node


def _catalog():
    path = runtime_config().get("asset_catalog")
    if not path:
        return {}
    key = (path, Path(path).stat().st_mtime_ns)
    if key not in _catalog_cache:
        _catalog_cache.clear()
        _catalog_cache[key] = json.loads(
            Path(path).read_text(encoding="utf-8-sig")
        )["assets"]
    return _catalog_cache[key]


def _materialx(prim):
    output = (
        UsdShade.Material(prim).GetSurfaceOutput("mtlx")
        if prim.IsA(UsdShade.Material)
        else None
    )
    return bool(output) and output.HasConnectedSource()


class NativeAssets:
    """hdue:asset prims, shown to Hydra as bounds meshes carrying the asset (the delegate places the cooked UE asset).

    Like usdview's session layer: authored only into the LOP network's viewport overrides (Houdini's session-only
    layer over the displayed stage), never into the graph or its layers. A Mesh with hdue:asset becomes a guide.
    Instances are left to Hydra, except around what the host maps by stage path (asset prims, hdue:surface:*
    attributes, and MaterialX materials when a cooked library is set): those instances are made non-instanceable
    there, as a prototype's Hydra paths are not its stage paths.
    """

    PROXY = "__HdUEAssetProxy"

    def __init__(self):
        self.network = None
        self.authored = []
        self.previous = None

    def sync(self, network, display, stage, materials=False):
        """Re-author when the viewed stage's asset prims change; True if the overrides changed."""
        if stage is None:
            return False
        refs = {}
        instances = set()
        # The viewed stage includes these overrides: instances made editable here still count as instances.
        authored = {
            path for kind, path in self.authored if kind == "instanceable"
        }
        for prim in Usd.PrimRange.Stage(stage, Usd.TraverseInstanceProxies()):
            attr = prim.GetAttribute("hdue:asset")
            asset = attr.Get() if attr else None
            if asset:
                if isinstance(asset, Sdf.AssetPath):
                    asset = asset.path
                if not isinstance(asset, str):
                    raise ValueError(
                        "hdue:asset requires a UE object path string: "
                        + str(prim.GetPath())
                    )
                refs[str(prim.GetPath())] = (asset, prim.IsA(UsdGeom.Mesh))
            elif not prim.IsInstanceProxy() or not (
                any(
                    name.startswith("hdue:surface:")
                    for name in prim.GetPropertyNames()
                )
                or (materials and _materialx(prim))
            ):
                continue
            parent = prim
            while not parent.IsPseudoRoot():
                if parent.IsInstance() or str(parent.GetPath()) in authored:
                    instances.add(str(parent.GetPath()))
                parent = parent.GetParent()
        current = (
            network.path(),
            refs,
            instances,
            UsdGeom.GetStageMetersPerUnit(stage),
            str(UsdGeom.GetStageUpAxis(stage)),
        )
        if current == self.previous:
            return False
        changed = bool(self.authored)
        self.clear()
        if not refs and not instances:
            self.previous = current
            return changed
        from asset_proxy import author_proxy

        catalog = _catalog()
        scratch = Usd.Stage.CreateInMemory()
        for path, (asset, _) in refs.items():
            proxy = author_proxy(
                scratch, path, asset, catalog, current[3] * 100, current[4]
            )
            proxy.CreatePurposeAttr(UsdGeom.Tokens.render)
        with network.viewportOverrides(display) as overrides:
            layer = overrides.layer()
            with Sdf.ChangeBlock():
                for path in sorted(instances):
                    Sdf.CreatePrimInLayer(layer, path).SetInfo(
                        "instanceable", False
                    )
                    self.authored.append(("instanceable", path))
                for path, (asset, is_mesh) in refs.items():
                    proxy = path + "/" + self.PROXY
                    Sdf.CreatePrimInLayer(layer, proxy)
                    Sdf.CopySpec(scratch.GetRootLayer(), proxy, layer, proxy)
                    self.authored.append(("spec", proxy))
                    if is_mesh:
                        purpose = Sdf.AttributeSpec(
                            layer.GetPrimAtPath(path),
                            "purpose",
                            Sdf.ValueTypeNames.Token,
                            Sdf.VariabilityUniform,
                        )
                        purpose.default = UsdGeom.Tokens.guide
                        self.authored.append(("spec", path + ".purpose"))
        self.network = network
        self.previous = current
        for path, (asset, _) in refs.items():
            for package in catalog.get(asset, {}).get(
                "unavailable_script_packages", []
            ):
                _report(
                    f"{path}: unavailable source script package {package}; its actors were omitted by cook"
                )
        return True

    def clear(self):
        authored, self.authored = self.authored, []
        network, self.network = self.network, None
        self.previous = None
        if not authored:
            return
        try:
            display = network.displayNode()
        except hou.ObjectWasDeleted:
            return
        if display is None:
            return
        with network.viewportOverrides(display) as overrides:
            layer = overrides.layer()
            edit = Sdf.BatchNamespaceEdit()
            for kind, path in authored:
                if kind == "instanceable":
                    spec = layer.GetPrimAtPath(path)
                    if spec:
                        spec.ClearInfo("instanceable")
                elif layer.GetObjectAtPath(path):
                    edit.Add(path, Sdf.Path.emptyPath)
            layer.Apply(edit)


RENDER_SETTINGS_TYPES = ("rendersettings", "hdue::rendersettings::1.0")


def _settings_prim(stage):
    """The render settings prim a viewport renders with: the stage's renderSettingsPrimPath, else the first under /Render."""
    path = stage.GetMetadata("renderSettingsPrimPath")
    prim = stage.GetPrimAtPath(path) if path else None
    if prim:
        return prim
    root = stage.GetPrimAtPath("/Render")
    return (
        next(
            (
                child
                for child in root.GetChildren()
                if child.IsA(UsdRender.Settings)
            ),
            None,
        )
        if root
        else None
    )


def render_settings_node(input_node, mode="realtime", samples=64):
    """An hdUE Render Settings LOP (otls/hdue_rendersettings.hda) at /Render/rendersettings (Houdini's default path), the
    stage's default render settings. A loaded scene's own render settings prim gives it its camera and hdUE mode and samples.
    """
    if not hou.nodeType(
        hou.lopNodeTypeCategory(), RENDER_SETTINGS_TYPES[1]
    ):  # hython without the integration on HOUDINI_PATH
        hou.hda.installFile(
            str(
                Path(__file__).resolve().parents[1]
                / "otls"
                / "hdue_rendersettings.hda"
            )
        )
    node = input_node.parent().createNode(
        RENDER_SETTINGS_TYPES[1], "hdUE_render_settings"
    )
    node.setInput(0, input_node)
    stage = input_node.stage()
    existing = _settings_prim(stage) if stage else None
    # The LOP's default camera, /cameras/camera1, rarely exists in loaded scenes; husk would render black.
    targets = (
        UsdRender.Settings(existing).GetCameraRel().GetForwardedTargets()
        if existing
        else []
    )
    camera = (
        stage.GetPrimAtPath(targets[0])
        if targets
        else (
            next(
                (prim for prim in stage.Traverse() if prim.IsA(UsdGeom.Camera)),
                None,
            )
            if stage
            else None
        )
    )
    if camera:
        node.parm("camera").set(str(camera.GetPath()))
    for name, value in (("renderMode", mode), ("samplesPerPixel", samples)):
        attr = existing.GetAttribute("hdUE:" + name) if existing else None
        _render_property(
            node,
            "xn__hdUE" + name,
            attr.Get() if attr and attr.HasAuthoredValue() else value,
        )
    return node


def _render_property(node, prefix, value):
    # Encoded hdUE:* parameter names come from HdUEHoudiniRendererPlugin_Global.ds. Houdini adds
    # them only while the hdUE delegate is registered, so add the same tagged spare parms otherwise.
    find = lambda control: next(
        (
            p
            for p in node.parms()
            if p.name().startswith(prefix + "_")
            and (("_control_" in p.name()) == control)
        ),
        None,
    )
    if find(False) is None:
        attribute = "hdUE:" + prefix[len("xn__hdUE") :]
        tags = {
            "spare_category": "Global",
            "usdvaluetype": "string" if isinstance(value, str) else "int",
        }
        template = (
            hou.StringParmTemplate
            if isinstance(value, str)
            else hou.IntParmTemplate
        )(hou.encode(attribute), attribute, 1, tags=tags)
        control = hou.StringParmTemplate(
            hou.encode(attribute + "_control"),
            attribute,
            1,
            default_value=("none",),
        )
        group = node.parmTemplateGroup()
        group.append(control)
        group.append(template)
        node.setParmTemplateGroup(group)
    control = find(
        True
    )  # hdUE Render Settings has none: it authors what differs from its default
    if control:
        control.set("set")
    find(False).set(value)


def render_options(stage):
    """HdUE render mode and samples from the stage's render settings prim, else the defaults."""
    prim = _settings_prim(stage)

    def get(name, default):
        attr = prim.GetAttribute("hdUE:" + name) if prim else None
        return attr.Get() if attr and attr.HasAuthoredValue() else default

    return get("renderMode", "realtime"), get("samplesPerPixel", 64)


def libraries(stage, node=None):
    """(cooked MaterialX, prepared Substrate) library files: hdUE:materialxLibrary and hdUE:substrateLibrary on the
    stage's render settings prim, as husk sees them; else an older hdUE_settings subnet's fields, HDUE_*_LIBRARY or
    the runtime profile's materialx_library / substrate_library. None where there is none.
    """
    prim = _settings_prim(stage)
    result = []
    for name in ("materialx", "substrate"):
        attr = prim.GetAttribute(f"hdUE:{name}Library") if prim else None
        value = attr.Get() if attr and attr.HasAuthoredValue() else None
        parm = node.parm(f"hdue_{name}_library") if node else None
        value = (
            getattr(value, "path", value)
            or (parm.eval() if parm else None)
            or os.environ.get(f"HDUE_{name.upper()}_LIBRARY")
            or runtime_config().get(f"{name}_library")
        )
        result.append(value or None)
    return tuple(result)


def open_usd(path):
    stage = hou.node("/stage")
    if stage is None:
        raise RuntimeError("Solaris /stage network is unavailable")
    source = stage.createNode("sublayer", "hdUE_source")
    source.parm("filepath1").set(str(Path(path).resolve()))
    node = render_settings_node(settings_node(source))
    node.setDisplayFlag(True)
    node.setCurrent(True, clear_all_selected=True)
    stage.layoutChildren()
    viewer = scene_viewer()
    viewer.setPwd(stage)
    _report("USD loaded at " + node.path())
    activate()
    import hdefereval
    from PySide6 import QtCore

    QtCore.QTimer.singleShot(
        500, lambda: hdefereval.execute_deferred(viewer.curViewport().frameAll)
    )
    return node


def set_mode(mode):
    if mode not in ("realtime", "pathtraced"):
        raise ValueError(mode)
    set_render_setting("xn__hdUErenderMode", mode)


def set_render_setting(prefix, value):
    """Author an hdUE:* setting on the displayed chain's Render Settings LOP, adding one if there is none."""
    viewer = scene_viewer()
    network = viewer.pwd()
    display = (
        network.displayNode()
        if isinstance(network, hou.LopNetwork)
        else viewer.currentNode()
    )
    node = display
    while node and node.type().name() not in RENDER_SETTINGS_TYPES:
        inputs = node.inputs()
        node = inputs[0] if inputs else None
    if node:
        _render_property(node, prefix, value)
    else:
        if not display:
            raise RuntimeError("Display a LOP network in the Scene Viewer")
        node = render_settings_node(display)
        _render_property(node, prefix, value)
        node.setDisplayFlag(True)
    node.cook(force=True)
    viewer.curViewport().draw()
