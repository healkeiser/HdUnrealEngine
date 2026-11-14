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
HEADERS = BRIDGE + (ROOT / "include/hdue/viewport_api.h").read_text(
    encoding="utf-8"
)
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


# The C types the mirrored structs use, by size (= alignment on x64); pointers and nested structs are handled apart.
SCALARS = {
    "char": 1,
    "uint32_t": 4,
    "float": 4,
    "uint64_t": 8,
    "double": 8,
    "HdueSession": 8,
}


def c_layout(name):
    """({field: offset}, size, alignment) of a struct in the headers, laid out as an x64 C compiler does: each field
    at the next multiple of its alignment, the size rounded up to the largest one.
    """
    body = re.search(
        rf"typedef struct {name} \{{(.*?)\}} {name};", HEADERS, re.S
    ).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    offsets, offset, alignment = {}, 0, 1
    for declaration in filter(None, (d.strip() for d in body.split(";"))):
        if "HDUE_CALL" in declaration:  # a function pointer: a pointer field
            base = "void*"
            declarators = [
                re.search(r"HDUE_CALL\*\s*(\w+)\)", declaration).group(1)
            ]
        else:
            first, *rest = declaration.split(",")
            *words, last = first.split()
            base = " ".join(w for w in words if w != "const")
            declarators = [last] + [r.strip() for r in rest]
        for declarator in declarators:
            count = 1
            for n in re.findall(r"\[(\d+)\]", declarator):
                count *= int(n)
            field = re.sub(r"\[.*", "", declarator)
            if base.endswith("*") or field.startswith("*"):
                size = align = 8
            elif base in SCALARS:
                size = align = SCALARS[base]
            else:
                _, size, align = c_layout(base)
            offset = -(-offset // align) * align
            offsets[field.lstrip("*")] = offset
            offset += size * count
            alignment = max(alignment, align)
    return offsets, -(-offset // alignment) * alignment, alignment


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

    def test_field_offsets(self):
        # By name, so two swapped fields of one size fail: the mirrors use the C field names.
        sizes = c_sizes()
        for name, mirror in MIRRORS.items():
            with self.subTest(name):
                offsets, size, _ = c_layout(name)
                # The layout computed here is the compiler's (tests/abi_layout.c asserts the sizes).
                self.assertEqual(size, sizes[name])
                self.assertEqual(
                    {
                        field[0]: getattr(mirror, field[0]).offset
                        for field in mirror._fields_
                    },
                    offsets,
                )

    def test_every_mirror_has_a_c_assert(self):
        self.assertEqual(sorted(set(MIRRORS) - set(c_sizes())), [])


if __name__ == "__main__":
    unittest.main()
