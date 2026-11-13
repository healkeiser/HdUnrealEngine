"""The Python ctypes copies of the C ABI match include/hdue/bridge.h and tests/abi_layout.c (no DLL needed)."""

import ctypes
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
import asset_runtime
import hdue_runtime
import hdue_viewport
import lighting_api
import material_api
import residency_api
import surface_api

BRIDGE = (ROOT / "include/hdue/bridge.h").read_text(encoding="utf-8")
LAYOUT = (ROOT / "tests/abi_layout.c").read_text(encoding="utf-8")

# C struct -> its Python mirror.
MIRRORS = {
    "HdueInitDesc": hdue_runtime.InitDesc,
    "HdueStatus": hdue_runtime.Status,
    "HdueApi": hdue_runtime.Api,
    "HdueAssetDesc": asset_runtime.AssetDesc,
    "HdueAssetStatus": asset_runtime.AssetStatus,
    "HdueAssetSceneStatus": asset_runtime.AssetSceneStatus,
    "HdueWetBinding": surface_api.Binding,
    "HdueSurfaceBatch": surface_api.Batch,
    "HdueSurfaceStatus": surface_api.Status,
    "HdueLightingStatus": lighting_api.Status,
    "HdueRendererInfo": material_api.RendererInfo,
    "HdueMaterialParameter": material_api.Parameter,
    "HdueMaterialUpdate": material_api.Update,
    "HdueMaterialStats": material_api.MaterialStats,
    "HdueResidencyOptions": residency_api.Options,
    "HdueResidencyStatus": residency_api.Status,
    "HdueFrameInfo": hdue_viewport.Frame,
    "HdueViewportStatus": hdue_viewport.Status,
    "HdueViewportMaterial": hdue_viewport.Material,
    "HdueViewportMaterials": hdue_viewport.Materials,
    "HdueViewportTexture": hdue_viewport.Texture,
    "HdueViewportTextures": hdue_viewport.Textures,
    "HdueViewportWet": hdue_viewport.Wet,
    "HdueViewportSurfaces": hdue_viewport.Surfaces,
}


def c_sizes():
    sizes = {}
    for name, expr in re.findall(
        r"_Static_assert\(sizeof\((\w+)\) == ([^,]+),", LAYOUT
    ):
        # Only digits, + and * reach eval (e.g. "8 + 45 * 8").
        assert re.fullmatch(r"[\d+* ]+", expr), expr
        sizes[name] = eval(expr, {"__builtins__": {}})
    return sizes


class AbiMirrors(unittest.TestCase):
    def test_version(self):
        version = int(
            re.search(r"#define HDUE_ABI_VERSION (\d+)u", BRIDGE).group(1)
        )
        self.assertEqual(version, hdue_runtime.ABI_VERSION)

    def test_function_table_names_and_order(self):
        block = BRIDGE.split("typedef struct HdueApi {")[1].split("} HdueApi;")[
            0
        ]
        names = re.findall(r"HDUE_CALL\*\s*(\w+)\)", block)
        self.assertEqual(
            names, [name for name, _ in hdue_runtime.Api._fields_[2:]]
        )
        self.assertEqual(len(names), hdue_runtime.API_FUNCTIONS)

    def test_struct_sizes(self):
        sizes = c_sizes()
        for name, mirror in MIRRORS.items():
            with self.subTest(name):
                self.assertEqual(ctypes.sizeof(mirror), sizes[name])

    def test_every_mirror_has_a_c_assert(self):
        self.assertEqual(sorted(set(MIRRORS) - set(c_sizes())), [])


if __name__ == "__main__":
    unittest.main()
