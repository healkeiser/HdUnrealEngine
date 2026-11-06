"""Renders the documentation's setting comparisons with hdUE through husk: one image per value of one setting, on the
CC0 Cornell box (samples/cornell) and its feature layers, written to docs/images/settings/<figure>/<n>.jpg, and
docs/images/settings/figures.json (each figure's captions). A page shows a figure with
<div class="hdue-figure" data-figure="<name>"></div>: docs/assets/hdue.js draws a before/after slider for two
values and a value scrubber for more.

    hython scripts/render_docs_images.py [--only <figure> ...] [--list]

Each run renders only what it is asked for; commit the images with the page that shows them.
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from render_gallery import (  # noqa: E402  (same husk, layers and display as the gallery)
    CORNELL,
    ROOT,
    dev_environment,
    display,
    husk,
    pixels,
    stage_metadata,
)

OUT = ROOT / "out" / "docs-images"
IMAGES = ROOT / "docs" / "images" / "settings"
SETTINGS = "/Render/Settings"
CAMERA = "/cornell_box/camera"
REALTIME = {SETTINGS: ['token unreal:renderMode = "realtime"']}
TONEMAPPED = {SETTINGS: ["bool unreal:tonemapper = 1"]}
NO_DENOISER = {
    SETTINGS: ["bool unreal:postProcess:PathTracingEnableDenoiser = 0"]
}


def path_tracer(samples):
    return {SETTINGS: [f"int unreal:samplesPerPixel = {samples}"]}


# name: (layer under samples/cornell, base overrides, [(caption, overrides)], tonemapped display).
# The Cornell box renders with the Path Tracer at 512 spp unless a base or value says otherwise.
FIGURES = {
    "render-mode": (
        "cornell.usda",
        {},
        [("Realtime (Lumen)", REALTIME), ("Path Tracer", {})],
        False,
    ),
    "samples": (
        "cornell.usda",
        NO_DENOISER,
        [(f"{n} spp", path_tracer(n)) for n in (4, 16, 64, 256, 1024)],
        False,
    ),
    "denoiser": (
        "cornell.usda",
        path_tracer(16),
        [("Denoiser off", NO_DENOISER), ("Denoiser on (NNEDenoiser)", {})],
        False,
    ),
    "warmup": (
        "cornell.usda",
        REALTIME,
        [
            (
                f"{n} warmup frames",
                {SETTINGS: [f"int unreal:warmupFrames = {n}"]},
            )
            for n in (1, 4, 16, 64)
        ],
        False,
    ),
    "global-illumination": (
        "cornell.usda",
        REALTIME,
        [
            (
                "No global illumination",
                {
                    SETTINGS: [
                        'token unreal:postProcess:DynamicGlobalIlluminationMethod = "EDynamicGlobalIlluminationMethod::None"'
                    ]
                },
            ),
            ("Lumen", {}),
        ],
        False,
    ),
    "tonemapper": (
        "cornell.usda",
        REALTIME,
        [("Tonemapper off (scene-linear)", {}), ("Tonemapper on", TONEMAPPED)],
        True,
    ),
    "exposure-compensation": (
        "cornell.usda",
        {SETTINGS: REALTIME[SETTINGS] + TONEMAPPED[SETTINGS]},
        [
            (
                f"{v:+g} EV",
                {
                    SETTINGS: [
                        f"float unreal:postProcess:AutoExposureBias = {v}"
                    ]
                },
            )
            for v in (-2, -1, 0, 1, 2)
        ],
        True,
    ),
    "bloom": (
        "cornell.usda",
        {SETTINGS: REALTIME[SETTINGS] + TONEMAPPED[SETTINGS]},
        [
            (
                f"Intensity {v:g}",
                {SETTINGS: [f"float unreal:postProcess:BloomIntensity = {v}"]},
            )
            for v in (0, 0.675, 2, 8)
        ],
        True,
    ),
    "depth-of-field": (
        "variants/depth_of_field.usda",
        REALTIME,
        [
            (f"f/{v:g}", {CAMERA: [f"float fStop = {v}"]})
            for v in (1, 2, 4, 8, 16)
        ],
        False,
    ),
    "motion-blur": (
        "variants/motion_blur.usda",
        REALTIME,
        [
            ("Motion blur off", {SETTINGS: ["bool unreal:motionBlur = 0"]}),
            ("Motion blur on", {SETTINGS: ["bool unreal:motionBlur = 1"]}),
        ],
        False,
    ),
    "subdivision": (
        "variants/subdivision.usda",
        REALTIME,
        [
            (f"Level {n}", {SETTINGS: [f"int unreal:subdivisionLevel = {n}"]})
            for n in (0, 1, 2, 3)
        ],
        False,
    ),
    "fog-density": (
        "variants/height_fog.usda",
        REALTIME,
        [
            (
                f"Density {v:g}",
                {SETTINGS: [f"float unreal:heightFog:FogDensity = {v}"]},
            )
            for v in (0, 0.02, 0.5, 2)
        ],
        False,
    ),
}


def merge(*overrides):
    out = {}
    for o in overrides:
        for path, lines in o.items():
            out.setdefault(path, []).extend(lines)
    return out


def overs(overrides):
    """Nested `over` prims for {prim path: [property lines]}."""
    tree = {}
    for path, lines in overrides.items():
        node = tree
        for name in path.strip("/").split("/"):
            node = node.setdefault(name, {})
        node.setdefault("", []).extend(lines)

    def write(node, indent):
        text = "".join(f"{indent}{line}\n" for line in node.get("", []))
        for name, child in node.items():
            if name:
                text += f'{indent}over "{name}"\n{indent}{{\n{write(child, indent + "    ")}{indent}}}\n'
        return text

    return write(tree, "")


def render(name):
    layer_name, base, values, tonemapped = FIGURES[name]
    layer = CORNELL / layer_name
    work = OUT / name
    work.mkdir(parents=True, exist_ok=True)
    target = IMAGES / name
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    from PIL import Image

    for index, (caption, override) in enumerate(values):
        root = work / f"{index}.usda"
        root.write_text(
            f"#usda 1.0\n(\n{stage_metadata(layer)}    subLayers = [@{layer.as_posix()}@]\n)\n\n"
            + overs(merge(base, override)),
            encoding="utf-8",
        )
        exr = work / f"{index}.exr"
        seconds = husk("Unreal Engine", root, exr)
        image = (display(pixels(exr), tonemapped) * 255 + 0.5).astype(np.uint8)
        Image.fromarray(image).save(
            target / f"{index}.jpg", quality=90, optimize=True
        )
        print(f"{name} {index}: {caption} ({seconds:.1f} s)", flush=True)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--only",
        nargs="+",
        choices=FIGURES,
        help="figures to render (default: all)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="print the figures and their captions",
    )
    args = parser.parse_args()
    if args.list:
        for name, (_, _, values, _) in FIGURES.items():
            print(name + ": " + " | ".join(caption for caption, _ in values))
        return
    IMAGES.mkdir(parents=True, exist_ok=True)
    (IMAGES / "figures.json").write_text(
        json.dumps(
            {
                name: [caption for caption, _ in f[2]]
                for name, f in FIGURES.items()
            },
            indent=1,
        )
        + "\n",
        encoding="utf-8",
    )
    dev_environment()
    for name in args.only or FIGURES:
        render(name)


if __name__ == "__main__":
    main()
