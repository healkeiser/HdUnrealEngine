"""The Unreal main menu (MainMenuCommon.xml): viewport and render settings shortcuts, UE's logs, and hdUE's disk caches.

The cache paths are resolved here only, from an environment dictionary, so tests check them without Houdini; hou is
imported inside the menu actions for the same reason.
"""

import ctypes
import os
import shutil
import traceback
import webbrowser
from pathlib import Path

DOCS = "https://healkeiser.github.io/HdUnrealEngine"
CACHE_LABELS = {
    "texture": "Texture cache",
    "virtual_texture": "Virtual texture cache",
    "nanite": "Nanite cache",
    "ocio": "OCIO texture cache",
}


def saved_directory(env, runtime_project=None):
    """UE's Saved folder, as the delegate passes -userdir (src/client/backend.cpp): HDUE_USER_DIR, else
    %LOCALAPPDATA%/hdUE; without either, next to the runtime project, as FPaths::ProjectSavedDir resolves it
    (ProjectUserDir() + Saved, Paths.cpp). None without any.
    """
    if env.get("HDUE_USER_DIR"):
        return Path(env["HDUE_USER_DIR"]) / "Saved"
    if env.get("LOCALAPPDATA"):
        return Path(env["LOCALAPPDATA"]) / "hdUE" / "Saved"
    if runtime_project:
        return Path(runtime_project).parent / "Saved"
    return None


def cache_directories(env, runtime_project=None):
    """{name: folder} of hdUE's own disk caches, as the code that writes them resolves each one:

    - texture: CachePath in Scene.cpp, %LOCALAPPDATA%/hdUE/TextureCache.
    - virtual_texture: CacheDirectory in VirtualTextureBuild.cpp, HDUE_VIRTUAL_TEXTURE_CACHE, else
      %LOCALAPPDATA%/hdUE/VirtualTextureCache.
    - nanite: BuildNanite in Scene.cpp, UE's Saved/NaniteCache.
    - ocio: color_management.cpp, std::filesystem::temp_directory_path()/hdue_ocio. MSVC's reads GetTempPath2W,
      which tries TMP, then TEMP, then USERPROFILE.

    A cache whose base is unknown (no LOCALAPPDATA, no runtime profile) is left out rather than guessed.
    """
    folders = {}
    local = env.get("LOCALAPPDATA")
    if local:
        folders["texture"] = Path(local) / "hdUE" / "TextureCache"
    virtual = env.get("HDUE_VIRTUAL_TEXTURE_CACHE")
    if virtual or local:
        folders["virtual_texture"] = (
            Path(virtual)
            if virtual
            else Path(local) / "hdUE" / "VirtualTextureCache"
        )
    saved = saved_directory(env, runtime_project)
    if saved:
        folders["nanite"] = saved / "NaniteCache"
    temp = env.get("TMP") or env.get("TEMP") or env.get("USERPROFILE")
    if temp:
        folders["ocio"] = Path(temp) / "hdue_ocio"
    return folders


def folder_size(folder):
    """(bytes, files) under folder; 0, 0 for a missing one. Files that vanish or deny access while walking are skipped."""
    total = count = 0
    for root, _, files in os.walk(folder):
        for name in files:
            try:
                total += os.stat(os.path.join(root, name)).st_size
            except OSError:
                continue
            count += 1
    return total, count


def format_size(size):
    if size < 1024:
        return f"{size} bytes"
    for unit in ("KB", "MB", "GB"):
        size /= 1024
        if size < 1024 or unit == "GB":
            return f"{size:.1f} {unit}"


def clear_folder(folder):
    """Delete everything inside folder and keep the folder. Returns the paths that could not be deleted (files another
    process still holds open: Windows refuses to delete them).
    """
    folder = Path(folder)
    # HDUE_VIRTUAL_TEXTURE_CACHE is user input: never empty a drive root or the home folder it might name by mistake.
    if folder.resolve() in (
        Path(folder.anchor).resolve(),
        Path.home().resolve(),
    ):
        raise ValueError(f"Refusing to clear {folder}")
    failed = []
    if not folder.is_dir():
        return failed
    for entry in folder.iterdir():
        try:
            if entry.is_dir() and not entry.is_symlink():
                # onexc (Python 3.12+; Houdini 22 runs 3.13): keep going past held files.
                shutil.rmtree(
                    entry, onexc=lambda _, path, __: failed.append(path)
                )
            else:
                entry.unlink()
        except OSError:
            failed.append(str(entry))
    return failed


def _runtime_project():
    if not os.environ.get("HDUE_RUNTIME_CONFIG"):
        return None
    from hdue_batch import runtime_config

    return runtime_config()["runtime_project"]


def _caches():
    return cache_directories(os.environ, _runtime_project())


def ue_loaded():
    """Whether this Houdini loaded hdUE's backend. It stays loaded, and UE with it, until Houdini exits (UE runs once
    per process): from then on it streams virtual texture chunks from their files and writes the other caches.
    """
    name = os.environ.get("HDUE_BACKEND_DLL")
    if not name and os.environ.get("HDUE_RUNTIME_CONFIG"):
        from hdue_batch import runtime_config

        name = runtime_config()["backend_dll"]
    if not name:
        return False
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel.GetModuleHandleW.restype = (
        ctypes.c_void_p
    )  # an HMODULE: an int would truncate it
    return bool(kernel.GetModuleHandleW(Path(name).name))


def run(action, *args):
    """Menu entry point: runs the action after the menu closes, since dialogs opened from a menu item can freeze it
    (Customize menus, "Deferring execution of <scriptCode>"), and shows failures in a dialog.
    """
    import hdefereval
    import hou

    def call():
        try:
            globals()[action](*args)
        except Exception as error:
            hou.ui.displayMessage(
                str(error) or type(error).__name__,
                severity=hou.severityType.Error,
                title="Unreal",
                details=traceback.format_exc(),
            )

    hdefereval.executeDeferred(call)


def use_viewport():
    import hdue_houdini

    hdue_houdini.activate()


def set_mode(mode):
    import hdue_houdini

    hdue_houdini.set_mode(mode)


def create_render_settings():
    """An hdUE Render Settings LOP after the Scene Viewer's displayed LOP, as hdue_houdini.set_render_setting adds one."""
    import hdue_houdini

    display = hdue_houdini.viewer_display(hdue_houdini.scene_viewer())
    if not display:
        raise RuntimeError("Display a LOP network in the Scene Viewer")
    node = hdue_houdini.render_settings_node(display)
    node.setDisplayFlag(True)
    node.setCurrent(True, clear_all_selected=True)


def open_folder(folder):
    folder = Path(folder)
    if not folder.is_dir():
        raise RuntimeError(f"{folder} does not exist yet")
    os.startfile(folder)


def open_logs():
    saved = saved_directory(os.environ, _runtime_project())
    if not saved:
        raise RuntimeError("HDUE_RUNTIME_CONFIG is not set")
    open_folder(saved / "Logs")


def docs_url(env):
    """The Houdini pages of the guide for this build: the site keeps one folder per minor version (mike), the
    package sets HDUE_VERSION (v0.5.0 -> 0.5); a development build gets the latest release's.
    """
    version = env.get("HDUE_VERSION", "").lstrip("v").split(".")
    folder = (
        ".".join(version[:2])
        if len(version) >= 2 and all(v.isdigit() for v in version[:2])
        else "latest"
    )
    return f"{DOCS}/{folder}/houdini/"


def open_docs():
    webbrowser.open(docs_url(os.environ))


def open_cache(name):
    open_folder(_caches()[name])


def show_cache_sizes():
    import hou

    lines = []
    for name, folder in _caches().items():
        size, files = folder_size(folder)
        lines.append(
            f"{CACHE_LABELS[name]}: {format_size(size)}, {files} files\n    {folder}"
        )
    hou.ui.displayMessage("\n".join(lines), title="hdUE caches")


def clear_caches(*names):
    """Ask, then empty the named caches (all of them without names). Refused once UE runs in this Houdini: it reads
    the caches' files for as long as it runs.
    """
    import hou

    caches = _caches()
    chosen = {name: caches[name] for name in names or caches if name in caches}
    if ue_loaded():
        hou.ui.displayMessage(
            "Unreal Engine is running in this Houdini session, and it reads hdUE's caches while it runs "
            "(virtual textures stream from their cache files). Restart Houdini and clear the caches before "
            "selecting Unreal Engine in a viewport.",
            severity=hou.severityType.Warning,
            title="hdUE caches",
        )
        return
    lines = []
    for name, folder in chosen.items():
        size, files = folder_size(folder)
        lines.append(
            f"{CACHE_LABELS[name]}: {format_size(size)}, {files} files\n    {folder}"
        )
    if not hou.ui.displayConfirmation(
        "Delete the contents of these folders? This cannot be undone; hdUE rebuilds them on the next renders.\n\n"
        + "\n".join(lines),
        severity=hou.severityType.Warning,
        title="Clear hdUE caches",
        suppress=hou.confirmType.NoConfirmType,
    ):
        return
    failed = [
        path for folder in chosen.values() for path in clear_folder(folder)
    ]
    if failed:
        hou.ui.displayMessage(
            f"{len(failed)} files are in use by another process (husk or another Houdini) and were kept.",
            severity=hou.severityType.Warning,
            title="hdUE caches",
            details="\n".join(map(str, failed)),
        )
