"""Starts the in-Houdini test named by HDUE_HOUDINI_TEST (Launch-Houdini.ps1 -TestKind)."""

import importlib
import os

MODULES = {
    "postprocess": "hdue_postprocess_test",
    "volume-material": "hdue_volume_material_test",
    "decal": "hdue_decal_test",
    "dlss": "hdue_dlss_test",
    "substrate-live": "hdue_substrate_live_test",
    "substrate-textures": "hdue_substrate_texture_test",
    "substrate": "hdue_substrate_test",
    "surfaces": "hdue_surface_test",
    "references": "hdue_reference_test",
    "lighting": "hdue_lighting_test",
    "materials": "hdue_materialx_test",
    "materialx-live": "hdue_materialx_live_test",
    "optextures": "hdue_op_texture_test",
    "viewport": "hdue_solaris_probe",
    "selection": "hdue_selection_test",
    "parity": "hdue_parity_test",
    "shared-builds": "hdue_shared_build_test",
    "heavy": "hdue_heavy_test",
    "recreation": "hdue_recreation_test",
}


def capture_logs():
    """Keeps what Houdini's Log Viewer would show (every hou.logging source) and writes it to log-viewer.json and
    log-viewer.txt in the run's log directory when Houdini quits, so a test run shows what it leaves there.
    """
    import atexit
    import json

    import hou

    sink = hou.logging.MemorySink()
    for source in hou.logging.sources():
        sink.connect(source)

    def write():
        folder = os.environ.get("HDUE_HOUDINI_LOG_DIR")
        if not folder:
            return
        entries = list(sink.logEntries())
        hou.logging.saveLogsToFile(
            entries, os.path.join(folder, "log-viewer.json")
        )
        counts = {}
        for entry in entries:
            key = (
                entry.source(),
                entry.severity().name() if entry.severity() else "None",
            )
            counts[key] = counts.get(key, 0) + 1
        lines = [
            f"{n:6} {source} / {severity}"
            for (source, severity), n in sorted(counts.items())
        ]
        lines += [
            "",
            *(
                f"[{e.source()}|{e.severity().name() if e.severity() else None}] {e.message().rstrip()}"
                for e in entries
            ),
        ]
        with open(
            os.path.join(folder, "log-viewer.txt"), "w", encoding="utf-8"
        ) as file:
            file.write("\n".join(lines) + "\n")
        print(
            json.dumps(
                {f"{s} / {v}": n for (s, v), n in sorted(counts.items())}
            )
        )

    # hscript "quit -f", which the tests end with, emits no aboutToQuit.
    atexit.register(write)


def run():
    """Houdini's Solaris desktop first, so a person watching sees the stage, scene graph and LOP network the test
    drives; then the hdUE host (which selects hdUE in that desktop's Scene Viewer) and the test.
    HDUE_TEST_DESKTOP=0 keeps the default desktop.
    """
    import hdefereval
    import hdue_houdini
    import hou
    from PySide6 import QtGui

    # Image checks assume a landscape viewport: Houdini restores its window on whatever monitor it last used, a
    # portrait one included, so maximize it on the widest landscape screen.
    window = hou.qt.mainWindow()
    screen = max(
        QtGui.QGuiApplication.screens(),
        key=lambda s: (
            s.geometry().width() > s.geometry().height(),
            s.geometry().width(),
        ),
    )
    window.showNormal()
    window.setGeometry(screen.availableGeometry())
    window.showMaximized()
    if (
        os.environ.get("HDUE_TEST_DESKTOP") != "0"
        and hou.isUIAvailable()
        and hou.ui.desktop("Solaris")
    ):
        hou.ui.desktop("Solaris").setAsCurrent()
    # Once the desktop has switched, so the host and the test find its Scene Viewer.
    hdefereval.executeDeferred(lambda: start(hdue_houdini.start()))


def start(host):
    test = os.environ["HDUE_HOUDINI_TEST"]
    importlib.import_module(MODULES.get(test, "hdue_houdini_test")).start(host)
