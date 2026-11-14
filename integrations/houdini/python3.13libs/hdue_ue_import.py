"""Imports assets from a UE project into the running hdUE runtime, for the hdUE Unreal Asset LOP: cooks the chosen
packages (scripts/Import-UEAssets.ps1, UE's editor in the background), adds the cooked packages to the runtime's
content (never replacing a file it already has, which a running UE may have loaded) and their entries to its asset
catalog. UE then loads them by path when a prim names them, without a restart.
"""

import contextlib
import filecmp
import json
import msvcrt
import os
import shutil
import subprocess
import time
import uuid
from pathlib import Path

from hdue_batch import runtime_config

ROOT = Path(__file__).resolve().parents[3]
# UE's prefixes for what a prim can place (Static Mesh, Skeletal Mesh, Blueprint, Sparse Volume Texture, Niagara System).
PLACEABLE = ("SM_", "SK_", "BP_", "SVT_", "NS_")


def project_assets(project):
    """The placeable assets of a UE project, as cooked object paths: levels, and the assets named with UE's prefixes
    for meshes, Blueprints, Sparse Volume Textures and Niagara Systems (a Blueprint's path is its class, _C).
    """
    content = Path(project).parent / "Content"
    if not content.is_dir():
        return []
    paths = []
    for file in sorted(content.rglob("*")):
        if file.suffix not in (".uasset", ".umap") or (
            file.suffix == ".uasset" and not file.stem.startswith(PLACEABLE)
        ):
            continue
        package = (
            "/Game/" + file.relative_to(content).with_suffix("").as_posix()
        )
        name = file.stem + ("_C" if file.stem.startswith("BP_") else "")
        paths.append(f"{package}.{name}")
    return paths


def asset_menu(project):
    """Houdini menu tokens and labels for project_assets: the path, labelled by its folder and name."""
    return [
        entry
        for path in project_assets(project)
        for entry in (path, path.split(".")[0].removeprefix("/Game/"))
    ]


def imported(path):
    """Whether the running runtime has the cooked package of an object path."""
    try:
        content = Path(runtime_config()["runtime_project"]).parent / "Content"
    except (KeyError, OSError, ValueError):
        return False
    package = path.split(".")[0].removeprefix("/Game/")
    return (content / f"{package}.uasset").is_file() or (
        content / f"{package}.umap"
    ).is_file()


def publish(cooked, windows):
    """Copies the cooked files (cooked/Windows/...) into the runtime (its Windows folder) where it has none: a file
    the runtime already has stays, as a running UE may have loaded it. Returns the project packages that differ from
    the runtime's copy (they render as the runtime has them until hdUE restarts with a newly prepared runtime).
    """
    stale = []
    for source in sorted(cooked.rglob("*")):
        if not source.is_file():
            continue
        relative = source.relative_to(cooked)
        target = windows / relative
        if target.exists():
            if (
                relative.parts[:2] == ("HdUEBackend", "Content")
                and relative.parts[2]
                != "HdUE"  # hdUE's own content, cooked again with each import
                and source.suffix in (".uasset", ".umap")
                and not filecmp.cmp(source, target, shallow=False)
            ):
                stale.append(relative.as_posix())
            continue
        if (
            len(relative.parts) < 3
        ):  # the cook's registry and metadata: the runtime keeps its own
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(target.name + ".partial")
        shutil.copyfile(source, partial)
        os.replace(partial, target)  # a reader never sees half a package
    return stale


REPLACE_ATTEMPTS = 6


def _write_atomic(path, text):
    # Readers (husk, the delegate, Update-HoudiniRuntime.ps1) must never see half a file.
    # A name of its own per writer, so two writers never share a pending file.
    pending = path.with_name(
        f"{path.name}.{os.getpid()}.{uuid.uuid4().hex}.pending"
    )
    pending.write_text(text, encoding="utf-8")
    try:
        for attempt in range(REPLACE_ATTEMPTS):
            try:
                os.replace(pending, path)
                return
            except PermissionError:
                # A reader that opened the file without delete sharing; it lets go in a moment.
                if attempt == REPLACE_ATTEMPTS - 1:
                    raise
                time.sleep(0.05 * (attempt + 1))
    finally:
        pending.unlink(missing_ok=True)


@contextlib.contextmanager
def _locked(config_path):
    """Serialises imports' read-modify-write of the catalog and the profile (a lock file next to the profile)."""
    with open(config_path.with_name(config_path.name + ".lock"), "a+b") as lock:
        lock.seek(0)
        # LK_LOCK retries for about 10 s, then raises OSError.
        msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def merge_catalog(catalog, config_path, config):
    """Adds the import's catalog entries to the runtime's asset catalog (made, and named in the profile, if it has
    none), so the Native Asset menus and the bounds proxies know them. The profile and the catalog are re-read and
    rewritten atomically under a lock, so a concurrent import keeps its entries and a starting husk reads whole files.
    """
    added = json.loads(Path(catalog).read_text(encoding="utf-8-sig"))["assets"]
    with _locked(config_path):
        profile = json.loads(config_path.read_text(encoding="utf-8-sig"))
        path = profile.get("asset_catalog")
        if path:
            # Relative paths resolve against the profile's folder (runtime_config, the delegate).
            target = (config_path.parent / path).resolve()
            current = json.loads(target.read_text(encoding="utf-8-sig"))
        else:
            target = (
                Path(config["runtime_project"]).parent / "hdue_catalog.json"
            )
            # Another profile may share this runtime project and have made the catalog already.
            current = (
                json.loads(target.read_text(encoding="utf-8-sig"))
                if target.is_file()
                else {"version": 1, "assets": {}}
            )
        current["assets"] = {**added, **current["assets"]}
        _write_atomic(target, json.dumps(current, indent=1))
        if not path:
            try:
                # Packages write profiles with relative paths; a moved package must keep working.
                value = os.path.relpath(target, config_path.parent)
            except ValueError:  # another drive
                value = str(target)
            profile["asset_catalog"] = value
            _write_atomic(config_path, json.dumps(profile, indent=2))


# One cook took about a minute; a hung editor must not hang Houdini forever.
COOK_TIMEOUT_SECONDS = 1800
PROJECT_DIR = ROOT / "unreal/HdUEBackend"
CONTENT_EDITOR = PROJECT_DIR / "Binaries/Win64/UnrealEditor-HdUEContent.dll"
# Both modules, the targets and the .Build.cs files: any of them changes what the editor needs to be.
SOURCE_ROOT = PROJECT_DIR / "Source"
PROJECT_FILE = PROJECT_DIR / "HdUEBackend.uproject"
# The project's plugins (DLSS, Streamline) build into the same editor.
PLUGINS_ROOT = PROJECT_DIR / "Plugins"


def _build_id(modules_file):
    try:
        return json.loads(Path(modules_file).read_text(encoding="utf-8-sig"))[
            "BuildId"
        ]
    except (OSError, ValueError, KeyError, TypeError):
        return None


def _engine_modules_file():
    """The engine's UnrealEditor.modules (its BuildId), or None when the editor root is not configured."""
    from local_paths import local_path

    try:
        root = local_path("editor_root")
    except SystemExit:
        return None
    return root / "Engine/Binaries/Win64/UnrealEditor.modules"


def editor_is_current():
    """Whether the project's editor build can be skipped: its DLL exists, nothing under Source (either module,
    *.Target.cs, *.Build.cs), the project's plugins or the .uproject is newer, and it was built by the engine now
    configured (same BuildId).
    """
    if not CONTENT_EDITOR.is_file():
        return False
    built = CONTENT_EDITOR.stat().st_mtime
    sources = [PROJECT_FILE, *SOURCE_ROOT.rglob("*")]
    for folder, children, files in os.walk(PLUGINS_ROOT):
        # A plugin's build output and content change with every build or import, not what the editor needs to be.
        children[:] = [
            c
            for c in children
            if c not in ("Binaries", "Intermediate", "Saved", "Content")
        ]
        sources += [Path(folder) / f for f in files]
    if any(f.is_file() and f.stat().st_mtime > built for f in sources):
        return False
    engine = _engine_modules_file()
    if engine is None or not engine.is_file():
        return True  # nothing to compare with; the build itself reports a missing engine
    project = _build_id(CONTENT_EDITOR.with_name("UnrealEditor.modules"))
    return project is not None and project == _build_id(engine)


def _kill_tree(process):
    """Ends the cook and its children: kill() alone ends only PowerShell, and Build.bat/UBT or the editor would run on."""
    try:
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        pass  # taskkill is missing or hung: kill() below still ends PowerShell
    process.kill()
    process.wait()


def import_assets(project, paths, output=None, progress=None):
    """Cooks paths (cooked object paths) from project and publishes them into the running runtime. Returns the
    packages the runtime keeps an older copy of. Raises RuntimeError with the log's path when the cook fails.
    progress is called about every 100 ms while the cook runs: an exception it raises (Houdini's Esc) kills the cook.
    """
    config_path = Path(os.environ["HDUE_RUNTIME_CONFIG"]).resolve()
    config = runtime_config()
    run = Path(
        output or ROOT / "out" / "imports" / Path(project).stem
    ).resolve()
    run.mkdir(parents=True, exist_ok=True)
    packages = sorted({p.split(".")[0] for p in paths})
    log = run / "import.log"
    from powershell_lookup import powershell_executable

    powershell = powershell_executable()
    # The scripts use PowerShell 7 APIs; the lookup may fall back to Windows PowerShell 5.1.
    if not powershell or Path(powershell).stem.lower() != "pwsh":
        raise RuntimeError(
            "PowerShell 7 (pwsh) is required to import UE assets; Windows PowerShell 5.1 cannot run the import scripts"
        )
    command = [
        powershell,
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(ROOT / "scripts/Import-UEAssets.ps1"),
        "-ProjectPath",
        str(project),
        "-OutputDirectory",
        str(run),
        "-Packages",
        ",".join(packages),
    ]
    if editor_is_current():
        command.append("-SkipBuild")
    # A log file, not a pipe: a pipe nobody reads fills up and hangs the child.
    with log.open("w", encoding="utf-8") as output_file:
        process = subprocess.Popen(
            command, stdout=output_file, stderr=subprocess.STDOUT
        )
        try:
            deadline = time.monotonic() + COOK_TIMEOUT_SECONDS
            while process.poll() is None:
                if time.monotonic() > deadline:
                    raise TimeoutError(
                        f"Importing from {project} took longer than {COOK_TIMEOUT_SECONDS} s; log: {log}"
                    )
                if progress:
                    progress()
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                _kill_tree(process)
    if process.returncode:
        raise RuntimeError(
            f"Importing from {project} failed (log: {log}):\n"
            + log.read_text(encoding="utf-8", errors="replace")[-2000:]
        )
    windows = Path(config["runtime_project"]).parent.parent
    stale = publish(run / "cooked" / "Windows", windows)
    merge_catalog(run / "catalog.json", config_path, config)
    return stale


def sync_node(node):
    """The hdUE Unreal Asset LOP's Imported toggle, from whether the running runtime has its asset (Asset callback)."""
    node.parm("imported").set(imported(node.evalParm("asset")))


def import_from_node(node):
    """The hdUE Unreal Asset LOP's Import button: cooks its asset from its project into the running runtime (UE's
    editor in the background, about a minute for one effect), then turns on Imported so the prim names it.
    """
    import hou

    project, asset = node.evalParm("project"), node.evalParm("asset")
    if not project or not asset:
        raise hou.NodeError("Choose a UE project and one of its assets first.")
    if not imported(asset):
        with hou.InterruptableOperation(
            f"Importing {asset.split('.')[-1]} from {Path(project).stem}",
            open_interrupt_dialog=True,
        ) as operation:
            stale = import_assets(
                project, [asset], progress=operation.updateProgress
            )
        if stale and hou.isUIAvailable():
            hou.ui.displayMessage(
                f"{len(stale)} package(s) of this project changed since hdUE last cooked them; they render as before "
                "until Houdini restarts with a newly prepared runtime (Prepare-ProjectAssets.ps1).",
                severity=hou.severityType.Warning,
            )
    node.parm("imported").set(True)
