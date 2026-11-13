"""Run through uv run -- <HFS>/bin/hython.exe tests/houdini_host_state_test.py."""

import json
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
_runtime_directory = tempfile.TemporaryDirectory(prefix="hdue-host-test-")
_runtime_config = Path(_runtime_directory.name) / "runtime.json"
_runtime_config.write_text(json.dumps({"version": 1}), encoding="utf-8")
os.environ.setdefault("HDUE_RUNTIME_CONFIG", str(_runtime_config))
import hdue_houdini as integration


def host(running, closed):
    stub = types.SimpleNamespace(
        running=running, closed=closed, failures=5, resumed=0
    )

    def resume():
        stub.resumed += 1

    stub.resume = resume
    return stub


class HostState(unittest.TestCase):
    def setUp(self):
        self.saved = (
            integration._host,
            integration._failure,
            integration.scene_viewer,
            integration.start,
        )

    def tearDown(self):
        (
            integration._host,
            integration._failure,
            integration.scene_viewer,
            integration.start,
        ) = self.saved

    def test_start_resumes_a_stopped_host(self):
        stub = integration._host = host(running=False, closed=False)
        self.assertIs(integration.start(), stub)
        self.assertEqual(stub.resumed, 1)

    def test_start_does_not_resume_a_closed_host(self):
        stub = integration._host = host(running=False, closed=True)
        self.assertIs(integration.start(), stub)
        self.assertEqual(stub.resumed, 0)

    def test_activate_ignores_a_stale_failure(self):
        integration._host = host(running=True, closed=False)
        integration._failure = "old traceback"
        chosen = []
        viewer = types.SimpleNamespace(
            hydraRenderers=lambda: ("Unreal Engine",),
            setHydraRenderer=chosen.append,
        )
        integration.scene_viewer = lambda: viewer
        self.assertIs(integration.activate(), viewer)
        self.assertEqual(chosen, ["Unreal Engine"])

    def test_activate_refuses_a_closed_host(self):
        integration._host = host(running=False, closed=True)
        with self.assertRaises(RuntimeError):
            integration.activate()


if __name__ == "__main__":
    unittest.main()
