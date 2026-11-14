"""The gallery checks fail on NaN, missing renders and unreadable brightness cells (no GPU or Houdini needed)."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import check_gallery
import render_gallery

HEADER = (
    "| Scene | a | Brightness (Lumen / Path Tracer) |\n| --- | ---: | ---: |\n"
)


def page_failures(*rows):
    with tempfile.TemporaryDirectory() as folder:
        page = Path(folder) / "gallery.md"
        page.write_text(HEADER + "\n".join(rows) + "\n", encoding="utf-8")
        return check_gallery.check(page)


class GalleryTable(unittest.TestCase):
    def test_normal_row_is_parsed(self):
        count, failures = page_failures("| Scene A | 1 s | 0.98 / 1.01 |")
        self.assertEqual((count, failures), (1, []))

    def test_nan_cell_fails(self):
        _, failures = page_failures("| Scene A | 1 s | nan / 1.01 |")
        self.assertEqual(len(failures), 1)
        self.assertIn("not a finite number", failures[0])

    def test_na_row_is_skipped(self):
        count, failures = page_failures(
            "| Scene A | 1 s | n/a |", "| Scene B | 1 s | 0.98 / 1.01 |"
        )
        self.assertEqual((count, failures), (1, []))


class Baselines(unittest.TestCase):
    def run_check(self, image, stored):
        """check_baselines for one scene/key with render `image` (None: no render file) against `stored`."""
        with tempfile.TemporaryDirectory() as folder:
            out, baselines = Path(folder) / "out", Path(folder) / "b.json"
            out.mkdir()
            baselines.write_text(json.dumps(stored))
            if image is not None:
                (out / "S_k.exr").write_bytes(b"")
            with (
                mock.patch.object(render_gallery, "OUT", out),
                mock.patch.object(render_gallery, "BASELINES", baselines),
                mock.patch.object(
                    render_gallery, "CORNELL_SCENES", [("S", 0, 0)]
                ),
                mock.patch.object(render_gallery, "TILE_LIMITS", {"k": 0.1}),
                mock.patch.object(
                    render_gallery, "TILE_MAX_LIMITS", {"k": 0.5}
                ),
                mock.patch.object(render_gallery, "pixels", lambda _: image),
            ):
                return render_gallery.check_baselines(False)

    def setUp(self):
        self.image = np.full((18, 32, 3), 0.5)
        self.stored = {
            "S": {"k": np.round(render_gallery.tiles(self.image), 5).tolist()}
        }

    def test_identical_image_passes(self):
        self.assertEqual(self.run_check(self.image, self.stored), [])

    def test_nan_image_fails(self):
        image = self.image.copy()
        image[0, 0, 0] = np.nan
        self.assertEqual(len(self.run_check(image, self.stored)), 1)

    def test_a_black_block_of_tiles_fails(self):
        # Six of the 144 tiles (2 x 3, each 2 x 2 pixels): under the 95th percentile, over the largest error.
        image = self.image.copy()
        image[0:4, 0:6] = 0
        failures = self.run_check(image, self.stored)
        self.assertEqual(len(failures), 1)
        self.assertIn("largest tile error", failures[0])

    def test_missing_render_with_baseline_fails(self):
        failures = self.run_check(None, self.stored)
        self.assertEqual(len(failures), 1)
        self.assertIn("render missing", failures[0])


class DevEnvironment(unittest.TestCase):
    def test_dev_profile_wins_over_inherited_release_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            paths = Path(folder) / "paths.json"
            paths.write_text(
                json.dumps({"runtime_config": "dev-runtime.json"}),
                encoding="utf-8",
            )
            env = {
                "HDUE_RUNTIME_CONFIG": "release-runtime.json",
                "HDUE_LOCAL_CONFIG": str(paths),
            }
            with mock.patch.dict("os.environ", env):
                render_gallery.dev_environment()
                profile = Path(os.environ["HDUE_RUNTIME_CONFIG"])
        self.assertEqual(profile, (Path(folder) / "dev-runtime.json").resolve())


if __name__ == "__main__":
    unittest.main()
