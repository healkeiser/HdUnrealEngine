"""usdview's asset catalog warnings, with real USD; the stage is left unchanged."""

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from asset_controls import AssetReferences
from pxr import Sdf, Usd, UsdGeom


class Signal:
    def connect(self, callback):
        pass


class AssetReferencesTest(unittest.TestCase):
    def test_catalog_warnings_without_stage_edits(self):
        stage = Usd.Stage.CreateInMemory()
        prim = UsdGeom.Xform.Define(stage, "/Asset").GetPrim()
        prim.CreateAttribute("unreal:asset", Sdf.ValueTypeNames.String).Set(
            "/Game/M.M"
        )
        model = SimpleNamespace(
            stage=stage,
            currentFrame=Usd.TimeCode.Default(),
            signalStageReplaced=Signal(),
            signalPrimsChanged=Signal(),
        )
        refs = AssetReferences(SimpleNamespace(_dataModel=model))
        refs.catalog = {
            "/Game/M.M": {"unavailable_script_packages": ["/Script/X"]}
        }
        before = stage.GetSessionLayer().ExportToString()
        refs.sync()
        self.assertEqual(len(refs.warnings), 1)
        self.assertIn("/Script/X", refs.warnings[0])
        self.assertEqual(stage.GetSessionLayer().ExportToString(), before)
        self.assertFalse(stage.GetPrimAtPath("/Asset/__HdUEAssetProxy"))

    def test_a_non_string_asset_is_a_warning_not_an_error(self):
        stage = Usd.Stage.CreateInMemory()
        UsdGeom.Xform.Define(stage, "/Bad").GetPrim().CreateAttribute(
            "unreal:asset", Sdf.ValueTypeNames.Int
        ).Set(3)
        UsdGeom.Xform.Define(stage, "/Good").GetPrim().CreateAttribute(
            "unreal:asset", Sdf.ValueTypeNames.String
        ).Set("/Game/M.M")
        model = SimpleNamespace(
            stage=stage,
            currentFrame=Usd.TimeCode.Default(),
            signalStageReplaced=Signal(),
            signalPrimsChanged=Signal(),
        )
        refs = AssetReferences(SimpleNamespace(_dataModel=model))
        refs.catalog = {
            "/Game/M.M": {"unavailable_script_packages": ["/Script/X"]}
        }
        refs.sync()
        self.assertEqual(len(refs.warnings), 2)
        self.assertIn("/Bad", refs.warnings[0])
        self.assertIn("/Good", refs.warnings[1])


if __name__ == "__main__":
    unittest.main()
