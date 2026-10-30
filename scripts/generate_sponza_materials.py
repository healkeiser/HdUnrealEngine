"""Writes samples/sponza/materials.usda: a MaterialX standard_surface network for every material of Intel's Sponza
2022 (main scene, curtains and cypress tree), driven by Intel's own texture maps.

Intel exports UsdPreviewSurface only. This layer adds `outputs:mtlx:surface` to the same Material prims (overs, so
bindings are untouched); hdUE and Karma both pick the mtlx render context over the universal one. Parameters follow the
surface class (stone, wood, metal, fabric, foliage, glass, ...), replacing the exporter's placeholders (IOR 1.1, metalness
maps that are grime noise on stone, leaf opacity read from colour instead of alpha).

    hython scripts/generate_sponza_materials.py [--intel DIR] [--output FILE]
"""

import argparse
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPONZA = ROOT / "samples" / "sponza"
SOURCES = [
    "main/NewSponza_Main_USD_Yup_003.usda",
    "curtains/NewSponza_Curtains_USD_YUp.usda",
    "cypress_tree/NewSponza_CypressTree_USDA_YUp.usda",
]

# Per class, standard_surface constants; "metalness_map" keeps Intel's metalness texture (real metal masks: the metal
# door, the iron fittings of the wooden door, the gold borders of the curtains). Elsewhere it is grime noise (stone maps
# reach 0.37), so metalness is 0. Dielectrics use IOR 1.5 (F0 4 %), not the exporter's 1.1.
STONE = {"specular_IOR": 1.5}
CLASSES = {
    "stone": STONE,
    "wood": {"specular_IOR": 1.5, "metalness_map": True},
    "metal": {"specular_IOR": 1.5, "metalness_map": True},
    # Woven fabric: sheen from the base colour, thin sheet with diffuse transmission (backlit curtains glow).
    "fabric": {
        "specular_IOR": 1.5,
        "specular": 0.5,
        "metalness_map": True,
        "sheen": 0.8,
        "sheen_roughness": 0.4,
        "subsurface": 0.3,
        "thin_walled": True,
    },
    # Leaves: thin, waxy, translucent, cut out by the texture's alpha.
    "foliage": {
        "specular_IOR": 1.5,
        "specular": 0.5,
        "specular_roughness": 0.45,
        "subsurface": 0.5,
        "thin_walled": True,
    },
    "bark": {"specular_IOR": 1.5, "specular_roughness": 0.8},
    "twigs": {"specular_IOR": 1.5, "specular_roughness": 0.8},
    # Window panes: clear thin glass. hdUE renders transmission as see-through coverage (opacity 1 - 0.95, Intel's
    # 0.05); Karma refracts it.
    "glass": {
        "base_color": (1, 1, 1),
        "specular_IOR": 1.5,
        "specular_roughness": 0,
        "transmission": 0.95,
        "thin_walled": True,
    },
    # Lantern glass: frosted blue, Intel's colour and coverage (0.6).
    "tinted_glass": {
        "specular_IOR": 1.5,
        "transmission": 0.4,
        "thin_walled": True,
    },
    "bulb": {"specular_IOR": 1.5, "specular_roughness": 0.1, "emission": 1},
    "decal": {"specular_IOR": 1.5, "specular_roughness": 0.7},
}
RULES = [
    (r"curtain", "fabric"),
    (r"^leaf", "foliage"),
    (r"^bark", "bark"),
    (r"smallbranches", "twigs"),
    (r"^glass$", "glass"),
    (r"lamp_glass", "tinted_glass"),
    (r"light_bulb", "bulb"),
    (r"decal", "decal"),
    (r"metal", "metal"),
    (r"wood", "wood"),
    (r".", "stone"),
]


def surface_class(name):
    return next(c for pattern, c in RULES if re.search(pattern, name.lower()))


def texture(shader, input_name):
    """(resolved file, UV primvar) of the UsdUVTexture driving a Preview Surface input, else None."""
    from pxr import UsdShade

    def upstream(shader_input):
        """The shader whose output drives an input, through the exports' NodeGraphs."""
        attrs = (
            shader_input.GetValueProducingAttributes() if shader_input else []
        )
        if not attrs or not UsdShade.Output.IsOutput(attrs[0]):
            return None
        return UsdShade.Shader(attrs[0].GetPrim())

    image = upstream(shader.GetInput(input_name))
    if not image:
        return None
    path = (
        image.GetInput("file")
        .GetValueProducingAttributes()[0]
        .Get()
        .resolvedPath
    )
    node = upstream(image.GetInput("st"))
    # Through the UsdTransform2d (identity in these exports) to the primvar reader.
    if node and node.GetIdAttr().Get() == "UsdTransform2d":
        node = upstream(node.GetInput("in"))
    varname = node.GetInput("varname") if node else None
    attrs = varname.GetValueProducingAttributes() if varname else []
    return path, (attrs[0].Get() if attrs else "st")


def preview_value(shader, name):
    value = shader.GetInput(name).Get()
    return tuple(value) if hasattr(value, "__len__") else value


def author(stage, material, preview, name, out_dir):
    from pxr import Gf, Sdf, UsdShade

    cls = surface_class(name)
    params = dict(CLASSES[cls])
    path = material.GetPath()
    maps = {
        key: texture(preview, key)
        for key in (
            "diffuseColor",
            "roughness",
            "metallic",
            "normal",
            "opacity",
        )
    }
    uv_name = next((m[1] for m in maps.values() if m), "st")

    def rel(file):
        return "./" + Path(os.path.relpath(file, out_dir)).as_posix()

    def node(node_name, node_id, output_type):
        shader = UsdShade.Shader.Define(stage, path.AppendChild(node_name))
        shader.CreateIdAttr(node_id)
        return shader, shader.CreateOutput("out", output_type)

    uv = None

    def image(node_name, file, node_id, output_type, color_space=None):
        nonlocal uv
        if uv is None:
            reader, uv = node(
                "mtlx_uv", "ND_geompropvalue_vector2", Sdf.ValueTypeNames.Float2
            )
            reader.CreateInput("geomprop", Sdf.ValueTypeNames.String).Set(
                uv_name
            )
        shader, out = node(node_name, node_id, output_type)
        attr = shader.CreateInput("file", Sdf.ValueTypeNames.Asset)
        attr.Set(Sdf.AssetPath(rel(file)))
        if color_space:
            attr.GetAttr().SetColorSpace(color_space)
        shader.CreateInput(
            "texcoord", Sdf.ValueTypeNames.Float2
        ).ConnectToSource(uv)
        return out

    surface = UsdShade.Shader.Define(stage, path.AppendChild("mtlx_surface"))
    surface.CreateIdAttr("ND_standard_surface_surfaceshader")
    surface_out = surface.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    inputs = {}

    def set_input(key, type_name, value):
        inputs[key] = surface.CreateInput(key, type_name)
        if hasattr(value, "GetAttr"):
            inputs[key].ConnectToSource(value)
        else:
            inputs[key].Set(value)

    F, C = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f
    set_input("base", F, 1.0)
    if maps["diffuseColor"]:
        base_color = image(
            "mtlx_base_color",
            maps["diffuseColor"][0],
            "ND_image_color3",
            C,
            "srgb_texture",
        )
    else:
        base_color = Gf.Vec3f(
            *params.get("base_color", preview_value(preview, "diffuseColor"))
        )
    set_input("base_color", C, base_color)
    if params.get("metalness_map") and maps["metallic"]:
        set_input(
            "metalness",
            F,
            image("mtlx_metalness", maps["metallic"][0], "ND_image_float", F),
        )
    else:
        set_input("metalness", F, 0.0)
    set_input("specular", F, params.get("specular", 1.0))
    set_input("specular_IOR", F, params["specular_IOR"])
    if "specular_roughness" in params or not maps["roughness"]:
        roughness = params.get(
            "specular_roughness", preview_value(preview, "roughness")
        )
    else:
        roughness = image(
            "mtlx_roughness", maps["roughness"][0], "ND_image_float", F
        )
    set_input("specular_roughness", F, roughness)
    if params.get("sheen"):
        set_input("sheen", F, params["sheen"])
        set_input("sheen_color", C, base_color)
        set_input("sheen_roughness", F, params["sheen_roughness"])
    if params.get("subsurface"):
        set_input("subsurface", F, params["subsurface"])
        set_input("subsurface_color", C, base_color)
    if params.get("transmission"):
        set_input("transmission", F, params["transmission"])
        set_input("transmission_color", C, base_color)
    if params.get("emission"):
        set_input("emission", F, params["emission"])
        set_input(
            "emission_color",
            C,
            Gf.Vec3f(*preview_value(preview, "emissiveColor")),
        )
    if params.get("thin_walled"):
        set_input("thin_walled", Sdf.ValueTypeNames.Bool, True)
    if maps["opacity"]:
        # Coverage: the leaf texture's alpha (Intel's export reads its colour), the decal's mask.
        if cls == "foliage":
            rgba = image(
                "mtlx_opacity",
                maps["opacity"][0],
                "ND_image_color4",
                Sdf.ValueTypeNames.Color4f,
            )
            extract, alpha = node("mtlx_alpha", "ND_extract_color4", F)
            extract.CreateInput(
                "in", Sdf.ValueTypeNames.Color4f
            ).ConnectToSource(rgba)
            extract.CreateInput("index", Sdf.ValueTypeNames.Int).Set(3)
        else:
            alpha = image(
                "mtlx_opacity", maps["opacity"][0], "ND_image_float", F
            )
        convert, coverage = node("mtlx_coverage", "ND_convert_float_color3", C)
        convert.CreateInput("in", F).ConnectToSource(alpha)
        set_input("opacity", C, coverage)
    if maps["normal"]:
        raw = image(
            "mtlx_normal_image",
            maps["normal"][0],
            "ND_image_vector3",
            Sdf.ValueTypeNames.Vector3f,
        )
        decode, normal = node(
            "mtlx_normal", "ND_normalmap_float", Sdf.ValueTypeNames.Vector3f
        )
        decode.CreateInput("in", Sdf.ValueTypeNames.Vector3f).ConnectToSource(
            raw
        )
        decode.CreateInput("scale", F).Set(1.0)
        set_input("normal", Sdf.ValueTypeNames.Vector3f, normal)
    UsdShade.Material(stage.GetPrimAtPath(path)).CreateSurfaceOutput(
        "mtlx"
    ).ConnectToSource(surface_out)
    return cls


def main():
    from pxr import Sdf, Usd, UsdShade

    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--intel", type=Path, default=SPONZA / "intel")
    parser.add_argument(
        "--output", type=Path, default=SPONZA / "materials.usda"
    )
    args = parser.parse_args()
    out_dir = args.output.resolve().parent
    layer = Sdf.Layer.CreateAnonymous(".usda")
    stage = Usd.Stage.Open(layer)
    summary = []
    for source in SOURCES:
        intel = Usd.Stage.Open(str(args.intel / source), Usd.Stage.LoadNone)
        for prim in intel.Traverse():
            if not prim.IsA(UsdShade.Material):
                continue
            preview = UsdShade.Material(prim).ComputeSurfaceSource()[0]
            cls = author(stage, prim, preview, prim.GetName(), out_dir)
            summary.append(f"{prim.GetPath()} {cls}")
    # Only the shaders are defined here; the Material prims and their parents stay overs on Intel's layers, so this
    # layer adds nothing where a package is missing.
    for spec in [layer.GetPrimAtPath(p.split()[0]) for p in summary]:
        while spec and spec.path != Sdf.Path.absoluteRootPath:
            spec.specifier = Sdf.SpecifierOver
            spec.typeName = ""
            spec = spec.nameParent
    layer.documentation = (
        "MaterialX standard_surface networks for Intel's Sponza 2022 (main, curtains, cypress tree), generated by "
        "scripts/generate_sponza_materials.py from Intel's UsdPreviewSurface materials and texture maps. Sublayer it "
        "over the scene and its add-ons: hdUE and Karma render the mtlx context."
    )
    layer.Export(str(args.output))
    print("\n".join(summary))
    print(f"wrote {args.output} ({len(summary)} materials)")


if __name__ == "__main__":
    main()
