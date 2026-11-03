"""Read a composed UsdShade graph without loading MaterialX into the viewer."""

import hashlib
import json
import math

from pxr import Sdf, Usd, UsdShade


def value_data(value):
    if isinstance(value, Sdf.AssetPath):
        # Houdini resolves opdef: to the URI itself; standalone OpenUSD leaves
        # it unresolved. Its identity must be independent of the host resolver.
        return {
            "asset": value.path,
            "resolved": (
                "" if value.path.startswith("opdef:") else value.resolvedPath
            ),
        }
    if value is None or isinstance(value, (str, bool, int)):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("Nonfinite MaterialX input")
        return value
    return [value_data(v) for v in value]


def graph_data(material, time=Usd.TimeCode.Default()):
    """Flatten NodeGraph interfaces, retaining all connection paths for checks."""
    stage = material.GetPrim().GetStage()
    nodes = {}
    visiting = set()
    for terminal in (
        material.GetDisplacementOutput("mtlx"),
        material.GetVolumeOutput("mtlx"),
    ):
        if terminal and terminal.GetAttr().GetConnections():
            raise ValueError(
                f"Unsupported MaterialX displacement/volume: {terminal.GetAttr().GetPath()}"
            )

    def resolve(attr, seen=()):
        path = str(attr.GetPath())
        if path in seen:
            raise ValueError(f"Cyclic MaterialX interface: {path}")
        links = attr.GetConnections()
        if len(links) > 1:
            raise ValueError(f"Multiple MaterialX sources: {path}")
        if links:
            source = stage.GetAttributeAtPath(links[0])
            if not source:
                raise ValueError(
                    f"Missing MaterialX connection: {path} -> {links[0]}"
                )
            if source.GetPrim().IsA(
                UsdShade.Shader
            ) and source.GetName().startswith("outputs:"):
                shader = UsdShade.Shader(source.GetPrim())
                visit(shader)
                return dict(
                    node=str(shader.GetPath()),
                    output=source.GetBaseName(),
                    trace=[path, str(links[0])],
                )
            result = resolve(source, seen + (path,))
            return dict(result, trace=[path] + result["trace"])
        value = attr.Get(time)
        return dict(
            attribute=path,
            type=str(attr.GetTypeName()),
            value=value_data(value),
            colorspace=str(attr.GetColorSpace()),
            trace=[path],
        )

    def visit(shader):
        path = str(shader.GetPath())
        if path in visiting:
            raise ValueError(f"Cyclic MaterialX shader: {path}")
        if path in nodes:
            return
        visiting.add(path)
        if shader.GetImplementationSource() != UsdShade.Tokens.id:
            raise ValueError(
                f"Inline MaterialX needs an info:id implementation: {path}"
            )
        identity = shader.GetIdAttr().Get()
        if not identity:
            raise ValueError(
                f"Inline MaterialX needs a nodedef info:id: {path}"
            )
        inputs = {
            str(p.GetBaseName()): resolve(p.GetAttr())
            for p in sorted(
                shader.GetInputs(), key=lambda p: str(p.GetBaseName())
            )
            if not str(p.GetBaseName()).startswith("unreal:")
        }
        nodes[path] = dict(id=str(identity), inputs=inputs)
        visiting.remove(path)

    output = material.GetSurfaceOutput("mtlx")
    if not output:
        raise ValueError(f"No MaterialX surface: {material.GetPath()}")
    terminal = resolve(output.GetAttr())
    if "node" not in terminal:
        raise ValueError(
            f"MaterialX surface has no shader: {material.GetPath()}"
        )
    return dict(
        material=str(material.GetPath()),
        terminal=terminal,
        nodes=dict(sorted(nodes.items())),
    )


def graph_signature(graph, mutable=()):
    data = json.loads(json.dumps(graph))
    for node in data["nodes"].values():
        for item in node["inputs"].values():
            if item.get("attribute") in mutable:
                item["value"] = "<editable>"
    return hashlib.sha256(
        json.dumps(data, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def rebase_graph(graph, old_root, new_root):
    """Rebase USD property identities after reference composition, not asset filenames."""
    data = json.loads(json.dumps(graph))

    def location(path):
        return (
            new_root + path[len(old_root) :]
            if path == old_root
            or path.startswith(old_root + "/")
            or path.startswith(old_root + ".")
            else path
        )

    def port(item):
        for key in ("node", "attribute"):
            if key in item:
                item[key] = location(item[key])
        item["trace"] = [location(p) for p in item.get("trace", [])]
        return item

    data["material"] = location(data["material"])
    data["terminal"] = port(data["terminal"])
    data["nodes"] = {
        location(path): dict(
            node, inputs={n: port(v) for n, v in node["inputs"].items()}
        )
        for path, node in data["nodes"].items()
    }
    return data


# What hdUE translates at render time (src/hydra/materials.cpp: readSurface and SurfaceReader::trace): these surfaces,
# and inputs reached through these nodes. Keep the two in step.
RUNTIME_SURFACES = (
    "ND_standard_surface_surfaceshader",
    "ND_open_pbr_surface_surfaceshader",
    "ND_UsdPreviewSurface_surfaceshader",
    "UsdPreviewSurface",
)
RUNTIME_NODES = (
    "ND_image_",
    "ND_tiledimage_",
    "UsdUVTexture",
    "ND_geompropvalue_vector2",
    "ND_geompropvalue_color3",
    "ND_geompropvalue_vector3",
    "UsdPrimvarReader_",
    "ND_texcoord_",
    "UsdTransform2d",
    "ND_constant_",
    "ND_convert_",
    "ND_dot_",
    "ND_extract_",
    "ND_normalmap",
)


def needs_import(graph):
    """Whether hdUE's render-time translation cannot express graph (a node it would leave at its constant, or a
    surface it does not read), so UE's MaterialX importer must prepare it.
    """
    nodes = graph["nodes"]
    if not nodes[graph["terminal"]["node"]]["id"].startswith(RUNTIME_SURFACES):
        return True
    for path, node in nodes.items():
        if path == graph["terminal"]["node"]:
            continue
        identity = node["id"]
        if identity.startswith("ND_multiply_"):
            # A multiply by a constant scales its other input; by another node it is a product hdUE does not form.
            if all("node" in node["inputs"].get(i, {}) for i in ("in1", "in2")):
                return True
        elif not identity.startswith(RUNTIME_NODES):
            return True
    return False


def _masked(graph, root):
    """The graph rebased to root, with every numeric value masked: the values an imported material may expose as
    parameters (bind_usd_materialx_library.py binds float, colour and vector inputs).
    """
    data = rebase_graph(graph, graph["material"], root)
    for node in data["nodes"].values():
        for item in node["inputs"].values():
            value = item.get("value")
            if isinstance(value, float) or (
                isinstance(value, list)
                and value
                and all(isinstance(v, (int, float)) for v in value)
            ):
                item["value"] = "<number>"
    return data


def structure_key(graph):
    """A prepared MaterialX graph's lookup key: its nodes, connections, files and non-numeric values, independent of
    the material's path. Values the import kept as parameters can then change without a new preparation; an entry's
    usd_graph_signature checks the ones it baked in.
    """
    return graph_signature(_masked(graph, "/M"))


def request_key(graph):
    """The exact identity of a graph to prepare, values included, independent of the material's path."""
    return graph_signature(rebase_graph(graph, graph["material"], "/M"))
