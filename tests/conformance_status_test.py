"""run_conformance's per-run status: a run with no checks is an error, not a pass."""

import sys
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
