"""The Unreal menu's cache folders, sizes and clearing (hdue_menu), without Houdini."""

import sys
import tempfile
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
import hdue_menu


class Docs(unittest.TestCase):
    def test_the_installed_version_else_latest(self):
        base = "https://healkeiser.github.io/HdUnrealEngine"
        self.assertEqual(
            hdue_menu.docs_url({"HDUE_VERSION": "v0.5.0"}),
            f"{base}/0.5/houdini/",
        )
        self.assertEqual(
            hdue_menu.docs_url({"HDUE_VERSION": "v0.6.0-3-gabc"}),
            f"{base}/0.6/houdini/",
        )
        self.assertEqual(
            hdue_menu.docs_url({"HDUE_VERSION": "d4a505f"}),
            f"{base}/latest/houdini/",
        )
        self.assertEqual(hdue_menu.docs_url({}), f"{base}/latest/houdini/")


class CacheFolders(unittest.TestCase):
    def test_defaults_follow_the_backend_and_delegate(self):
        local, temp = Path("/local"), Path("/temp")
        folders = hdue_menu.cache_directories(
            {"LOCALAPPDATA": str(local), "TMP": str(temp), "TEMP": "/other"},
            "/runtime/HdUEBackend/HdUEBackend.uproject",
        )
        self.assertEqual(
            folders,
            {
                "texture": local / "hdUE/TextureCache",
                "virtual_texture": local / "hdUE/VirtualTextureCache",
                "nanite": local / "hdUE/Saved/NaniteCache",
                "ocio": temp / "hdue_ocio",
            },
        )

    def test_overrides_and_missing_bases(self):
        folders = hdue_menu.cache_directories(
            {
                "HDUE_VIRTUAL_TEXTURE_CACHE": "/vt",
                "HDUE_USER_DIR": "/prefs/hdUE",
                "TEMP": "/temp",
            },
            "/runtime/HdUEBackend.uproject",
        )
        self.assertEqual(
            folders,
            {
                "virtual_texture": Path("/vt"),
                "nanite": Path("/prefs/hdUE/Saved/NaniteCache"),
                "ocio": Path("/temp/hdue_ocio"),
            },
        )
        self.assertEqual(hdue_menu.cache_directories({}), {})

    def test_every_cache_has_menu_items(self):
        menu = ElementTree.parse(
            ROOT / "integrations/houdini/MainMenuCommon.xml"
        )
        code = " ".join(item.text for item in menu.iter("scriptCode"))
        for name in hdue_menu.CACHE_LABELS:
            self.assertIn(f'"open_cache", "{name}"', code)
            self.assertIn(f'"clear_caches", "{name}"', code)
        for action in (
            name.split('"')[1] for name in code.split("hdue_menu.run(")[1:]
        ):
            self.assertTrue(callable(getattr(hdue_menu, action)), action)


class Clearing(unittest.TestCase):
    def test_size_and_clear_keep_the_folder(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / "cache"
            (folder / "nested").mkdir(parents=True)
            (folder / "a.bin").write_bytes(b"x" * 1000)
            (folder / "nested/b.bin").write_bytes(b"x" * 24)
            self.assertEqual(hdue_menu.folder_size(folder), (1024, 2))
            self.assertEqual(hdue_menu.clear_folder(folder), [])
            self.assertTrue(folder.is_dir())
            self.assertEqual(list(folder.iterdir()), [])
            self.assertEqual(hdue_menu.folder_size(folder / "missing"), (0, 0))
            self.assertEqual(hdue_menu.clear_folder(folder / "missing"), [])

    def test_refuses_roots(self):
        for folder in (Path.home(), Path(Path.cwd().anchor)):
            with self.assertRaises(ValueError):
                hdue_menu.clear_folder(folder)

    def test_format_size(self):
        self.assertEqual(hdue_menu.format_size(512), "512 bytes")
        self.assertEqual(hdue_menu.format_size(1536), "1.5 KB")
        self.assertEqual(hdue_menu.format_size(3 * 1024**4), "3072.0 GB")


if __name__ == "__main__":
    unittest.main()
