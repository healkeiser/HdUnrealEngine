"""Streamed scene uploads in husk: the Cornell box path traced with every mesh uploaded in its own streaming batch
(HDUE_STREAM_BYTES=1, as a scene larger than a batch streams while Hydra syncs it) renders as the whole-scene upload.

    python tests/husk_streaming.py [--runtime-config <profile>]

Streaming batches convert their meshes after the state lock is released (delegate.cpp, convertDeferred); the timing
trace must show them, and the two renders must match.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import OpenImageIO as oiio

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from local_paths import local_path  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument(
    "--runtime-config", type=Path, default=ROOT / ".local/runtime-houdini.json"
)
ARGS = parser.parse_args()
SCENE = ROOT / "samples/cornell/cornell.usda"


def husk(out, extra_env):
    """Path traces the Cornell box's frame 1: (stderr, RGB image)."""
    root = out / "root.usda"
    # The scene's own metadata, path tracing for a deterministic image.
    root.write_text(
        "#usda 1.0\n(\n"
        f"    subLayers = [@{SCENE.as_posix()}@]\n"
        '    metersPerUnit = 1\n    upAxis = "Z"\n    renderSettingsPrimPath = "/Render/Settings"\n)\n'
        'over "Render"\n{\n    over "Settings"\n    {\n        token unreal:renderMode = "pathtraced"\n    }\n}\n',
        encoding="utf-8",
    )
    houdini = local_path("houdini_root")
    env = dict(
        os.environ,
        HFS=str(houdini),
        HOUDINI_PATH=f"{ROOT / 'integrations/houdini'};&",
        PXR_PLUGINPATH_NAME=str(ROOT / "build/houdini/resources"),
        HDUE_RUNTIME_CONFIG=str(ARGS.runtime_config.resolve()),
        HDUE_HOUDINI_ENABLE="1",
        **extra_env,
    )
    env.pop("HDUE_TRACE_FILE", None)  # the trace goes to stderr
    result = subprocess.run(
        [
            str(houdini / "bin/husk.exe"),
            "--renderer",
            "HdUnrealEngine",
            "-f",
            "1",
            "-n",
            "1",
        ]
        + ["-o", "beauty.exr", "root.usda"],
        cwd=out,
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=600,
    )
    if result.returncode or not (out / "beauty.exr").exists():
        raise SystemExit(
            f"husk failed ({result.returncode}):\n{result.stderr[-3000:]}"
        )
    return (
        result.stderr,
        oiio.ImageBuf(str(out / "beauty.exr")).get_pixels(oiio.FLOAT)[..., :3],
    )


with (
    tempfile.TemporaryDirectory() as whole_dir,
    tempfile.TemporaryDirectory() as stream_dir,
):
    _, whole = husk(Path(whole_dir), {})
    trace, streamed = husk(
        Path(stream_dir), {"HDUE_STREAM_BYTES": "1", "HDUE_TRACE_TIMING": "1"}
    )
batches = [line for line in trace.splitlines() if "scene convert:" in line]
assert (
    len(batches) >= 3
), f"expected one streaming batch per mesh, trace:\n{trace[-3000:]}"
difference = float(np.abs(streamed - whole).max())
assert whole.mean() > 0.01, "the whole-scene render is black"
assert (
    difference < 1e-3
), f"streamed render differs from the whole-scene upload by up to {difference}"
print(
    f"husk streaming: ok ({len(batches)} batches, max difference {difference:g})"
)
