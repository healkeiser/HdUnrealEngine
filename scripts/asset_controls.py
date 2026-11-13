"""The asset catalog's warnings about unreal:asset prims, for usdview.

The delegate itself turns unreal:asset prims into bounds meshes carrying the asset (HdUE_NativeAssetSceneIndex), in
every host; the stage is not changed.
"""

import json
from pathlib import Path

from pxr import Sdf


class AssetReferences:
    def __init__(self, controller, catalog=None):
        self.controller = controller
        self.catalog = (
            json.loads(Path(catalog).read_text(encoding="utf-8-sig"))["assets"]
            if catalog
            else {}
        )
        self.dirty = True
        self.warnings = []
        controller._dataModel.signalStageReplaced.connect(self.invalidate)
        controller._dataModel.signalPrimsChanged.connect(self.invalidate)

    def invalidate(self, *args):
        self.dirty = True

    def sync(self):
        stage = self.controller._dataModel.stage
        if not stage or not self.dirty:
            return
        refs = {}
        invalid = []
        for prim in stage.Traverse():
            attr = prim.GetAttribute("unreal:asset")
            value = attr.Get() if attr else None
            if value:
                if isinstance(value, Sdf.AssetPath):
                    value = value.path
                if not isinstance(value, str):
                    invalid.append(
                        f"{prim.GetPath()}: unreal:asset requires a UE object path string"
                    )
                    continue
                refs[str(prim.GetPath())] = value
        self.warnings = invalid + [
            f"{path}: unavailable source script package {package} (its actors are omitted by cook)"
            for path, asset in refs.items()
            for package in self.catalog.get(asset, {}).get(
                "unavailable_script_packages", []
            )
        ]
        self.dirty = False
