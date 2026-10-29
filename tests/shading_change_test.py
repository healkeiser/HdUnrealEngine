"""Run with uv run -- <HFS>/bin/hython.exe tests/shading_change_test.py: which stage edits re-prepare hdUE materials."""

import sys
from pathlib import Path

sys.path.insert(
    0,
    str(
        Path(__file__).resolve().parents[1]
        / "integrations/houdini/python3.13libs"
    ),
)
from hdue_houdini import _shading_change
from pxr import Sdf, Usd, UsdGeom, UsdLux, UsdShade

stage = Usd.Stage.CreateInMemory()
UsdGeom.Camera.Define(stage, "/cameras/orbit")
UsdGeom.Mesh.Define(stage, "/geo/mesh")
UsdShade.Shader.Define(stage, "/materials/m/surface")
UsdLux.DomeLight.Define(stage, "/lights/dome")
changed = lambda path, info=True: _shading_change(stage, Sdf.Path(path), info)
assert not changed(
    "/__HoudiniFreeCamera__.xformOp:transform"
)  # Houdini's viewport camera, on every camera move
assert not changed("/cameras/orbit.xformOp:rotateXYZ") and not changed(
    "/geo/mesh.xformOp:translate"
)  # playback
assert not changed("/Render/Settings.hdUE:renderMode") and not changed(
    "/Render", False
)
assert changed("/materials/m/surface.inputs:roughness") and changed(
    "/lights/dome.inputs:texture:file"
)
assert changed("/geo/mesh.material:binding") and changed(
    "/geo/mesh.hdue:surface:wet:amount"
)
assert changed("/geo/new", False)  # structure changes always count
print("shading changes: ok")
