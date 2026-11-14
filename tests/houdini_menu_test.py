"""The Unreal menu's cache folders, sizes and clearing (hdue_menu), without Houdini."""

import sys
import tempfile
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
import hdue_menu

OCIO = hdue_menu.CACHE_FILES["ocio"]


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
            folder = Path(directory) / "hdue_ocio"
            (folder / "nested").mkdir(parents=True)
            (folder / "a.bin").write_bytes(b"x" * 1000)
            (folder / "nested/b.bin").write_bytes(b"x" * 24)
            self.assertEqual(hdue_menu.folder_size(folder), (1024, 2))
            self.assertEqual(hdue_menu.clear_folder(folder, OCIO), [])
            self.assertTrue(folder.is_dir())
            self.assertEqual(list(folder.iterdir()), [])
            self.assertEqual(hdue_menu.folder_size(folder / "missing"), (0, 0))
            self.assertEqual(
                hdue_menu.clear_folder(folder / "missing", OCIO), []
            )

    def test_virtual_texture_keeps_foreign_files(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            owned = [
                "0123456789ABCDEF0123456789abcdef01234567",
                "vt_x_128_4.manifest",
                "chunkABC.tmp",
                "manifestABC.tmp",
            ]
            for name in owned + ["notes.txt"]:
                (folder / name).write_bytes(b"x")
            (folder / "project").mkdir()
            rule = hdue_menu.CACHE_FILES["virtual_texture"]
            self.assertEqual(hdue_menu.clear_folder(folder, rule), [])
            self.assertEqual(
                sorted(entry.name for entry in folder.iterdir()),
                ["notes.txt", "project"],
            )

    def test_size_counts_only_what_clearing_would_delete(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "0123456789ABCDEF0123456789abcdef01234567").write_bytes(
                b"x" * 10
            )
            (folder / "notes.txt").write_bytes(b"x" * 500)
            (folder / "project").mkdir()
            (folder / "project/big.bin").write_bytes(b"x" * 500)
            rule = hdue_menu.CACHE_FILES["virtual_texture"]
            self.assertEqual(hdue_menu.folder_size(folder), (1010, 3))
            self.assertEqual(hdue_menu.folder_size(folder, rule), (10, 1))
            self.assertEqual(
                hdue_menu.folder_size(folder / "missing", rule), (0, 0)
            )

    def test_fixed_caches_refuse_other_names(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / "Documents"
            folder.mkdir()
            (folder / "letter.txt").write_bytes(b"x")
            with self.assertRaises(ValueError):
                hdue_menu.clear_folder(folder, hdue_menu.CACHE_FILES["texture"])
            self.assertEqual(
                [entry.name for entry in folder.iterdir()], ["letter.txt"]
            )

    def test_refuses_roots(self):
        # The deleters are mocks: a broken guard fails this test instead of emptying the home folder or the drive.
        boom = mock.Mock(side_effect=AssertionError("would delete"))
        with (
            mock.patch.object(hdue_menu.shutil, "rmtree", boom),
            mock.patch.object(Path, "unlink", boom),
        ):
            for folder in (Path.home(), Path(Path.cwd().anchor)):
                with self.assertRaises(ValueError):
                    hdue_menu.clear_folder(
                        folder, hdue_menu.CACHE_FILES["virtual_texture"]
                    )
        boom.assert_not_called()

    def test_format_size(self):
        self.assertEqual(hdue_menu.format_size(512), "512 bytes")
        self.assertEqual(hdue_menu.format_size(1536), "1.5 KB")
        self.assertEqual(hdue_menu.format_size(3 * 1024**4), "3072.0 GB")


if __name__ == "__main__":
    unittest.main()
