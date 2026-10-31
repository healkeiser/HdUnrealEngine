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


def start(host):
    test = os.environ["HDUE_HOUDINI_TEST"]
    importlib.import_module(MODULES.get(test, "hdue_houdini_test")).start(host)
