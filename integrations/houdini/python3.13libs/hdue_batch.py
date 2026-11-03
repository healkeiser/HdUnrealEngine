"""Prepared materials for batch renders (husk): the delegate calls resolve() with the stage it renders, and applies
what the Solaris viewport's host applies (hdue_materialx.PreparedMaterials): the cooked MaterialX and Substrate Builder
assets bound to its materials (nativeMaterialAssets), their parameters (nativeMaterialParameters) and the wet surface
bindings (surfaceEffects). The same library classes decide, so husk renders what the viewport shows.
"""

import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))


def _profile_library(key):
    """The runtime profile's library (materialx_library, substrate_library), as the viewport host falls back to."""
    config = os.environ.get("HDUE_RUNTIME_CONFIG")
    if not config:
        return ""
    path = Path(config).resolve()
    value = json.loads(path.read_text(encoding="utf-8-sig")).get(key)
    if not value:
        return ""
    value = Path(value)
    return str(
        value if value.is_absolute() else (path.parent / value).resolve()
    )


def _plain(value):
    """A setting value as JSON: Gf vectors as lists, dictionaries recursively."""
    if isinstance(value, dict):
        return {k: _plain(v) for k, v in value.items()}
    if isinstance(value, (str, bool, int, float)) or value is None:
        return value
    return [float(c) for c in value]  # Gf.Vec2f/3f/4f


def resolve(stage, time, materialx_file="", substrate_file=""):
    """The three settings for stage at time, from the MaterialX library and the Substrate Builder library (the render
    settings' unreal:materialxLibrary and unreal:substrateLibrary, else the runtime profile's). Raises ValueError with
    the library's message when a material is unprepared or stale.
    """
    from materialx_controls import MaterialXLibrary, OfflineView
    from substrate_library import SubstrateLibrary
    from surface_effects import resolve as resolve_effects

    materialx_file = materialx_file or _profile_library("materialx_library")
    substrate_file = substrate_file or _profile_library("substrate_library")
    view = OfflineView(stage, time, object())
    entries = {}
    if materialx_file:
        library = MaterialXLibrary(Path(materialx_file), view)
        if not library.sync():
            raise ValueError(library.preparation_error)
        entries = library.bound_entries
    else:
        view.settings["surfaceEffects"], _ = resolve_effects(stage, {}, time)
    SubstrateLibrary(Path(substrate_file) if substrate_file else None).sync(
        stage, time, entries, view.settings
    )
    return {
        key: _plain(view.settings.get(key) or {})
        for key in (
            "nativeMaterialAssets",
            "nativeMaterialParameters",
            "surfaceEffects",
        )
    }


def resolve_json(stage_id, frame, materialx_file="", substrate_file=""):
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
            resolve(stage, Usd.TimeCode(frame), materialx_file, substrate_file)
        )
    except (
        Exception
    ) as error:  # every failure fails the frame, with its message
        return json.dumps({"error": f"{type(error).__name__}: {error}"})
