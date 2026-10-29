"""Renders the README gallery with hdUE (Lumen and Path Tracer) and Karma CPU through husk, then writes labelled
comparison images, docs/gallery.md and the README blocks. Two independent sets:

- cornell: the CC0 Cornell box (samples/cornell) and one layer per feature in samples/cornell/variants;
- sponza: Intel's Sponza 2022 (CC BY 4.0, not included) from its main camera, lit by samples/sponza/lighting.usda.

    hython scripts/render_gallery.py [--set cornell|sponza] [--sponza FILE] [--dev] [--only NAME ...] [--docs-only]

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
OUT = ROOT / "out" / "gallery"
IMAGES = ROOT / "docs" / "images" / "gallery"
HUSK = Path(os.environ.get("HFS", "")) / "bin" / "husk.exe"

# name, title, layer (relative to samples/cornell); captions come from each layer's doc string.
CORNELL_SCENES = [
    ("cornell", "Global illumination", "cornell.usda"),
    ("glossy", "Glossy materials", "variants/glossy.usda"),
    ("depth_of_field", "Depth of field", "variants/depth_of_field.usda"),
    ("motion_blur", "Motion blur", "variants/motion_blur.usda"),
    ("subdivision", "Subdivision surfaces", "variants/subdivision.usda"),
    ("instances", "Instances, points, curves", "variants/instances.usda"),
    ("volume", "Volumes (OpenVDB)", "variants/volume.usda"),
    ("ies", "IES profile (experimental)", "variants/ies.usda"),
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
    ("lumen", "hdUE Lumen", "HdUEHoudiniRendererPlugin", "realtime"),
    (
        "pathtraced",
        "hdUE Path Tracer",
        "HdUEHoudiniRendererPlugin",
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
    build = next((ROOT / "build").glob("houdini-*"))
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
        [f'token hdUE:renderMode = "{mode}"'],
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
        int hdUE:samplesPerPixel = 1
        int hdUE:warmupFrames = 1
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
    buf = oiio.ImageBuf(str(path))
    spec = buf.spec()
    return (
        buf.get_pixels(oiio.FLOAT)
        .reshape(spec.height, spec.width, spec.nchannels)[..., :3]
        .astype(np.float64)
    )


def display(image, tonemapped):
    if tonemapped:  # UE's tone curve, already display-linear: encode only
        return np.clip(image, 0, 1) ** (1 / 2.2)
    return np.clip(image / (1 + image), 0, 1) ** (1 / 2.2)


def panel_image(target, panels):
    """panels: list of (image, label, tonemapped), side by side with a caption bar each."""
    tiles = [display(image, tonemapped) for image, _, tonemapped in panels]
    height, width = tiles[0].shape[:2]
    # Wide frames side by side get too small to read; four of them go in a 2 x 2 grid.
    columns = 2 if len(tiles) == 4 and width > 1.4 * height else len(tiles)
    grid = [
        np.concatenate(tiles[r : r + columns], axis=1)
        for r in range(0, len(tiles), columns)
    ]
    strip = np.concatenate(grid, axis=0).astype(np.float32)
    buf = oiio.ImageBuf(
        oiio.ImageSpec(strip.shape[1], strip.shape[0], 3, oiio.FLOAT)
    )
    buf.set_pixels(oiio.ROI(), strip)
    font = str(
        Path(os.environ.get("SystemRoot", os.environ.get("WINDIR", "")))
        / "Fonts"
        / "segoeui.ttf"
    )
    for i, (_, label, _) in enumerate(panels):
        x, y = (i % columns) * width, (i // columns) * height
        oiio.ImageBufAlgo.render_box(
            buf, x, y, x + width - 1, y + 34, (0, 0, 0), True
        )
        oiio.ImageBufAlgo.render_text(
            buf, x + 10, y + 25, label, 20, font, (1, 1, 1)
        )
    buf.set_write_format(oiio.UINT8)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.stem}.tmp.png")
    if not buf.write(str(temporary)):
        raise RuntimeError(buf.geterror())
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


def duration(seconds):
    return "< 0.1 s" if seconds < 0.05 else f"{seconds:.1f} s"


def label(key, seconds, samples):
    detail = "16 warmup frames" if key == "lumen" else f"{samples} spp"
    return f"{dict((k, l) for k, l, _, _ in RENDERERS)[key]}  ·  {detail}  ·  {duration(seconds)}"


def compose(which, name, entry, samples):
    out, images, _ = SETS[which]
    panels = [
        (
            pixels(out / f"{name}_{key}.exr"),
            label(key, entry["renders"][key]["seconds"], samples),
            name == "post_processing" and key in ("lumen", "pathtraced"),
        )
        for key, _, _, _ in RENDERERS
        if key in entry["renders"]
    ]
    panel_image(images / f"{name}.png", panels)


def settings_of(layer):
    text = layer.read_text(encoding="utf-8")
    resolution = (
        text.split("uniform int2 resolution = ")[1]
        .split("\n")[0]
        .strip("()")
        .replace(", ", " x ")
    )
    return resolution, int(
        text.split("int hdUE:samplesPerPixel = ")[1].split("\n")[0]
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
            [SPONZA / "lighting.usda", sponza_file, *fixes, *addons],
            metadata,
            [f"rel camera = </root/{camera}>"],
        )
        yield camera, "Main view", f"`/root/{camera}` (Intel's Render_Main_A view), exposure +{exposure:g} stops as authored.", layer


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
        help="Intel Sponza 2022's NewSponza_Main_USD_Yup_003.usda (for --set sponza)",
    )
    parser.add_argument(
        "--addon",
        type=Path,
        action="append",
        default=[],
        help="a Sponza add-on's USD (e.g. NewSponza_CypressTree_USDA_YUp.usda), sublayered over the scene; repeatable",
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
    args = parser.parse_args()
    if args.dev:
        dev_environment()
    if (
        args.set == "sponza"
        and not args.docs_only
        and not (args.sponza and args.sponza.exists())
    ):
        parser.error(
            "--set sponza needs --sponza pointing at NewSponza_Main_USD_Yup_003.usda"
        )
    out = SETS[args.set][0]
    out.mkdir(parents=True, exist_ok=True)
    results_path = out / "results.json"
    results = (
        json.loads(results_path.read_text()) if results_path.exists() else {}
    )
    samples = settings_of(
        CORNELL / "cornell.usda"
        if args.set == "cornell"
        else SPONZA / "lighting.usda"
    )[1]
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
                    "seconds": round(max(seconds, 0.0), 2),
                    "luminance": luminance(pixels(output)),
                }
                print(name, key, f"{seconds:.1f}s", flush=True)
            results[name] = entry
            results_path.write_text(json.dumps(results, indent=2))
            compose(args.set, name, entry, samples)
    else:
        for name, entry in results.items():
            if not args.only or name in args.only:
                compose(args.set, name, entry, samples)
    write_docs(hardware())


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
            "(Frank Meinl and Anton Kaplanyan, CC BY 4.0; not included in this repository): 28 UsdPreviewSurface materials "
            "with 96 textures, normal maps and alpha cutouts, from its main camera. The gallery renders without hdUE’s optional Open Image Denoise (Houdini build), so its Path Tracer "
            "image shows more noise than Karma at the same sample count. The scene has no lights, so "
            "[samples/sponza/lighting.usda](../samples/sponza/lighting.usda) adds a sun and a sky dome."
        )
        addons = sorted(
            {a for entry in results.values() for a in entry.get("addons", [])}
        )
        if addons:
            intro += f" Add-ons from the same package, sublayered unchanged: {', '.join(f'`{a}`' for a in addons)}."
        intro += f" Rendered with husk at {resolution}:"
        command = (
            "hython scripts/render_gallery.py --set sponza --sponza <NewSponza_Main_USD_Yup_003.usda>"
            + "".join(" --addon <add-on .usda>" for _ in addons)
        )
    setup = (
        f"{intro}\n\n"
        f"- **hdUE Lumen**: realtime mode, 16 warmup frames.\n"
        f"- **hdUE Path Tracer**: {samples} samples per pixel, up to 8 bounces.\n"
        f"- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, {samples} samples per pixel, 8 diffuse bounces.\n"
        f"- Machine: {machine}.\n"
        "- Times are per frame without process or UE startup: hdUE from the extra time of a 16-frame (Lumen) or "
        "4-frame (Path Tracer) husk run, Karma as husk's wall time minus its startup on an empty stage.\n"
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
        cells = " | ".join(f"**{total[k]:.1f} s**" for k, _, _, _ in present)
        summary.append(
            f"| **All {len(order)}** | {cells} | **{total[reference] / total['lumen']:.0f}x / "
            f"{total[reference] / total['pathtraced']:.0f}x** | |"
        )
    folder = (
        "images/gallery/" if which == "cornell" else "images/gallery/sponza/"
    )
    images = [
        "| Scene | " + " &#124; ".join(l for _, l, _, _ in present) + " |",
        "| --- | --- |",
    ]
    for name in order:
        images.append(
            f"| **{results[name]['title']}**<br>{results[name]['doc']} | ![{results[name]['title']}]({folder}{name}.png) |"
        )
    return setup + "\n\n" + "\n".join(summary) + "\n\n" + "\n".join(images)


def write_docs(machine):
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
