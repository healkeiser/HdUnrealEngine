"""Checks that each hdUE setting's default lives once, in the generator's HDUE list, and every reader uses it."""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_render_settings as generator  # noqa: E402

HEADER = ROOT / "src/hydra/render_setting_defaults.h"
VIEWPORT = (
    ROOT / "integrations/houdini/soho/parameters/HdUnrealEngine_Viewport.ds"
)
SETTINGS = {s["attribute"].removeprefix("unreal:"): s for s in generator.HDUE}


def sources():
    hydra = ROOT / "src/hydra"
    return {
        f.name: f.read_text(encoding="utf-8")
        for f in sorted(hydra.glob("*.cpp")) + sorted(hydra.glob("*.h"))
    }


class RenderSettingDefaults(unittest.TestCase):
    def test_header_is_current(self):
        self.assertEqual(
            generator.defaults_header(),
            HEADER.read_text(encoding="utf-8"),
            "run scripts/generate_render_settings.py",
        )

    def test_cpp_reads_use_the_header(self):
        names = "|".join(SETTINGS)
        read = re.compile(rf'setting<[^>]+>\(\s*"({names})",\s*([^\n]*)')
        literal, found = [], 0
        for file, text in sources().items():
            for match in read.finditer(text):
                found += 1
                name, default = match.groups()
                # viewerLightIntensity reads double with a float read of the same setting as its fallback.
                nested = re.match(
                    rf'\w+(\.|->)setting<[^>]+>\("{name}"', default
                )
                if not (
                    default.startswith("HdUESettingDefaults::")
                    or default.startswith("float(HdUESettingDefaults::")
                    or nested
                ):
                    line = text.count("\n", 0, match.start()) + 1
                    literal.append(f"{file}:{line}: {name}")
        # A pattern that stopped matching the reads (a renamed setting<>) would check nothing.
        self.assertGreater(
            found, 0, "no setting<T>(name, default) read matched"
        )
        self.assertEqual(literal, [])

    def test_descriptors_use_the_header(self):
        text = (ROOT / "src/hydra/delegate.cpp").read_text(encoding="utf-8")
        start = text.index("GetRenderSettingDescriptors() const {")
        body = text[start : text.index("\n}", start)]
        entries = re.findall(r'TfToken\("(\w+)"\),\s*(VtValue\([^}]*)\}', body)
        checked = [(n, v) for n, v in entries if n in SETTINGS]
        self.assertGreater(len(checked), 10)
        for name, value in checked:
            self.assertIn("HdUESettingDefaults::" + name, value, name)

    def test_viewport_ds_defaults(self):
        text = VIEWPORT.read_text(encoding="utf-8").split(
            "/// BEGIN generated"
        )[0]
        parms = dict(
            re.findall(
                r'name\s+"(xn__unreal\w+)".*?default\s*\{\s*([^}]*?)\s*\}',
                text,
                re.S,
            )
        )
        self.assertTrue(parms)
        by_parm = {generator.houdini_name("unreal:" + n): n for n in SETTINGS}
        for parm, written in parms.items():
            name = by_parm[parm]
            setting = SETTINGS[name]
            default = setting["default"][0]
            with self.subTest(name):
                if setting["kind"] == "toggle":
                    self.assertEqual(written, "1" if default else "0")
                elif setting["kind"] in ("menu", "string", "file"):
                    self.assertEqual(written, f'"{default}"')
                else:
                    self.assertEqual(float(written), float(default))


if __name__ == "__main__":
    unittest.main()
