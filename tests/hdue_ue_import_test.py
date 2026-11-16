"""The Unreal Asset LOP's cook (hdue_ue_import.import_assets), without Houdini or UE."""

import itertools
import json
import os
import sys
import tempfile
import threading
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
        self.pid = 4242

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
        self.source_root = source.parent
        self.project_file = temp / "HdUEBackend.uproject"
        self.project_file.write_text("{}")
        os.utime(self.project_file, (time.time() - 200, time.time() - 200))
        self.dll = temp / "UnrealEditor-HdUEContent.dll"
        self.engine_modules = None
        patches = [
            mock.patch.object(hdue_ue_import, "ROOT", temp),
            mock.patch.object(hdue_ue_import, "SOURCE_ROOT", self.source_root),
            mock.patch.object(
                hdue_ue_import,
                "PLUGINS_ROOT",
                temp / "unreal/HdUEBackend/Plugins",
            ),
            mock.patch.object(
                hdue_ue_import, "PROJECT_FILE", self.project_file
            ),
            mock.patch.object(
                hdue_ue_import,
                "_engine_modules_file",
                lambda: self.engine_modules,
            ),
            mock.patch.object(hdue_ue_import, "CONTENT_EDITOR", self.dll),
            mock.patch.object(
                hdue_ue_import,
                "runtime_config",
                lambda: {
                    "runtime_project": str(temp / "rt/Windows/p.uproject")
                },
            ),
            mock.patch.object(hdue_ue_import, "publish", lambda *a: []),
            mock.patch.object(hdue_ue_import.subprocess, "run"),
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

    def age(self, file, seconds):
        os.utime(file, (time.time() - seconds, time.time() - seconds))

    def built(self):
        """A DLL built now, after every source and plugin file (all 300 s old)."""
        self.dll.write_text("dll")
        for file in (self.source_root.parent / "Plugins").rglob("*"):
            if file.is_file():
                self.age(file, 300)
        for file in self.source_root.rglob("*"):
            if file.is_file():
                self.age(file, 300)

    def test_any_source_file_of_either_module_a_plugin_or_the_project_rebuilds(
        self,
    ):
        plugins = self.source_root.parent / "Plugins"
        output = plugins / "DLSS/Binaries/Win64/UnrealEditor-DLSS.dll"
        output.parent.mkdir(parents=True)
        output.write_text("dll")
        for file in (
            self.source_root / "HdUEBackend/Bridge.cpp",
            self.source_root / "HdUEBackend.Target.cs",
            self.source_root / "HdUEContent/HdUEContent.Build.cs",
            plugins / "DLSS/Source/DLSS/Private/DLSS.cpp",
            plugins / "DLSS/DLSS.uplugin",
        ):
            with self.subTest(file.name):
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text("//")
                self.built()
                self.assertTrue(hdue_ue_import.editor_is_current())
                # Only this file is newer than the DLL: every other one is older.
                self.age(self.dll, 150)
                self.age(file, 100)
                self.assertFalse(hdue_ue_import.editor_is_current())
        # A plugin's build output, newer than the DLL, is not a source.
        self.built()
        self.age(self.dll, 150)
        self.age(output, 100)
        self.assertTrue(hdue_ue_import.editor_is_current())
        self.built()
        for file in self.source_root.rglob("*"):
            if file.is_file():
                self.age(file, 100)
        self.assertTrue(hdue_ue_import.editor_is_current())
        self.age(self.project_file, -10)
        self.assertFalse(hdue_ue_import.editor_is_current())

    def test_a_different_engine_build_id_rebuilds(self):
        self.built()
        self.assertTrue(hdue_ue_import.editor_is_current())
        engine = Path(self.temp.name) / "engine.modules"
        self.engine_modules = engine
        built = self.dll.with_name("UnrealEditor.modules")
        built.write_text('{"BuildId": "1"}')
        engine.write_text('{"BuildId": "1"}')
        self.assertTrue(hdue_ue_import.editor_is_current())
        engine.write_text('{"BuildId": "2"}')
        self.assertFalse(hdue_ue_import.editor_is_current())
        built.unlink()  # a project that never recorded its build
        self.assertFalse(hdue_ue_import.editor_is_current())

    def test_windows_powershell_5_is_refused(self):
        with (
            mock.patch(
                "powershell_lookup.powershell_executable",
                return_value="powershell.exe",
            ),
            self.assertRaisesRegex(RuntimeError, "PowerShell 7"),
        ):
            self.run_import(FakeProcess())

    def test_an_interrupt_kills_the_cook(self):
        process = FakeProcess(polls_before_exit=100)

        def interrupt():
            raise KeyboardInterrupt

        with self.assertRaises(KeyboardInterrupt):
            self.run_import(process, interrupt)
        self.assertTrue(process.killed)

    def test_a_timeout_kills_the_whole_process_tree(self):
        process = FakeProcess(polls_before_exit=10**9)
        with (
            mock.patch.object(hdue_ue_import, "COOK_TIMEOUT_SECONDS", -1),
            mock.patch.object(hdue_ue_import.subprocess, "run") as run,
            self.assertRaises(TimeoutError),
        ):
            self.run_import(process)
        self.assertEqual(
            run.call_args.args[0], ["taskkill", "/PID", "4242", "/T", "/F"]
        )
        self.assertTrue(process.killed)

    def test_a_missing_taskkill_still_kills_the_shell(self):
        process = FakeProcess(polls_before_exit=10**9)
        with (
            mock.patch.object(hdue_ue_import, "COOK_TIMEOUT_SECONDS", -1),
            mock.patch.object(
                hdue_ue_import.subprocess, "run", side_effect=OSError
            ),
            self.assertRaises(TimeoutError),
        ):
            self.run_import(process)
        self.assertTrue(process.killed)

    def test_a_non_zero_exit_raises_with_the_log_tail(self):
        process = FakeProcess()
        process.poll = lambda: 3
        process.returncode = 3
        log = Path(self.temp.name) / "run/import.log"

        def popen(command, stdout, **kwargs):
            stdout.write("cook exploded")
            stdout.flush()
            return process

        with (
            mock.patch.object(hdue_ue_import.subprocess, "Popen", popen),
            self.assertRaisesRegex(RuntimeError, "cook exploded"),
        ):
            hdue_ue_import.import_assets(
                "p/p.uproject", ["/Game/A.A"], output=log.parent
            )


class Folder(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, path, text="x"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path


class ProjectAssets(Folder):
    def test_placeable_assets_are_listed_as_object_paths(self):
        content = self.root / "Content"
        for name in (
            "Meshes/SM_Rock.uasset",
            "BP_Door.uasset",
            "Maps/L_Test.umap",
            "T_Albedo.uasset",
        ):
            self.write(content / name)
        self.assertEqual(
            hdue_ue_import.project_assets(self.root / "P.uproject"),
            [
                "/Game/BP_Door.BP_Door_C",
                "/Game/Maps/L_Test.L_Test",
                "/Game/Meshes/SM_Rock.SM_Rock",
            ],
        )


class Publish(Folder):
    def test_new_files_are_copied_and_existing_ones_stay(self):
        cooked, windows = self.root / "cooked", self.root / "windows"
        content = Path("HdUEBackend/Content/Game")
        self.write(cooked / content / "New.uasset", "new")
        self.write(cooked / content / "Same.uasset", "same")
        self.write(windows / content / "Same.uasset", "same")
        self.write(cooked / content / "Changed.uasset", "cooked")
        self.write(windows / content / "Changed.uasset", "runtime")
        self.write(cooked / "AssetRegistry.bin", "registry")
        stale = hdue_ue_import.publish(cooked, windows)
        self.assertEqual(stale, ["HdUEBackend/Content/Game/Changed.uasset"])
        self.assertEqual((windows / content / "New.uasset").read_text(), "new")
        self.assertEqual(
            (windows / content / "Changed.uasset").read_text(), "runtime"
        )
        self.assertFalse((windows / "AssetRegistry.bin").exists())
        self.assertEqual(list(windows.rglob("*.partial")), [])


class WriteAtomic(Folder):
    def test_a_reader_holding_the_file_makes_the_replace_retry(self):
        target = self.write(self.root / "a.json", "old")
        real = os.replace
        calls = []

        def replace(source, destination):
            calls.append(source)
            if len(calls) < 3:
                raise PermissionError
            real(source, destination)

        with (
            mock.patch.object(hdue_ue_import.os, "replace", replace),
            mock.patch.object(hdue_ue_import.time, "sleep"),
        ):
            hdue_ue_import._write_atomic(target, "new")
        self.assertEqual(target.read_text(), "new")
        self.assertEqual(len(calls), 3)
        self.assertEqual(list(self.root.glob("*.pending")), [])

    def test_a_replace_that_keeps_failing_raises_and_leaves_no_pending_file(
        self,
    ):
        target = self.write(self.root / "a.json", "old")
        with (
            mock.patch.object(
                hdue_ue_import.os, "replace", side_effect=PermissionError
            ),
            mock.patch.object(hdue_ue_import.time, "sleep"),
            self.assertRaises(PermissionError),
        ):
            hdue_ue_import._write_atomic(target, "new")
        self.assertEqual(target.read_text(), "old")
        self.assertEqual(list(self.root.glob("*.pending")), [])

    def test_each_write_uses_its_own_pending_name(self):
        names = []
        real = os.replace

        def replace(source, destination):
            names.append(Path(source).name)
            real(source, destination)

        with mock.patch.object(hdue_ue_import.os, "replace", replace):
            for _ in range(2):
                hdue_ue_import._write_atomic(self.root / "a.json", "x")
        self.assertEqual(len(set(names)), 2)


class MergeCatalog(Folder):
    def setUp(self):
        super().setUp()
        self.project = self.root / "rt/Windows/HdUEBackend/HdUEBackend.uproject"
        self.project.parent.mkdir(parents=True)
        self.profile = self.root / "runtime.json"
        self.profile.write_text(
            json.dumps({"version": 1, "runtime_project": str(self.project)}),
            encoding="utf-8",
        )
        self.config = {"runtime_project": str(self.project)}
        self.counter = itertools.count()

    def profile_with(self, **fields):
        profile = json.loads(self.profile.read_text())
        profile.update(fields)
        self.profile.write_text(json.dumps(profile), encoding="utf-8")
        self.config.update(fields)

    def added(self, **assets):
        return self.write(
            self.root / f"added{next(self.counter)}.json",
            json.dumps({"assets": assets}),
        )

    def test_a_missing_catalog_is_created_and_named_in_the_profile(self):
        hdue_ue_import.merge_catalog(
            self.added(A={"v": 1}), self.profile, self.config
        )
        catalog = self.project.parent / "hdue_catalog.json"
        self.assertEqual(
            json.loads(catalog.read_text())["assets"], {"A": {"v": 1}}
        )
        self.assertIn("asset_catalog", json.loads(self.profile.read_text()))

    def test_a_catalog_another_profile_made_is_read_before_merging(self):
        catalog = self.write(
            self.project.parent / "hdue_catalog.json",
            json.dumps({"version": 1, "assets": {"Old": {"v": 0}}}),
        )
        hdue_ue_import.merge_catalog(
            self.added(A={"v": 1}), self.profile, self.config
        )
        self.assertEqual(
            sorted(json.loads(catalog.read_text())["assets"]), ["A", "Old"]
        )

    def test_an_existing_entry_wins_over_a_re_imported_one(self):
        catalog = self.write(
            self.root / "catalog.json",
            json.dumps({"version": 1, "assets": {"A": {"v": "old"}}}),
        )
        self.config["asset_catalog"] = str(catalog)
        self.profile_with(asset_catalog=str(catalog))
        hdue_ue_import.merge_catalog(
            self.added(A={"v": "new"}, B={"v": "b"}),
            self.profile,
            self.config,
        )
        self.assertEqual(
            json.loads(catalog.read_text())["assets"],
            {"A": {"v": "old"}, "B": {"v": "b"}},
        )

    def test_files_are_written_atomically_and_the_profile_path_is_relative(
        self,
    ):
        hdue_ue_import.merge_catalog(
            self.added(A={"v": 1}), self.profile, self.config
        )
        self.assertEqual(list(self.root.rglob("*.pending")), [])
        value = json.loads(self.profile.read_text())["asset_catalog"]
        self.assertFalse(Path(value).is_absolute())
        with mock.patch.dict(
            os.environ, {"HDUE_RUNTIME_CONFIG": str(self.profile)}
        ):
            resolved = hdue_ue_import.runtime_config()["asset_catalog"]
        # resolve(): a short (8.3) temp folder, as CI's RUNNER~1, reads back in its long form.
        self.assertEqual(
            Path(resolved),
            (self.project.parent / "hdue_catalog.json").resolve(),
        )

    def test_concurrent_merges_keep_both_entries(self):
        hdue_ue_import.merge_catalog(
            self.added(Z={"v": 0}), self.profile, self.config
        )
        files = [self.added(**{f"A{i}": {"v": i}}) for i in range(4)]
        threads = [
            threading.Thread(
                target=hdue_ue_import.merge_catalog,
                args=(file, self.profile, self.config),
            )
            for file in files
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        catalog = self.project.parent / "hdue_catalog.json"
        self.assertEqual(
            sorted(json.loads(catalog.read_text())["assets"]),
            sorted(["Z"] + [f"A{i}" for i in range(4)]),
        )


if __name__ == "__main__":
    unittest.main()
