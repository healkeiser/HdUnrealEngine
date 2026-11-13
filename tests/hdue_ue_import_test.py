"""The Unreal Asset LOP's cook (hdue_ue_import.import_assets), without Houdini or UE."""

import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
sys.path.insert(0, str(ROOT / "scripts"))
import hdue_ue_import


class FakeProcess:
    def __init__(self, polls_before_exit=2):
        self.polls = 0
        self.polls_before_exit = polls_before_exit
        self.returncode = None
        self.killed = False

    def poll(self):
        self.polls += 1
        if not self.killed and self.polls > self.polls_before_exit:
            self.returncode = 0
        return self.returncode

    def kill(self):
        self.killed = True
        self.returncode = -9

    def wait(self):
        return self.returncode


class ImportAssets(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        temp = Path(self.temp.name)
        source = temp / "unreal/HdUEBackend/Source/HdUEContent"
        source.mkdir(parents=True)
        self.source_file = source / "Module.cpp"
        self.source_file.write_text("//")
        self.dll = temp / "UnrealEditor-HdUEContent.dll"
        patches = [
            mock.patch.object(hdue_ue_import, "ROOT", temp),
            mock.patch.object(hdue_ue_import, "CONTENT_SOURCE", source),
            mock.patch.object(hdue_ue_import, "CONTENT_EDITOR", self.dll),
            mock.patch.object(
                hdue_ue_import,
                "runtime_config",
                lambda: {
                    "runtime_project": str(temp / "rt/Windows/p.uproject")
                },
            ),
            mock.patch.object(hdue_ue_import, "publish", lambda *a: []),
            mock.patch.object(hdue_ue_import, "merge_catalog", lambda *a: None),
            mock.patch.dict(
                os.environ, {"HDUE_RUNTIME_CONFIG": str(temp / "runtime.json")}
            ),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def run_import(self, process, progress=None):
        commands = []

        def popen(command, **kwargs):
            commands.append(command)
            return process

        with mock.patch.object(hdue_ue_import.subprocess, "Popen", popen):
            hdue_ue_import.import_assets(
                "p/p.uproject",
                ["/Game/A.A"],
                output=Path(self.temp.name) / "run",
                progress=progress,
            )
        return commands[0]

    def test_progress_is_called_while_the_cook_runs_and_a_current_editor_is_not_rebuilt(
        self,
    ):
        self.dll.write_text("dll")
        os.utime(self.source_file, (time.time() - 100, time.time() - 100))
        calls = []
        command = self.run_import(FakeProcess(), lambda: calls.append(1))
        self.assertGreaterEqual(len(calls), 2)
        self.assertIn("-SkipBuild", command)

    def test_a_changed_source_rebuilds_the_editor(self):
        self.dll.write_text("dll")
        os.utime(self.dll, (time.time() - 100, time.time() - 100))
        self.assertNotIn("-SkipBuild", self.run_import(FakeProcess()))

    def test_an_interrupt_kills_the_cook(self):
        process = FakeProcess(polls_before_exit=100)

        def interrupt():
            raise KeyboardInterrupt

        with self.assertRaises(KeyboardInterrupt):
            self.run_import(process, interrupt)
        self.assertTrue(process.killed)


if __name__ == "__main__":
    unittest.main()
