"""Asynchronous, single-flight preparation of immutable Substrate variants."""

import hashlib
import json
import os
import shutil
import subprocess
import threading
import time
import uuid
from pathlib import Path

from substrate_graph import COMPILER_VERSION, signature

# Clean-up ages: another process may be publishing (interrupted publishes), or still using a job (the rest).
INCOMING_AGE = 3600
STALE_AGE = 86400
UNUSED_AGE = 30 * 86400
WORKER_AGE = 7 * 86400


def _jobs(content):
    """The runtime's published live jobs (Content/HdUE/Live/<process>/<job>): (folder, library file or None, entries
    usable now). A job is usable while its compiler is current, its textures are unchanged and its packages exist.
    """
    from prepare_substrate import LIBRARY_NAME
    from substrate_textures import validate_dependencies

    for folder in sorted(
        p for p in (content / "HdUE/Live").glob("*/*") if p.is_dir()
    ):
        if ".incoming-" in folder.name:
            continue
        file = folder / LIBRARY_NAME
        if not file.is_file():
            yield folder, None, []
            continue
        try:
            data = json.loads(file.read_text(encoding="utf-8"))
            if data.get("compiler") != COMPILER_VERSION:
                raise ValueError("compiler")
            validate_dependencies(data)
            entries = [
                dict(entry, _published=str(file))
                for entry in data.get("materials", [])
                if all(
                    (
                        content
                        / (
                            entry[f].split(".")[0].removeprefix("/Game/")
                            + ".uasset"
                        )
                    ).is_file()
                    for f in ("asset", "preview_asset")
                )
            ]
            if len(entries) != len(data.get("materials", [])):
                raise ValueError("missing packages")
        except (OSError, ValueError, KeyError):
            entries = []
        yield folder, file, entries


def published(runtime_project, previews=True):
    """The materials earlier processes prepared into this runtime (prepare_substrate.publish), by identity, from the
    usable jobs; with previews, only those prepared with their mask preview (a batch render prepares none). Each
    entry names its library (_published), which binding it marks as used (used()).
    """
    content = Path(runtime_project).resolve().parent / "Content"
    entries = {}
    for _, _, usable in _jobs(content):
        for entry in usable:
            if previews and not entry.get("preview", True):
                continue
            entries[(entry["signature"], entry.get("base_identity", ""))] = (
                entry
            )
    return entries


_USED = set()


def used(entry):
    """Marks a published entry's job as used now (its library's modification time), once per process: clean()
    removes jobs no process has bound for UNUSED_AGE.
    """
    file = entry.get("_published")
    if file and file not in _USED:
        _USED.add(file)
        try:
            os.utime(file)
        except OSError:
            pass


def clean(runtime_project, log_directory=None, now=None):
    """Removes, from the runtime's live content, what no process needs: interrupted publishes, jobs without a library
    (crashed, or published before libraries were kept) or no longer usable, jobs a newer job fully replaces, and jobs
    unused for UNUSED_AGE; from log_directory, worker folders older than WORKER_AGE. Files a running UE holds open
    stay (the next clean() retries). Returns the removed paths.
    """
    now = time.time() if now is None else now
    content = Path(runtime_project).resolve().parent / "Content"
    removed = []

    def age(path):
        try:
            return now - path.stat().st_mtime
        except OSError:
            return 0

    def remove(path):
        (
            shutil.rmtree(path, ignore_errors=True)
            if path.is_dir()
            else path.unlink(missing_ok=True)
        )
        if not path.exists():
            removed.append(path)

    live = content / "HdUE/Live"
    if not live.is_dir():
        return removed
    for path in live.glob("**/*.incoming-*"):
        if age(path) > INCOMING_AGE:
            remove(path)
    jobs = list(_jobs(content))
    # The newest usable job for each identity: older jobs whose identities all have a newer one are replaced.
    newest = {}
    for _, file, entries in jobs:
        for entry in entries:
            key = (
                entry["signature"],
                entry.get("base_identity", ""),
                entry.get("preview", True),
            )
            if key not in newest or age(file) < age(newest[key]):
                newest[key] = file
    for folder, file, entries in jobs:
        if file is None or not entries:
            stale = age(folder if file is None else file) > STALE_AGE
        else:
            replaced = all(
                newest[
                    (
                        e["signature"],
                        e.get("base_identity", ""),
                        e.get("preview", True),
                    )
                ]
                != file
                for e in entries
            )
            stale = (replaced and age(file) > STALE_AGE) or age(
                file
            ) > UNUSED_AGE
        if stale:
            remove(folder)
    for process in live.glob("*"):
        if process.is_dir() and not any(process.iterdir()):
            remove(process)
    if log_directory:
        for worker in Path(log_directory, "substrate-live").glob("*"):
            if worker.is_dir() and age(worker) > WORKER_AGE:
                remove(worker)
    return removed


class LivePreparation:
    def __init__(
        self,
        runtime_project,
        log_directory,
        root=None,
        debounce=0.65,
        previews=True,
    ):
        self.root = Path(root) if root else Path(__file__).resolve().parents[1]
        self.runtime_project = Path(runtime_project).resolve()
        self.namespace = (
            f"/Game/HdUE/Live/P{os.getpid()}_{uuid.uuid4().hex[:12]}"
        )
        self.directory = (
            Path(log_directory)
            / "substrate-live"
            / self.namespace.rsplit("/", 1)[1]
        )
        self.debounce = debounce
        self.enabled = True
        self.closed = False
        self.entries = {}
        self.failures = {}
        self.wanted = None
        self.changed_at = 0.0
        self.active = None
        self.revision = 0
        self.requested_revision = 0  # revision at the last request(), so due() sees a finished preparation
        self.started = 0
        self.completed = 0
        self.phase = "idle"
        self.message = ""
        self.last_progress = 0.0
        self.previews = previews  # mask-preview variants: the viewport's wet-mask preview needs them, husk does not
        self.entries.update(published(self.runtime_project, previews))

        # What earlier processes left behind, in the background: start-up does not wait for it.
        def clean_quietly():
            try:
                clean(self.runtime_project, log_directory)
            except (
                OSError
            ):  # a clean-up never disturbs preparation; the next process retries
                pass

        threading.Thread(
            target=clean_quietly,
            name="hdUE live clean-up",
            daemon=True,
        ).start()

    @staticmethod
    def identity(record):
        return (signature(record["graph"]), record.get("base_identity", ""))

    def status(self):
        return dict(
            phase=self.phase,
            message=self.message,
            started=self.started,
            completed=self.completed,
            worker_pid=self.active["process"].pid if self.active else None,
            wanted=self.wanted[0] if self.wanted else None,
            active=self.active["key"] if self.active else None,
            cache_entries=len(self.entries),
            revision=self.revision,
        )

    def tick(self):
        if not self.active:
            return
        active = self.active
        code = active["process"].poll()
        if code is None:
            now = time.monotonic()
            if now - self.last_progress < 0.2:
                return
            self.last_progress = now
            try:
                progress = json.loads(
                    (active["folder"] / "progress.json").read_text(
                        encoding="utf-8-sig"
                    )
                )
                self.phase = progress["phase"]
                self.message = progress["message"]
            except (OSError, ValueError, KeyError):
                pass
            return
        active["log"].close()
        self.active = None
        try:
            if code:
                raise ValueError(
                    "Substrate preparation failed; see "
                    + str(active["folder"] / "worker.log")
                )
            receipt = json.loads(
                (active["output"] / "published.json").read_text(
                    encoding="utf-8-sig"
                )
            )
            data = json.loads(
                (active["output"] / "library.json").read_text(
                    encoding="utf-8-sig"
                )
            )
            if (
                not receipt.get("inline_shader_code")
                or data.get("compiler") != COMPILER_VERSION
            ):
                raise ValueError(
                    "The live material has an incompatible preparation receipt"
                )
            for entry in data["materials"]:
                self.entries[
                    (entry["signature"], entry.get("base_identity", ""))
                ] = entry
            # Published into the runtime: the worker's cooked copy is no longer needed (its logs stay).
            shutil.rmtree(active["output"] / "cooked", ignore_errors=True)
            self.revision += 1
            self.completed += 1
            self.phase = "ready"
            self.message = "Edited Substrate material is ready"
        except (OSError, ValueError, KeyError) as error:
            self.failures[active["key"]] = str(error)
            self.phase = "failed"
            self.message = str(error)

    def due(self, now=None):
        """Whether the host should sync materials again without a stage edit: the latest edit has settled and can
        start preparing, or a preparation finished since the last request and its material can bind. The host syncs
        only after stage edits, and request() moves only when called.
        """
        if not self.wanted or not self.enabled or self.closed or self.active:
            return False
        if self.wanted[0] in self.failures:
            return False
        if self.revision != self.requested_revision:
            return True
        now = time.monotonic() if now is None else now
        return now - self.changed_at >= self.debounce

    def request(self, records, known, now=None):
        self.tick()
        self.requested_revision = self.revision
        if not self.enabled or self.closed:
            return None
        identities = {
            (e["signature"], e.get("base_identity", "")) for e in known
        }
        missing = [r for r in records if self.identity(r) not in identities]
        if not missing:
            return None
        keys = sorted({self.identity(r) for r in missing})
        key = hashlib.sha256(json.dumps(keys).encode()).hexdigest()
        now = time.monotonic() if now is None else now
        if self.wanted is None or self.wanted[0] != key:
            self.wanted = (key, missing)
            self.changed_at = now
        if key in self.failures:
            return self.failures[key]
        if self.active:
            suffix = (
                "; latest edit is queued" if self.active["key"] != key else ""
            )
            return (
                "Substrate: "
                + (self.message or "Preparing the edited graph")
                + suffix
            )
        if now - self.changed_at < self.debounce:
            self.phase = "queued"
            self.message = "Waiting for graph edits to settle"
            return "Substrate: " + self.message
        self._start(key, missing)
        return "Substrate: Preparing the edited graph in the background"

    def _start(self, key, records):
        executable = shutil.which("pwsh") or shutil.which("powershell")
        if not executable:
            self.phase = "failed"
            self.message = (
                "PowerShell is required for automatic Substrate preparation"
            )
            self.failures[key] = self.message
            return
        self.started += 1
        folder = self.directory / f"{self.started:04d}-{key[:12]}"
        job = folder / "job.json"
        output = folder / "prepared"
        destination = self.namespace + f"/V{self.started}_{key[:12]}"
        log = None
        command = [
            executable,
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(self.root / "scripts/Prepare-SubstrateLive.ps1"),
            "-GraphJob",
            str(job.resolve()),
            "-OutputDirectory",
            str(output.resolve()),
            "-Destination",
            destination,
            "-RuntimeProject",
            str(self.runtime_project),
        ]
        # A GUI host's USD plugin paths/startup hooks must not leak into hython
        # or the editor worker. Inherited hooks can load another backend/USD ABI.
        environment = {
            name: value
            for name, value in os.environ.items()
            if not (
                name.startswith(("HOUDINI_", "PXR_", "QT_"))
                or (
                    name.startswith("HDUE_HOUDINI_")
                    and name != "HDUE_HOUDINI_ROOT"
                )
                or name
                in (
                    "HFS",
                    "PYTHONHOME",
                    "PYTHONPATH",
                    "HDUE_BACKEND_DLL",
                    "HDUE_RUNTIME_CONFIG",
                )
            )
        }
        try:
            folder.mkdir(parents=True, exist_ok=False)
            job.write_text(
                json.dumps(
                    dict(version=1, materials=records, previews=self.previews),
                    indent=2,
                ),
                encoding="utf-8",
            )
            log = (folder / "worker.log").open("wb")
            process = subprocess.Popen(
                command,
                cwd=self.root,
                stdout=log,
                stderr=subprocess.STDOUT,
                env=environment,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except OSError as error:
            if log:
                log.close()
            self.failures[key] = str(error)
            self.phase = "failed"
            self.message = str(error)
            return
        self.active = dict(
            key=key, process=process, folder=folder, output=output, log=log
        )
        self.phase = "starting"
        self.message = "Starting background Substrate preparation"

    def retry(self):
        self.failures.clear()
        self.changed_at = 0.0

    def close(self):
        self.closed = True
        # Do not interrupt an editor while it is saving/cooking packages. It can
        # finish its immutable output safely after the owning viewport closes.
        if self.active:
            self.active["log"].close()
