"""Queue ordering, retries, environment isolation, and immutable publication."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from live_materials import LivePreparation
from prepare_substrate import publish
from substrate_graph import COMPILER_VERSION, signature


def record(extra=False, value=0.5):
    nodes = {"surface": dict(type="slab", inputs={"roughness": value})}
    if extra:
        nodes["value"] = dict(type="constant", properties={"value": value})
        nodes["surface"]["inputs"]["roughness"] = {"node": "value"}
    return dict(
        material="/Looks/Test",
        graph=dict(version=1, output="surface", nodes=nodes),
        base_identity="",
        base_asset="",
    )


class Process:
    pid = 42

    def __init__(self, *args, **kwargs):
        self.code = None

    def poll(self):
        return self.code


class LiveTests(unittest.TestCase):
    def test_clean_removes_only_what_no_process_needs(self):
        import os
        import time

        from live_materials import STALE_AGE, UNUSED_AGE, clean
        from prepare_substrate import LIBRARY_NAME

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            project = root / "runtime/HdUEBackend.uproject"
            project.parent.mkdir(parents=True)
            project.write_text("{}")
            live = root / "runtime/Content/HdUE/Live"
            now = time.time()

            def job(name, signature, age, library=True, package=True):
                folder = live / "P1" / name
                folder.mkdir(parents=True)
                asset = f"/Game/HdUE/Live/P1/{name}/M_x.M_x"
                if package:
                    (folder / "M_x.uasset").write_bytes(b"")
                if library:
                    (folder / LIBRARY_NAME).write_text(
                        json.dumps(
                            dict(
                                version=1,
                                compiler=COMPILER_VERSION,
                                materials=[
                                    dict(
                                        signature=signature,
                                        asset=asset,
                                        preview_asset=asset,
                                    )
                                ],
                            )
                        )
                    )
                for path in [folder, *folder.iterdir()]:
                    os.utime(path, (now - age, now - age))
                return folder

            kept = job("V1_current", "a", 0)
            fresh_orphan = job("V2_publishing", "b", 60, library=False)
            orphan = job("V3_orphan", "c", STALE_AGE + 60, library=False)
            replaced = job("V4_replaced", "a", STALE_AGE + 60)
            broken = job("V5_missing", "d", STALE_AGE + 60, package=False)
            unused = job("V6_unused", "e", UNUSED_AGE + 60)
            recent_replaced = job("V7_replaced_recently", "a", 60)
            # The editor project's packages of each job: kept while the runtime job exists or the job is recent.
            editor = root / "editor/Content"

            def editor_job(name, age):
                folder = editor / "HdUE/Live/P1" / name
                folder.mkdir(parents=True)
                os.utime(folder, (now - age, now - age))
                return folder

            editor_kept = editor_job("V1_current", STALE_AGE + 60)
            editor_running = editor_job("V8_running", 60)
            editor_gone = editor_job("V9_gone", STALE_AGE + 60)
            editor_twin_removed = editor_job("V3_orphan", STALE_AGE + 60)
            removed = set(clean(project, now=now, editor_content=editor))
            self.assertEqual(
                removed, {orphan, replaced, broken, unused, editor_gone}
            )
            for path in (
                kept,
                fresh_orphan,
                recent_replaced,
                editor_kept,
                editor_running,
            ):
                self.assertTrue(path.is_dir(), path)
            # Its runtime twin was removed by this pass: the next pass removes it.
            self.assertIn(
                editor_twin_removed,
                set(clean(project, now=now, editor_content=editor)),
            )

    def test_published_jobs_count_as_prepared(self):
        # A job published by an earlier process (prepare_substrate.publish) is known to the next one, unless its
        # packages are gone.
        from prepare_substrate import LIBRARY_NAME

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            project = root / "runtime/HdUEBackend.uproject"
            project.parent.mkdir(parents=True)
            project.write_text("{}")
            for job, keep in (("V1_a", True), ("V2_b", False)):
                packages = root / "runtime/Content/HdUE/Live/P1" / job
                packages.mkdir(parents=True)
                entry = dict(
                    signature="sig-" + job,
                    base_identity="",
                    asset=f"/Game/HdUE/Live/P1/{job}/M_x.M_x",
                    preview_asset=f"/Game/HdUE/Live/P1/{job}/M_x_preview.M_x_preview",
                )
                if keep:
                    (packages / "M_x.uasset").write_bytes(b"")
                    (packages / "M_x_preview.uasset").write_bytes(b"")
                (packages / LIBRARY_NAME).write_text(
                    json.dumps(
                        dict(
                            version=1,
                            compiler=COMPILER_VERSION,
                            materials=[entry],
                        )
                    )
                )
            manager = LivePreparation(project, root / "logs", root)
            self.assertEqual(list(manager.entries), [("sig-V1_a", "")])

    def test_due_asks_the_host_to_sync_again(self):
        # The viewport host syncs materials only after stage edits: due() is what brings it back once an edit has
        # settled (to start the preparation) and once a preparation finished (to bind it).
        with (
            tempfile.TemporaryDirectory() as folder,
            patch("live_materials.shutil.which", return_value="pwsh"),
            patch("live_materials.subprocess.Popen", side_effect=Process),
        ):
            root = Path(folder)
            manager = LivePreparation(
                root / "runtime/HdUEBackend.uproject",
                root / "logs",
                root,
                debounce=0.5,
            )
            self.assertFalse(manager.due(now=0))
            manager.request([record()], [], now=0)
            self.assertEqual(manager.phase, "queued")
            self.assertFalse(manager.due(now=0.2))
            self.assertTrue(manager.due(now=0.6))
            manager.request([record()], [], now=0.6)
            self.assertEqual(manager.started, 1)
            self.assertFalse(manager.due(now=1))
            manager.active[
                "log"
            ].close()  # a finished preparation, as tick() records it
            manager.active, manager.revision = None, manager.revision + 1
            self.assertTrue(manager.due(now=1))
            manager.wanted = (
                None  # what the host does once the edited graph binds
            )
            self.assertFalse(manager.due(now=2))

    def test_single_flight_latest_graph_cache_failure_and_retry(self):
        with (
            tempfile.TemporaryDirectory() as folder,
            patch("live_materials.shutil.which", return_value="pwsh"),
            patch(
                "live_materials.subprocess.Popen", side_effect=Process
            ) as popen,
        ):
            root = Path(folder)
            manager = LivePreparation(
                root / "runtime/HdUEBackend.uproject",
                root / "logs",
                root,
                debounce=0.5,
            )
            a = record()
            b = record(True)
            manager.request([a], [], now=0)
            self.assertEqual(manager.started, 0)
            manager.request([a], [], now=1)
            self.assertEqual(manager.started, 1)
            manager.request([record(value=0.8)], [], now=1.1)
            self.assertEqual(manager.started, 1)
            manager.request([b], [], now=1.2)
            self.assertEqual(manager.started, 1)
            active = manager.active
            active["output"].mkdir()
            entry = dict(
                signature=signature(a["graph"]),
                base_identity="",
                asset="/Game/A.A",
                preview_asset="/Game/AP.AP",
            )
            (active["output"] / "published.json").write_text(
                json.dumps(dict(inline_shader_code=True))
            )
            (active["output"] / "library.json").write_text(
                json.dumps(dict(compiler=COMPILER_VERSION, materials=[entry]))
            )
            active["process"].code = 0
            manager.tick()
            self.assertEqual(manager.completed, 1)
            self.assertEqual(len(manager.entries), 1)
            # A completed stale job is cached, while B remains the requested graph.
            self.assertEqual(
                manager.wanted[0],
                __import__("hashlib")
                .sha256(json.dumps([(signature(b["graph"]), "")]).encode())
                .hexdigest(),
            )
            manager.request([b], list(manager.entries.values()), now=2)
            self.assertEqual(manager.started, 2)
            manager.active["process"].code = 1
            manager.tick()
            self.assertIn("failed", manager.request([b], [], now=3))
            self.assertEqual(manager.started, 2)
            manager.retry()
            manager.request([b], [], now=4)
            self.assertEqual(manager.started, 3)
            self.assertIsNone(
                manager.request([a], list(manager.entries.values()), now=5)
            )
            manager.close()
            self.assertEqual(popen.call_count, 3)

    def test_a_job_finishing_inside_request_is_known(self):
        # husk's wait loop builds known before request(); the tick() inside request() can then add the job that just
        # finished. That job must count, or the same material is prepared a second time.
        with (
            tempfile.TemporaryDirectory() as folder,
            patch("live_materials.shutil.which", return_value="pwsh"),
            patch("live_materials.subprocess.Popen", side_effect=Process),
        ):
            root = Path(folder)
            manager = LivePreparation(
                root / "runtime/HdUEBackend.uproject",
                root / "logs",
                root,
                debounce=0,
            )
            a = record()
            manager.request([a], [], now=0)
            active = manager.active
            active["output"].mkdir()
            (active["output"] / "published.json").write_text(
                json.dumps(dict(inline_shader_code=True))
            )
            (active["output"] / "library.json").write_text(
                json.dumps(
                    dict(
                        compiler=COMPILER_VERSION,
                        materials=[
                            dict(
                                signature=signature(a["graph"]),
                                base_identity="",
                                asset="/Game/A.A",
                                preview_asset="/Game/AP.AP",
                            )
                        ],
                    )
                )
            )
            active["process"].code = 0
            self.assertIsNone(manager.request([a], [], now=1))
            self.assertEqual(manager.started, 1)
            manager.close()

    def test_worker_does_not_inherit_gui_usd_startup_environment(self):
        with (
            tempfile.TemporaryDirectory() as folder,
            patch("live_materials.shutil.which", return_value="pwsh"),
            patch(
                "live_materials.subprocess.Popen", side_effect=Process
            ) as popen,
            patch.dict(
                os.environ,
                {
                    "HOUDINI_PATH": "gui-hooks",
                    "PXR_PLUGINPATH_NAME": "gui-usd",
                    "PYTHONPATH": "gui-python",
                    "HDUE_HOUDINI_ENABLE": "1",
                    "HDUE_HOUDINI_ROOT": "sdk-root",
                },
            ),
        ):
            manager = LivePreparation(
                Path(folder) / "runtime/HdUEBackend.uproject",
                Path(folder) / "logs",
                Path(folder),
                debounce=0,
            )
            manager.request([record()], [], now=0)
            environment = popen.call_args.kwargs["env"]
            for key in (
                "HOUDINI_PATH",
                "PXR_PLUGINPATH_NAME",
                "PYTHONPATH",
                "HDUE_HOUDINI_ENABLE",
            ):
                self.assertNotIn(key, environment)
            self.assertEqual(environment["HDUE_HOUDINI_ROOT"], "sdk-root")
            manager.close()

    def test_unavailable_worker_is_latched_until_retry(self):
        with (
            tempfile.TemporaryDirectory() as folder,
            patch("live_materials.shutil.which", return_value=None),
        ):
            manager = LivePreparation(
                Path(folder) / "runtime/HdUEBackend.uproject",
                Path(folder) / "logs",
                Path(folder),
                debounce=0,
            )
            manager.request([record()], [], now=0)
            self.assertEqual(manager.phase, "failed")
            self.assertEqual(manager.started, 0)
            self.assertIn("PowerShell", manager.request([record()], [], now=1))
            self.assertEqual(manager.started, 0)
            manager.close()

    def test_publication_is_atomic_immutable_and_scoped(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            output = root / "job"
            project = root / "runtime/HdUEBackend.uproject"
            project.parent.mkdir()
            project.write_text("{}")
            destination = "/Game/HdUE/Live/Session/V1"
            source = (
                output
                / "cooked/Windows/HdUEBackend/Content/HdUE/Live/Session/V1"
            )
            source.mkdir(parents=True)
            for name in ("M.uasset", "M.uexp", "P.uasset"):
                (source / name).write_bytes(b"owned fixture")
            library = dict(
                version=1,
                compiler=COMPILER_VERSION,
                materials=[
                    dict(
                        asset=destination + "/M.M",
                        preview_asset=destination + "/P.P",
                    )
                ],
            )
            (output / "library.json").write_text(json.dumps(library))
            publish(output, project, destination)
            self.assertEqual(
                (
                    project.parent / "Content/HdUE/Live/Session/V1/M.uexp"
                ).read_bytes(),
                b"owned fixture",
            )
            self.assertTrue((output / "published.json").is_file())
            with self.assertRaises(FileExistsError):
                publish(output, project, destination)
            with self.assertRaises(ValueError):
                publish(output, project, "/Game/HdUE/Live/../../Outside")


if __name__ == "__main__":
    unittest.main()
