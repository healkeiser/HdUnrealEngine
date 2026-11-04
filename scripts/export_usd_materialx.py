# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = ["MaterialX==1.39.4"]
# ///
"""Export supported composed USD MaterialX graphs to UE's MaterialX version."""

import argparse
import hashlib
import json
import re
from pathlib import Path

from materialx_textures import file_identity, resolved, udim_tiles
from pxr import Sdf, Usd, UsdShade
from usd_materialx_graph import graph_data


def definitions():
    """MaterialX's standard data libraries (the version UE 5.8 reads), and Houdini's own nodes when Houdini is
    installed: (library document, names of the standard nodedefs). Houdini's nodes are node graphs over standard ones;
    write() flattens them, so UE's importer only sees standard nodes.
    """
    import MaterialX as mx

    standard = mx.createDocument()
    mx.loadLibraries(
        mx.getDefaultDataLibraryFolders(),
        mx.getDefaultDataSearchPath(),
        standard,
    )
    names = {d.getName() for d in standard.getNodeDefs()}
    library = mx.createDocument()
    library.importLibrary(standard)
    # local_path exits when Houdini is not configured: then MaterialX's own nodes only.
    try:
        from local_paths import local_path

        houdini = local_path("houdini_root") / "houdini/materialx/libraries"
    except (ImportError, SystemExit):
        houdini = None
    if houdini and houdini.is_dir():
        extra = mx.createDocument()
        mx.loadLibraries(["houdini"], mx.FileSearchPath(str(houdini)), extra)
        library.importLibrary(extra)
    return library, names


def texture_path(stage, item, time, embedded=None):
    value = item["value"]
    if not isinstance(value, dict):
        raise ValueError(f"Texture needs an asset input: {item['attribute']}")
    raw = value["asset"]
    if raw.startswith("opdef:"):
        entry = (embedded or {}).get(raw)
        if not entry:
            raise ValueError(
                "Embedded texture needs Houdini preparation: " + raw
            )
        file = Path(entry["path"]).resolve()
        if (
            not file.is_file()
            or hashlib.sha256(file.read_bytes()).hexdigest() != entry["sha256"]
        ):
            raise ValueError("Embedded texture changed; resolve again: " + raw)
        return str(file), None
    if "<UDIM>" not in raw:
        if not value.get("resolved"):
            raise ValueError(f"Texture is unresolved: {item['attribute']}")
        file = Path(value["resolved"]).resolve()
        match = re.fullmatch(r"(.+[._])\d{4}(\.[^.]+)", file.name)
        if match:
            pattern = str(file.parent / (match[1] + "<UDIM>" + match[2]))
            try:
                tiles = {str(k): v for k, v in udim_tiles(pattern).items()}
            except ValueError:
                # A literal file such as photo.2020.png or a lone 1003 file
                # remains a normal 2D image. Actual multi-block UE imports
                # are checked again from their source-block receipt.
                tiles = {}
            if len(tiles) > 1:
                return str(file), dict(pattern=pattern, tiles=tiles)
        return str(file), None
    if Path(raw).is_absolute():
        pattern = resolved(raw)
    else:
        attr = stage.GetAttributeAtPath(item["attribute"])
        layer = next(
            (
                s.layer
                for s in attr.GetPropertyStack(time)
                if s.HasDefaultValue() or s.layer.ListTimeSamplesForPath(s.path)
            ),
            None,
        )
        if layer is None:
            raise ValueError(
                f"Cannot anchor UDIM asset to its USD layer: {item['attribute']}"
            )
        anchored = Path(Sdf.ComputeAssetPathRelativeToLayer(layer, raw))
        # The USD resolver returns an unresolved search path for <UDIM>,
        # since the pattern itself is not a real file. Anchor it explicitly.
        if not anchored.is_absolute():
            if not layer.realPath or not Path(layer.realPath).is_file():
                raise ValueError(
                    f"UDIM requires a filesystem USD layer: {item['attribute']}"
                )
            anchored = Path(layer.realPath).parent / raw
        pattern = resolved(anchored)
    tiles = {str(k): v for k, v in udim_tiles(pattern).items()}
    return pattern, dict(pattern=pattern, tiles=tiles)


def inline_materials(stage):
    """The stage's Materials with a MaterialX surface that are not read from a .mtlx document (a reference through
    usdMtlx, or a sourceAsset shader): Import-MaterialX.ps1 imports those from their document.
    """
    for prim in stage.Traverse():
        if not prim.IsA(UsdShade.Material):
            continue
        material = UsdShade.Material(prim)
        if not material.GetSurfaceOutput("mtlx"):
            continue
        surface = material.ComputeSurfaceSource("mtlx")[0]
        if (
            surface
            and surface.GetImplementationSource() == UsdShade.Tokens.sourceAsset
        ) or any(
            spec.layer.GetFileFormat().formatId == "mtlx"
            for spec in prim.GetPrimStack()
        ):
            continue
        yield material


def record(
    material,
    time=Usd.TimeCode.Default(),
    embedded=None,
    graph=None,
    identities=None,
):
    """What write() needs of one material, read on the host that has the stage: its graph and its images resolved to
    files (UDIM sets with their tiles), with the files' identities (materialx_textures.file_identity). identities: a
    walk's memo of them, shared by the materials it records (one file serves many).
    """
    stage = material.GetPrim().GetStage()
    graph = graph or graph_data(material, time)
    textures, dependencies = {}, {}
    for node in graph["nodes"].values():
        for item in node["inputs"].values():
            value = item.get("value")
            if not isinstance(value, dict) or "asset" not in value:
                continue
            filename, tiles = texture_path(stage, item, time, embedded)
            for source_file in (
                list(tiles["tiles"].values()) if tiles else [filename]
            ):
                key = str(source_file)
                identity = (
                    identities.get(key) if identities is not None else None
                )
                if identity is None:
                    if not Path(key).is_file():
                        raise ValueError(f"Texture is missing: {key}")
                    identity = file_identity(key)
                    if identities is not None:
                        identities[key] = identity
                dependencies[key] = identity
            textures[item["attribute"]] = [filename, tiles]
    return dict(
        material=str(material.GetPrim().GetPath()),
        graph=graph,
        textures=textures,
        dependencies=dependencies,
    )


def _image_roles(graph):
    """The image nodes a normalmap reads directly: UE imports them as normal maps."""
    normal_images = set()
    for path, node in graph["nodes"].items():
        if node["id"].startswith("ND_normalmap"):
            if any(
                name in node["inputs"]
                for name in ("normal", "tangent", "bitangent")
            ):
                raise ValueError(
                    f"Custom normalmap tangent frames are unsupported: {path}"
                )
            source_input = node["inputs"].get("in", {})
            source_node = graph["nodes"].get(source_input.get("node"), {})
            if source_node.get("id") == "ND_image_vector3":
                normal_images.add(source_input["node"])
    return normal_images


def _value_text(value):
    if isinstance(value, list):
        return ", ".join(str(v) for v in value)
    return str(value).lower() if isinstance(value, bool) else str(value)


def write(records, output, name, usd_stage="", dependencies=None, extra=None):
    """One .mtlx document of records' materials for UE's MaterialX importer, and export.json describing it (the
    exposed inputs that stay live parameters, for bind_usd_materialx_library.py). Nodes outside MaterialX's standard
    libraries (Houdini's) are flattened into standard ones.
    """
    import MaterialX as mx

    library, standard = definitions()
    doc = mx.createDocument()
    doc.setColorSpace("lin_rec709")
    doc.importLibrary(library)
    entries = []
    texture_sets = {}
    texture_roles = {}
    dependencies = dict(dependencies or {})
    for source in records:
        graph = source["graph"]
        dependencies.update(source["dependencies"])
        normal_images = _image_roles(graph)
        tag = hashlib.sha1(
            (usd_stage + source["material"]).encode()
        ).hexdigest()[:10]
        material_name = (
            re.sub(r"[^A-Za-z0-9_]", "_", Sdf.Path(source["material"]).name)
            + "_"
            + tag
        )
        names = {
            path: material_name + "_n" + str(i)
            for i, path in enumerate(graph["nodes"])
        }
        exported = {}
        fields = []
        for path, node in graph["nodes"].items():
            definition = library.getNodeDef(node["id"])
            if definition is None:
                raise ValueError(
                    f"Unknown MaterialX nodedef: {path}: {node['id']}"
                )
            exported[path] = doc.addNode(
                definition.getNodeString(), names[path], definition.getType()
            )
            exported[path].setNodeDefString(node["id"])
        terminal = graph["terminal"]["node"]
        if exported[terminal].getType() != "surfaceshader":
            raise ValueError(f"Unsupported MaterialX surface: {terminal}")

        def output_type(item):
            definition = library.getNodeDef(graph["nodes"][item["node"]]["id"])
            if definition.getType() == "surfaceshader":
                return "surfaceshader"
            outputs = {
                o.getName(): o.getType() for o in definition.getActiveOutputs()
            }
            if item["output"] not in outputs:
                raise ValueError(
                    f"Unknown MaterialX output: {item['node']}.outputs:{item['output']}"
                )
            return outputs[item["output"]]

        output_type(graph["terminal"])
        for path, node in graph["nodes"].items():
            definition = library.getNodeDef(node["id"])
            for input_name, item in node["inputs"].items():
                declared = definition.getActiveInput(input_name)
                if declared is None:
                    raise ValueError(
                        f"Unknown MaterialX input: {path}.inputs:{input_name}"
                    )
                port = exported[path].addInput(input_name, declared.getType())
                if item.get("colorspace"):
                    port.setColorSpace(item["colorspace"])
                if "node" in item:
                    upstream = exported[item["node"]]
                    if output_type(item) != declared.getType():
                        raise ValueError(
                            f"MaterialX connection type mismatch: {path}.inputs:{input_name}"
                        )
                    port.setNodeName(upstream.getName())
                    if upstream.getType() == "multioutput":
                        port.setOutputString(item["output"])
                    continue
                if item["value"] is None:
                    exported[path].removeInput(input_name)
                    continue
                value = item["value"]
                if (
                    exported[path].getCategory() == "texcoord"
                    and input_name == "index"
                    and value != 0
                ):
                    raise ValueError(
                        f"Only UV set 0 is supported: {item['attribute']}"
                    )
                if declared.getType() == "filename":
                    filename, tiles = source["textures"][item["attribute"]]
                    if tiles:
                        texture_sets[tiles["pattern"]] = tiles
                    normal = path in normal_images
                    role = "normal" if normal else "color"
                    role_key = tiles["pattern"] if tiles else filename
                    if texture_roles.setdefault(role_key, role) != role:
                        raise ValueError(
                            f"Separate normal/color source files are required for UE texture sampling: {filename}"
                        )
                    colorspace = item.get("colorspace")
                    if normal:
                        if colorspace not in (
                            None,
                            "",
                            "raw",
                            "lin_rec709",
                            "lin_rec709_scene",
                        ):
                            raise ValueError(
                                f"normalmap image must use raw/linear data: {item['attribute']}"
                            )
                        colorspace = "raw"
                    value = filename.replace("\\", "/")
                    if colorspace:
                        port.setColorSpace(str(colorspace))
                port.setValueString(_value_text(value))
                fields.append(
                    dict(
                        node=names[path],
                        input=input_name,
                        attribute=item["attribute"],
                        type=declared.getType(),
                    )
                )
        m = doc.addNode("surfacematerial", material_name, "material")
        m.addInput("surfaceshader", "surfaceshader").setNodeName(
            names[terminal]
        )
        entries.append(
            dict(
                material=material_name,
                usd_material=source["material"],
                usd_shader=terminal,
                graph=graph,
                fields=fields,
            )
        )
    if not entries:
        raise ValueError("No supported inline MaterialX materials were found")
    # Houdini's nodes become the standard nodes of their node graphs. A connected input's value is dropped: MaterialX
    # ignores it, and its 1.39 validation (UE's) rejects an input with both ("too many bindings"), which Houdini's
    # generated node graphs have.
    doc.flattenSubgraphs(
        "",
        lambda node: node.getNodeDef() is not None
        and node.getNodeDef().getName() not in standard,
    )
    for node in doc.getNodes():
        for port in node.getInputs():
            if port.getConnectedNode() is not None:
                port.removeAttribute("value")
    # The document alone, without the libraries it was built against (their elements carry their file's URI),
    # validated against the standard ones UE reads.
    options = mx.XmlWriteOptions()
    options.writeXIncludeEnable = False
    options.elementPredicate = lambda element: not element.hasSourceUri()
    alone = mx.createDocument()
    mx.readFromXmlString(alone, mx.writeToXmlString(doc, options))
    text = mx.writeToXmlString(alone)
    validation = mx.createDocument()
    standard_library = mx.createDocument()
    mx.loadLibraries(
        mx.getDefaultDataLibraryFolders(),
        mx.getDefaultDataSearchPath(),
        standard_library,
    )
    validation.importLibrary(standard_library)
    mx.readFromXmlString(validation, text)
    unknown = sorted(
        {
            n.getCategory()
            for n in validation.getNodes()
            if n.getNodeDef() is None
            or n.getNodeDef().getName() not in standard
        }
    )
    if unknown:
        raise ValueError(
            f"MaterialX nodes outside the standard libraries: {unknown}"
        )
    valid, message = validation.validate()
    if not valid:
        raise ValueError(f"MaterialX validation failed: {message}")
    output.mkdir(parents=True)
    file = output / (name + ".mtlx")
    # MaterialX writes literal <UDIM> inside attributes. Escape it so the
    # document also conforms to strict XML readers used by registry tooling.
    file.write_text(text.replace("<UDIM>", "&lt;UDIM&gt;"), encoding="utf-8")
    manifest = dict(
        version=1,
        usd_stage=usd_stage,
        source=str(file.resolve()),
        materials=entries,
        dependencies=dependencies,
        texture_sets=list(texture_sets.values()),
        **(extra or {}),
    )
    (output / "export.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )
    print(f"{file}: {len(entries)} USD MaterialX materials")
    return manifest


def export(source, output, time=Usd.TimeCode.Default(), texture_manifest=None):
    source = source.resolve()
    if output.exists():
        raise FileExistsError(f"Choose a fresh export directory: {output}")
    stage = Usd.Stage.Open(str(source))
    if not stage:
        raise ValueError(f"Cannot open USD: {source}")
    dependencies = {}
    embedded = {}
    if texture_manifest:
        manifest = json.loads(texture_manifest.read_text(encoding="utf-8-sig"))
        if manifest.get("version") != 1:
            raise ValueError("Unsupported embedded texture manifest")
        embedded = manifest["textures"]
        dependencies[str(texture_manifest.resolve())] = hashlib.sha256(
            texture_manifest.read_bytes()
        ).hexdigest()
    for layer in stage.GetUsedLayers():
        if layer.realPath:
            file = Path(layer.realPath).resolve()
            dependencies[str(file)] = hashlib.sha256(
                file.read_bytes()
            ).hexdigest()
    records = [record(m, time, embedded) for m in inline_materials(stage)]
    return write(
        records,
        output,
        "UsdMaterials_" + hashlib.sha1(str(source).encode()).hexdigest()[:10],
        str(source),
        dependencies,
        dict(
            time=None if time.IsDefault() else time.GetValue(),
            embedded_textures=embedded,
        ),
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--time", type=float)
    parser.add_argument("--texture-manifest", type=Path)
    args = parser.parse_args()
    export(
        args.source,
        args.output,
        (
            Usd.TimeCode(args.time)
            if args.time is not None
            else Usd.TimeCode.Default()
        ),
        args.texture_manifest,
    )
