"""Run with uv run python tests/materialx_textures_cache_test.py: the UDIM and path caches follow their directories."""

import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from materialx_textures import resolved, udim_tiles  # noqa: E402


class DirectoryCaches(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        (self.folder / "albedo.1001.png").write_bytes(b"")
        (self.folder / "albedo.1002.png").write_bytes(b"")
        self.pattern = str(self.folder / "albedo.<UDIM>.png")
        # Older than the caches' recent window, so lookups are cached.
        stamp = time.time_ns() - 10_000_000_000
        os.utime(self.folder, ns=(stamp, stamp))

    def touch_folder(self):
        # A new entry changes the folder's modification time; make sure it differs from the cached one.
        stamp = os.stat(self.folder).st_mtime_ns + 1_000_000
        os.utime(self.folder, ns=(stamp, stamp))

    def test_new_tile_is_found(self):
        self.assertEqual(sorted(udim_tiles(self.pattern)), [1001, 1002])
        (self.folder / "albedo.1011.png").write_bytes(b"")
        self.touch_folder()
        self.assertEqual(sorted(udim_tiles(self.pattern)), [1001, 1002, 1011])

    def test_tile_added_in_the_same_tick_is_found(self):
        # A folder written just now: a new entry may not change its modification time, so it is not cached.
        folder = Path(tempfile.mkdtemp())
        (folder / "albedo.1001.png").write_bytes(b"")
        pattern = str(folder / "albedo.<UDIM>.png")
        self.assertEqual(sorted(udim_tiles(pattern)), [1001])
        stamp = os.stat(folder).st_mtime_ns
        (folder / "albedo.1002.png").write_bytes(b"")
        os.utime(folder, ns=(stamp, stamp))
        self.assertEqual(sorted(udim_tiles(pattern)), [1001, 1002])

    def test_cached_result_is_a_copy(self):
        tiles = udim_tiles(self.pattern)
        tiles.clear()
        self.assertEqual(len(udim_tiles(self.pattern)), 2)

    def test_errors_are_cached_and_cleared(self):
        missing = str(self.folder / "rough.<UDIM>.png")
        for _ in range(2):
            with self.assertRaises(ValueError):
                udim_tiles(missing)
        (self.folder / "rough.1001.png").write_bytes(b"")
        self.touch_folder()
        self.assertEqual(list(udim_tiles(missing)), [1001])

    def test_resolved_matches_pathlib(self):
        file = self.folder / "albedo.1001.png"
        self.assertEqual(resolved(file), str(file.resolve()))
        self.assertEqual(resolved(str(file)), str(file.resolve()))

    def test_repeated_lookups_are_cheap(self):
        udim_tiles(self.pattern)
        start = time.perf_counter()
        for _ in range(2000):
            udim_tiles(self.pattern)
        # One stat per lookup instead of a directory listing and a resolve per tile.
        self.assertLess(time.perf_counter() - start, 1.0)


if __name__ == "__main__":
    unittest.main()
