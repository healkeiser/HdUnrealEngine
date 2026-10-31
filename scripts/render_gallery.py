"""Renders the README gallery with hdUE (Lumen and Path Tracer) and Karma CPU through husk, then writes labelled
comparison images, docs/gallery.md and the README blocks. Two independent sets:

- cornell: the CC0 Cornell box (samples/cornell) and one layer per feature in samples/cornell/variants;
- sponza: Intel's Sponza 2022 (CC BY 4.0, not included) from its main camera, lit by samples/sponza/lighting.usda,
  with the MaterialX materials of samples/sponza/materials.usda.

    hython scripts/render_gallery.py [--set cornell|sponza] [--sponza FILE] [--addon FILE ...] [--dev] [--only NAME ...] [--docs-only] [--bless]

It then fails (exit 1) when a scene's brightness against Karma CPU leaves its band (scripts/check_gallery.py) or a
Cornell hdUE render departs from its tile baseline in tests/gallery_baselines.json; --bless stores new baselines.

Sponza is read from samples/sponza/intel by default (samples/sponza/README.md).

hdUE must be registered (installed package), or pass --dev to use this checkout's build and .local runtime profile.
hdUE times are per frame from the extra time of a 16-frame (Lumen) or 4-frame husk run over a 1-frame run; Karma
times are husk wall times minus Karma's startup, measured on an empty stage.
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import OpenImageIO as oiio

ROOT = Path(__file__).resolve().parent.parent
CORNELL = ROOT / "samples" / "cornell"
SPONZA = ROOT / "samples" / "sponza"
# Intel's download, kept out of git (samples/sponza/README.md): the scene and the add-ons the gallery layers over it.
SPONZA_SCENE = SPONZA / "intel" / "main" / "NewSponza_Main_USD_Yup_003.usda"
# MaterialX networks for the scene and its add-ons (scripts/generate_sponza_materials.py); its textures are Intel's, in
# samples/sponza/intel.
SPONZA_MATERIALS = SPONZA / "materials.usda"
SPONZA_ADDONS = [
    SPONZA / "intel" / "curtains" / "NewSponza_Curtains_USD_YUp.usda",
    SPONZA / "intel" / "cypress_tree" / "NewSponza_CypressTree_USDA_YUp.usda",
]
OUT = ROOT / "out" / "gallery"
IMAGES = ROOT / "docs" / "images" / "gallery"
HUSK = Path(os.environ.get("HFS", "")) / "bin" / "husk.exe"
BASELINES = ROOT / "tests" / "gallery_baselines.json"
# 95th-percentile relative tile error allowed against the baseline: Lumen's temporal GI varies between runs, the
# Path Tracer's tiles agree within 1%.
TILE_LIMITS = {"lumen": 0.3, "pathtraced": 0.05}

# name, title, layer (relative to samples/cornell); captions come from each layer's doc string.
CORNELL_SCENES = [
    ("cornell", "Global illumination", "cornell.usda"),
    ("glossy", "Glossy materials", "variants/glossy.usda"),
    ("depth_of_field", "Depth of field", "variants/depth_of_field.usda"),
    ("motion_blur", "Motion blur", "variants/motion_blur.usda"),
    ("subdivision", "Subdivision surfaces", "variants/subdivision.usda"),
    ("instances", "Instances, points, curves", "variants/instances.usda"),
    ("volume", "Volumes (OpenVDB)", "variants/volume.usda"),
    ("ies", "IES profile", "variants/ies.usda"),
    ("light_linking", "Light linking", "variants/light_linking.usda"),
    ("clearcoat", "Clearcoat", "variants/clearcoat.usda"),
    ("render_region", "Render region", "variants/render_region.usda"),
    ("lens_shift", "Lens shift", "variants/lens_shift.usda"),
    ("orthographic", "Orthographic camera", "variants/orthographic.usda"),
    ("exposure", "Camera exposure", "variants/exposure.usda"),
    (
        "post_processing",
        "Unreal post-processing",
        "variants/post_processing.usda",
    ),
]
SPONZA_CAMERAS = ["PhysCamera001"]  # the main view, Intel's Render_Main_A
RENDERERS = [  # key, label, husk renderer, hdUE render mode
    ("lumen", "hdUE Lumen", "HdUnrealEngine", "realtime"),
    (
        "pathtraced",
        "hdUE Path Tracer",
        "HdUnrealEngine",
        "pathtraced",
    ),
    ("xpu", "Karma XPU", "BRAY_HdKarmaXPU", None),
    ("karma", "Karma CPU", "BRAY_HdKarma", None),
]
SETS = {  # out folder, images folder, README markers
    "cornell": (
        OUT,
        IMAGES,
        ("<!-- gallery:start -->", "<!-- gallery:end -->"),
    ),
    "sponza": (
        OUT / "sponza",
        IMAGES / "sponza",
        ("<!-- sponza:start -->", "<!-- sponza:end -->"),
    ),
}


def dev_environment():
    profile = ROOT / ".local" / "runtime-houdini.json"
    build = ROOT / "build" / "houdini"
    os.environ["HOUDINI_PATH"] = f'{ROOT / "integrations" / "houdini"};&'
    os.environ["PXR_PLUGINPATH_NAME"] = str(build / "resources")
    os.environ["HDUE_HOUDINI_ENABLE"] = "1"
    os.environ["HDUE_RUNTIME_CONFIG"] = str(profile)


def stage_metadata(layer):
    """The root layer's stage metadata (up axis, units, render settings, time); a new root layer must repeat it."""
    from pxr import Usd

    stage = Usd.Stage.Open(str(layer), Usd.Stage.LoadNone)
    keys = (
        "defaultPrim",
        "metersPerUnit",
        "upAxis",
        "startTimeCode",
        "endTimeCode",
        "timeCodesPerSecond",
        "renderSettingsPrimPath",
    )
    quote = lambda value: f'"{value}"' if isinstance(value, str) else value
    return "".join(
        f"    {key} = {quote(stage.GetMetadata(key))}\n"
        for key in keys
        if stage.HasAuthoredMetadata(key)
    )


def override_layer(path, sublayers, metadata, settings):
    """Writes a root layer over `sublayers` with `metadata` and RenderSettings overrides; assets keep resolving
    relative to the layers that author them.
    """
    subs = ", ".join(f"@{Path(s).as_posix()}@" for s in sublayers)
    body = "".join(f"        {line}\n" for line in settings)
    path.write_text(
        f'#usda 1.0\n(\n{metadata}    subLayers = [{subs}]\n)\n\nover "Render"\n{{\n    over "Settings"\n    {{\n'
        f"{body}    }}\n}}\n",
        encoding="utf-8",
    )
    return path


def mode_layer(layer, mode, out):
    return override_layer(
        out / f"{layer.stem}.{mode}.usda",
        [layer],
        stage_metadata(layer),
        [f'token unreal:renderMode = "{mode}"'],
    )


def husk(renderer, layer, output, frames=1):
    start = time.perf_counter()
    result = subprocess.run(
        [
            str(HUSK),
            "--renderer",
            renderer,
            "-f",
            "1",
            "-n",
            str(frames),
            "-o",
            str(output),
            str(layer),
        ],
        cwd=layer.parent,
        capture_output=True,
        text=True,
        errors="replace",
    )
    elapsed = time.perf_counter() - start
    if frames > 1:
        output = Path(str(output).replace("$F4", "0001"))
    if result.returncode or not output.exists():
        raise RuntimeError(
            f"husk {renderer} {layer.name} failed:\n{result.stdout[-2000:]}{result.stderr[-2000:]}"
        )
    return elapsed


def startup(renderer, mode):
    """Wall time for an empty stage: process start, delegate and (for hdUE) UE startup, image write."""
    empty = OUT / "empty.usda"
    empty.write_text(
        """#usda 1.0
(
    renderSettingsPrimPath = "/Render/Settings"
)
def Camera "camera"
{
}
def Scope "Render"
{
    def RenderSettings "Settings"
    {
        rel camera = </camera>
        uniform int2 resolution = (64, 64)
        int unreal:samplesPerPixel = 1
        int unreal:warmupFrames = 1
        int karma:global:pathtracedsamples = 1
    }
}
""",
        encoding="utf-8",
    )
    layer = mode_layer(empty, mode, OUT) if mode else empty
    return min(
        husk(renderer, layer, OUT / f"empty_{renderer}_{mode}.exr")
        for _ in range(2)
    )


def pixels(path):
    """The full display window: a render region's pixels in place, black around them."""
    buf = oiio.ImageBuf(str(path))
    full = buf.spec().roi_full
    return (
        buf.get_pixels(oiio.FLOAT, full)
        .reshape(full.height, full.width, buf.spec().nchannels)[..., :3]
        .astype(np.float64)
    )


def display(image, tonemapped):
    if tonemapped:  # UE's tone curve, already display-linear: encode only
        return np.clip(image, 0, 1) ** (1 / 2.2)
    return np.clip(image / (1 + image), 0, 1) ** (1 / 2.2)


# Layout of the comparison images, in pixels: a title bar, then the renders on a dark sheet with gutters, each with
# a caption under it (renderer in its colour, then settings, time and brightness).
# Comparison images are typeset like the charts (scripts/gallery_charts.py): matplotlib draws their text and markers
# with the charts' font stack, sizes, weights and GitHub dark-theme colours. Both are shown at the README's width, so
# sizes are the charts' (in units of their 648-wide SVG) scaled by the image's width.
CHART_WIDTH = (
    648  # the charts' SVG width in their own units (9 in at 72 per inch)
)
FONTS = ["Segoe UI", "DejaVu Sans"]
BACKGROUND, PANEL = "#0d1117", "#161b22"
TEXT, MUTED = "#f0f6fc", "#9198a1"
ENGINES = {"hdue": "#3987e5", "karma": "#d95926"}
SERIES = {  # renderer key: engine, marker (as in the charts)
    "lumen": ("hdue", "o"),
    "pathtraced": ("hdue", "D"),
    "xpu": ("karma", "^"),
    "karma": ("karma", "s"),
}
MARKER_SIZE = {
    "o": 8,
    "D": 7.5,
    "^": 9.5,
    "s": 7.5,
}  # chart points, optically equal areas


def wrap(text, size, width):
    """Greedy line breaks for Segoe UI at size px (about 0.5 em per character)."""
    lines, line = [], ""
    for word in text.split():
        if line and (len(line) + 1 + len(word)) * size * 0.5 > width:
            lines.append(line)
            line = word
        else:
            line = f"{line} {word}".strip()
    return [*lines, line] if line else lines


def panel_image(target, title, doc, panels):
    """panels: list of (image, renderer key, name, detail, tonemapped). Four square frames sit side by side; wide
    ones go in a 2 x 2 grid so they stay readable.
    """
    import matplotlib

    matplotlib.use("agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D

    tiles = [display(image, tonemapped) for image, *_, tonemapped in panels]
    height, width = tiles[0].shape[:2]
    columns = 2 if len(tiles) == 4 and width > 1.4 * height else len(tiles)
    rows = -(-len(tiles) // columns)
    scale = columns * width / CHART_WIDTH  # chart pixels to image pixels
    margin, gutter = round(10 * scale), round(8 * scale)
    title_px, doc_px, name_px, detail_px = (
        13 * scale,
        9.5 * scale,
        10 * scale,
        9 * scale,
    )
    sheet_width = 2 * margin + columns * width + (columns - 1) * gutter
    doc_lines = wrap(doc, doc_px, sheet_width - 2 * margin)
    header = round(
        margin + 1.35 * title_px + 1.45 * doc_px * len(doc_lines) + 0.4 * margin
    )
    caption = round(1.5 * name_px + 3 * detail_px + 0.6 * margin)
    sheet_height = (
        header + rows * (height + caption) + (rows - 1) * gutter + margin
    )
    colour = (
        lambda h: np.array([int(h[i : i + 2], 16) for i in (1, 3, 5)]) / 255
    )
    sheet = np.empty((sheet_height, sheet_width, 3))
    sheet[:] = colour(BACKGROUND)
    places = []
    for i, tile in enumerate(tiles):
        x = margin + (i % columns) * (width + gutter)
        y = header + (i // columns) * (height + caption + gutter)
        sheet[y : y + height + caption, x : x + width] = colour(PANEL)
        sheet[y : y + height, x : x + width] = tile
        accent = round(1.5 * scale)
        sheet[y + height : y + height + accent, x : x + width] = colour(
            ENGINES[SERIES[panels[i][1]][0]]
        )
        places.append((x, y + height))
    dpi = 100
    points = lambda px: px * 72 / dpi
    fig = plt.figure(figsize=(sheet_width / dpi, sheet_height / dpi), dpi=dpi)
    fig.figimage((sheet * 255).astype(np.uint8), 0, 0, origin="upper")
    at = lambda x, y: (
        x / sheet_width,
        1 - y / sheet_height,
    )  # pixels from the top left to figure fractions
    text = dict(family=FONTS, va="baseline")
    fig.text(
        *at(margin, margin + title_px),
        title,
        size=points(title_px),
        weight="bold",
        color=TEXT,
        **text,
    )
    for n, line in enumerate(doc_lines):
        fig.text(
            *at(margin, margin + 1.35 * title_px + 1.45 * doc_px * (n + 0.8)),
            line,
            size=points(doc_px),
            color=MUTED,
            **text,
        )
    for (x, y), (_, key, name, detail, _) in zip(places, panels, strict=True):
        engine, shape = SERIES[key]
        first = y + 0.6 * margin + name_px
        fig.add_artist(
            Line2D(
                [at(x + 0.6 * margin + 0.5 * name_px, 0)[0]],
                [at(0, first - 0.35 * name_px)[1]],
                transform=fig.transFigure,
                linestyle="none",
                marker=shape,
                markersize=points(MARKER_SIZE[shape] * scale),
                markerfacecolor=ENGINES[engine],
                markeredgecolor=PANEL,
                markeredgewidth=points(1.5 * scale),
            )
        )
        fig.text(
            *at(x + 0.6 * margin + 1.4 * name_px, first),
            name,
            size=points(name_px),
            color=TEXT,
            **text,
        )
        settings, brightness = detail
        fig.text(
            *at(x + 0.6 * margin, first + 3 * detail_px),
            brightness,
            size=points(detail_px),
            color=MUTED,
            **text,
        )
        fig.text(
            *at(x + 0.6 * margin, first + 1.5 * detail_px),
            settings,
            size=points(detail_px),
            color=MUTED,
            **text,
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.stem}.tmp.png")
    fig.savefig(temporary, dpi=dpi, facecolor=BACKGROUND)
    plt.close(fig)
    try:
        os.replace(temporary, target)
    except PermissionError as error:
        raise RuntimeError(
            f"{target} is locked by another program (an image viewer?); close it and run again"
        ) from error


def hardware():
    query = (
        lambda command: subprocess.run(command, capture_output=True, text=True)
        .stdout.strip()
        .splitlines()
    )
    gpu = query(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"])
    cpu = query(
        [
            "powershell",
            "-NoProfile",
            "-Command",
            "(Get-CimInstance Win32_Processor).Name",
        ]
    )
    return " and ".join(x[0].strip() for x in (gpu, cpu) if x)


def luminance(image):
    return float((image @ np.array([0.2126, 0.7152, 0.0722])).mean())


def tiles(image, rows=9, columns=16):
    """Mean luminance per tile: a small, noise-averaged picture of a render."""
    lum = image @ np.array([0.2126, 0.7152, 0.0722])
    lum = lum[
        : len(lum) - len(lum) % rows, : lum.shape[1] - lum.shape[1] % columns
    ]
    return lum.reshape(rows, len(lum) // rows, columns, -1).mean(axis=(1, 3))


def check_baselines(bless):
    """Compares each Cornell hdUE render with its stored tile baseline (tests/gallery_baselines.json): a
    regression of one region (a light, a material) fails even when the mean brightness stays in its band.
    --bless stores the current renders instead.
    """
    stored = json.loads(BASELINES.read_text()) if BASELINES.exists() else {}
    failures = []
    for name, _, _ in CORNELL_SCENES:
        for key in TILE_LIMITS:
            path = OUT / f"{name}_{key}.exr"
            if not path.exists():
                continue
            current = tiles(pixels(path))
            if bless:
                stored.setdefault(name, {})[key] = np.round(current, 5).tolist()
            elif key not in stored.get(name, {}):
                failures.append(f"{name} {key}: no baseline (--bless)")
            else:
                base = np.array(stored[name][key])
                error = np.abs(current - base) / (base + 0.05 * base.mean())
                p95 = float(np.percentile(error, 95))
                if p95 > TILE_LIMITS[key]:
                    failures.append(
                        f"{name} {key}: 95th-percentile tile error {p95:.3f} > {TILE_LIMITS[key]}"
                    )
    if bless:
        BASELINES.write_text(json.dumps(stored, separators=(",", ":")) + "\n")
    return failures


def duration(seconds):
    return f"{seconds * 1000:.0f} ms" if seconds < 1 else f"{seconds:.2f} s"


def detail(key, render, reference, samples, resolution, compared=True):
    """(settings and time per frame, brightness against Karma CPU): the caption's second and third lines."""
    settings = "32 warmup, then 1" if key == "lumen" else f"{samples} spp"
    if key == "karma":
        brightness = "Brightness reference"
    elif compared and reference:
        brightness = (
            f"Brightness {render['luminance'] / reference:.2f}× Karma CPU"
        )
    else:
        brightness = ""
    return (
        f"{resolution}  ·  {settings}  ·  {duration(render['seconds'])}",
        brightness,
    )


def compose(which, name, entry, samples, resolution):
    out, images, _ = SETS[which]
    renders = entry["renders"]
    reference = renders.get("karma", {}).get("luminance")
    names = dict((k, l) for k, l, _, _ in RENDERERS)
    # UE's tone curve makes the post-processing images display-linear: their brightness is not comparable.
    tonemapped = lambda key: name == "post_processing" and key in (
        "lumen",
        "pathtraced",
    )
    panels = [
        (
            pixels(out / f"{name}_{key}.exr"),
            key,
            names[key],
            detail(
                key,
                renders[key],
                reference,
                samples,
                resolution,
                not tonemapped(key),
            ),
            tonemapped(key),
        )
        for key, _, _, _ in RENDERERS
        if key in renders
    ]
    panel_image(
        images / f"{name}.png",
        entry["title"],
        entry["doc"].replace("`", ""),
        panels,
    )


def settings_of(layer):
    text = layer.read_text(encoding="utf-8")
    resolution = (
        text.split("uniform int2 resolution = ")[1]
        .split("\n")[0]
        .strip("()")
        .replace(", ", " × ")
    )
    return resolution, int(
        text.split("int unreal:samplesPerPixel = ")[1].split("\n")[0]
    )


def addon_fixes(addon, units, out):
    """A layer over a Sponza add-on: scales its root into the scene's units (sublayers ignore metersPerUnit; the trees
    are in metres) and applies MaterialBindingAPI where the exporter bound materials without it, which USD ignores.
    """
    from pxr import Usd, UsdGeom, UsdShade

    source = Usd.Stage.Open(str(addon), Usd.Stage.LoadNone)
    fixes = Usd.Stage.CreateInMemory()
    scale = UsdGeom.GetStageMetersPerUnit(source) / units
    root = source.GetDefaultPrim()
    if root and abs(scale - 1) > 1e-6:
        ops = UsdGeom.Xformable(root).GetOrderedXformOps()
        over = UsdGeom.Xformable(fixes.OverridePrim(root.GetPath()))
        unit = over.AddScaleOp(UsdGeom.XformOp.PrecisionDouble, "hdueUnits")
        unit.Set((scale, scale, scale))
        over.CreateXformOpOrderAttr().Set(
            [unit.GetOpName(), *(op.GetOpName() for op in ops)]
        )
    for prim in source.Traverse():
        if prim.HasRelationship("material:binding") and not prim.HasAPI(
            UsdShade.MaterialBindingAPI
        ):
            UsdShade.MaterialBindingAPI.Apply(
                fixes.OverridePrim(prim.GetPath())
            )
    path = out / f"{addon.stem}.fixes.usda"
    fixes.GetRootLayer().Export(str(path))
    return path


def scenes(which, sponza_file, addons=()):
    """(name, title, doc, root layer) per scene; Sponza scenes are written into out/gallery/sponza."""
    if which == "cornell":
        for name, title, relative in CORNELL_SCENES:
            layer = CORNELL / relative
            text = layer.read_text(encoding="utf-8")
            doc = (
                text.split("doc = ", 1)[1].split("\n")[0].strip().strip('"')
                if "doc = " in text
                else ""
            )
            if name == "cornell":
                doc = "The CC0 Cornell box with its measured camera and a RectLight matching the ceiling light."
            yield name, title, doc, layer
        return
    from pxr import Usd, UsdGeom

    out = SETS["sponza"][0]
    out.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.Open(str(sponza_file), Usd.Stage.LoadNone)
    metadata = stage_metadata(sponza_file).replace(
        "    defaultPrim",
        '    renderSettingsPrimPath = "/Render/Settings"\n    defaultPrim',
    )
    fixes = [
        addon_fixes(addon, UsdGeom.GetStageMetersPerUnit(stage), out)
        for addon in addons
    ]
    for index, camera in enumerate(SPONZA_CAMERAS, 1):
        exposure = (
            stage.GetPrimAtPath(f"/root/{camera}")
            .GetAttribute("exposure")
            .Get()
        )
        layer = override_layer(
            out / f"{camera}.usda",
            [
                SPONZA / "lighting.usda",
                SPONZA_MATERIALS,
                SPONZA / "no_decals.usda",
                sponza_file,
                *fixes,
                *addons,
            ],
            metadata,
            [f"rel camera = </root/{camera}>"],
        )
        yield camera, "Main view", f"`/root/{camera}`, levelled as in Intel's Render_Main_A, exposure +{exposure:g} stops as authored; sun and sky fitted to Intel's renders.", layer


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--set",
        choices=sorted(SETS),
        default="cornell",
        help="image set to render",
    )
    parser.add_argument(
        "--sponza",
        type=Path,
        default=SPONZA_SCENE,
        help="Intel Sponza 2022's NewSponza_Main_USD_Yup_003.usda (for --set sponza; default: samples/sponza/intel)",
    )
    parser.add_argument(
        "--addon",
        type=Path,
        action="append",
        help="a Sponza add-on's USD sublayered over the scene; repeatable (default: the curtains and cypress tree in "
        "samples/sponza/intel, when present)",
    )
    parser.add_argument(
        "--dev",
        action="store_true",
        help="use this checkout's build and .local runtime profile",
    )
    parser.add_argument(
        "--only",
        nargs="*",
        help="scene names to render; others keep their previous results",
    )
    parser.add_argument(
        "--renderers",
        nargs="*",
        choices=[k for k, _, _, _ in RENDERERS],
        help="render only these columns and keep the others from the last run",
    )
    parser.add_argument(
        "--docs-only",
        action="store_true",
        help="rebuild images and tables from the last renders",
    )
    parser.add_argument(
        "--bless",
        action="store_true",
        help="store the Cornell hdUE renders as the new tile baselines (tests/gallery_baselines.json)",
    )
    args = parser.parse_args()
    if args.dev:
        dev_environment()
    if (
        args.set == "sponza"
        and not args.docs_only
        and not (args.sponza and args.sponza.exists())
    ):
        parser.error(
            f"--set sponza needs Intel's Sponza 2022 in {SPONZA_SCENE.parent.parent} (samples/sponza/README.md), "
            "or --sponza pointing at NewSponza_Main_USD_Yup_003.usda"
        )
    if args.addon is None:
        args.addon = [a for a in SPONZA_ADDONS if a.exists()]
    out = SETS[args.set][0]
    out.mkdir(parents=True, exist_ok=True)
    results_path = out / "results.json"
    results = (
        json.loads(results_path.read_text()) if results_path.exists() else {}
    )
    resolution, samples = settings_of(
        CORNELL / "cornell.usda"
        if args.set == "cornell"
        else SPONZA / "lighting.usda"
    )
    if not args.docs_only:
        wanted = [
            r for r in RENDERERS if not args.renderers or r[0] in args.renderers
        ]
        baselines = {
            key: startup(renderer, mode) for key, _, renderer, mode in wanted
        }
        print(
            "startup",
            {k: round(v, 1) for k, v in baselines.items()},
            flush=True,
        )
        addons = [a.resolve() for a in args.addon]
        for name, title, doc, layer in scenes(
            args.set, args.sponza.resolve() if args.sponza else None, addons
        ):
            if args.only and name not in args.only:
                continue
            entry = {
                "title": title,
                "doc": doc,
                "addons": [a.name for a in addons],
                "renders": (
                    dict(results.get(name, {}).get("renders", {}))
                    if args.renderers
                    else {}
                ),
            }
            for key, _, renderer, mode in wanted:
                output = out / f"{name}_{key}.exr"
                source = mode_layer(layer, mode, out) if mode else layer
                single = husk(renderer, source, output)
                if mode:
                    # hdUE frames are short next to UE startup: time extra frames of the same scene in one process
                    # (16 for Lumen, whose frames take tenths of a second).
                    frames = 16 if mode == "realtime" else 4
                    seconds = (
                        husk(
                            renderer,
                            source,
                            out / f"{name}_{key}.$F4.exr",
                            frames=frames,
                        )
                        - single
                    ) / (frames - 1)
                else:
                    seconds = single - baselines[key]
                entry["renders"][key] = {
                    "seconds": round(max(seconds, 0.0), 4),
                    "luminance": luminance(pixels(output)),
                }
                print(name, key, duration(max(seconds, 0.0)), flush=True)
            results[name] = entry
            results_path.write_text(json.dumps(results, indent=2))
            compose(args.set, name, entry, samples, resolution)
    else:
        for name, entry in results.items():
            if not args.only or name in args.only:
                compose(args.set, name, entry, samples, resolution)
    write_docs(hardware())
    from check_gallery import check

    failures = check()[1]
    if args.set == "cornell":
        failures += check_baselines(args.bless)
    print("\n".join(failures) or "Gallery check passed.")
    return 1 if failures else 0


def section(which, results, machine):
    """Summary table and image table for one set; links are relative to docs/."""
    resolution, samples = settings_of(
        CORNELL / "cornell.usda"
        if which == "cornell"
        else SPONZA / "lighting.usda"
    )
    if which == "cornell":
        order = [name for name, _, _ in CORNELL_SCENES if name in results]
        intro = (
            f"Every scene is the [CC0 Cornell box](../samples/cornell/) plus one small layer from "
            f"[samples/cornell/variants](../samples/cornell/variants), rendered with husk at {resolution}:"
        )
        command = "hython scripts/render_gallery.py"
    else:
        order = [name for name in SPONZA_CAMERAS if name in results]
        intro = (
            "[Intel's Sponza 2022](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) "
            "(Frank Meinl and Anton Kaplanyan, CC BY 4.0; not included in this repository), from its main camera. Intel exports "
            "UsdPreviewSurface only; [samples/sponza/materials.usda](../samples/sponza/materials.usda) gives every material a "
            "MaterialX standard_surface network on Intel's textures (stone, metal, wood, sheen and translucent curtains, "
            "translucent leaves cut out by their alpha, glass), which hdUE and Karma render. The gallery renders without hdUE’s optional Open Image Denoise (Houdini build), so its Path Tracer "
            "image shows more noise than Karma at the same sample count. The scene has no lights, so "
            "[samples/sponza/lighting.usda](../samples/sponza/lighting.usda) adds a sun and a sky dome fitted to Intel's own renders of the scene, "
            "and levels the camera as Intel's render does; "
            "[samples/sponza/no_decals.usda](../samples/sponza/no_decals.usda) deactivates the dirt decal meshes."
        )
        addons = sorted(
            {a for entry in results.values() for a in entry.get("addons", [])}
        )
        if addons:
            intro += f" Add-ons from the same package, sublayered unchanged: {', '.join(f'`{a}`' for a in addons)}."
        intro += f" Rendered with husk at {resolution}:"
        command = "hython scripts/render_gallery.py --set sponza"
    setup = (
        f"{intro}\n\n"
        "- **hdUE Lumen**: realtime mode, 32 warmup frames on a sequence's first frame, then one draw per frame "
        "(as Movie Render Queue).\n"
        f"- **hdUE Path Tracer**: {samples} samples per pixel, up to 32 bounces.\n"
        f"- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, {samples} samples per pixel, 8 diffuse bounces.\n"
        f"- Machine: {machine}.\n"
        "- Times are per frame without process or UE startup: hdUE from the extra time of a 16-frame (Lumen) or "
        "4-frame (Path Tracer) husk run over a 1-frame run (a Lumen time is a sequence frame's, after the first "
        "frame's warm-up), Karma as husk's wall time minus its startup on an empty stage.\n"
        "- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean "
        "luminance relative to Karma CPU, the reference (1.00 = same).\n\n"
        f"Regenerate with `{command}`."
    )
    seconds = lambda name, key: results[name]["renders"][key]["seconds"]
    brightness = (
        lambda name, key: results[name]["renders"][key]["luminance"]
        / results[name]["renders"]["karma"]["luminance"]
    )
    present = [
        r
        for r in RENDERERS
        if all(r[0] in results[name]["renders"] for name in order)
    ]
    reference = "xpu" if any(k == "xpu" for k, _, _, _ in present) else "karma"
    reference_label = dict((k, l) for k, l, _, _ in RENDERERS)[reference]
    speed = lambda name, key: (
        f"{seconds(name, reference) / seconds(name, key):.0f}x"
        if seconds(name, key) > 0
        else "n/a"
    )
    summary = [
        "| Scene | "
        + " | ".join(l for _, l, _, _ in present)
        + f" | Faster than {reference_label} (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |",
        "| --- |" + " ---: |" * (len(present) + 2),
    ]
    for name in order:
        # UE's tone curve makes the post-processing images display-linear, unlike Karma's scene-linear one.
        shade = (
            "n/a"
            if name == "post_processing"
            else f"{brightness(name, 'lumen'):.2f} / {brightness(name, 'pathtraced'):.2f}"
        )
        cells = " | ".join(duration(seconds(name, k)) for k, _, _, _ in present)
        summary.append(
            f"| {results[name]['title']} | {cells} | {speed(name, 'lumen')} / {speed(name, 'pathtraced')} | {shade} |"
        )
    total = {
        k: sum(seconds(name, k) for name in order) for k, _, _, _ in present
    }
    if (
        len(order) > 1
        and total.get("lumen", 0) > 0
        and total.get("pathtraced", 0) > 0
    ):
        cells = " | ".join(f"**{duration(total[k])}**" for k, _, _, _ in present)
        summary.append(
            f"| **All {len(order)}** | {cells} | **{total[reference] / total['lumen']:.0f}x / "
            f"{total[reference] / total['pathtraced']:.0f}x** | |"
        )
    folder = (
        "images/gallery/" if which == "cornell" else "images/gallery/sponza/"
    )
    # Each image carries its title, description and per-render details.
    images = [
        f"![{results[name]['title']}: {results[name]['doc']}]({folder}{name}.png)"
        for name in order
    ]
    return setup + "\n\n" + "\n".join(summary) + "\n\n" + "\n\n".join(images)


def write_docs(machine):
    # What the charts state about the renders (scripts/gallery_charts.py).
    setup = {"machine": machine, "sets": {}}
    for which in SETS:
        resolution, samples = settings_of(
            CORNELL / "cornell.usda"
            if which == "cornell"
            else SPONZA / "lighting.usda"
        )
        setup["sets"][which] = {"resolution": resolution, "samples": samples}
    (OUT / "setup.json").write_text(
        json.dumps(setup, indent=2), encoding="utf-8"
    )
    sections = {}
    for which, (out, _, _) in SETS.items():
        path = out / "results.json"
        if path.exists() and json.loads(path.read_text()):
            sections[which] = section(
                which, json.loads(path.read_text()), machine
            )
    titles = {
        "cornell": "Cornell box: one feature per scene",
        "sponza": "Sponza: a production-style scene",
    }
    picture = lambda name, alt: (
        f'<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="images/gallery/chart-{name}-dark.svg">\n'
        f'  <img src="images/gallery/chart-{name}-light.svg" alt="{alt}" width="100%">\n</picture>\n'
    )
    charts = (
        picture(
            "times", "Render time per frame per scene and renderer, log scale"
        )
        + "\n"
        + picture(
            "accuracy",
            "Mean brightness relative to Karma CPU per scene and renderer",
        )
    )
    page = (
        "# Gallery\n\n"
        + charts
        + "\nCharts: `uv run --with matplotlib python scripts/gallery_charts.py`.\n\n"
        + "\n\n".join(
            f"## {titles[w]}\n\n{body}" for w, body in sections.items()
        )
        + "\n"
    )
    (ROOT / "docs" / "gallery.md").write_text(page, encoding="utf-8")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    for which, body in sections.items():
        start, end = SETS[which][2]
        if start in readme and end in readme:
            block = body.replace("](../", "](").replace(
                "](images/", "](docs/images/"
            )
            readme = (
                readme.split(start)[0]
                + start
                + "\n"
                + block
                + "\n"
                + end
                + readme.split(end)[1]
            )
    (ROOT / "README.md").write_text(readme, encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
