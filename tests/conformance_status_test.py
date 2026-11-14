"""run_conformance's per-run status: a run with no checks is an error, not a pass."""

import sys
import unittest
import unittest.mock
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import run_conformance


def entry(*passes):
    return {"checks": [{"pass": p} for p in passes]}


class Status(unittest.TestCase):
    def test_no_checks_is_an_error(self):
        self.assertEqual(run_conformance.status(entry()), "error")

    def test_all_pass(self):
        self.assertEqual(run_conformance.status(entry(True, True)), "pass")

    def test_one_failing(self):
        self.assertEqual(
            run_conformance.status(entry(True, False)), "partial 1/2"
        )

    def test_none_passing(self):
        self.assertEqual(run_conformance.status(entry(False)), "fail")


class SplitBlocks(unittest.TestCase):
    def measure(self, image):
        return run_conformance.measure(
            {1: {"depth": image}}, {"aov": "depth", "stat": "split_blocks"}
        )

    def test_a_half_size_image_repeated_in_blocks_has_none(self):
        half = np.arange(16, dtype=np.float32).reshape(4, 4)
        self.assertEqual(self.measure(half.repeat(2, 0).repeat(2, 1)), 0.0)

    def test_an_edge_off_the_block_grid_splits_its_blocks(self):
        image = np.zeros((8, 8), dtype=np.float32)
        image[:, 3:] = (
            1  # columns 2-3 hold both values in each of 4 rows of blocks
        )
        self.assertEqual(self.measure(image), 4 / 16)


class Ratio(unittest.TestCase):
    def verify(self, a, b, **window):
        probes = {"p": a, "q": b}
        check = {"ratio": ["p", "q"], **window}
        with unittest.mock.patch.object(
            run_conformance, "measure", lambda frames, probe: probes[probe]
        ):
            return run_conformance.verify(check, {}, "")

    def test_two_black_probes_fail_a_window_around_one(self):
        value, passed = self.verify(0.0, 0.0, min=0.8, max=1.2)
        self.assertFalse(passed)
        self.assertIn("black", value)

    def test_two_black_probes_fail_even_without_a_window(self):
        self.assertFalse(self.verify(0.0, 0.0)[1])

    def test_a_ratio_inside_its_window_passes(self):
        self.assertEqual(self.verify(0.5, 0.5, min=0.8, max=1.2), (1.0, True))

    def test_a_black_reference_alone_is_infinite(self):
        value, passed = self.verify(0.5, 0.0, min=0.8, max=1.2)
        self.assertEqual(value, float("inf"))
        self.assertFalse(passed)


if __name__ == "__main__":
    unittest.main()
