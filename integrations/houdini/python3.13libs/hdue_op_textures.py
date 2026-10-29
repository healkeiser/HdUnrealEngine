"""Resolve Houdini COP images without sending Houdini objects across the UE ABI."""

import hashlib
import os
import re
import tempfile
from pathlib import Path

import hou
from pxr import Sdf, Usd, UsdLux, UsdShade

_cache = None


def _source(uri, validate=False):
    match = re.fullmatch(r"op:(/[^\[\]<>]+?)/?(?:\[(\d+)\])?", uri)
    if not match:
        raise ValueError(
            "Expected an absolute COP path, optionally with [output]: " + uri
        )
    node = hou.node(match[1])
    if not isinstance(node, (hou.CopNode, hou.Cop2Node)):
        raise ValueError(
            "Texture op: path is not a COP/Copernicus node: " + uri
        )
    output = int(match[2] or 0)
    # Querying a Copernicus output's data type can itself evaluate the node.
    # Do that only when exporting, after checking its dirty state.
    if validate and isinstance(node, hou.CopNode):
        if output >= len(node.outputDataTypes()) or node.outputDataTypes()[
            output
        ] not in ("Mono", "UV", "RGB", "RGBA"):
            raise ValueError("Texture op: output must be a 2D image: " + uri)
    elif validate and isinstance(node, hou.Cop2Node) and output:
        raise ValueError(
            "Legacy COP textures support the color plane at output 0: " + uri
        )
    return node, output


def opdef_stamp(uri):
    """Changes when an opdef: image can have changed: its HDA library file on disk. Embedded definitions (no file)
    fall back to hashing the bytes.
    """
    node_type = hou.nodeType(uri[len("opdef:") :].split("?", 1)[0].strip("/"))
    definition = node_type.definition() if node_type else None
    path = definition.libraryFilePath() if definition else ""
    if not os.path.isfile(path):
        return hashlib.sha256(hou.readBinaryFile(uri)).hexdigest()
    stat = os.stat(path)
    return path, stat.st_mtime_ns, stat.st_size


class Cache:
    def __init__(self):
        self.directory = tempfile.TemporaryDirectory(prefix="hdue-cop-")
        self.entries = {}
        self.bindings = {}
        self.exporter = None

    def close(self):
        if self.exporter:
            try:
                self.exporter.destroy()
            except hou.ObjectWasDeleted:
                pass
            self.exporter = None
        self.entries.clear()
        self.bindings.clear()
        self.directory.cleanup()

    @staticmethod
    def stamp(node):
        # needsToCook includes changes to upstream inputs. cookCount also catches
        # changes already evaluated by another viewport before our polling tick.
        return (
            node.sessionId(),
            node.cookCount(),
            hou.frame() if node.isTimeDependent(for_last_cook=True) else None,
        )

    def resolve(self, uri, raw=False):
        if uri.startswith("opdef:"):
            # Raw/color interpretation does not alter embedded source bytes: one entry, re-read only when the HDA changes.
            stamp = opdef_stamp(uri)
            previous = self.entries.get(uri)
            if previous and previous["stamp"] == stamp:
                return previous["path"]
            from houdini_resources import embedded_image

            self.entries[uri] = dict(
                stamp=stamp,
                path=embedded_image(uri, self.directory.name)["path"],
            )
            return self.entries[uri]["path"]
        node, output = _source(uri)
        key = (uri, raw)
        previous = self.entries.get(key)
        if (
            previous
            and not node.needsToCook()
            and previous["stamp"] == self.stamp(node)
        ):
            return previous["path"]
        _source(uri, validate=True)
        root = Path(self.directory.name)
        pending = root / (
            "pending-" + hashlib.sha256(repr(key).encode()).hexdigest() + ".exr"
        )
        if isinstance(node, hou.Cop2Node):
            # Limit the save to this frame; the default saves the whole sequence.
            frame = hou.frame()
            if frame != int(frame):
                raise ValueError(
                    "Legacy COP image export requires an integer frame: " + uri
                )
            node.saveImage(str(pending), (int(frame), int(frame)))
        else:
            # The Image ROP understands Copernicus GPU layers, including selected
            # output ports, without a Python loop over millions of pixels.
            if self.exporter is None:
                self.exporter = hou.node("/out").createNode(
                    "image", "__hdue_texture_export", run_init_scripts=False
                )
            rop = self.exporter
            rop.parm("coppath").set(node.path())
            rop.parm("copoutput").set(str(pending))
            rop.parm("savebackground").set(0)
            rop.parm("setudim").set("off")
            rop.parm("setprecision").set(1)
            rop.parm("precision").set("b32")
            rop.parm("outputaovs").set(1)
            if rop.parm("enabled1"):
                rop.parm("enabled1").set(
                    1
                )  # Houdini 22.0.368's Image ROP has no per-AOV toggle
            rop.parm("aov1").set("C")
            rop.parm("useport1").set(1)
            rop.parm("port1").set(output)
            rop.parm("colorconversion").set("raw" if raw else "ocio")
            if not raw:
                # UE uses linear Rec.709. COP scene_linear can be ACEScg.
                rop.parm("ociocolorspace").set("Linear Rec.709 (sRGB)")
            rop.render(frame_range=(hou.frame(), hou.frame()))
        if not pending.is_file():
            raise RuntimeError("COP texture did not produce an image: " + uri)
        # Immutable content paths invalidate Hydra/UE texture caches and let an
        # in-flight render continue reading its previous image safely.
        digest = hashlib.sha256(pending.read_bytes()).hexdigest()
        result = root / (digest + ".exr")
        if result.exists():
            pending.unlink()
        else:
            pending.replace(result)
        self.entries[key] = dict(stamp=self.stamp(node), path=str(result))
        return str(result)

    def dirty(self, key):
        """Whether an image exported for key (see prepare_stage) changed: a COP recook, a new frame, new HDA bytes."""
        for uri in self.bindings.get(key, ()):
            try:
                if uri.startswith("opdef:"):
                    if uri not in self.entries or self.entries[uri][
                        "stamp"
                    ] != opdef_stamp(uri):
                        return True
                    continue
                node, _ = _source(uri)
                if not any((uri, raw) in self.entries for raw in (False, True)):
                    return True
                for raw in (False, True):
                    entry = self.entries.get((uri, raw))
                    if entry and (
                        node.needsToCook() or entry["stamp"] != self.stamp(node)
                    ):
                        return True
            except (ValueError, hou.Error):
                return True
        return False


def cache():
    global _cache
    if _cache is None:
        _cache = Cache()
    return _cache


def close():
    global _cache
    if _cache:
        _cache.close()
        _cache = None


def prepare_stage(stage, key="viewport"):
    """Export every op:/opdef: image the stage's shaders and lights use, on the host thread (outside any LOP cook).

    Returns {uri: file} for the delegate, which reads these paths through the host (hdue_set_viewport_textures).
    Images are exported raw: the delegate converts colour inputs from scene_linear as for any other EXR.
    """
    files = {}
    for prim in Usd.PrimRange.Stage(stage, Usd.TraverseInstanceProxies()):
        if not (prim.IsA(UsdShade.Shader) or prim.HasAPI(UsdLux.LightAPI)):
            continue
        for input_ in UsdShade.ConnectableAPI(prim).GetInputs():
            value = input_.GetAttr().Get()
            uri = (
                value.path
                if isinstance(value, Sdf.AssetPath)
                else value if isinstance(value, str) else ""
            )
            if uri.startswith(("op:", "opdef:")) and uri not in files:
                files[uri] = None
    cache().bindings[key] = set(files)
    for uri in files:
        files[uri] = cache().resolve(uri, True)
    return files
