"""Version numbers the Python side and UE's content side must agree on."""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from substrate_graph import COMPILER_VERSION


class VersionTests(unittest.TestCase):
    def test_substrate_compiler_versions_match(self):
        # A mismatch fails at prepare time; this catches it before.
        header = (
            ROOT / "unreal/HdUEBackend/Source/HdUEContent/Private/IdSwitch.h"
        ).read_text(encoding="utf-8")
        match = re.search(r"HdUESubstrateCompilerVersion = (\d+)", header)
        self.assertIsNotNone(match, "IdSwitch.h declares no compiler version")
        self.assertEqual(COMPILER_VERSION, int(match.group(1)))


if __name__ == "__main__":
    unittest.main()
