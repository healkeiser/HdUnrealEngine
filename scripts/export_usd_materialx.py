# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = ["MaterialX==1.39.4"]
# ///
"""Export supported composed USD MaterialX graphs to UE's MaterialX version."""

import argparse
import functools
import hashlib
import json
import os
import re
import sys
import tempfile
import uuid
from pathlib import Path

from materialx_lowering import lower, lower_flakes, lower_primvars
from materialx_textures import (
    file_identity,
    resolved,
    udim_tile_name,
    udim_tiles,
)
from pxr import Sdf, Usd, UsdShade
from usd_materialx_graph import graph_data, image_color_space, reaches_color

# The node-graph categories UE 5.8's MaterialX importer translates whole instead of expanding them (CategoriesToSkip,
# Engine/Plugins/Interchange/Runtime/Source/Import/Private/MaterialX/MaterialXUtils/MaterialXManager.cpp).
UE_KEEP = frozenset(
    """UsdPreviewSurface acescg_to_lin_rec709 add adobergb_to_lin_rec709 blur checkerboard
    chiang_hair_absorption_from_color chiang_hair_roughness circle colorcorrect combine2 combine3 combine4 constant
    contrast deon_hair_absorption_from_melanin distance divide extract fractal2d fractal3d geomcolor gltf_pbr
    gooch_shade hsvadjust ifequal ifgreater ifgreatereq latlongimage layer lin_adobergb_to_lin_rec709
    lin_displayp3_to_lin_rec709 line max min mix multiply noise2d noise3d normalmap open_pbr_surface overlay place2d
    ramp ramp4 ramp_gradient randomcolor randomfloat range refract separate2 separate3 separate4
    srgb_displayp3_to_lin_rec709 srgb_texture_to_lin_rec709 standard_surface subtract switch tiledcircles time
    unifiednoise3d xor""".split()
)


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
    # The MaterialX 1.39.5 nodes Houdini 22 ships and UE's 1.39.4 lacks (generate_materialx_library.py): flake2d,
    # flake3d as node graphs, extract of a matrix (lowered by materialx_lowering.py).
    hdue = mx.createDocument()
    mx.readFromXmlFile(
        hdue,
        str(
            Path(__file__).resolve().parent / "materialx_library/hdue_defs.mtlx"
        ),
    )
    library.importLibrary(hdue)
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
        match = udim_tile_name(file.name)
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


# UE's working space, by its name in OCIO's built-in config (color_management.cpp Rec709).
REC709 = "Linear Rec.709 (sRGB)"


@functools.cache
def _color_management():
    """(PyOpenColorIO, the config, its scene_linear colour space's name, the processor from it to linear Rec.709), as
    the delegate reads them (color_management.cpp management()); None when nothing converts: colour management off,
    no scene_linear role, or no PyOpenColorIO (a standalone Python, whose delegate converts none).
    """
    # As TfGetenvBool reads it: unset or empty is the default (on).
    switch = os.environ.get("HDUE_COLOR_MANAGEMENT", "")
    if switch and switch.lower() not in ("1", "true", "yes", "on"):
        return None
    try:
        import PyOpenColorIO as ocio
    except ImportError:
        return None
    try:
        config = ocio.GetCurrentConfig()
        scene = config.getColorSpace(ocio.ROLE_SCENE_LINEAR)
        if scene is None:
            return None
        processor = ocio.Config.GetProcessorToBuiltinColorSpace(
            config, scene.getName(), REC709
        ).getDefaultCPUProcessor()
    except ocio.Exception:
        return None  # the delegate warns and converts none either
    return ocio, config, scene.getName(), processor


def _from_scene_linear():
    """The OCIO processor from the config's scene_linear role to linear Rec.709, as the delegate converts authored
    colours (HdUEFromSceneLinear); None when nothing converts (scene_linear already linear Rec.709 included).
    """
    management = _color_management()
    return (
        None if management is None or management[3].isNoOp() else management[3]
    )


# Bump with color_management.cpp's kConversion: a converted image of another version is not reused.
CONVERSION = 1


def _linear_rec709(file, tiles, space):
    """A colour image as the delegate converts it (color_management.cpp HdUELinearRec709Texture): from its colour
    space, or the scene_linear role for an untagged float image (EXR, HDR), to linear Rec.709, as half-float EXRs in
    the folder the delegate caches its conversions in. Returns the converted file (a tile's, or the pattern of a UDIM
    set) and None when the image is used as is: no colour management, a raw, data or sRGB-encoded space (decoded as
    sRGB), an untagged 8-bit image, a space the config does not have, or one that is already linear Rec.709. Raises
    ValueError naming the image when it cannot be converted. Keep in step with color_management.cpp.
    """
    management = _color_management()
    if management is None:
        return None
    ocio, config, scene, _ = management
    lower = (space or "").lower()
    floating = Path(file).suffix.lower() in (".exr", ".hdr")
    unnamed = lower in ("", "auto")
    srgb = "srgb" in lower and "lin" not in lower
    if "raw" in lower or lower == "data" or srgb or (unnamed and not floating):
        return None
    source_name = scene if unnamed else space
    try:
        source = config.getColorSpace(source_name)
        if source is None:
            print(
                f"hdUE: the OCIO config has no colour space {source_name}; {file} is used as is",
                file=sys.stderr,
            )
            return None
        if source.isData():
            return None
        processor = ocio.Config.GetProcessorToBuiltinColorSpace(
            config, source.getName(), REC709
        ).getDefaultCPUProcessor()
    except ocio.Exception as error:
        raise ValueError(f"{file}: {source_name}: {error}") from error
    if processor.isNoOp():
        return None
    images = (
        {"." + str(k): v for k, v in tiles["tiles"].items()}
        if tiles
        else {"": file}
    )
    key = hashlib.sha256(
        "|".join(
            [file, source_name, scene, config.getCacheID(), str(CONVERSION)]
            + [k + file_identity(v) for k, v in images.items()]
        ).encode()
    ).hexdigest()[:32]
    folder = Path(tempfile.gettempdir()) / "hdue_ocio"
    folder.mkdir(exist_ok=True)
    converted = {}
    for tile, image in images.items():
        cached = folder / f"{key}{tile}.exr"
        if not cached.is_file():
            _convert_image(image, cached, processor, source_name)
        converted[image] = str(cached)
    if tiles and file == tiles["pattern"]:
        return str(folder / f"{key}.<UDIM>.exr")
    return converted[file]


def _convert_image(source, cached, processor, space):
    """One image into cached through processor: read as float (an 8-bit image in a linear space would band), its
    first three channels converted, written as a half-float EXR beside and renamed (another process may share the
    folder), as color_management.cpp's converted() does.
    """
    try:
        import OpenImageIO as oiio
    except ImportError as error:
        raise ValueError(
            f"{source} needs converting from {space} to linear Rec.709, and this Python has no OpenImageIO"
        ) from error
    image = oiio.ImageBuf(source)
    if not image.read(0, 0, True, oiio.FLOAT) or image.nchannels < 3:
        raise ValueError(
            f"Cannot read {source} to convert it from {space} to linear Rec.709: {image.geterror()}"
        )
    import numpy

    pixels = image.get_pixels(oiio.FLOAT)
    rgb = numpy.ascontiguousarray(pixels[..., :3], dtype=numpy.float32)
    processor.applyRGB(rgb)
    pixels[..., :3] = rgb
    image.set_pixels(image.roi, pixels)
    pending = cached.with_name(f"{cached.stem}.{uuid.uuid4().hex}.pending.exr")
    try:
        if not image.write(str(pending), oiio.HALF):
            raise ValueError(
                f"Cannot write {source} converted from {space}: {image.geterror()}"
            )
        try:
            os.replace(pending, cached)
        except PermissionError:
            if (
                not cached.is_file()
            ):  # else another process wrote the same image
                raise
    finally:
        pending.unlink(missing_ok=True)


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
    textures, dependencies, spaces = {}, {}, {}
    normal_images = _image_roles(graph)

    def shading(path):
        # A shading node's outputs are closures (surfaceshader, BSDF, ...), which USD types as tokens.
        outputs = UsdShade.Shader(stage.GetPrimAtPath(path)).GetOutputs()
        return any(str(o.GetTypeName()) == "token" for o in outputs)

    def input_type(path, name):
        port = UsdShade.Shader(stage.GetPrimAtPath(path)).GetInput(name)
        return str(port.GetTypeName()) if port else ""

    for path, node in graph["nodes"].items():
        for item in node["inputs"].values():
            value = item.get("value")
            if not isinstance(value, dict) or "asset" not in value:
                continue
            filename, tiles = texture_path(stage, item, time, embedded)
            # Decoded as the render-time translation decodes the slot it drives: a normal map raw, an image reaching a
            # colour input converted to linear Rec.709 from its colour space (or sRGB-decoded), any other as data.
            kind = (
                "normal"
                if path in normal_images
                else (
                    "color"
                    if reaches_color(graph, path, shading, input_type)
                    else "data"
                )
            )
            space = image_color_space(
                item.get("colorspace"),
                kind,
                item["attribute"],
                Path(filename).suffix.lower() in (".exr", ".hdr"),
            )
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
            if kind == "color" and (
                converted := _linear_rec709(
                    filename, tiles, item.get("colorspace")
                )
            ):
                filename, space = converted, ""
            spaces[item["attribute"]] = space
            textures[item["attribute"]] = [filename, tiles]
    # Colour constants (and colour images, above) in linear Rec.709, as the render-time translation converts them; the
    # worker that writes the document has no OCIO. shortcut: prepared materials are matched without the config;
    # re-prepare after changing it.
    colors = {}
    if convert := _from_scene_linear():
        for node in graph["nodes"].values():
            for item in node["inputs"].values():
                value = item.get("value")
                if item.get("type", "").startswith("color") and isinstance(
                    value, list
                ):
                    colors[item["attribute"]] = (
                        list(convert.applyRGB(value[:3])) + value[3:]
                    )
    return dict(
        material=str(material.GetPrim().GetPath()),
        graph=graph,
        textures=textures,
        dependencies=dependencies,
        colors=colors,
        spaces=spaces,
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
    # MaterialX writes arrays of vectors (vector2array: curveadjust's knots) as one flat list of numbers.
    if isinstance(value, list):
        return ", ".join(_value_text(v) for v in value)
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
        terminal = graph.get("terminal", {}).get("node")
        # A node that is a material itself (Lama Surface's): it is the material UE imports, under the material's name.
        if terminal:
            terminal_id = graph["nodes"][terminal]["id"]
            terminal_definition = library.getNodeDef(terminal_id)
            if terminal_definition is None:
                raise ValueError(
                    f"Unknown MaterialX nodedef: {terminal}: {terminal_id}"
                )
            if terminal_definition.getType() == "material":
                names[terminal] = material_name
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
        if terminal and exported[terminal].getType() not in (
            "surfaceshader",
            "material",
        ):
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

        for key in ("terminal", "displacement", "volume"):
            if key in graph:
                output_type(graph[key])
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
                value = source.get("colors", {}).get(
                    item["attribute"], item["value"]
                )
                if (
                    declared.getType() == "filename"
                    and item["attribute"] not in source["textures"]
                ):
                    # A file name that is not a texture record (a string folded from a node): its text, or none.
                    if not value:
                        exported[path].removeInput(input_name)
                        continue
                    value = str(
                        value.get("asset", "")
                        if isinstance(value, dict)
                        else value
                    )
                elif declared.getType() == "filename":
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
                    colorspace = source["spaces"][item["attribute"]]
                    value = filename.replace("\\", "/")
                    if colorspace:
                        port.setColorSpace(colorspace)
                    else:
                        port.removeAttribute("colorspace")
                port.setValueString(_value_text(value))
                fields.append(
                    dict(
                        node=names[path],
                        input=input_name,
                        attribute=item["attribute"],
                        type=declared.getType(),
                    )
                )
        # Primvar readers as UE reads them: the UV sets and vertex colour hdUE uploads for this material.
        primvar_notes = []
        lower_primvars(doc, exported, graph["nodes"], primvar_notes)
        # Flakes with plain inputs: markers hdUE's import replaces with MaterialX's flake code.
        lower_flakes(doc, exported, graph["nodes"])
        for note in sorted(set(primvar_notes)):
            print(f"MaterialX for UE: {note}")
        if terminal and exported[terminal].getType() == "material":
            pass  # the terminal is the material
        elif terminal:
            m = doc.addNode("surfacematerial", material_name, "material")
            m.addInput("surfaceshader", "surfaceshader").setNodeName(
                names[terminal]
            )
            if "displacement" in graph:
                m.addInput(
                    "displacementshader", "displacementshader"
                ).setNodeName(names[graph["displacement"]["node"]])
        else:  # a volume material (UE's MaterialX importer makes a Volume-domain material)
            m = doc.addNode("volumematerial", material_name, "material")
            m.addInput("volumeshader", "volumeshader").setNodeName(
                names[graph["volume"]["node"]]
            )
        entries.append(
            dict(
                material=material_name,
                usd_material=source["material"],
                usd_shader=terminal or graph["volume"]["node"],
                graph=graph,
                fields=fields,
            )
        )
    if not entries:
        raise ValueError("No supported inline MaterialX materials were found")
    # Houdini's nodes become the standard nodes of their node graphs. A connected input's value is dropped: MaterialX
    # ignores it, and its 1.39 validation (UE's) rejects an input with both ("too many bindings"), which Houdini's
    # generated node graphs have.
    # So are the standard nodes UE's importer would expand itself (all node-graph nodes but those it translates
    # whole, UE_KEEP): expanded here, the rewrites below reach their insides (integers, matrices), which UE's own
    # expansion handed to its translator as they were (Lama's nodes crashed it).
    doc.flattenSubgraphs(
        "",
        lambda node: node.getNodeDef() is not None
        and (
            node.getNodeDef().getName() not in standard
            or node.getCategory() not in UE_KEEP
        ),
    )
    for node in doc.getNodes():
        for port in node.getInputs():
            if port.getConnectedNode() is not None:
                port.removeAttribute("value")
    # The nodes UE's importer cannot translate (integers and booleans, matrices, Houdini's 1.38 nodes, frame, ...)
    # rewritten into nodes it can (materialx_lowering.py).

    for note in lower(doc, library):
        print(f"MaterialX for UE: {note}")
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
