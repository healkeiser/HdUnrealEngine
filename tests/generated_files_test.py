"""Checks that the deterministic generators reproduce their committed files, and that every generated schema class is
registered in plugInfo.json.in.

The render settings outputs (.ds, docs, the defaults header) need the engine source and are checked by running
scripts/generate_render_settings.py and diffing; the header alone is checked by render_setting_defaults_test.py.
"""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_groom_conformance  # noqa: E402
import generate_materialx_conformance  # noqa: E402
import generate_reference_figures  # noqa: E402
import generate_render_settings  # noqa: E402


def run(main):
    with contextlib.redirect_stdout(io.StringIO()):
        main()


class GeneratedFiles(unittest.TestCase):
    def assert_same(self, written, committed):
        self.assertEqual(
            written.read_bytes(),
            committed.read_bytes(),
            f"{committed.relative_to(ROOT)} is out of date: run its generator",
        )

    def test_conformance_scenes(self):
        for module in (
            generate_materialx_conformance,
            generate_groom_conformance,
        ):
            with self.subTest(module.__name__):
                committed = module.OUT
                with tempfile.TemporaryDirectory() as folder:
                    module.OUT = Path(folder) / committed.name
                    try:
                        run(module.main)
                        self.assert_same(module.OUT, committed)
                    finally:
                        module.OUT = committed

    def test_reference_figures(self):
        committed = generate_reference_figures.SPECS
        with tempfile.TemporaryDirectory() as folder:
            generate_reference_figures.SPECS = Path(folder)
            try:
                run(generate_reference_figures.main)
                written = sorted(Path(folder).glob("ref-*.json"))
                self.assertEqual(
                    [f.name for f in written],
                    sorted(f.name for f in committed.glob("ref-*.json")),
                )
                for file in written:
                    self.assert_same(file, committed / file.name)
            finally:
                generate_reference_figures.SPECS = committed

    def test_schema_classes_are_registered(self):
        text = (ROOT / "resources/plugInfo.json.in").read_text(encoding="utf-8")
        for name, _ in generate_render_settings.SCHEMA_CLASSES:
            for key in ("primTypeName", "schemaIdentifier"):
                with self.subTest(f"{name} {key}"):
                    self.assertIn(f'"{key}": "{name}"', text)


if __name__ == "__main__":
    unittest.main()
