"""Prepared materials for batch renders (husk): the delegate calls resolve_json() with the stage it renders, and applies
what the Solaris viewport's host applies (hdue_materialx.PreparedMaterials): the cooked MaterialX and Substrate Builder
assets bound to its materials (nativeMaterialAssets), their parameters (nativeMaterialParameters) and the wet surface
bindings (surfaceEffects). The same library classes decide, so husk renders what the viewport shows. Also the runtime
profile and library lookup the viewport host (hdue_houdini) shares; this module does not need hou.
"""

import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))


def runtime_config():
    """The runtime profile (HDUE_RUNTIME_CONFIG) with its paths made absolute."""
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


def library_file(name, value=None):
    """The library file for name ("materialx" or "substrate"): value (the render setting, a path or an asset path),
    else HDUE_<NAME>_LIBRARY, else the runtime profile's <name>_library. None where there is none.
    """
    value = getattr(value, "path", value)
    if not value:
        value = os.environ.get(f"HDUE_{name.upper()}_LIBRARY")
    if not value and os.environ.get("HDUE_RUNTIME_CONFIG"):
        value = runtime_config().get(f"{name}_library")
    return value or None


def configured(materialx_file="", substrate_file="", prepare=True):
    """Whether any library applies (the delegate checks this when it has no stage to resolve). prepare is resolve()'s,
    which the delegate passes alike.
    """
    return bool(
        library_file("materialx", materialx_file)
        or library_file("substrate", substrate_file)
    )


def _plain(value):
    """A setting value as JSON: Gf vectors as lists, dictionaries recursively."""
    if isinstance(value, dict):
        return {k: _plain(v) for k, v in value.items()}
    if isinstance(value, (str, bool, int, float)) or value is None:
        return value
    return [float(c) for c in value]  # Gf.Vec2f/3f/4f


def _identity(file):
    """A library file as cached: (path, mtime), or None for no library."""
    if not file:
        return None
    path = Path(file).resolve()
    return str(path), path.stat().st_mtime_ns


class _Libraries:
    """The libraries of the last resolve, kept while their files are unchanged: loading one checks the hash of every
    dependency, as the viewport host does once per file (hdue_materialx.PreparedMaterials). Its view keeps the settings
    between frames, which the libraries read to replace what they set before.
    """

    def __init__(self, materialx, substrate):
        from materialx_controls import MaterialXLibrary, OfflineView
        from substrate_library import SubstrateLibrary

        self.key = (materialx, substrate)
        self.view = OfflineView(None, None, object())
        self.materialx = (
            MaterialXLibrary(Path(materialx[0]), self.view)
            if materialx
            else None
        )
        self.substrate = SubstrateLibrary(
            Path(substrate[0]) if substrate else None
        )
        self.substrate_base = list(self.substrate.materials)
        self.live = None

    def prepare(self, stage, time):
        """Prepares the stage's Substrate Builder graphs that the library lacks, as the viewport host does while you
        edit (substrate_live.LivePreparation: UE's editor compiles them into the runtime), and waits for them: a batch
        render has no later frame to show them on.
        """
        import tempfile
        import time as clock

        from prepare_substrate import collect
        from substrate_live import LivePreparation

        if self.live is None:
            self.live = LivePreparation(
                runtime_config()["runtime_project"],
                Path(tempfile.gettempdir()) / "hdue-husk",
                debounce=0,
            )
        live = self.live
        records = collect(
            stage, time, self.materialx.bound_entries if self.materialx else {}
        )
        deadline = clock.monotonic() + 1800
        while live.request(
            records, self.substrate_base + list(live.entries.values())
        ):
            if live.phase == "failed":
                raise ValueError(live.message)
            if clock.monotonic() > deadline:
                raise ValueError("Substrate preparation took over 30 minutes")
            clock.sleep(0.5)
        self.substrate.materials = self.substrate_base + list(
            live.entries.values()
        )


_LIBRARIES = None


def resolve(stage, time, materialx_file="", substrate_file="", prepare=True):
    """The three settings for stage at time, from the MaterialX and Substrate Builder libraries (library_file), as the
    viewport host applies them, and whether they are the same at every time (no prepared material and no wet surface:
    material bindings cannot be animated). With prepare (the render setting unreal:substrateAutoPrepare), Substrate
    Builder graphs the library lacks are prepared first, as the viewport prepares them. Raises ValueError with the
    library's message when a material is unprepared (and not prepared) or stale.
    """
    global _LIBRARIES
    from prepared_materials import sync
    from substrate_library import NeedsPreparation

    key = (
        _identity(library_file("materialx", materialx_file)),
        _identity(library_file("substrate", substrate_file)),
    )
    if _LIBRARIES is None or _LIBRARIES.key != key:
        _LIBRARIES = None
        _LIBRARIES = _Libraries(*key)
    libraries = _LIBRARIES
    view = libraries.view
    view._dataModel.stage, view._dataModel.currentFrame = stage, time
    if libraries.materialx:
        libraries.materialx.invalidate()
    try:
        settings = sync(
            stage, time, libraries.materialx, libraries.substrate, view
        )
    except NeedsPreparation:
        if not prepare:
            raise
        libraries.prepare(stage, time)
        settings = sync(
            stage, time, libraries.materialx, libraries.substrate, view
        )
    view.settings.update(settings)
    result = {
        key: _plain(view.settings.get(key) or {})
        for key in (
            "nativeMaterialAssets",
            "nativeMaterialParameters",
            "surfaceEffects",
        )
    }
    # shortcut: a wet-surface switch animated on from off is missed after an empty first frame; resolve per frame
    # whenever surface_effects can report an animated attribute.
    result["static"] = not any(result.values())
    return result


def resolve_json(
    stage_id, frame, materialx_file="", substrate_file="", prepare=True
):
    """resolve() for the delegate (the HDK's Python API returns a string): the stage from UsdUtils' stage cache by id,
    the result or {"error": message} as JSON.
    """
    from pxr import Usd, UsdUtils

    try:
        stage = UsdUtils.StageCache.Get().Find(
            Usd.StageCache.Id.FromLongInt(stage_id)
        )
        if not stage:
            raise ValueError(
                "the rendered stage is not in UsdUtils' stage cache"
            )
        return json.dumps(
            resolve(
                stage,
                Usd.TimeCode(frame),
                materialx_file,
                substrate_file,
                prepare,
            )
        )
    except (
        Exception
    ) as error:  # every failure fails the frame, with its message
        return json.dumps({"error": f"{type(error).__name__}: {error}"})
