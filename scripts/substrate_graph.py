"""Versioned, typed Substrate graph shared by VOP, USD and UE preparation."""

import hashlib
import json
import math
import struct

from pxr import Sdf, UsdShade

# (type, default). None is an optional, unconnected shader input.
NODES = {
    "materialx": dict(
        label="MaterialX Surface",
        output="surface",
        inputs={},
        properties={"material": ("string", "/Looks/Base")},
    ),
    "slab": dict(
        label="Substrate Slab",
        output="surface",
        inputs={
            "diffuse_albedo": ("color3", (0.18, 0.18, 0.18)),
            "f0": ("color3", (0.04, 0.04, 0.04)),
            "roughness": ("float", 0.5),
            "normal": ("color3", None),
            "mfp": ("color3", (0.0, 0.0, 0.0)),
            # Optional: UE's own value when unconnected (F90 white, no second roughness, no emission).
            "f90": ("color3", None),
            "second_roughness": ("float", None),
            "second_roughness_weight": ("float", None),
            "emission": ("color3", None),
        },
        properties={"subsurface": ("string", "none")},
    ),
    "weight": dict(
        label="Substrate Weight",
        output="surface",
        inputs={"surface": ("surface", None), "weight": ("float", 1.0)},
        properties={},
    ),
    "vertical": dict(
        label="Substrate Vertical Layer",
        output="surface",
        inputs={
            "top": ("surface", None),
            "base": ("surface", None),
            "thickness": ("float", 0.01),
        },
        properties={"parameter_blending": ("bool", False)},
    ),
    "horizontal": dict(
        label="Substrate Horizontal Mix",
        output="surface",
        inputs={
            "background": ("surface", None),
            "foreground": ("surface", None),
            "mix": ("float", 0.5),
        },
        properties={"parameter_blending": ("bool", False)},
    ),
    "scalar": dict(
        label="Scalar Parameter",
        output="float",
        inputs={},
        properties={
            "parameter_name": ("string", "Amount"),
            "value": ("float", 1.0),
        },
    ),
    "color": dict(
        label="Color Parameter",
        output="color3",
        inputs={},
        properties={
            "parameter_name": ("string", "Color"),
            "value": ("color3", (1.0, 1.0, 1.0)),
        },
    ),
    "constant": dict(
        label="Float Constant",
        output="float",
        inputs={},
        properties={"value": ("float", 1.0)},
    ),
    "constant_color": dict(
        label="Color Constant",
        output="color3",
        inputs={},
        properties={"value": ("color3", (1.0, 1.0, 1.0))},
    ),
    "multiply": dict(
        label="Float Multiply",
        output="float",
        inputs={"a": ("float", 1.0), "b": ("float", 1.0)},
        properties={},
    ),
    "saturate": dict(
        label="Float Saturate",
        output="float",
        inputs={"value": ("float", 0.0)},
        properties={},
    ),
    "primvar": dict(
        label="Float Mask Primvar [0..1]",
        output="float",
        inputs={},
        properties={"primvar": ("string", "mask"), "default": ("float", 1.0)},
    ),
    "color_from_float": dict(
        label="Float to Color",
        output="color3",
        inputs={"value": ("float", 0.0)},
        properties={},
    ),
    # More of UE's Substrate BSDFs and operators, with UE's defaults for unconnected inputs (MaterialExpressionSubstrate.cpp,
    # each Compile). Normal and tangent inputs are optional: unconnected, the surface's own.
    "add": dict(
        label="Substrate Add",
        output="surface",
        inputs={"a": ("surface", None), "b": ("surface", None)},
        properties={"parameter_blending": ("bool", False)},
    ),
    "select": dict(
        label="Substrate Select",
        output="surface",
        inputs={
            "a": ("surface", None),
            "b": ("surface", None),
            "select": ("float", 0.0),
        },
        properties={"threshold": ("float", 0.5)},
    ),
    "hair": dict(
        label="Substrate Hair",
        output="surface",
        inputs={
            "base_color": ("color3", (0.0, 0.0, 0.0)),
            "scatter": ("float", 0.0),
            "specular": ("float", 0.5),
            "roughness": ("float", 0.5),
            "backlit": ("float", 0.0),
            "tangent": ("color3", None),
            "emission": ("color3", (0.0, 0.0, 0.0)),
        },
        properties={},
    ),
    "eye": dict(
        label="Substrate Eye",
        output="surface",
        inputs={
            "diffuse_color": ("color3", (0.0, 0.0, 0.0)),
            "roughness": ("float", 0.5),
            "cornea_normal": ("color3", None),
            "iris_normal": ("color3", None),
            "iris_plane_normal": ("color3", None),
            "iris_mask": ("float", 0.0),
            "iris_distance": ("float", 0.0),
            "emission": ("color3", (0.0, 0.0, 0.0)),
        },
        properties={},
    ),
    "water": dict(
        label="Substrate Single Layer Water",
        output="surface",
        inputs={
            "base_color": ("color3", (0.0, 0.0, 0.0)),
            "metallic": ("float", 0.0),
            "specular": ("float", 0.5),
            "roughness": ("float", 0.5),
            "normal": ("color3", None),
            "emission": ("color3", (0.0, 0.0, 0.0)),
            "top_opacity": ("float", 0.0),
            "water_albedo": ("color3", (0.0, 0.0, 0.0)),
            "water_extinction": ("color3", (0.0, 0.0, 0.0)),
            "water_phase_g": ("float", 0.0),
            "color_behind_water": ("color3", (1.0, 1.0, 1.0)),
        },
        properties={},
    ),
    "thin_film": dict(
        label="Substrate Thin Film",
        output="color3",
        outputs={"f0": "color3", "f90": "color3"},
        inputs={
            "normal": ("color3", None),
            "f0": ("color3", (0.04, 0.04, 0.04)),
            "f90": ("color3", (1.0, 1.0, 1.0)),
            "thickness": ("float", 1.0),
            "ior": ("float", 1.44),
        },
        properties={},
    ),
    "metalness": dict(
        label="Metalness to Albedo and F0",
        output="color3",
        outputs={"diffuse_albedo": "color3", "f0": "color3"},
        inputs={
            "base_color": ("color3", (0.18, 0.18, 0.18)),
            "metallic": ("float", 0.0),
            "specular": ("float", 0.5),
        },
        properties={},
    ),
    "haziness": dict(
        label="Haziness to Second Roughness",
        output="float",
        outputs={
            "second_roughness": "float",
            "second_roughness_weight": "float",
        },
        inputs={"base_roughness": ("float", 0.1), "haziness": ("float", 0.5)},
        properties={},
    ),
    "transmittance": dict(
        label="Transmittance to MFP",
        output="color3",
        outputs={"mfp": "color3", "thickness": "float"},
        inputs={
            "transmittance": ("color3", (0.5, 0.5, 0.5)),
            "thickness": ("float", 0.01),
        },
        properties={},
    ),
    # A decal material (an UnrealDecal prim's): UE's Substrate Convert To Decal over a surface, in UE's Deferred Decal
    # domain, its coverage the decal's opacity.
    "decal": dict(
        label="Substrate Convert To Decal",
        output="decal",
        inputs={"surface": ("surface", None), "coverage": ("float", 1.0)},
        properties={},
    ),
    # Volume materials (a Volume prim's heterogeneous volume): UE's Substrate Volumetric-Fog-Cloud BSDF, which
    # heterogeneous volumes read (HeterogeneousVolumesLiveShadingUtils.ush). Unconnected, extinction and emission are
    # those of hdUE's default volume material (M_HdUEVolume): the density grid times the volume's extinction scale, and
    # the BlackBody of its emission field (primvars:unreal:emissionField, ...).
    "volume_fog_cloud": dict(
        label="Substrate Volumetric Fog Cloud",
        output="volume",
        inputs={
            "albedo": ("color3", (0.8, 0.8, 0.8)),
            "extinction": ("color3", None),
            "emission": ("color3", None),
        },
        properties={},
    ),
    # A grid of the Volume prim, at the shaded point: its density field, or the field primvars:unreal:emissionField
    # names (the grids the backend uploads).
    "volume_field": dict(
        label="Volume Field",
        output="float",
        inputs={},
        properties={"field": ("string", "density")},
    ),
    # Ramps: keys [position, value, interpolation] (linear, constant or smooth), compiled into a UE curve in the
    # material's curve atlas (CurveAtlasRowParameter). Editing a key recompiles the material.
    "float_ramp": dict(
        label="Float Ramp",
        output="float",
        inputs={"value": ("float", 0.0)},
        properties={
            "ramp": ("ramp", [[0.0, 0.0, "linear"], [1.0, 1.0, "linear"]])
        },
    ),
    "color_ramp": dict(
        label="Color Ramp",
        output="color3",
        inputs={"value": ("float", 0.0)},
        properties={
            "ramp": (
                "ramp",
                [
                    [0.0, [0.0, 0.0, 0.0], "linear"],
                    [1.0, [1.0, 1.0, 1.0], "linear"],
                ],
            )
        },
    ),
    "blackbody": dict(
        label="Blackbody",
        output="color3",
        inputs={"temperature": ("float", 6500.0)},
        properties={},
    ),
    "unlit": dict(
        label="Substrate Unlit",
        output="surface",
        inputs={"emission": ("color3", (0.0, 0.0, 0.0))},
        properties={},
    ),
    "uv": dict(
        label="UV Coordinates",
        output="vector2",
        inputs={
            "scale": ("vector2", (1.0, 1.0)),
            "offset": ("vector2", (0.0, 0.0)),
        },
        properties={"index": ("float", 0.0)},
    ),
    "texture": dict(
        label="Texture Sample",
        output="color3",
        outputs={
            "out": "color3",
            "rgba": "color4",
            "r": "float",
            "g": "float",
            "b": "float",
            "a": "float",
        },
        inputs={"uv": ("vector2", None)},
        properties={
            "file": ("asset", ""),
            "color_space": ("string", "srgb"),
            "address": ("string", "wrap"),
        },
    ),
    "channel": dict(
        label="Extract Channel",
        output="float",
        inputs={"value": ("color4", None)},
        properties={"channel": ("string", "r")},
    ),
    "normalmap": dict(
        label="Normal Map",
        output="color3",
        inputs={
            "color": ("color3", (0.5, 0.5, 1.0)),
            "strength": ("float", 1.0),
        },
        properties={"flip_green": ("bool", True)},
    ),
}
USD_TYPES = {
    "float": Sdf.ValueTypeNames.Float,
    "color3": Sdf.ValueTypeNames.Color3f,
    "surface": Sdf.ValueTypeNames.Token,
    "volume": Sdf.ValueTypeNames.Token,
    "decal": Sdf.ValueTypeNames.Token,
    "string": Sdf.ValueTypeNames.String,
    "ramp": Sdf.ValueTypeNames.String,  # its keys as JSON
    "bool": Sdf.ValueTypeNames.Bool,
    "asset": Sdf.ValueTypeNames.Asset,
    "vector2": Sdf.ValueTypeNames.Float2,
    "color4": Sdf.ValueTypeNames.Color4f,
}
# BSDFs UE's Substrate accepts only as the root of a material (MaterialExpressionSubstrate.cpp's topology checks).
ROOT_ONLY = ("unlit", "hair", "eye", "water")
COMPILER_VERSION = (
    3  # HdUESubstrateCompilerVersion (IdSwitch.h) must match; 3: ID switch
)
ERROR_ATTRIBUTE = "unreal:substrate:authoringError"


def live_type(kind, category, port):
    value_type = NODES[kind][category][port][0]
    if category == "inputs" and value_type in (
        "float",
        "color3",
        "vector2",
        "color4",
    ):
        return value_type
    if category == "properties":
        if kind in ("constant", "constant_color") and port == "value":
            return value_type
        if kind == "primvar" and port == "default":
            return "float"
        if kind == "normalmap" and port == "flip_green":
            return "float"
        if kind == "channel" and port == "channel":
            return "color4"
    return None


def live_parameters(graph):
    """Stable reserved names shared by editor generation and runtime binding."""
    result = []
    for node_name, node in validate(graph)["nodes"].items():
        for category in ("inputs", "properties"):
            for port, (_, default) in NODES[node["type"]][category].items():
                kind = live_type(node["type"], category, port)
                value = node.get(category, {}).get(port, default)
                if kind is None or value is None or isinstance(value, dict):
                    continue
                if node["type"] == "normalmap" and port == "flip_green":
                    value = float(value)
                if node["type"] == "channel" and port == "channel":
                    value = [float(c == value) for c in "rgba"]
                key = node_name + "." + category + "." + port
                name = (
                    "HdUE_Auto_" + hashlib.sha256(key.encode()).hexdigest()[:24]
                )
                result.append(dict(key=key, name=name, type=kind, value=value))
    return result


def outputs(definition):
    return definition.get("outputs", {"out": definition["output"]})


def _ramp(value, where, color):
    if not isinstance(value, list) or not value:
        raise ValueError(where + ": a ramp needs at least one key")
    previous = -math.inf
    for key in value:
        if not isinstance(key, list) or len(key) != 3:
            raise ValueError(
                where + ": ramp keys are [position, value, interpolation]"
            )
        position, data, interpolation = key
        _value(position, "float", where)
        _value(data, "color3" if color else "float", where)
        if position < previous:
            raise ValueError(where + ": ramp keys must be in position order")
        if interpolation not in ("linear", "constant", "smooth"):
            raise ValueError(
                where + ": ramp interpolation is linear, constant or smooth"
            )
        previous = position


def _value(value, kind, where):
    if kind == "ramp":
        return  # checked with its node (float or color ramp)
    if kind in ("string", "asset"):
        if not isinstance(value, str):
            raise ValueError(where + ": expected string")
    elif kind == "bool":
        if not isinstance(value, bool):
            raise ValueError(where + ": expected bool")
    elif kind in ("float", "color3", "vector2", "color4"):
        values = [value] if kind == "float" else value
        size = {"color3": 3, "vector2": 2, "color4": 4}.get(kind)
        if size and (
            not isinstance(values, (list, tuple)) or len(values) != size
        ):
            raise ValueError(where + ": expected " + kind)
        if not all(
            isinstance(v, (int, float))
            and not isinstance(v, bool)
            and math.isfinite(v)
            for v in values
        ):
            raise ValueError(where + ": expected finite numeric data")
        try:
            if not all(
                math.isfinite(struct.unpack("<f", struct.pack("<f", v))[0])
                for v in values
            ):
                raise ValueError(where + ": float32 overflow")
        except OverflowError as error:
            raise ValueError(where + ": float32 overflow") from error
    else:
        raise ValueError(where + ": surface inputs require a connection")


def validate(graph):
    if (
        graph.get("version") != 1
        or not isinstance(graph.get("nodes"), dict)
        or not 0 < len(graph["nodes"]) <= 256
    ):
        raise ValueError(
            "Unsupported or empty Substrate graph (maximum 256 nodes)"
        )
    nodes = graph["nodes"]
    output = graph.get("output")
    visited = set()
    active = set()
    parameters = set()
    bases = []
    masks = []
    fields = []

    def visit(name):
        if name in active:
            raise ValueError("Substrate graph cycle at " + name)
        if name in visited:
            return
        node = nodes.get(name)
        if not node or node.get("type") not in NODES:
            raise ValueError("Unknown Substrate node: " + str(name))
        if not Sdf.Path.IsValidIdentifier(name):
            raise ValueError("Invalid Substrate node identifier: " + name)
        definition = NODES[node["type"]]
        active.add(name)
        if set(node.get("inputs", {})) - set(definition["inputs"]) or set(
            node.get("properties", {})
        ) - set(definition["properties"]):
            raise ValueError(name + ": unknown input/property")
        for port, (kind, default) in definition["inputs"].items():
            value = node.get("inputs", {}).get(port, default)
            if isinstance(value, dict):
                if set(value) - {"node", "output"} or "node" not in value:
                    raise ValueError(name + "." + port + ": invalid connection")
                visit(value["node"])
                if (
                    outputs(NODES[nodes[value["node"]]["type"]]).get(
                        value.get("output", "out")
                    )
                    != kind
                ):
                    raise ValueError(
                        name
                        + "."
                        + port
                        + ": connection type mismatch or unknown output"
                    )
            elif value is None:
                if kind == "surface" or node["type"] == "channel":
                    raise ValueError(
                        name + "." + port + ": missing required connection"
                    )
            else:
                _value(value, kind, name + "." + port)
        for prop, (kind, default) in definition["properties"].items():
            _value(
                node.get("properties", {}).get(prop, default),
                kind,
                name + "." + prop,
            )
        props = node.get("properties", {})
        if node["type"] in ("scalar", "color"):
            public = props.get(
                "parameter_name", definition["properties"]["parameter_name"][1]
            )
            if (
                not public
                or public.lower().startswith("hdue_")
                or public.casefold() in parameters
            ):
                raise ValueError(
                    name + ": duplicate, empty or reserved parameter name"
                )
            parameters.add(public.casefold())
        if node["type"] == "slab" and props.get("subsurface", "none") not in (
            "none",
            "simple_volume",
        ):
            raise ValueError(name + ": unsupported subsurface mode")
        if node["type"] == "volume_field" and props.get(
            "field", "density"
        ) not in ("density", "emission"):
            raise ValueError(name + ": expected the density or emission field")
        if node["type"] == "volume_field":
            fields.append(name)
        if (
            node["type"] == "select"
            and not 0 <= props.get("threshold", 0.5) <= 1
        ):
            raise ValueError(name + ": Select threshold must be in [0, 1]")
        if node["type"] in ("float_ramp", "color_ramp"):
            _ramp(
                props.get("ramp", definition["properties"]["ramp"][1]),
                name + ".ramp",
                node["type"] == "color_ramp",
            )
        if node["type"] == "uv" and props.get("index", 0) != 0:
            raise ValueError(
                name
                + ": only UV0 (USD st) is transported by the current mesh bridge"
            )
        if node["type"] == "texture":
            if not props.get("file", "").strip():
                raise ValueError(
                    name + ": Texture Sample requires an image file"
                )
            if props.get("color_space", "srgb") not in ("srgb", "raw"):
                raise ValueError(name + ": expected srgb or raw color space")
            if props.get("address", "wrap") not in ("wrap", "clamp", "mirror"):
                raise ValueError(name + ": unsupported texture address mode")
        if node["type"] == "channel" and props.get("channel", "r") not in (
            "r",
            "g",
            "b",
            "a",
        ):
            raise ValueError(name + ": expected r, g, b or a channel")
        if node["type"] == "normalmap":
            source = node.get("inputs", {}).get("color")
            if isinstance(source, dict):
                upstream = nodes[source["node"]]
                if (
                    upstream["type"] == "texture"
                    and upstream.get("properties", {}).get(
                        "color_space", "srgb"
                    )
                    != "raw"
                ):
                    raise ValueError(
                        name
                        + ": Normal Map requires a raw Texture Sample, not sRGB"
                    )
        if node["type"] == "materialx":
            bases.append(name)
        if node["type"] == "primvar":
            masks.append(name)
        active.remove(name)
        visited.add(name)

    visit(output)
    # UE's Substrate takes these only as a material's whole surface: no operator may combine them ("Operator Add cannot
    # be used with the following data flowing through {Unlit}").
    for name, node in nodes.items():
        if node["type"] in ROOT_ONLY and name != output:
            raise ValueError(
                name
                + ": "
                + NODES[node["type"]]["label"]
                + " must be the material's output, not under an operator"
            )
    kind = NODES[nodes[output]["type"]]["output"]
    if kind not in ("surface", "volume", "decal"):
        raise ValueError(
            "Substrate output must be a surface, a volume or a decal"
        )
    if fields and kind != "volume":
        raise ValueError(
            "Volume Field reads a volume's grids: use it in a volume graph"
        )
    if set(nodes) != visited:
        raise ValueError(
            "Disconnected Substrate nodes: "
            + ", ".join(sorted(set(nodes) - visited))
        )
    if len(bases) > 1:
        raise ValueError(
            "The initial compiler supports one MaterialX surface input per graph"
        )
    if len(masks) > 1:
        raise ValueError(
            "The initial mask transport supports one Float Primvar node per graph"
        )
    return graph


def signature(graph):
    graph = json.loads(json.dumps(validate(graph)))
    graph["compiler"] = COMPILER_VERSION
    for node in graph["nodes"].values():
        schema = NODES[node["type"]]
        for category in ("inputs", "properties"):
            fields = node.setdefault(category, {})
            for name, (kind, default) in schema[category].items():
                if name not in fields and default is not None:
                    fields[name] = default
                value = fields.get(name)
                dynamic = live_type(node["type"], category, name)
                if (
                    dynamic
                    and value is not None
                    and not isinstance(value, dict)
                ):
                    fields[name] = {"parameter": dynamic}
                    continue
                if isinstance(value, dict) and value.get("output") == "out":
                    value.pop("output")
                if (
                    value is not None
                    and not isinstance(value, dict)
                    and kind in ("float", "color3", "vector2", "color4")
                ):
                    f32 = lambda v: struct.unpack("<f", struct.pack("<f", v))[0]
                    fields[name] = (
                        f32(value)
                        if kind == "float"
                        else [f32(v) for v in value]
                    )
        if node["type"] in ("scalar", "color"):
            node.get("properties", {}).pop("value", None)
        if node["type"] == "primvar":
            node.get("properties", {}).pop("primvar", None)
        if node["type"] == "materialx":
            node.get("properties", {}).pop("material", None)
    return hashlib.sha256(
        json.dumps(graph, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def output_kind(graph):
    """The graph's material kind: surface, volume or decal (its output node's type)."""
    return NODES[graph["nodes"][graph["output"]]["type"]]["output"]


def author(stage, path, graph, replace=True):
    """Author graph as the material at path; replace=False writes into a material the caller just defined (Material Library)."""
    from pxr import Gf

    validate(graph)
    existing = stage.GetPrimAtPath(path)
    if replace and existing:
        if existing.GetAttribute("unreal:substrate:version").Get() != 1:
            raise ValueError(
                "Refusing to replace a material not owned by Substrate Builder: "
                + path
            )
        stage.RemovePrim(path)
    material = UsdShade.Material.Define(stage, path)
    if material.GetPrim().GetAttribute(ERROR_ATTRIBUTE):
        material.GetPrim().GetAttribute(ERROR_ATTRIBUTE).Block()
    material.GetPrim().CreateAttribute(
        "unreal:substrate:version", Sdf.ValueTypeNames.Int
    ).Set(1)
    shaders = {
        name: UsdShade.Shader.Define(stage, path + "/Graph/" + name)
        for name in graph["nodes"]
    }
    for name, node in graph["nodes"].items():
        shader = shaders[name]
        definition = NODES[node["type"]]
        shader.CreateIdAttr("unreal:substrate:" + node["type"])
        for port, kind in outputs(definition).items():
            shader.CreateOutput(port, USD_TYPES[kind])
        for category in ("inputs", "properties"):
            for port, (kind, default) in definition[category].items():
                value = node.get(category, {}).get(port, default)
                if node["type"] == "materialx" and port == "material":
                    shader.GetPrim().CreateRelationship(
                        "unreal:materialx"
                    ).SetTargets([Sdf.Path(value)])
                    continue
                attribute = shader.CreateInput(port, USD_TYPES[kind])
                if isinstance(value, dict):
                    attribute.ConnectToSource(
                        shaders[value["node"]].ConnectableAPI(),
                        value.get("output", "out"),
                    )
                else:
                    # Value edits and disconnects must override weaker source
                    # connections, including a referenced Builder material.
                    attribute.GetAttr().SetConnections([])
                    if value is None:
                        attribute.GetAttr().Block()
                        continue
                    constructors = {
                        "color3": Gf.Vec3f,
                        "vector2": Gf.Vec2f,
                        "color4": Gf.Vec4f,
                    }
                    attribute.Set(
                        constructors[kind](*value)
                        if kind in constructors
                        else (
                            Sdf.AssetPath(value)
                            if kind == "asset"
                            else json.dumps(value) if kind == "ramp" else value
                        )
                    )
    if output_kind(graph) == "volume":
        # A volume material: UsdShade's volume terminal, which Volume prims bind; no Preview Surface.
        material.CreateVolumeOutput("unreal").ConnectToSource(
            shaders[graph["output"]].ConnectableAPI(), "out"
        )
        return material
    material.CreateSurfaceOutput("unreal").ConnectToSource(
        shaders[graph["output"]].ConnectableAPI(), "out"
    )
    preview = UsdShade.Shader.Define(stage, path + "/Preview")
    preview.CreateIdAttr("UsdPreviewSurface")
    preview.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.18)
    )
    preview.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    material.CreateSurfaceOutput().ConnectToSource(
        preview.ConnectableAPI(), "surface"
    )
    return material


def read(material, time):
    error = material.GetPrim().GetAttribute(ERROR_ATTRIBUTE)
    if error and error.Get(time):
        raise ValueError(str(material.GetPath()) + ": " + error.Get(time))
    if material.GetPrim().GetAttribute("unreal:substrate:version").Get() != 1:
        raise ValueError("Unsupported Substrate material version")
    terminal = None
    for output in (
        material.GetSurfaceOutput("unreal"),
        material.GetVolumeOutput("unreal"),
    ):
        if output and output.GetConnectedSource():
            terminal = output.GetConnectedSource()
            break
    if not terminal:
        raise ValueError(
            str(material.GetPath()) + ": missing hdue surface or volume output"
        )
    nodes = {}

    def visit(shader):
        name = shader.GetPrim().GetName()
        if name in nodes:
            return name
        kind = str(shader.GetIdAttr().Get()).removeprefix("unreal:substrate:")
        if kind not in NODES:
            raise ValueError(
                "Unsupported Substrate shader: " + str(shader.GetPath())
            )
        node = dict(type=kind, inputs={}, properties={})
        nodes[name] = node
        for category in ("inputs", "properties"):
            for port, (value_type, default) in NODES[kind][category].items():
                if kind == "materialx" and port == "material":
                    targets = (
                        shader.GetPrim()
                        .GetRelationship("unreal:materialx")
                        .GetForwardedTargets()
                    )
                    if len(targets) != 1:
                        raise ValueError(
                            "MaterialX input requires one material relationship target"
                        )
                    node[category][port] = str(targets[0])
                    continue
                attr = shader.GetInput(port)
                if not attr:
                    continue
                if attr.GetTypeName() != USD_TYPES[value_type]:
                    raise ValueError(
                        str(attr.GetAttr().GetPath())
                        + ": invalid graph input type"
                    )
                connection = attr.GetConnectedSource()
                if connection:
                    if category != "inputs":
                        raise ValueError("Invalid property connection")
                    source = UsdShade.Shader(connection[0].GetPrim())
                    source_kind = str(source.GetIdAttr().Get()).removeprefix(
                        "unreal:substrate:"
                    )
                    if (
                        source_kind not in NODES
                        or outputs(NODES[source_kind]).get(str(connection[1]))
                        != value_type
                    ):
                        raise ValueError("Invalid graph source output")
                    node[category][port] = dict(node=visit(source))
                    if str(connection[1]) != "out":
                        node[category][port]["output"] = str(connection[1])
                else:
                    value = attr.Get(time)
                    if (
                        value is None
                        and category == "inputs"
                        and default is None
                    ):
                        continue
                    if value_type == "asset":
                        from substrate_textures import resolve_asset

                        value = resolve_asset(attr.GetAttr(), value, time)
                    if value_type == "ramp":
                        value = json.loads(value)
                    node[category][port] = (
                        list(value)
                        if value_type in ("color3", "vector2", "color4")
                        else value
                    )
        return name

    root = visit(UsdShade.Shader(terminal[0].GetPrim()))
    return validate(dict(version=1, output=root, nodes=nodes))
