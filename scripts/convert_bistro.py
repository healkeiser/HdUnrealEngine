"""Converts Amazon Lumberyard Bistro (NVIDIA ORCA, CC BY 4.0) to USD for hdUE: the interior with wine glasses, its
camera and UsdPreviewSurface materials built from the scene's texture convention. The download is local only
(samples/bistro/README.md); this writes samples/bistro/usd/ (also local):

    hython scripts/convert_bistro.py [--source samples/bistro/source/Bistro_v5_2]

Bistro's textures (its README): BaseColor RGB with opacity in alpha; Specular with occlusion in red, roughness in
green and metalness in blue; Normal in DirectX convention (green down); Emissive RGB. They are DDS, which UE's runtime
image import does not read, so they are converted to PNG once (normal maps flipped to OpenGL's convention). Every
material becomes a MaterialX standard_surface, which hdUE translates at render time; glass, water, ice, beer and
wine take transmission, the wines the absorption the scene's Falcor file gives them.
"""

import argparse
import math
from pathlib import Path

import hou
import OpenImageIO as oiio
from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "samples" / "bistro" / "usd"
# The scenes: the interior with wine glasses, and the street outside.
SCENES = {
    "interior": "BistroInterior_Wine.fbx",
    "exterior": "BistroExterior.fbx",
}
# Falcor's absorption coefficients (BistroInterior_Wine.pyscene, per metre), and the IORs it sets.
ABSORPTION = {
    "White_Wine": (12.28758, 16.51818, 20.30273),
    "Red_Wine": (117.13133, 251.91133, 294.33867),
    "TransparentGlassWine": (102.68063, 168.015, 246.80438),
    "Beer": (11.78552, 25.45862, 58.37241),
}
IOR = {"Water": 1.33, "White_Wine": 1.33, "Red_Wine": 1.33, "Ice": 1.31}
GLASS_WORDS = ("glass", "water", "ice", "wine")
# Megascans imperfection maps (a local library; .local/paths.json "megascans_root" or --megascans): roughness for
# the drinking glasses (fine smudges and wipes) and the lacquer coat of the tables (worn varnish), scaled into a
# glossy range. Without the library the glasses take a generated smudge map and the tables no coat.
IMPERFECTIONS = {
    "Smudges": (
        "surface/imperfections_grunge_slnnfalc",
        "slnnfalc_2K_Roughness.jpg",
        0.25,
    ),
    "Varnish": (
        "surface/imperfections_varnish_tjnjadic",
        "tjnjadic_2K_Roughness.jpg",
        0.5,
    ),
    "Metal": (
        "surface/Imperfections_Grunge_sgupegfc",
        "sgupegfc_2K_Roughness.jpg",
        0.25,
    ),
    "Grunge": (
        "surface/imperfections_grunge_slnnegvc",
        "slnnegvc_2K_Roughness.jpg",
        0.12,
    ),
    "Wet": (
        "surface/imperfections_grunge_slnnejbc",
        "slnnejbc_2K_Roughness.jpg",
        0.2,
    ),
    "Grime": (
        "surface/imperfections_grunge_slnnebuc",
        "slnnebuc_2K_Roughness.jpg",
        0.45,
    ),
}
# What each material's roughness takes: its own map (Specular's green), offset by the imperfection map of its kind
# around the map's mean, by the amplitude above (subtle: a few hundredths to a quarter of the range).
METAL_WORDS = (
    "metal",
    "chrome",
    "cutlery",
    "bronze",
    "brass",
    "copper",
    "steel",
    "iron",
    "silver",
    "gold",
    "tin",
)
WOOD_WORDS = (
    "wood",
    "table",
    "chair",
    "bar",
    "floor",
    "door",
    "shelf",
    "frame",
    "bench",
    "stool",
)
# Weathered surfaces take a grime layer: Megascans grime in the mesh's second UV set (uv2, a unique layout over each
# building, so the dirt does not repeat with the texture's tile), as the tint of a coat of IOR 1, which reflects
# nothing and only darkens what is under it. GRIME_COLOR is the dirt's colour where the map is darkest.
GRIME_WORDS = (
    "brick",
    "concrete",
    "plaster",
    "trim",
    "cornice",
    "balcony",
    "roofing",
    "chimney",
    "building",
    "bollard",
    "trashcan",
)
GRIME_COLOR = (0.55, 0.5, 0.45)
# A rainy night's street: pavements take a water coat (IOR 1.33, nearly smooth) whose coverage is a grunge map, so
# the wetness is patchy with puddles, and their own roughness is lowered (the "Wet" bake). WET_COLOR darkens what is
# under the water, as wet stone darkens.
WET_WORDS = ("pavement", "cobble", "curb", "manhole", "sidewalk")
WET_COLOR = (0.7, 0.7, 0.72)
# Emission weight of the emissive textures (lamps, signs); Falcor scales them by 1000 in its own units.
EMISSION = 8.0
# Exposure of the lamps' bulbs (2700 K sphere lights of 5 cm).
LAMP_EXPOSURE = 10.0
# How far light travels for the absorption to reach the authored transmission colour, in metres (USD scene units).
DEPTH = 0.01


def png_textures(source, target):
    """Bistro's DDS and TGA textures as PNG (normal maps with their blue channel rebuilt when stored as two)."""
    target.mkdir(parents=True, exist_ok=True)
    done = 0
    for path in sorted((source / "Textures").iterdir()):
        if path.suffix.lower() not in (".dds", ".tga"):
            continue
        out = target / (path.stem + ".png")
        if out.exists():
            continue
        buf = oiio.ImageBuf(str(path))
        if buf.spec().nchannels == 2:  # BC5 normal maps: x and y only
            rg = buf.get_pixels(oiio.FLOAT)
            x, y = rg[..., 0] * 2 - 1, rg[..., 1] * 2 - 1
            import numpy as np

            z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1)) * 0.5 + 0.5
            rgb = np.dstack([rg[..., 0], rg[..., 1], z])
            buf = oiio.ImageBuf(
                oiio.ImageSpec(rgb.shape[1], rgb.shape[0], 3, oiio.FLOAT)
            )
            buf.set_pixels(oiio.ROI(), rgb)
        if path.stem.endswith(
            "_Normal"
        ):  # DirectX (green down) to MaterialX's OpenGL convention
            pixels = buf.get_pixels(oiio.FLOAT)
            pixels[..., 1] = 1 - pixels[..., 1]
            flipped = oiio.ImageBuf(
                oiio.ImageSpec(
                    pixels.shape[1],
                    pixels.shape[0],
                    pixels.shape[2],
                    oiio.FLOAT,
                )
            )
            flipped.set_pixels(oiio.ROI(), pixels)
            buf = flipped
        buf.set_write_format(oiio.UINT8)
        if not buf.write(str(out)):
            raise RuntimeError(f"{path.name}: {buf.geterror()}")
        done += 1
    print(f"textures: {done} converted to {target}")


def smudge_texture(target):
    """glass_smudges.png: roughness for the glasses, 0.02 clean to about 0.25 where fingers and a cloth left marks:
    soft blotches and wipe arcs from a fixed seed, so every conversion makes the same texture.
    """
    import numpy as np

    if target.exists():
        return
    size = 1024
    rng = np.random.default_rng(7)
    y, x = np.mgrid[0:size, 0:size] / size
    smudge = np.zeros((size, size))
    for _ in range(90):  # fingertip blotches
        cx, cy, r = rng.random(), rng.random(), 0.01 + 0.04 * rng.random()
        smudge += rng.random() * np.exp(
            -((x - cx) ** 2 + (y - cy) ** 2) / (2 * r * r)
        )
    for _ in range(25):  # cloth wipes: thin arcs
        cx, cy, radius, width = (
            rng.random(),
            rng.random(),
            0.1 + 0.4 * rng.random(),
            0.002 + 0.006 * rng.random(),
        )
        ring = np.abs(np.hypot(x - cx, y - cy) - radius)
        smudge += (
            0.4
            * rng.random()
            * np.exp(-(ring**2) / (2 * width * width))
            * (rng.random(x.shape) > 0.3)
        )
    grain = (
        rng.random((size // 8, size // 8)).repeat(8, 0).repeat(8, 1)
    )  # dust specks
    smudge = np.clip(smudge / np.percentile(smudge, 99), 0, 1) * 0.8 + 0.2 * (
        grain > 0.97
    )
    roughness = 0.02 + 0.23 * np.clip(smudge, 0, 1)
    buf = oiio.ImageBuf(oiio.ImageSpec(size, size, 1, oiio.FLOAT))
    buf.set_pixels(oiio.ROI(), roughness.astype(np.float32)[..., None])
    buf.set_write_format(oiio.UINT8)
    buf.write(str(target))


def import_stage(source, scene):
    """The FBX through Houdini's FBX import and a Scene Import LOP, flattened to one layer."""
    hou.hipFile.clear(suppress_save_prompt=True)
    node, _ = hou.hipFile.importFBX(
        str(source / SCENES[scene]),
        suppress_save_prompt=True,
        import_animation=False,
        import_joints_and_skin=False,
        import_lights=False,
        convert_units=True,
    )
    lop = hou.node("/stage").createNode("sceneimport", "bistro")
    lop.parm("rootobject").set(node.path())
    lop.parm("objects").set("*")
    lop.cook(force=True)
    if lop.errors():
        raise RuntimeError(lop.errors())
    return lop.stage().Flatten()


def imperfection_textures(library, target):
    """Copies the Megascans imperfection maps used here; returns {kind: (png name, scale)} for those found."""
    import shutil

    found = {}
    for kind, (folder, file, scale) in IMPERFECTIONS.items():
        source = library / "Downloaded" / folder / file if library else None
        if source and source.exists():
            out = target / f"megascans_{file}"
            if not out.exists():
                shutil.copyfile(source, out)
            found[kind] = (
                out.name,
                scale,
            )  # the glasses' and coats' map scale, the bake's amplitude
    if "Smudges" not in found:
        smudge_texture(target / "glass_smudges.png")
        found["Smudges"] = ("glass_smudges.png", 1.0)
    return found


def surface_kind(name):
    lower = name.lower()
    if "wet" in lower or any(word in lower for word in WET_WORDS):
        return "Wet"
    if any(word in lower for word in METAL_WORDS):
        return "Metal"
    if any(word in lower for word in WOOD_WORDS) and "cloth" not in lower:
        return "Varnish"
    return "Grunge"


def baked_specular(textures, file, imperfection):
    """<material>_Specular_imperfect.png: the material's Specular map with the imperfection map added to its
    roughness (green), resampled to the map's size; cached.
    """
    import numpy as np

    out = textures / file.replace("_Specular.png", "_Specular_imperfect.png")
    if out.exists():
        return out.name
    name, amplitude = imperfection
    spec = oiio.ImageBuf(str(textures / file))
    width, height = spec.spec().width, spec.spec().height
    imp = oiio.ImageBufAlgo.resize(
        oiio.ImageBuf(str(textures / name)),
        roi=oiio.ROI(0, width, 0, height, 0, 1, 0, 1),
    )
    pixels = spec.get_pixels(oiio.FLOAT)
    grey = imp.get_pixels(oiio.FLOAT)[..., 0]
    rough = pixels[..., 1] + amplitude * (grey - grey.mean()) * 2
    if surface_kind(Path(file).stem.replace("_Specular", "")) == "Wet":
        # Where water stays on a street: the crevices between the stones (low occlusion, Specular's red), widened into
        # damp patches by the grunge map; the stones' tops dry. Under the water the stone's own roughness drops.
        crevice = 1 - pixels[..., 0] / max(float(pixels[..., 0].max()), 1e-3)
        wetness = np.clip(
            (crevice - 0.15) * 2.5 + (grey - grey.mean()) * 1.5, 0, 1
        )
        rough = rough * (1 - 0.65 * wetness)
        mask = oiio.ImageBuf(oiio.ImageSpec(width, height, 1, oiio.FLOAT))
        mask.set_pixels(oiio.ROI(), wetness.astype(np.float32)[..., None])
        mask.set_write_format(oiio.UINT8)
        mask.write(str(out).replace("_Specular_imperfect.png", "_Wetness.png"))
    pixels[..., 1] = np.clip(rough, 0.02, 1)
    buf = oiio.ImageBuf(
        oiio.ImageSpec(width, height, pixels.shape[2], oiio.FLOAT)
    )
    buf.set_pixels(oiio.ROI(), pixels)
    buf.set_write_format(oiio.UINT8)
    buf.write(str(out))
    return out.name


def network_inputs(stage, material):
    """What Houdini's FBX import found for a material: its texture files by kind, and its principled constants."""
    files, constants = {}, {}
    for prim in Usd.PrimRange(stage.GetPrimAtPath(material)):
        for attr in prim.GetAttributes():
            value = attr.Get()
            if isinstance(value, Sdf.AssetPath) and value.path:
                stem = Path(value.path).stem
                for kind in ("BaseColor", "Specular", "Normal", "Emissive"):
                    if stem.endswith("_" + kind):
                        files[kind] = stem + ".png"
            elif value is not None and attr.GetName() in (
                "inputs:basecolor",
                "inputs:rough",
                "inputs:metallic",
                "inputs:emitcolor",
            ):
                constants.setdefault(attr.GetName()[7:], value)
    return files, constants


def glass_name(name):
    """Glass and liquids by name: whole words, so "office" or "price" stay opaque."""
    words = {w.lower() for w in name.replace("-", "_").split("_")}
    return bool(words & {"water", "ice", "wine", "beer"}) or (
        "glass" in name.lower() and "metal" not in name.lower()
    )


def build_material(stage, path, textures, imperfections):
    """Replaces a Material prim's network with a MaterialX standard surface built from Bistro's texture convention:
    base colour and coverage from BaseColor, roughness and metalness from Specular's green and blue, the normal map,
    and emission. Specular's red (occlusion) is left out: the renderers compute occlusion themselves.
    """
    files, constants = network_inputs(stage, path)
    if "BaseColor" in files:
        prefix = files["BaseColor"][: -len("_BaseColor.png")]
        for kind in ("Specular", "Emissive", "Normal"):
            files.setdefault(kind, f"{prefix}_{kind}.png")
    files = {
        kind: file for kind, file in files.items() if (textures / file).exists()
    }
    surface_type = surface_kind(path.name)
    if "Specular" in files and surface_type in imperfections:
        files["Specular"] = baked_specular(
            textures, files["Specular"], imperfections[surface_type]
        )
    material_prim = stage.GetPrimAtPath(path)
    for child in list(material_prim.GetChildren()):
        stage.RemovePrim(child.GetPath())
    for prop in list(material_prim.GetAuthoredProperties()):
        material_prim.RemoveProperty(prop.GetName())
    name = path.name
    F, C = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f
    V3, C4 = Sdf.ValueTypeNames.Vector3f, Sdf.ValueTypeNames.Color4f

    def node(node_name, node_id, output_type):
        shader = UsdShade.Shader.Define(stage, path.AppendChild(node_name))
        shader.CreateIdAttr(node_id)
        return shader, shader.CreateOutput("out", output_type)

    uv = None

    def image(node_name, kind, node_id, output_type, color_space=None):
        nonlocal uv
        if uv is None:
            reader, uv = node(
                "mtlx_uv", "ND_geompropvalue_vector2", Sdf.ValueTypeNames.Float2
            )
            reader.CreateInput("geomprop", Sdf.ValueTypeNames.String).Set("st")
        shader, out = node(node_name, node_id, output_type)
        attr = shader.CreateInput("file", Sdf.ValueTypeNames.Asset)
        attr.Set(Sdf.AssetPath(f"./textures/{files[kind]}"))
        if color_space:
            attr.GetAttr().SetColorSpace(color_space)
        shader.CreateInput(
            "texcoord", Sdf.ValueTypeNames.Float2
        ).ConnectToSource(uv)
        return out

    def scaled(node_name, source, scale):
        if scale == 1.0:
            return source
        shader, out = node(node_name, "ND_multiply_float", F)
        shader.CreateInput("in1", F).ConnectToSource(source)
        shader.CreateInput("in2", F).Set(scale)
        return out

    def channel(node_name, source, input_type, extract_id, index):
        shader, out = node(node_name, extract_id, F)
        shader.CreateInput("in", input_type).ConnectToSource(source)
        shader.CreateInput("index", Sdf.ValueTypeNames.Int).Set(index)
        return out

    surface = UsdShade.Shader.Define(stage, path.AppendChild("mtlx_surface"))
    surface.CreateIdAttr("ND_standard_surface_surfaceshader")
    surface_out = surface.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    UsdShade.Material(material_prim).CreateSurfaceOutput(
        "mtlx"
    ).ConnectToSource(surface_out)

    def set_input(key, type_name, value):
        attr = surface.CreateInput(key, type_name)
        if hasattr(value, "GetAttr"):
            attr.ConnectToSource(value)
        else:
            attr.Set(value)

    if glass_name(name):
        set_input("base", F, 0.0)
        set_input("transmission", F, 1.0)
        # Frosted panes scatter; window and picture panes are single sheets (thin walled, as glass of no thickness).
        if (
            "Frozen" in name
            or name == "Ice"
            or not glass_name(name)
            or name in IOR
        ):
            set_input(
                "specular_roughness",
                F,
                0.35 if "Frozen" in name else 0.1 if name == "Ice" else 0.0,
            )
        elif (
            "BaseColor" in files
        ):  # textured glass: its grime map, as smudged and streaked reflections
            set_input(
                "specular_roughness",
                F,
                scaled(
                    "mtlx_glass_grime",
                    image(
                        "mtlx_glass_grime_map", "BaseColor", "ND_image_float", F
                    ),
                    0.6,
                ),
            )
        else:  # drinking glasses and bottles: fingerprints and cloth marks
            files["Smudges"], scale = imperfections["Smudges"]
            set_input(
                "specular_roughness",
                F,
                scaled(
                    "mtlx_smudges",
                    image("mtlx_smudges_map", "Smudges", "ND_image_float", F),
                    scale,
                ),
            )
        if "Exterior" in name or "Paintings" in name or "Frozen" in name:
            set_input("thin_walled", Sdf.ValueTypeNames.Bool, True)
        set_input("specular_IOR", F, IOR.get(name, 1.55))
        if name in ABSORPTION:
            set_input(
                "transmission_color",
                C,
                Gf.Vec3f(*(math.exp(-s * DEPTH) for s in ABSORPTION[name])),
            )
            set_input("transmission_depth", F, DEPTH)
        return "glass"
    set_input("base", F, 1.0)
    if "BaseColor" in files:
        rgba = image(
            "mtlx_base_color",
            "BaseColor",
            "ND_image_color4",
            C4,
            "srgb_texture",
        )
        rgb, rgb_out = node("mtlx_base_rgb", "ND_convert_color4_color3", C)
        rgb.CreateInput("in", C4).ConnectToSource(rgba)
        set_input("base_color", C, rgb_out)
        alpha = channel("mtlx_alpha", rgba, C4, "ND_extract_color4", 3)
        coverage, coverage_out = node(
            "mtlx_coverage", "ND_convert_float_color3", C
        )
        coverage.CreateInput("in", F).ConnectToSource(alpha)
        set_input("opacity", C, coverage_out)
    else:
        set_input(
            "base_color",
            C,
            Gf.Vec3f(*constants.get("basecolor", (0.8, 0.8, 0.8))),
        )
    if "Specular" in files:
        orm = image("mtlx_specular", "Specular", "ND_image_vector3", V3)
        set_input(
            "specular_roughness",
            F,
            channel("mtlx_roughness", orm, V3, "ND_extract_vector3", 1),
        )
        set_input(
            "metalness",
            F,
            channel("mtlx_metalness", orm, V3, "ND_extract_vector3", 2),
        )
    else:
        set_input("specular_roughness", F, float(constants.get("rough", 0.5)))
        set_input(
            "metalness",
            F,
            (
                1.0
                if surface_type == "Metal"
                else float(constants.get("metallic", 0.0))
            ),
        )
    if "Normal" in files:
        raw = image("mtlx_normal_image", "Normal", "ND_image_vector3", V3)
        decode, normal = node("mtlx_normal", "ND_normalmap_float", V3)
        decode.CreateInput("in", V3).ConnectToSource(raw)
        decode.CreateInput("scale", F).Set(1.0)
        set_input("normal", V3, normal)
    if (
        "Varnish" in imperfections
        and "table" in name.lower()
        and "cloth" not in name.lower()
    ):
        # Lacquered tables: a clear coat whose roughness is worn varnish.
        files["Varnish"], scale = imperfections["Varnish"]
        set_input("coat", F, 1.0)
        set_input("coat_IOR", F, 1.5)
        set_input(
            "coat_roughness",
            F,
            scaled(
                "mtlx_varnish",
                image("mtlx_varnish_map", "Varnish", "ND_image_float", F),
                scale,
            ),
        )
    if "Wet" in imperfections and any(
        word in name.lower() for word in WET_WORDS
    ):
        wetness = files.get("Specular", "").replace(
            "_Specular_imperfect.png", "_Wetness.png"
        )
        if (
            "ground_wet" in name.lower()
        ):  # the scene's own puddle decals: standing water
            set_input("coat", F, 1.0)
        elif wetness and (textures / wetness).exists():
            files["Wetness"] = wetness
            set_input(
                "coat", F, image("mtlx_wetness", "Wetness", "ND_image_float", F)
            )
        else:
            files["Puddles"] = imperfections["Wet"][0]
            set_input(
                "coat", F, image("mtlx_puddles", "Puddles", "ND_image_float", F)
            )
        set_input("coat_IOR", F, 1.33)
        # The film follows each stone (the surface's normal map) and is nearly glassy, as water is.
        set_input("coat_roughness", F, 0.05)
        if "Normal" in files:
            set_input("coat_normal", V3, normal)
        set_input("coat_color", C, Gf.Vec3f(*WET_COLOR))
    elif "Grime" in imperfections and any(
        word in name.lower() for word in GRIME_WORDS
    ):
        files["Grime"], amount = imperfections["Grime"]
        reader, uv2 = node(
            "mtlx_uv2", "ND_geompropvalue_vector2", Sdf.ValueTypeNames.Float2
        )
        reader.CreateInput("geomprop", Sdf.ValueTypeNames.String).Set("uv2")
        grime, grime_out = node("mtlx_grime_map", "ND_image_float", F)
        grime.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
            Sdf.AssetPath(f"./textures/{files['Grime']}")
        )
        grime.CreateInput(
            "texcoord", Sdf.ValueTypeNames.Float2
        ).ConnectToSource(uv2)
        tint, tint_out = node(
            "mtlx_grime", "ND_mix_color3_float", C
        )  # white where clean, dirt colour where grimy
        tint.CreateInput("bg", C).Set(Gf.Vec3f(1, 1, 1))
        tint.CreateInput("fg", C).Set(
            Gf.Vec3f(*(1 - amount * (1 - c) for c in GRIME_COLOR))
        )
        tint.CreateInput("mix", F).ConnectToSource(grime_out)
        set_input("coat", F, 1.0)
        set_input("coat_IOR", F, 1.0)
        set_input("coat_roughness", F, 1.0)
        set_input("coat_color", C, tint_out)
    if "Emissive" in files:
        set_input("emission", F, EMISSION)
        set_input(
            "emission_color",
            C,
            image(
                "mtlx_emission",
                "Emissive",
                "ND_image_color3",
                C,
                "srgb_texture",
            ),
        )
    return "textured" if "BaseColor" in files else "constant"


def lamp_lights(stage, target):
    """bistro_lights.usda: a sphere light in each lamp whose shade is emissive, as a UE level pairs a lamp mesh with a
    light (an emissive surface alone barely lights a room). The bulb sits near the bottom of the lamp's bounds.
    """
    from pxr import UsdLux

    emissive = {
        prim.GetPath()
        for prim in stage.Traverse()
        if prim.GetTypeName() == "Material"
        and stage.GetPrimAtPath(prim.GetPath().AppendChild("mtlx_emission"))
    }
    bounds = UsdGeom.BBoxCache(Usd.TimeCode.Default(), ["default", "render"])
    target.unlink(missing_ok=True)
    lights = Usd.Stage.CreateNew(str(target))
    UsdGeom.SetStageUpAxis(lights, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(lights, 1.0)
    UsdGeom.Xform.Define(lights, "/lamps")
    shades = []
    for prim in stage.Traverse():
        if prim.IsA(UsdGeom.Mesh):
            material, _ = UsdShade.MaterialBindingAPI(
                prim
            ).ComputeBoundMaterial()
            if material and material.GetPath() in emissive:
                shades.append(prim)
    sky = UsdLux.DomeLight.Define(lights, "/lamps/sky")
    sky.CreateTextureFileAttr(
        "../source/Bistro_v5_2/san_giuseppe_bridge_4k.hdr"
    )
    sky.CreateTextureFormatAttr("latlong")
    sky.CreateIntensityAttr(1.0)
    lit = [sky]
    for count, prim in enumerate(shades):
        box = bounds.ComputeWorldBound(prim).ComputeAlignedRange()
        low, high = box.GetMin(), box.GetMax()
        bulb = Gf.Vec3d(
            (low[0] + high[0]) / 2,
            low[1] + 0.15 * (high[1] - low[1]),
            (low[2] + high[2]) / 2,
        )
        light = UsdLux.SphereLight.Define(lights, f"/lamps/lamp{count:02d}")
        light.CreateRadiusAttr(0.05)
        light.CreateIntensityAttr(1.0)
        light.CreateExposureAttr(LAMP_EXPOSURE)
        light.CreateEnableColorTemperatureAttr(True)
        light.CreateColorTemperatureAttr(2700.0)
        light.GetPrim().CreateAttribute(
            "unreal:light:VolumetricScatteringIntensity",
            Sdf.ValueTypeNames.Float,
        ).Set(2.0)
        light.AddTranslateOp().Set(bulb)
        lit.append(light)
    # The shades are glowing milk glass: no light's shadows include them. (UE has no per-light shadow exclusion; a
    # mesh outside every light's shadow link casts no shadow, in Realtime and in the Path Tracer alike.)
    for light in lit:
        link = UsdLux.LightAPI(light.GetPrim()).GetShadowLinkCollectionAPI()
        link.CreateIncludeRootAttr(True)
        link.CreateExcludesRel().SetTargets([prim.GetPath() for prim in shades])
    lights.Save()
    print(f"lights: {len(shades)} lamps and the sky in {target}")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=ROOT / "samples" / "bistro" / "source" / "Bistro_v5_2",
    )
    parser.add_argument("--scene", choices=SCENES, default="interior")
    parser.add_argument(
        "--megascans",
        type=Path,
        help='Megascans library (default: .local/paths.json "megascans_root")',
    )
    args = parser.parse_args()
    png_textures(args.source, OUT / "textures")
    library = args.megascans
    paths = ROOT / ".local" / "paths.json"
    if not library and paths.is_file():
        import json

        library = json.loads(paths.read_text(encoding="utf-8-sig")).get(
            "megascans_root"
        )
    imperfections = imperfection_textures(
        Path(library) if library else None, OUT / "textures"
    )
    print("imperfections:", imperfections)
    layer = import_stage(args.source, args.scene)
    stage = Usd.Stage.Open(layer)
    materials = [
        prim.GetPath()
        for prim in stage.Traverse()
        if prim.GetTypeName() == "Material"
    ]
    counts = {}
    # Collected first: rebuilding a network edits the layer the traversal reads.
    for path in materials:
        kind = build_material(stage, path, OUT / "textures", imperfections)
        counts[kind] = counts.get(kind, 0) + 1
    print(
        "materials:",
        counts,
        "roots:",
        list(layer.pseudoRoot.nameChildren.keys()),
    )
    print(
        "metersPerUnit",
        UsdGeom.GetStageMetersPerUnit(stage),
        "upAxis",
        UsdGeom.GetStageUpAxis(stage),
    )
    suffix = "" if args.scene == "interior" else "_exterior"
    lamp_lights(stage, OUT / f"bistro{suffix}_lights.usda")
    target = OUT / f"bistro_{args.scene}.usdc"
    layer.Export(str(target))
    print(f"wrote {target}")


if __name__ == "__main__":
    main()
