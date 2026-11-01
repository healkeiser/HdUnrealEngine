"""Starts the in-Houdini test named by HDUE_HOUDINI_TEST (Launch-Houdini.ps1 -TestKind)."""

import importlib
import os

MODULES = {
    "postprocess": "hdue_postprocess_test",
    "substrate-live": "hdue_substrate_live_test",
    "substrate-textures": "hdue_substrate_texture_test",
    "substrate": "hdue_substrate_test",
    "surfaces": "hdue_surface_test",
    "references": "hdue_reference_test",
    "lighting": "hdue_lighting_test",
    "materials": "hdue_materialx_test",
    "optextures": "hdue_op_texture_test",
    "viewport": "hdue_solaris_probe",
    "selection": "hdue_selection_test",
    "parity": "hdue_parity_test",
}


def run():
    """Houdini's Solaris desktop first, so a person watching sees the stage, scene graph and LOP network the test
    drives; then the hdUE host (which selects hdUE in that desktop's Scene Viewer) and the test.
    HDUE_TEST_DESKTOP=0 keeps the default desktop.
    """
    import hdefereval
    import hdue_houdini
    import hou

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
