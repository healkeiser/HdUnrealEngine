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


def _write_atomic(path, text):
    # Readers (husk, the delegate, Update-HoudiniRuntime.ps1) must never see half a file.
    pending = path.with_name(path.name + ".pending")
    pending.write_text(text, encoding="utf-8")
    os.replace(pending, path)


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
            current = {"version": 1, "assets": {}}
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
CONTENT_EDITOR = (
    ROOT / "unreal/HdUEBackend/Binaries/Win64/UnrealEditor-HdUEContent.dll"
)
CONTENT_SOURCE = ROOT / "unreal/HdUEBackend/Source/HdUEContent"


def editor_is_current():
    # The editor is rebuilt only when its source is newer.
    if not CONTENT_EDITOR.is_file():
        return False
    built = CONTENT_EDITOR.stat().st_mtime
    return all(
        file.stat().st_mtime <= built
        for file in CONTENT_SOURCE.rglob("*")
        if file.is_file()
    )


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
    command = [
        shutil.which("pwsh") or "pwsh",  # PowerShell 7, as the build scripts
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
                process.kill()
                process.wait()
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
