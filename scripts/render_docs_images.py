r"""Renders the user guide's figures with hdUE through husk: one image per value of a setting, from the scenes in
samples/ (the demo scenes of samples/docs, the Cornell box), written to docs/images/settings/<figure>/<n>.jpg.

Each figure is docs/figures/<name>.json:

    {
     "scene": "docs/shaderball.usda",          # under samples/
     "display": "tonemapped",                 # or "linear" (a scene without UE's tonemapper: a simple display curve)
     "base": {"/Render/Settings": ["token unreal:renderMode = \"pathtraced\""]},
     "values": [
      {"caption": "0 (default)", "set": {"/scene/Looks/ball/S": ["float inputs:roughness = 0"]}},
      {"caption": "1", "set": {"/scene/key": ["(active = false)"]}}
     ]
    }

"base" and each value's "set" map prim paths to lines authored in an `over` of that prim: property lines, or one
"(metadata)" line for the prim's metadata. A page shows a figure with <div class="hdue-figure" data-figure="<name>">
</div>: docs/assets/hdue.js draws a before/after slider for two values and a value scrubber for more, with the
captions this script collects into docs/images/settings/figures.json.

    hython scripts/render_docs_images.py [--only <figure> ...]     # render figures (default: all)
    hython scripts/render_docs_images.py --preview docs/room.usda  # render one scene as is, to out/docs-images/

Commit the images with the page that shows them.
"""

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from render_gallery import (  # noqa: E402  (same husk, pixels and display as the gallery)
    ROOT,
    dev_environment,
    display,
    husk,
    pixels,
    stage_metadata,
)

SAMPLES = ROOT / "samples"
SPECS = ROOT / "docs" / "figures"
OUT = ROOT / "out" / "docs-images"
IMAGES = ROOT / "docs" / "images" / "settings"


def figures():
    return {
        path.stem: json.loads(path.read_text(encoding="utf-8"))
        for path in sorted(SPECS.glob("*.json"))
    }


def overs(overrides):
    """Nested `over` prims for {prim path: [lines]}; a "(...)" line is the prim's metadata (the last one wins)."""
    tree = {}
    for path, lines in overrides.items():
        node = tree
        for name in path.strip("/").split("/"):
            node = node.setdefault(name, {})
        node.setdefault("", []).extend(lines)

    def write(node, indent):
        # The last line per property wins too: a value's "float fStop = 2.8" over the base's "float fStop = 0".
        properties = {}
        for line in node.get("", []):
            if not line.startswith("("):
                properties[line.split("=")[0].split()[-1]] = line
        text = "".join(f"{indent}{line}\n" for line in properties.values())
        for name, child in node.items():
            if name:
                # The last metadata line wins: a value's "(active = true)" over the base's "(active = false)".
                meta = next(
                    (
                        line
                        for line in reversed(child.get("", []))
                        if line.startswith("(")
                    ),
                    "",
                )
                text += (
                    f'{indent}over "{name}"{" " + meta if meta else ""}\n{indent}{{\n'
                    f'{write(child, indent + "    ")}{indent}}}\n'
                )
        return text

    return write(tree, "")


def root_layer(layer, overrides):
    """A root layer over `layer` with `overrides`; it sets ${HFS} for scenes that reference Houdini's sample
    assets (only the root layer's expression variables apply).
    """
    hfs = Path(os.environ["HFS"]).as_posix()
    metadata = stage_metadata(layer)
    return (
        f"#usda 1.0\n(\n{metadata}"
        f'    expressionVariables = {{\n        string HFS = "{hfs}"\n    }}\n'
        f"    subLayers = [@{layer.as_posix()}@]\n)\n\n" + overs(overrides)
    )


def merge(*overrides):
    out = {}
    for o in overrides:
        for path, lines in o.items():
            out.setdefault(path, []).extend(lines)
    return out


def save(exr, target, tonemapped):
    from PIL import Image

    image = (display(pixels(exr), tonemapped) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(image).save(target, quality=90, optimize=True)


def render(name, spec):
    layer = SAMPLES / spec["scene"]
    work = OUT / name
    work.mkdir(parents=True, exist_ok=True)
    target = IMAGES / name
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for index, value in enumerate(spec["values"]):
        root = work / f"{index}.usda"
        root.write_text(
            root_layer(layer, merge(spec.get("base", {}), value["set"])),
            encoding="utf-8",
        )
        exr = work / f"{index}.exr"
        seconds = husk("Unreal Engine", root, exr)
        save(exr, target / f"{index}.jpg", spec["display"] == "tonemapped")
        print(
            f"{name} {index}: {value['caption']} ({seconds:.1f} s)", flush=True
        )


def preview(scene):
    layer = SAMPLES / scene
    OUT.mkdir(parents=True, exist_ok=True)
    root = OUT / f"preview_{layer.stem}.usda"
    root.write_text(root_layer(layer, {}), encoding="utf-8")
    exr = OUT / f"preview_{layer.stem}.exr"
    seconds = husk("Unreal Engine", root, exr)
    save(exr, OUT / f"preview_{layer.stem}.jpg", True)
    print(f"{OUT / f'preview_{layer.stem}.jpg'} ({seconds:.1f} s)")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--only", nargs="+", help="figures to render (default: all)"
    )
    parser.add_argument(
        "--preview",
        nargs="+",
        help="scenes under samples/ to render as they are (tone-mapped)",
    )
    args = parser.parse_args()
    dev_environment()
    # Figures of a local download's cooked assets (the effects page's Niagara pack) render with the runtime that
    # Prepare-ProjectAssets.ps1 prepared for it.
    if os.environ.get("HDUE_DOCS_RUNTIME_CONFIG"):
        os.environ["HDUE_RUNTIME_CONFIG"] = os.environ[
            "HDUE_DOCS_RUNTIME_CONFIG"
        ]
    if args.preview:
        for scene in args.preview:
            preview(scene)
        return
    specs = figures()
    unknown = set(args.only or ()) - set(specs)
    if unknown:
        raise SystemExit(f"unknown figures: {sorted(unknown)}")
    IMAGES.mkdir(parents=True, exist_ok=True)
    (IMAGES / "figures.json").write_text(
        json.dumps(
            {
                name: [v["caption"] for v in spec["values"]]
                for name, spec in specs.items()
            },
            indent=1,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
    failed = []
    for name in args.only or specs:
        try:
            render(name, specs[name])
        except (
            RuntimeError
        ) as error:  # one broken figure must not stop the others
            print(f"{name} FAILED: {str(error)[:500]}", flush=True)
            failed.append(name)
    if failed:
        raise SystemExit(f"failed figures: {failed}")


if __name__ == "__main__":
    main()
