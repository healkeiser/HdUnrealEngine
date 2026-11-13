"""The Houdini viewport's material parameters (hdue_viewport.Viewport.materials), without Houdini."""

import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
sys.path.insert(0, str(ROOT / "scripts"))
import hdue_viewport


class Materials(unittest.TestCase):
    def test_colors_get_alpha_one_as_husk_and_usdview_send_them(self):
        sent = []
        stub = types.SimpleNamespace(
            set_materials=lambda update: sent.append(update._obj) or 0,
            runtime=types.SimpleNamespace(check=lambda *args: None),
        )
        hdue_viewport.Viewport.materials(
            stub,
            1,
            {"/M": "/Game/M.M"},
            {"/M": {"tint": (0.5, 0.25, 0.125), "rgba": (1, 2, 3, 4)}},
        )
        material = sent[0].materials[0]
        parameters = {
            p.name.decode(): p.value[:]
            for p in material.parameters[: material.parameter_count]
        }
        self.assertEqual(parameters["tint"], [0.5, 0.25, 0.125, 1.0])
        self.assertEqual(parameters["rgba"], [1, 2, 3, 4])


if __name__ == "__main__":
    unittest.main()
