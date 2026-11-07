# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = ["MaterialX==1.39.4"]
# ///
"""Which MaterialX nodes reach UE: every node definition Houdini ships, each wired alone into an emissive Standard
Surface (through a conversion to color3, or a surface for closures), goes through hdUE's export for UE's importer
(export_usd_materialx: Houdini's nodes flattened, UE's MaterialX version), then UE's importer's own expansion
(flattenSubgraphs, except the categories it maps itself) and its list of categories it translates. Writes
out/materialx_coverage.json and prints the categories that fail, with the reason.

    scripts/Test-MaterialXCoverage.ps1    # sets up OpenUSD's Python (as Import-UsdMaterialX.ps1), then runs this
"""

import json
import sys
import tempfile
import traceback
from pathlib import Path

import MaterialX as mx
from pxr import Sdf, Usd, UsdShade

sys.path.insert(0, str(Path(__file__).resolve().parent))
from export_usd_materialx import record, write  # noqa: E402
from local_paths import local_path  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "samples" / "docs" / "textures" / "checker.png"
# UE 5.8's MaterialX importer (Engine/Plugins/Interchange/Runtime/Source/Import/Private/MaterialX/MaterialXUtils):
# the categories it translates (MaterialXManager.cpp:130-385, MaterialXSurfaceShaderAbstract.cpp:1832-1872) and those
# it keeps from flattening because it translates them itself (MaterialXManager.cpp:287-333). Generated from the
# engine source by ue_materialx_categories() below.
SDF = {
    "float": Sdf.ValueTypeNames.Float,
    "integer": Sdf.ValueTypeNames.Int,
    "boolean": Sdf.ValueTypeNames.Bool,
    "color3": Sdf.ValueTypeNames.Color3f,
    "color4": Sdf.ValueTypeNames.Color4f,
    "vector2": Sdf.ValueTypeNames.Float2,
    "vector3": Sdf.ValueTypeNames.Float3,
    "vector4": Sdf.ValueTypeNames.Float4,
    "matrix33": Sdf.ValueTypeNames.Matrix3d,
    "matrix44": Sdf.ValueTypeNames.Matrix4d,
    "string": Sdf.ValueTypeNames.String,
    "filename": Sdf.ValueTypeNames.Asset,
    "surfaceshader": Sdf.ValueTypeNames.Token,
    "BSDF": Sdf.ValueTypeNames.Token,
    "EDF": Sdf.ValueTypeNames.Token,
    "VDF": Sdf.ValueTypeNames.Token,
    "displacementshader": Sdf.ValueTypeNames.Token,
    "volumeshader": Sdf.ValueTypeNames.Token,
    "lightshader": Sdf.ValueTypeNames.Token,
    "material": Sdf.ValueTypeNames.Token,
}


def ue_materialx_categories(engine_root):
    """UE's importer's translated categories and its flattening exceptions, read from its source: the
    MaterialX::Category names its registrations use (InterchangeMaterialXDefinitions.h maps them to the names).
    """
    import re

    base = Path(engine_root) / "Engine/Plugins/Interchange"
    definitions = next(
        base.rglob("InterchangeMaterialXDefinitions.h")
    ).read_text(encoding="utf-8")
    block = definitions[definitions.index("namespace Category") :]
    block = block[: block.index("}")]
    names = dict(re.findall(r"(\w+)\s*=\s*\"([^\"]+)\"", block))
    utils = next(base.rglob("MaterialXManager.cpp")).parent
    source = "\n".join(
        p.read_text(encoding="utf-8") for p in utils.glob("*.cpp")
    )
    manager = (utils / "MaterialXManager.cpp").read_text(encoding="utf-8")
    skip = manager[manager.index("CategoriesToSkip") :]
    skip = skip[: skip.index("};")]
    used = set(re.findall(r"Category::(\w+)", source))
    translated = {names[c] for c in used if c in names}
    skipped = {
        names[c] for c in re.findall(r"Category::(\w+)", skip) if c in names
    }
    return translated, skipped


def converter(doc, output_type):
    """The nodedef chain that turns output_type into color3, or None (closures and non-visual types)."""
    if output_type == "color3":
        return []
    direct = f"ND_convert_{output_type}_color3"
    if doc.getNodeDef(direct):
        return [direct]
    if output_type == "vector2":
        return ["ND_convert_vector2_vector3", "ND_convert_vector3_color3"]
    if output_type == "matrix33":
        return ["ND_transformmatrix_vector3", "ND_convert_vector3_color3"]
    if output_type == "matrix44":
        return ["ND_transformmatrix_vector3M4", "ND_convert_vector3_color3"]
    return None


def author(stage, path, nodedef, library):
    """A material testing nodedef, or (None, reason) when its output cannot reach a surface."""
    definition = library.getNodeDef(nodedef)
    outputs = definition.getActiveOutputs()
    output = next((o for o in outputs if o.getName() == "out"), outputs[0])
    material = UsdShade.Material.Define(stage, path)

    def shader(name, node_id):
        s = UsdShade.Shader.Define(stage, f"{path}/{name}")
        s.CreateIdAttr(node_id)
        return s

    node = shader("node", nodedef)
    # Shader and closure inputs get a real source, as in a real graph (an empty one is no material for UE).
    sources = {
        "surfaceshader": "ND_standard_surface_surfaceshader",
        "displacementshader": "ND_displacement_float",
        "volumeshader": "ND_volume",
        "BSDF": "ND_oren_nayar_diffuse_bsdf",
        "EDF": "ND_uniform_edf",
        "VDF": "ND_absorption_vdf",
    }
    for port in definition.getActiveInputs():
        if port.getType() in sources:
            source = shader(f"source_{port.getName()}", sources[port.getType()])
            node.CreateInput(
                port.getName(), Sdf.ValueTypeNames.Token
            ).ConnectToSource(
                source.CreateOutput("out", Sdf.ValueTypeNames.Token)
            )
        elif port.getType() == "filename":
            node.CreateInput(port.getName(), Sdf.ValueTypeNames.Asset).Set(
                Sdf.AssetPath(str(CHECKER))
            )
    value = node.CreateOutput(
        output.getName(), SDF.get(output.getType(), Sdf.ValueTypeNames.Token)
    )
    kind = output.getType()
    if definition.getNodeString() == "osl_set_ci":
        return None, "OSL's closure output: only in shaders generated for OSL"
    if kind.endswith("array") and kind != "vector2array":
        return (
            None,
            "no MaterialX node reads this array type (only curveadjust's vector2array knots)",
        )
    if kind in ("surfaceshader", "material"):
        material.CreateSurfaceOutput("mtlx").ConnectToSource(value)
        return material, None
    if kind == "lightshader":
        return (
            None,
            "a light shader: USD lights are UsdLux prims, which no USD material binds",
        )
    if kind == "volumeshader":
        material.CreateVolumeOutput("mtlx").ConnectToSource(value)
        return material, None
    if kind == "VDF":
        volume = shader("volume", "ND_volume")
        volume.CreateInput("vdf", Sdf.ValueTypeNames.Token).ConnectToSource(
            value
        )
        material.CreateVolumeOutput("mtlx").ConnectToSource(
            volume.CreateOutput("out", Sdf.ValueTypeNames.Token)
        )
        return material, None
    if kind == "displacementshader":
        material.CreateDisplacementOutput("mtlx").ConnectToSource(value)
        value = None  # an emissive surface beside it
    elif kind in ("filename", "string"):
        image = shader("image", "ND_image_color3")
        image.CreateInput(
            "file",
            (
                Sdf.ValueTypeNames.Asset
                if kind == "filename"
                else Sdf.ValueTypeNames.String
            ),
        ).ConnectToSource(value)
        value, kind = (
            image.CreateOutput("out", Sdf.ValueTypeNames.Color3f),
            "color3",
        )
    elif kind.endswith("array"):
        adjust = shader("curve", "ND_curveadjust_float")
        adjust.CreateInput("in", Sdf.ValueTypeNames.Float).Set(0.5)
        adjust.CreateInput(
            "knots", Sdf.ValueTypeNames.Float2Array
        ).ConnectToSource(value)
        value, kind = (
            adjust.CreateOutput("out", Sdf.ValueTypeNames.Float),
            "float",
        )
    if value is None:
        surface = shader("surface", "ND_standard_surface_surfaceshader")
        surface.CreateInput("base", Sdf.ValueTypeNames.Float).Set(0.5)
        material.CreateSurfaceOutput("mtlx").ConnectToSource(
            surface.CreateOutput("out", Sdf.ValueTypeNames.Token)
        )
        return material, None
    if kind in ("BSDF", "EDF"):
        surface = shader("surface", "ND_surface")
        surface.CreateInput(
            "bsdf" if kind == "BSDF" else "edf", SDF[kind]
        ).ConnectToSource(value)
        out = surface.CreateOutput("out", Sdf.ValueTypeNames.Token)
        material.CreateSurfaceOutput("mtlx").ConnectToSource(out)
        return material, None
    chain = converter(library, kind)
    if chain is None:
        return None, f"output type {kind} has no surface to reach"
    current = value
    for i, step in enumerate(chain):
        convert = shader(f"convert{i}", step)
        convert.CreateInput(
            "mat" if step.startswith("ND_transformmatrix") else "in",
            SDF[kind] if i == 0 else Sdf.ValueTypeNames.Float3,
        ).ConnectToSource(current)
        if step.startswith("ND_transformmatrix"):
            convert.CreateInput("in", Sdf.ValueTypeNames.Float3).Set(
                (1.0, 0.5, 0.25)
            )
        current = convert.CreateOutput(
            "out",
            (
                Sdf.ValueTypeNames.Color3f
                if step.endswith("color3")
                else Sdf.ValueTypeNames.Float3
            ),
        )
    surface = shader("surface", "ND_standard_surface_surfaceshader")
    surface.CreateInput("base", Sdf.ValueTypeNames.Float).Set(0.0)
    surface.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
    surface.CreateInput("emission", Sdf.ValueTypeNames.Float).Set(1.0)
    surface.CreateInput(
        "emission_color", Sdf.ValueTypeNames.Color3f
    ).ConnectToSource(current)
    material.CreateSurfaceOutput("mtlx").ConnectToSource(
        surface.CreateOutput("out", Sdf.ValueTypeNames.Token)
    )
    return material, None


def main():
    houdini = local_path("houdini_root") / "houdini" / "materialx"
    library = mx.createDocument()
    mx.loadLibraries(["libraries"], mx.FileSearchPath(str(houdini)), library)
    translated, skipped = ue_materialx_categories(local_path("engine_root"))
    stage = Usd.Stage.CreateInMemory()
    results = {}
    passing = []
    for definition in sorted(library.getNodeDefs(), key=lambda d: d.getName()):
        name = definition.getName()
        entry = results[name] = {"category": definition.getNodeString()}
        try:
            material, skip = author(stage, f"/mtlx/{name}", name, library)
            if skip:
                entry["status"], entry["reason"] = "untestable", skip
                continue
            with tempfile.TemporaryDirectory() as folder:
                write([record(material)], Path(folder) / "out", "coverage")
                doc = mx.createDocument()
                mx.readFromXmlFile(
                    doc, str(Path(folder) / "out" / "coverage.mtlx")
                )
            ue = mx.createDocument()
            mx.loadLibraries(
                mx.getDefaultDataLibraryFolders(),
                mx.getDefaultDataSearchPath(),
                ue,
            )
            doc.importLibrary(ue)
            # As UE's importer: node graphs expanded, except the categories it translates itself.
            doc.flattenSubgraphs(
                "", lambda node: node.getCategory() not in skipped
            )
            left = sorted(
                {n.getCategory() for n in doc.getNodes()}
                - translated
                - {"surfacematerial", "volumematerial"}
            )
            entry["status"], entry["reason"] = (
                ("ok", "")
                if not left
                else ("unsupported", f"UE translates no {left}")
            )
            if not left:
                passing.append(record(material))
        except (
            Exception
        ) as error:  # noqa: BLE001  (each failure is the result for its node)
            entry["status"], entry["reason"] = (
                "export",
                f"{type(error).__name__}: {error}".splitlines()[0][:300],
            )
            if "--trace" in sys.argv:
                traceback.print_exc()
    if (
        "--export" in sys.argv
    ):  # one document of every passing node, for UE's importer (Import-MaterialX.ps1)
        import shutil

        # Chunks of 40 (one editor run each): UE's importer slows down sharply on large documents.
        folder = ROOT / "out" / "materialx_coverage"
        shutil.rmtree(folder, ignore_errors=True)
        size = (
            1 if "--one" in sys.argv else 40
        )  # --one: a document per node, to find one that UE cannot import
        only = next(
            (
                a.split("=", 1)[1].split(",")
                for a in sys.argv
                if a.startswith("--only=")
            ),
            None,
        )
        chosen = [
            r
            for r in passing
            if not only or r["material"].split("/")[-1] in only
        ]
        for start in range(0, len(chosen), size):
            part = chosen[start : start + size]
            name = (
                part[0]["material"].split("/")[-1]
                if size == 1
                else f"part{start // size:02d}"
            )
            write(part, folder / name, name)
    out = ROOT / "out" / "materialx_coverage.json"
    out.write_text(json.dumps(results, indent=1), encoding="utf-8")
    by_category = {}
    for entry in results.values():
        by_category.setdefault(entry["category"], []).append(entry)
    failing = {
        c: e
        for c, e in by_category.items()
        if any(x["status"] != "ok" for x in e)
    }
    print(
        f"{len(results)} nodedefs, {len(by_category)} categories; {len(failing)} categories with a failing nodedef"
    )
    for category, entries in sorted(failing.items()):
        bad = [e for e in entries if e["status"] != "ok"]
        print(
            f"{category}: {len(bad)}/{len(entries)} {bad[0]['status']}: {bad[0]['reason']}"
        )
    print(out)


if __name__ == "__main__":
    main()
