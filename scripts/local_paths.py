"""Python twin of Local-Paths.ps1: machine-specific paths from the environment or the ignored .local/paths.json."""

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VARIABLES = dict(
    engine_root="HDUE_ENGINE_ROOT",
    editor_root="HDUE_EDITOR_ROOT",
    openusd_root="HDUE_OPENUSD_ROOT",
    houdini_root="HDUE_HOUDINI_ROOT",
    runtime_config="HDUE_RUNTIME_CONFIG",
    review_scene="HDUE_REVIEW_SCENE",
    houdini_build_root="HDUE_HOUDINI_BUILD_ROOT",
)


def local_path(key):
    """The environment variable (HFS for houdini_root), else .local/paths.json (relative to it), else the default."""
    value = os.environ.get(VARIABLES[key]) or (
        os.environ.get("HFS") if key == "houdini_root" else None
    )
    if value:
        return Path(value).resolve()
    file = Path(
        os.environ.get("HDUE_LOCAL_CONFIG") or ROOT / ".local/paths.json"
    )
    value = (
        json.loads(file.read_text(encoding="utf-8-sig")).get(key)
        if file.is_file()
        else None
    )
    if value:
        return (file.parent / value).resolve()
    if key == "houdini_build_root":
        return ROOT / "build/houdini"
    raise SystemExit(
        f"Configure {key} in .local/paths.json or set {VARIABLES[key]}."
    )


def relaunch(script, **env):
    """Run script again in hython with HDUE_BUILD_HDA=1 and env, and exit with its code."""
    hython = local_path("houdini_root") / "bin/hython.exe"
    sys.exit(
        subprocess.run(
            [str(hython), str(script)],
            env=dict(os.environ, HDUE_BUILD_HDA="1", **env),
        ).returncode
    )
