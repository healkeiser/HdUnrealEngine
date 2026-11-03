"""Prepared materials in husk, as in Houdini's viewport (hdue_batch.resolve through the delegate):

    python tests/husk_prepared_materials.py [--runtime-config <profile>] [--decal-library <library.json>]
        [--volume-library <library.json>] [--materialx-library <library.json>]

The libraries are Prepare-Substrate.ps1's for samples/decal/decal.usda and samples/volume_material/volume_material.usda
(staged with Stage-Runtime.ps1), and Import-MaterialX.ps1's for samples/materialx; by default the newest under out/
(out/substrate/decal-*, out/substrate/volume-*, out/materialx), and .local/runtime-houdini.json. A missing decal or
volume library is prepared, and the runtime staged, first; without a MaterialX library its checks are skipped.
Checks: the decal paints the wall; a volume material's albedo edit tints the fog; MaterialX materials render their
cooked assets; a procedural MaterialX graph hdUE cannot translate renders translated without automatic preparation, and
is prepared through UE's MaterialX importer with it; a graph changed since its preparation fails the frame with the
library's message without automatic preparation, and is prepared and rendered with it; wet surfaces bind; an object
behind glass shows through it with its own alpha.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from local_paths import local_path


def husk(scene, settings, out, extra_layers="", trace=False):
    """Renders scene's frame 1 with settings (lines on /Render/Settings): (exit code, output, RGB image)."""
    root = out / "root.usda"
    root.write_text(
        "#usda 1.0\n(\n"
        f"    subLayers = [@{scene.as_posix()}@]\n"
        '    metersPerUnit = 1\n    upAxis = "Y"\n    renderSettingsPrimPath = "/Render/Settings"\n)\n'
        # def, not over: the samples define these, the MaterialX showcase does not.
        'def Scope "Render"\n{\n    def RenderSettings "Settings"\n    {\n'
        + "".join(f"        {line}\n" for line in settings)
        + "    }\n}\n"
        + extra_layers,
        encoding="utf-8",
    )
    houdini = local_path("houdini_root")
    env = dict(
        os.environ,
        HFS=str(houdini),
        HOUDINI_PATH=f"{ROOT / 'integrations/houdini'};&",
        PXR_PLUGINPATH_NAME=str(ROOT / "build/houdini/resources"),
        HDUE_RUNTIME_CONFIG=str(ARGS.runtime_config),
        HDUE_HOUDINI_ENABLE="1",
        HDUE_TRACE_SCENE="1" if trace else "",
    )
    result = subprocess.run(
        [
            str(houdini / "bin/husk.exe"),
            "--renderer",
            "HdUnrealEngine",
            "-f",
            "1",
            "-n",
            "1",
            "-V",
            "2",
            "--headlight",
            "none",
            "-o",
            "beauty.exr",
            "root.usda",
        ],
        cwd=out,
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=600,
    )
    image = None
    if (out / "beauty.exr").exists():
        import OpenImageIO as oiio

        file = oiio.ImageInput.open(str(out / "beauty.exr"))
        image = file.read_image("float")[..., :3]
        file.close()
    LAST_LOG.append(result.stdout + result.stderr)
    return result.returncode, result.stdout + result.stderr, image


LAST_LOG = []


def edit(path, line):
    """An override layer's text: over prims down path, authoring line on the last one."""
    names = path.strip("/").split("/")
    lines = []
    for depth, name in enumerate(names):
        lines += ["    " * depth + f'over "{name}"', "    " * depth + "{"]
    lines.append("    " * len(names) + line)
    lines += ["    " * depth + "}" for depth in reversed(range(len(names)))]
    return "\n".join(lines) + "\n"


def red_fraction(rgb):
    return float(
        (
            (rgb[..., 0] > 3 * rgb[..., 1])
            & (rgb[..., 0] > 0.05 * rgb[..., 0].max())
        ).mean()
    )


def forget_prepared(material):
    """Removes the runtime's published live libraries that prepared material (earlier runs of this test): its stale
    graph is then unprepared again, as on a fresh runtime. The packages stay; nothing refers to them.
    """
    import json

    config = json.loads(ARGS.runtime_config.read_text(encoding="utf-8-sig"))
    live = (
        Path(config["runtime_project"]).resolve().parent / "Content/HdUE/Live"
    )
    for library in live.glob("**/hdue-live-library.json"):
        entries = json.loads(library.read_text(encoding="utf-8")).get(
            "materials", []
        )
        if any(
            material in (entry.get("material"), entry.get("usd_material"))
            for entry in entries
        ):
            library.unlink()


def check(name, condition, detail):
    print(("ok   " if condition else "FAIL ") + name + ": " + detail)
    if not condition and LAST_LOG:
        print("    " + "\n    ".join(LAST_LOG[-1].splitlines()[-25:]))
    return condition


def main():
    results = []
    with tempfile.TemporaryDirectory(prefix="hdue-husk-materials-") as temp:
        temp = Path(temp)
        library = f'string unreal:substrateLibrary = "{Path(ARGS.decal_library).as_posix()}"'
        mode = 'token unreal:renderMode = "realtime"'
        (temp / "decal").mkdir()
        code, log, image = husk(
            ROOT / "samples/decal/decal.usda", [library, mode], temp / "decal"
        )
        results.append(
            check(
                "decal",
                code == 0 and image is not None and red_fraction(image) > 0.02,
                f"exit {code}, red fraction {red_fraction(image) if image is not None else None}",
            )
        )
        # A graph changed since its preparation (the paint Slab's subsurface mode, which recompiles): without automatic
        # preparation it fails the frame; with it (the default), husk prepares it as the viewport does, then renders.
        forget_prepared("/Looks/Paint")
        (temp / "stale").mkdir()
        stale = edit(
            "/Looks/Paint/Graph/paint",
            'string inputs:subsurface = "simple_volume"',
        )
        code, log, _ = husk(
            ROOT / "samples/decal/decal.usda",
            [library, mode, "bool unreal:substrateAutoPrepare = 0"],
            temp / "stale",
            stale,
        )
        results.append(
            check(
                "stale graph fails the frame without automatic preparation",
                code != 0 and "hdUE prepared materials" in log,
                f"exit {code}",
            )
        )
        (temp / "prepared").mkdir()
        code, log, image = husk(
            ROOT / "samples/decal/decal.usda",
            [library, mode],
            temp / "prepared",
            stale,
        )
        results.append(
            check(
                "stale graph prepared by husk",
                code == 0 and image is not None and red_fraction(image) > 0.02,
                f"exit {code}, red fraction {red_fraction(image) if image is not None else None}",
            )
        )
        library = f'string unreal:substrateLibrary = "{Path(ARGS.volume_library).as_posix()}"'
        means = {}
        for name, layer in (
            ("volume", ""),
            (
                "volume red",
                edit(
                    "/Looks/Smoke/Graph/volume",
                    "color3f inputs:albedo = (0.8, 0.05, 0.05)",
                ),
            ),
        ):
            (temp / name).mkdir()
            code, log, image = husk(
                ROOT / "samples/volume_material/volume_material.usda",
                [library, mode],
                temp / name,
                layer,
            )
            means[name] = (
                image.reshape(-1, 3).mean(0)
                if code == 0 and image is not None
                else None
            )
        ok = all(m is not None for m in means.values())
        ratio = lambda m: m[1] / m[0]  # noqa: E731
        results.append(
            check(
                "volume material albedo",
                ok
                and ratio(means["volume red"]) < 0.98 * ratio(means["volume"]),
                f"green/red {[round(float(ratio(m)), 4) for m in means.values()] if ok else means}",
            )
        )
        if ARGS.materialx_library:
            # Every MaterialX material of the showcase uploads as its cooked asset (the scene trace names it).
            (temp / "materialx").mkdir()
            setting = f'string unreal:materialxLibrary = "{Path(ARGS.materialx_library).as_posix()}"'
            code, log, _ = husk(
                ROOT / "samples/materialx/showcase.usda",
                [setting, mode, "rel camera = </Camera>"],
                temp / "materialx",
                trace=True,
            )
            native = set(
                re.findall(r"hdUE material (\S+) rgb=\S+ native=/Game/", log)
            )
            results.append(
                check(
                    "MaterialX library",
                    code == 0 and len(native) >= 10,
                    f"exit {code}, {len(native)} materials as cooked assets",
                )
            )
            # The wet surface bindings (surfaceEffects) resolve in husk too.
            (temp / "wet").mkdir()
            code, log, _ = husk(
                ROOT / "samples/wet_surface/showcase.usda",
                [setting, mode, "rel camera = </Camera>"],
                temp / "wet",
                trace=True,
            )
            wet = re.findall(
                r"hdUE prepared materials at frame \S+ \d+ assets, \d+ parameter sets, (\d+) wet",
                log,
            )
            results.append(
                check(
                    "wet surfaces",
                    code == 0 and bool(wet) and int(wet[-1]) > 0,
                    f"exit {code}, wet bindings {wet}",
                )
            )
        # A MaterialX graph hdUE cannot translate at render time (place2d, Houdini's checkers node, mix): without
        # automatic preparation it renders translated (its base colour at its grey constant), with it husk prepares it
        # through UE's MaterialX importer and renders the red and grey checkers.
        forget_prepared("/World/Looks/Checkers")
        procedural = ROOT / "samples/materialx/procedural.usda"
        (temp / "procedural-off").mkdir()
        code, log, image = husk(
            procedural,
            [
                mode,
                "rel camera = </Camera>",
                "bool unreal:substrateAutoPrepare = 0",
            ],
            temp / "procedural-off",
        )
        results.append(
            check(
                "procedural MaterialX translated without preparation",
                code == 0 and image is not None and red_fraction(image) < 0.01,
                f"exit {code}, red fraction {red_fraction(image) if image is not None else None}",
            )
        )
        (temp / "procedural").mkdir()
        code, log, image = husk(
            procedural,
            [mode, "rel camera = </Camera>"],
            temp / "procedural",
            trace=True,
        )
        fraction = red_fraction(image) if image is not None else 0.0
        results.append(
            check(
                "procedural MaterialX prepared by UE's importer",
                code == 0
                and re.search(
                    r"Checkers rgb=\S+ native=/Game/HdUE/Live/\S+/MX/", log
                )
                and 0.2 < fraction < 0.4,
                f"exit {code}, red fraction {fraction}",
            )
        )
        # Glass (hdUE's own MaterialX translation, the parent Interchange gives it): the orange sphere behind the clear
        # pane shows through it, and the beauty's alpha there stays the opaque sphere's (refraction kept coverage).
        (temp / "glass").mkdir()
        code, log, image = husk(
            ROOT / "samples/materialx/glass.usda",
            [mode, "rel camera = </Camera>"],
            temp / "glass",
        )
        sphere, covered = None, 0.0
        if image is not None:
            import OpenImageIO as oiio

            file = oiio.ImageInput.open(str(temp / "glass/beauty.exr"))
            alpha = file.read_image("float")[..., 3]
            file.close()
            h, w = alpha.shape
            # The sphere behind the left pane, at 30% across and 51% down the frame.
            y, x, r = int(0.51 * h), int(0.30 * w), max(4, h // 40)
            sphere = image[y - r : y + r, x - r : x + r].reshape(-1, 3).mean(0)
            covered = float(alpha[y - r : y + r, x - r : x + r].min())
        results.append(
            check(
                "glass",
                code == 0
                and sphere is not None
                and sphere[0] > 1.5 * sphere[2]
                and covered > 0.99,
                f"exit {code}, sphere rgb {None if sphere is None else sphere.round(3).tolist()}, alpha {covered}",
            )
        )
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    def newest(pattern):
        found = sorted(ROOT.glob(pattern), key=lambda p: p.stat().st_mtime)
        return str(found[-1]) if found else ""

    parser.add_argument(
        "--runtime-config",
        type=Path,
        default=ROOT / ".local/runtime-houdini.json",
    )
    parser.add_argument(
        "--decal-library", default=newest("out/substrate/decal-*/library.json")
    )
    parser.add_argument(
        "--volume-library",
        default=newest("out/substrate/volume-*/library.json"),
    )
    parser.add_argument(
        "--materialx-library", default=newest("out/materialx/library.json")
    )
    ARGS = parser.parse_args()
    # A missing library is prepared (exported, cooked) and staged here, as the docs prepare them: a fresh checkout
    # then tests them too. Staging replaces the runtime's binaries, so the runtime profile is updated after it.
    missing = [
        (name, scene)
        for name, scene in (
            ("decal", "samples/decal/decal.usda"),
            ("volume", "samples/volume_material/volume_material.usda"),
        )
        if not getattr(ARGS, f"{name}_library")
    ]
    if missing:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        scripts = ROOT / "scripts"
        for name, scene in missing:
            print(f"Preparing samples for the {name} library", flush=True)
            subprocess.run(
                [
                    "pwsh",
                    "-NoProfile",
                    "-File",
                    str(scripts / "Prepare-Substrate.ps1"),
                    "-InputUsd",
                    str(ROOT / scene),
                    "-SkipBuild",
                    "-OutputDirectory",
                    str(ROOT / f"out/substrate/{name}-{stamp}"),
                ],
                check=True,
            )
            setattr(
                ARGS,
                f"{name}_library",
                str(ROOT / f"out/substrate/{name}-{stamp}/library.json"),
            )
        subprocess.run(
            ["pwsh", "-NoProfile", "-File", str(scripts / "Stage-Runtime.ps1")],
            check=True,
        )
        subprocess.run(
            [
                "pwsh",
                "-NoProfile",
                "-File",
                str(scripts / "Update-HoudiniRuntime.ps1"),
                "-OutputConfig",
                str(ARGS.runtime_config),
            ],
            check=True,
        )
    main()
