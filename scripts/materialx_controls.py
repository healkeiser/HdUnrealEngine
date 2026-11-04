"""Bind external or prepared inline MaterialX to cooked Substrate materials."""

import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

from materialx_textures import validate_texture_sets
from pxr import Gf, Sdf, UsdShade
from usd_materialx_graph import (
    graph_data,
    graph_signature,
    needs_import,
    rebase_graph,
    structure_key,
)


def mtlx_material(prim):
    """(resolved .mtlx path casefolded, material name) of a Material that references a material in a .mtlx document
    through USD's usdMtlx file format (references = @file.mtlx@</MaterialX/Materials/<name>>), or (None, None).
    An older form, a Shader with info:mtlx:sourceAsset and a subIdentifier naming the material, is still read: USD
    expects a nodedef name there and warns ("Invalid MaterialX NodeDef").
    """
    for spec in prim.GetPrimStack():
        if spec.layer.GetFileFormat().formatId == "mtlx":
            return (
                str(Path(spec.layer.realPath).resolve()).casefold(),
                spec.path.name,
            )
    output = UsdShade.Material(prim).GetSurfaceOutput("mtlx")
    connection = output.GetConnectedSource() if output else None
    if connection:
        shader = connection[0].GetPrim()
        source = shader.GetAttribute("info:mtlx:sourceAsset").Get()
        name = shader.GetAttribute("info:mtlx:sourceAsset:subIdentifier").Get()
        if isinstance(source, Sdf.AssetPath) and name:
            return str(
                Path(source.resolvedPath or source.path).resolve()
            ).casefold(), str(name)
    return None, None


class OfflineView:
    """usdview's stage view and data model as MaterialXLibrary reads them, for hosts without usdview. It is its own
    controller; renderer settings land in .settings.
    """

    def __init__(self, stage=None, time=None, renderer=None):
        self.settings = {}
        self._renderer = renderer
        signal = SimpleNamespace(connect=lambda callback: None)
        self._dataModel = SimpleNamespace(
            stage=stage,
            currentFrame=time,
            signalStageReplaced=signal,
            signalPrimsChanged=signal,
        )
        self._stageView = self

    def GetRendererSetting(self, name):
        return self.settings.get(name)

    def SetRendererSetting(self, name, value):
        self.settings[name] = value


class MaterialXLibrary:
    """Binds MaterialX materials to prepared native assets: a cooked library's (Import-MaterialX.ps1,
    Import-UsdMaterialX.ps1; file, optional) and the ones live preparation made (set_prepared). A material neither has
    renders through hdUE's render-time translation; one that translation cannot express is listed in missing for the
    host to prepare (live_materials.LivePreparation), and renders translated meanwhile.
    """

    def __init__(self, file, controller):
        self.controller = controller
        library = (
            json.loads(Path(file).read_text(encoding="utf-8-sig"))
            if file
            else dict(version=1, materials=[], dependencies={})
        )
        if library.get("version") != 1:
            raise ValueError("Unsupported MaterialX library version")
        validate_texture_sets(library.get("texture_sets", []))
        for name, expected in library["dependencies"].items():
            path = Path(name)
            if (
                not path.is_file()
                or hashlib.sha256(path.read_bytes()).hexdigest() != expected
            ):
                raise ValueError(
                    f"MaterialX source changed; import/cook again: {path}"
                )
        self.materials = {
            (str(Path(p["source"]).resolve()).casefold(), p["material"]): p
            for p in library["materials"]
        }
        for entry in library["materials"]:
            entry["_hdue_identity"] = hashlib.sha256(
                json.dumps(
                    dict(
                        asset=entry["asset"],
                        model=entry["model"],
                        dependencies=sorted(library["dependencies"].values()),
                    ),
                    sort_keys=True,
                ).encode()
            ).hexdigest()
        self.inline = {
            (
                str(Path(p["usd_stage"]).resolve()).casefold(),
                p["usd_material"],
            ): p
            for p in library["materials"]
            if "usd_stage" in p
        }
        self.bound_entries = {}
        self.prepared = {}
        self.missing = []
        self.missing_graphs = (
            {}
        )  # path -> graph of each missing material, for its preparation record
        self.preparation_error = None
        self.preparation_exception = None
        self.owned = set()
        self.dirty = True
        self.last_renderer = None
        self.last_time = None
        self.bindings = []
        model = controller._dataModel
        model.signalStageReplaced.connect(self.invalidate)
        model.signalPrimsChanged.connect(self.invalidate)

    def invalidate(self, *args):
        self.dirty = True

    def set_prepared(self, entries):
        """The MaterialX materials live preparation made (kind "materialx" entries), by structure_key."""
        prepared = {}
        for entry in entries:
            entry.setdefault(
                "_hdue_identity",
                hashlib.sha256(
                    json.dumps(
                        dict(asset=entry["asset"], model=entry["model"]),
                        sort_keys=True,
                    ).encode()
                ).hexdigest(),
            )
            prepared.setdefault(entry["structure"], []).append(entry)
        if prepared != self.prepared:
            self.prepared = prepared
            self.dirty = True

    @staticmethod
    def _values(entry, path, stage, time):
        values = []
        for binding in entry["usd_bindings"]:
            attribute = (
                path + binding["attribute"][len(entry["usd_material"]) :]
            )
            attr = stage.GetAttributeAtPath(attribute)
            values.append((binding, attr.Get(time) if attr else None))
        return values

    def prepared_binding(self, prim, graph, stage, time):
        """A live-prepared entry for graph: the same structure, and the same values where the import baked them in."""
        path = str(prim.GetPath())
        for entry in self.prepared.get(structure_key(graph), []):
            rebased = rebase_graph(graph, path, entry["usd_material"])
            mutable = [b["attribute"] for b in entry["usd_bindings"]]
            if (
                graph_signature(rebased, mutable)
                == entry["usd_graph_signature"]
            ):
                return entry, self._values(entry, path, stage, time)
        return None, []

    def inline_binding(self, prim, stage, time):
        path = str(prim.GetPath())
        stage_file = str(
            Path(stage.GetRootLayer().realPath).resolve()
        ).casefold()
        exact = self.inline.get((stage_file, path))
        authored = {
            str(spec.path.StripAllVariantSelections())
            for spec in prim.GetPrimStack()
        }
        candidates = (
            [exact]
            if exact
            else [
                e for e in self.inline.values() if e["usd_material"] in authored
            ]
        )
        if not candidates:
            names = {prim.GetName()} | {
                spec.path.name for spec in prim.GetPrimStack()
            }
            candidates = [
                e
                for e in self.inline.values()
                if Sdf.Path(e["usd_material"]).name in names
            ]
        if not candidates:
            return None, []
        graph = graph_data(UsdShade.Material(prim), time)
        matches = []
        for entry in candidates:
            rebased = rebase_graph(graph, path, entry["usd_material"])
            if (
                graph_signature(
                    rebased, [p["attribute"] for p in entry["usd_bindings"]]
                )
                == entry["usd_graph_signature"]
            ):
                matches.append(entry)
        if len(matches) != 1:
            raise ValueError(
                "MaterialX graph/static input changed or ambiguous; prepare/import/cook again: "
                + path
            )
        entry = matches[0]
        return entry, self._values(entry, path, stage, time)

    def sync(self):
        from cooperative import run

        return run(self.sync_steps())

    def sync_steps(self):
        """sync() as a cooperative walk (cooperative.Job): yields between materials."""
        try:
            yield from self._sync_steps()
            self.preparation_error = self.preparation_exception = None
            return True
        except ValueError as error:
            self.preparation_error = str(error)
            self.preparation_exception = error
            return False

    def _sync_steps(self):
        model = self.controller._dataModel
        view = self.controller._stageView
        if not model.stage or view._renderer is None:
            return
        if (
            model.currentFrame != self.last_time
            or id(view._renderer) != self.last_renderer
        ):
            self.dirty = True
        if not self.dirty:
            return
        asset_map = dict(view.GetRendererSetting("nativeMaterialAssets") or {})
        overrides = dict(
            view.GetRendererSetting("nativeMaterialParameters") or {}
        )
        for path in self.owned:
            asset_map.pop(path, None)
            overrides.pop(path, None)
        owned, bindings, bound_entries, missing, graphs = set(), [], {}, [], {}
        for prim in model.stage.Traverse():
            yield
            if not prim.IsA(UsdShade.Material):
                continue
            output = UsdShade.Material(prim).GetSurfaceOutput("mtlx")
            connection = output.GetConnectedSource() if output else None
            if not connection:
                continue
            path = str(prim.GetPath())
            entry, values = self.inline_binding(
                prim, model.stage, model.currentFrame
            )
            file, name = mtlx_material(prim) if not entry else (None, None)
            if not entry and (file, name) not in self.materials:
                # Not in the cooked library: hdUE translates it at render time, unless the translation cannot
                # express it; then a live-prepared material, else the host prepares one (missing).
                try:
                    graph = graph_data(
                        UsdShade.Material(prim), model.currentFrame
                    )
                except ValueError:
                    continue  # displacement or volume: the render-time translation reads it
                if not needs_import(graph):
                    continue
                entry, values = self.prepared_binding(
                    prim, graph, model.stage, model.currentFrame
                )
                if not entry:
                    missing.append(UsdShade.Material(prim))
                    graphs[path] = graph
                    continue
            elif not entry:
                shader = UsdShade.Shader(connection[0].GetPrim())
                entry = self.materials[(file, name)]
                values = []
                material = UsdShade.Material(prim)
                for name, p in entry["bindings"].items():
                    # usdMtlx publishes a referenced material's inputs on the Material (its shader connects to
                    # them); hdUE's unreal:*_texture overrides are authored there too.
                    source_input = material.GetInput(name) or shader.GetInput(
                        name
                    )
                    values.append(
                        (
                            p,
                            (
                                source_input.Get(model.currentFrame)
                                if source_input
                                else None
                            ),
                        )
                    )
            asset_map[path] = entry["asset"]
            fields = {}
            for p, value in values:
                if value is None:
                    continue
                if p["type"] == "scalar":
                    value = float(value)
                elif p["type"] == "vector":
                    if len(value) == 2:
                        value = Gf.Vec4f(
                            *value, *p.get("default", [0, 0, 0, 1])[2:]
                        )
                    else:
                        value = (
                            Gf.Vec4f(*value)
                            if len(value) == 4
                            else Gf.Vec3f(*value)
                        )
                else:
                    if not isinstance(value, str) or not value.startswith("/"):
                        raise ValueError(
                            f"Texture overrides require a cooked UE asset path: {path}"
                        )
                fields[p["parameter"]] = value
            overrides[path] = fields
            bindings.append(
                dict(
                    path=path,
                    asset=entry["asset"],
                    model=entry["model"],
                    parameters=list(fields),
                )
            )
            bound_entries[path] = entry
            owned.add(path)
        if asset_map != (view.GetRendererSetting("nativeMaterialAssets") or {}):
            view.SetRendererSetting("nativeMaterialAssets", asset_map)
        if overrides != (
            view.GetRendererSetting("nativeMaterialParameters") or {}
        ):
            view.SetRendererSetting("nativeMaterialParameters", overrides)
        self.owned, self.bindings = owned, bindings
        self.bound_entries, self.missing = bound_entries, missing
        self.missing_graphs = graphs
        from surface_effects import resolve_steps

        try:
            effects, self.surface_diagnostics = yield from resolve_steps(
                model.stage, bound_entries, model.currentFrame
            )
        except ValueError as error:
            view.SetRendererSetting("wetValidationError", str(error))
            raise
        view.SetRendererSetting("wetValidationError", "")
        if effects != (view.GetRendererSetting("surfaceEffects") or {}):
            view.SetRendererSetting("surfaceEffects", effects)
        # Keep only an identity token: retaining the Engine object postpones
        # its GL resource destruction until after the Qt context has closed.
        self.last_renderer, self.last_time, self.dirty = (
            id(view._renderer),
            model.currentFrame,
            False,
        )
