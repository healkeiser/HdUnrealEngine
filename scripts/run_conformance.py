"""Renders the spec-driven conformance scenes (samples/conformance/<area>/<feature>.usda) with hdUE through husk,
checks each scene's expected results and prints a pass/fail table; writes out/conformance/report.json and, with
--matrix, the support matrix in docs/conformance.md.

    hython scripts/run_conformance.py [--only PATTERN ...] [--mode realtime|pathtraced] [--matrix]
        [--houdini HFS] [--build DIR] [--integrations DIR] [--runtime-config FILE] [--timeout SECONDS]

Each scene's customLayerData["conformance"] is a JSON string: what the spec says is in the layer's doc string, what
to verify is "checks" (see `measure` and `verify`), extra husk options are "husk" (render_settings/render_pass).
Exits 1 when a check fails or a render does not finish.
"""

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import OpenImageIO as oiio

sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_paths import local_path
from render_gallery import override_layer, stage_metadata

ROOT = Path(__file__).resolve().parent.parent
SCENES = ROOT / "samples" / "conformance"
OUT = ROOT / "out" / "conformance"
DOC = ROOT / "docs" / "conformance.md"
MARKERS = ("<!-- matrix:start -->", "<!-- matrix:end -->")
MODES = ("realtime", "pathtraced")
LUMA = np.array([0.2126, 0.7152, 0.0722])
VIEW = 5.0  # the shared camera frames x, y in [-VIEW, VIEW] on the z = 0 plane
METADATA = ":metadata"  # a frame's image metadata, by name, next to its vars


def scenes(patterns):
    for path in sorted(SCENES.glob("*/*.usda")):
        name = f"{path.parent.name}/{path.stem}"
        if path.parent.name == "common":
            continue
        if not patterns or any(
            fnmatch.fnmatch(name, p) or p in name for p in patterns
        ):
            yield name, path


def spec_of(path):
    from pxr import Sdf

    layer = Sdf.Layer.FindOrOpen(str(path))
    spec = json.loads(layer.customLayerData["conformance"])
    spec["doc"] = " ".join(layer.documentation.split())
    return spec


def render(path, mode, frames, out, env, timeout, extra=()):
    """Renders `frames` frames of `path` in `mode` into `out` (with husk options `extra`); returns (seconds, log,
    error).
    """
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    root = override_layer(
        out / "root.usda",
        [path],
        stage_metadata(path),
        [f'token unreal:renderMode = "{mode}"'],
    )
    command = [
        str(env["HFS"] + "/bin/husk.exe"),
        "--renderer",
        "HdUnrealEngine",
        "-f",
        "1",
        "-n",
        str(frames),
        "-V",
        "2",
        # husk adds its own headlight when no light is visible (lux/invisible_light); the scenes test USD only.
        "--headlight",
        "none",
        # Tf warnings (TF_WARN) reach husk's output only in its end-of-render summary.
        "--error-summary",
        "force",
        # Each frame is its own shot: a sequence keeps UE's temporal history as Movie Render Queue does, so a light
        # switched between frames would lag a few frames, where the scenes test the switched state.
        "--restart-delegate",
        "1",
        *extra,
        str(root),
    ]
    start = time.perf_counter()
    try:
        result = subprocess.run(
            command,
            cwd=out,
            env=env,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
        )
        log, error = result.stdout + result.stderr, (
            f"husk exited with {result.returncode}"
            if result.returncode
            else None
        )
    except subprocess.TimeoutExpired as expired:  # run() kills husk
        log = (expired.stdout or b"").decode(errors="replace")
        error = f"timed out after {timeout} s"
    (out / "husk.log").write_text(log, encoding="utf-8")
    return time.perf_counter() - start, log, error


def read_product(path):
    """{var name: array} for every subimage over the full display window (a data window's outside reads 0); integer
    vars come back as int32 (-1 = no prim).
    """
    images, sub = {METADATA: {}}, 0
    while True:
        buf = oiio.ImageBuf(str(path), sub, 0)
        if buf.has_error or buf.nsubimages <= sub:
            break
        spec, full = buf.spec(), buf.spec().roi_full
        images[METADATA].update({a.name: a.value for a in spec.extra_attribs})
        name = spec.getattribute("name") or "C"
        if spec.format == oiio.TypeDesc("uint"):
            pixels = buf.get_pixels(oiio.UINT32, full).astype(np.uint32)
            pixels = pixels.view(np.int32)[..., 0]
        else:
            pixels = buf.get_pixels(oiio.FLOAT, full)
        images[name] = pixels.reshape(full.height, full.width, -1).squeeze()
        sub += 1
    return images


def frame_images(out, frame):
    images = {METADATA: {}}
    for path in sorted(out.glob(f"*.{frame:04d}.exr")):
        product = read_product(path)
        images[METADATA].update(product.pop(METADATA))
        images.update(product)
    return images


def cryptomatte(pixels):
    """Rank-0 ids (float32 bits as uint32; 0 = none) and the summed coverage of a Cryptomatte layer's two ranks."""
    ids = np.ascontiguousarray(pixels[..., 0], dtype=np.float32).view(np.uint32)
    return ids, pixels[..., 1] + pixels[..., 3]


def region(image, box):
    """Pixels of `box` = [x0, y0, x1, y1] in view units (-5..5, y up) of the shared camera's frame."""
    height, width = image.shape[:2]
    x0, y0, x1, y1 = box or (-VIEW, -VIEW, VIEW, VIEW)
    column = lambda x: int(round((x + VIEW) / (2 * VIEW) * width))
    row = lambda y: int(round((VIEW - y) / (2 * VIEW) * height))
    return image[
        row(y1) : max(row(y0), row(y1) + 1),
        column(x0) : max(column(x1), column(x0) + 1),
    ]


def measure(frames, probe):
    """One number (or an RGB triple for "rgb") from a probe: {"frame": 1, "region": box, "aov": "C", "stat": "luma"}."""
    images = frames[probe.get("frame", 1)]
    stat = probe.get("stat", "luma")
    aov = probe.get("aov", "primId" if stat in ("ids", "instances") else "C")
    for name in [aov] + (["instanceId"] if stat == "instances" else []):
        if name not in images:
            raise KeyError(f"no {name} in frame {probe.get('frame', 1)}")
    image = images[aov]
    if stat == "width":
        return image.shape[1]
    if stat == "height":
        return image.shape[0]
    pixels = region(image, probe.get("region"))
    if stat in ("ids", "instances"):
        # Distinct primId (or primId, instanceId pairs) covering at least 3 pixels: filtered edges can mix.
        if stat == "instances":
            instance = region(images["instanceId"], probe.get("region"))
            pixels = pixels.astype(np.int64) << 32 | (
                instance.astype(np.int64) & 0xFFFFFFFF
            )
        values, counts = np.unique(pixels[pixels >= 0], return_counts=True)
        return int((counts >= 3).sum())
    if stat.startswith("crypto_"):
        # A Cryptomatte layer: distinct rank-0 ids over at least 3 pixels, the most common one (hex), the mean
        # coverage of ranks 0 and 1, or the share of pixels whose rank-0 coverage is partial (anti-aliasing).
        ids, coverage = cryptomatte(pixels)
        values, counts = np.unique(ids[ids != 0], return_counts=True)
        return {
            "crypto_ids": lambda: int((counts >= 3).sum()),
            "crypto_id": lambda: (
                f"{int(values[counts.argmax()]):08x}" if len(values) else ""
            ),
            "crypto_coverage": lambda: float(coverage.mean()),
            "crypto_soft": lambda: float(
                ((pixels[..., 1] > 0.01) & (pixels[..., 1] < 0.99)).mean()
            ),
        }[stat]()
    if "channel" in probe:
        return float(pixels[..., probe["channel"]].mean())
    if image.ndim == 2:
        return float(pixels.mean() if stat != "max" else pixels.max())
    rgb = pixels[..., :3].astype(np.float64)
    alpha = pixels[..., 3] if pixels.shape[-1] > 3 else np.ones(rgb.shape[:-1])
    if stat == "partial":
        # Share of pixels between 5% and 95% of the region's brightest: filtered (anti-aliased) edges.
        luma = rgb @ LUMA
        return float(
            ((luma > 0.05 * luma.max()) & (luma < 0.95 * luma.max())).mean()
        )
    if stat in (
        "bbox_aspect",
        "bbox_height",
    ):  # covered pixels' bounding box: width / height, or height / image's
        rows, columns = np.nonzero(alpha > 0.5)
        if not len(rows):
            return 0.0
        if stat == "bbox_height":
            return (np.ptp(rows) + 1) / alpha.shape[0]
        return (np.ptp(columns) + 1) / (np.ptp(rows) + 1)
    return {
        "luma": lambda: float((rgb @ LUMA).mean()),
        "max": lambda: float((rgb @ LUMA).max()),
        "alpha": lambda: float(alpha.mean()),
        "coverage": lambda: float((alpha > 0.5).mean()),
        "soft": lambda: float(((alpha > 0.05) & (alpha < 0.95)).mean()),
        "rgb": lambda: rgb.reshape(-1, 3).mean(0).tolist(),
    }[stat]()


def verify(check, frames, log):
    """(value, passed) for one check: "log" (regex expected in husk's output, or absent with "present": false),
    "value" (a probe), "ratio" (two probes) or "color" (a probe's normalized RGB against "rgb" within "tol").
    """
    if "log" in check:
        found = re.search(check["log"], log) is not None
        return found, found == check.get("present", True)
    if "metadata" in check:  # an image metadata value matching a regex
        value = str(
            frames[check.get("frame", 1)][METADATA].get(check["metadata"], "")
        )
        return value[:80], re.search(check["match"], value) is not None
    if "color" in check:
        rgb = np.array(measure(frames, {**check["color"], "stat": "rgb"}))
        want = np.array(check["rgb"], dtype=np.float64)
        if rgb.sum() <= 1e-6:
            return [0, 0, 0], False
        got = rgb / rgb.sum()
        distance = float(np.abs(got - want / want.sum()).max())
        return [round(float(v), 3) for v in got], distance <= check.get(
            "tol", 0.12
        )
    if "ratio" in check:
        a, b = (measure(frames, p) for p in check["ratio"])
        value = a / b if b else float("inf") if a else 1.0
    else:
        value = measure(frames, check["value"])
    if "equals" in check:
        return value, value == check["equals"]
    return value, bool(
        check.get("min", -np.inf) <= value <= check.get("max", np.inf)
    )


def run_scene(name, path, spec, mode, env, timeout):
    frames = spec.get("frames", 1)
    out = OUT / name / mode
    seconds, log, error = render(
        path, mode, frames, out, env, timeout, spec.get("husk", ())
    )
    entry = {"seconds": round(seconds, 1), "checks": []}
    loaded = {}
    for frame in range(1, frames + 1):
        loaded[frame] = frame_images(out, frame)
    if error and not any(loaded.values()):
        entry["error"] = error
        return entry
    for check in spec["checks"]:
        if mode not in check.get("modes", MODES):
            continue
        try:
            value, passed = verify(check, loaded, log)
        except (KeyError, IndexError, ValueError) as failure:
            value, passed = f"error: {failure}", False
        if isinstance(value, float):
            value = round(value, 4)
        entry["checks"].append(
            {"name": check["name"], "value": value, "pass": passed}
            | {k: check[k] for k in ("min", "max", "equals") if k in check}
            | ({"baseline": True} if check.get("baseline") else {})
        )
    if error:
        entry["error"] = error
    return entry


def status(entry):
    if "error" in entry and not entry["checks"]:
        return "error"
    passed = sum(c["pass"] for c in entry["checks"])
    if passed == len(entry["checks"]) and "error" not in entry:
        return "pass"
    return "fail" if not passed else f"partial {passed}/{len(entry['checks'])}"


def environment(args):
    env = dict(os.environ)
    env.update(
        HFS=str(args.houdini),
        HOUDINI_PATH=f"{args.integrations};&",
        PXR_PLUGINPATH_NAME=str(args.build / "resources"),
        HDUE_RUNTIME_CONFIG=str(args.runtime_config),
        HDUE_HOUDINI_ENABLE="1",
    )
    return env


def matrix(report, specs):
    """The support matrix rows, one per scene: schema, feature, scene link, result per mode, failed checks."""
    rows = [
        "| Schema | Feature | Scene | Realtime | Path traced | Failed checks |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for name, spec in specs.items():
        results = report.get(name, {})
        cells = [
            status(results[m]) if m in results else "not run" for m in MODES
        ]
        failed = sorted(
            {
                f"{c['name']} ({m})"
                for m, entry in results.items()
                for c in entry["checks"]
                if not c["pass"]
            }
            | {
                f"{entry['error']} ({m})"
                for m, entry in results.items()
                if "error" in entry
            }
        )
        rows.append(
            f"| {spec.get('schema', '')} | {spec.get('feature', name)} | "
            f"[{name}](../samples/conformance/{name}.usda) | {cells[0]} | {cells[1]} | "
            f"{'; '.join(failed)} |"
        )
    return "\n".join(rows)


def write_matrix(report, specs):
    text = DOC.read_text(encoding="utf-8")
    start, end = MARKERS
    head, rest = text.split(start, 1)
    DOC.write_text(
        f"{head}{start}\n{matrix(report, specs)}\n{end}{rest.split(end, 1)[1]}",
        encoding="utf-8",
        newline="\n",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--only", nargs="*", help="scene names (area/feature) or glob patterns"
    )
    parser.add_argument(
        "--mode",
        choices=MODES,
        action="append",
        help="render modes (default both)",
    )
    parser.add_argument(
        "--matrix",
        action="store_true",
        help="rewrite the matrix in docs/conformance.md",
    )
    parser.add_argument(
        "--houdini",
        type=Path,
        default=None,
        help="Houdini install (default HFS or .local/paths.json)",
    )
    parser.add_argument(
        "--build",
        type=Path,
        default=None,
        help="hdUE Houdini build root (default build/houdini)",
    )
    parser.add_argument(
        "--integrations",
        type=Path,
        default=ROOT / "integrations" / "houdini",
        help="HOUDINI_PATH entry with hdUE's Houdini integration",
    )
    parser.add_argument(
        "--runtime-config",
        type=Path,
        default=None,
        help="runtime profile (default HDUE_RUNTIME_CONFIG or .local/runtime-houdini.json)",
    )
    parser.add_argument(
        "--timeout", type=float, default=180, help="seconds per husk run"
    )
    args = parser.parse_args()
    args.houdini = args.houdini or local_path("houdini_root")
    args.build = args.build or local_path("houdini_build_root")
    args.runtime_config = args.runtime_config or Path(
        os.environ.get("HDUE_RUNTIME_CONFIG")
        or ROOT / ".local" / "runtime-houdini.json"
    )
    env = environment(args)
    report_path = OUT / "report.json"
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    specs = {name: spec_of(path) for name, path in scenes(None)}
    failures, start = 0, time.perf_counter()
    print(f"{'scene':42} {'mode':10} {'s':>6}  result", flush=True)
    for name, path in scenes(args.only):
        spec = specs[name]
        for mode in args.mode or spec.get("modes", MODES):
            entry = run_scene(name, path, spec, mode, env, args.timeout)
            report.setdefault(name, {})[mode] = entry
            result = status(entry)
            failures += result != "pass"
            print(
                f"{name:42} {mode:10} {entry['seconds']:6.1f}  {result}",
                flush=True,
            )
            for check in entry["checks"]:
                if not check["pass"]:
                    limits = {
                        k: check[k]
                        for k in ("min", "max", "equals")
                        if k in check
                    }
                    print(
                        f"    FAIL {check['name']}: {check['value']} {limits or ''}",
                        flush=True,
                    )
            if "error" in entry:
                print(f"    ERROR {entry['error']}", flush=True)
            OUT.mkdir(parents=True, exist_ok=True)
            report_path.write_text(
                json.dumps(report, indent=1), encoding="utf-8"
            )
    print(
        f"{failures} failing runs, {time.perf_counter() - start:.0f} s; report: {report_path}"
    )
    if args.matrix:
        write_matrix(report, specs)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
