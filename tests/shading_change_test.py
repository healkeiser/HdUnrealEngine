"""Run with uv run -- <HFS>/bin/hython.exe tests/shading_change_test.py: which stage edits restart the Houdini host's
walks (material preparation, op: textures, asset overrides), and the cooperative jobs that run them.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
sys.path.insert(0, str(ROOT / "scripts"))
from cooperative import Job, run
from hdue_houdini import ALL_CHANGES, CHANGE_SCAN_LIMIT, _changes
from pxr import Sdf, Usd, UsdGeom, UsdLux, UsdShade

stage = Usd.Stage.CreateInMemory()
UsdGeom.Camera.Define(stage, "/cameras/orbit")
UsdGeom.Mesh.Define(stage, "/geo/mesh")
material = UsdShade.Material.Define(stage, "/materials/m")
UsdShade.Shader.Define(stage, "/materials/m/surface")
UsdLux.DomeLight.Define(stage, "/lights/dome")
UsdLux.SphereLight.Define(stage, "/lights/key")
UsdGeom.Xform.Define(stage, "/props/chair")
UsdGeom.Mesh.Define(stage, "/props/chair/seat")


def info(path, fields=()):
    return _changes(stage, Sdf.Path(path), set(fields))


def resync(path):
    return _changes(stage, Sdf.Path(path), None)


# Houdini's viewport camera on every camera move, playback, render settings: nothing.
assert info("/__HoudiniFreeCamera__.xformOp:transform") == set()
assert info("/cameras/orbit.xformOp:rotateXYZ") == set()
assert info("/geo/mesh.xformOp:translate") == set()
assert info("/Render/Settings.unreal:renderMode") == set()
assert resync("/Render") == set()
# A light edit restarts no material preparation (it froze Houdini's UI on NVIDIA's Da Vinci Workshop for 45 s);
# only a light's image can change the op: textures.
assert info("/lights/key.inputs:exposure") == set()
assert info("/lights/key.unreal:light:VolumetricScatteringIntensity") == set()
assert info("/lights/dome.inputs:texture:file") == {"textures"}
# A new light, or a new prop without shading, restarts nothing beyond the op: textures a light may read.
assert resync("/lights/key") == {"textures"}
assert resync("/props/chair") == set()
# Shading edits restart material preparation (and the op: textures, which shaders read).
assert info("/materials/m/surface.inputs:roughness") == {
    "materials",
    "textures",
}
assert info("/geo/mesh.material:binding") == {"materials", "assets"}
assert info("/geo/mesh.unreal:surface:wet:amount") == {"materials", "assets"}
assert info("/geo/mesh.unreal:asset") == {"assets"}
assert resync("/materials") == ALL_CHANGES
# Instancing and root metadata (units, up axis) change the asset overrides.
assert info("/props/chair", ["instanceable"]) == {"assets"}
assert info("/", ["metersPerUnit"]) == {"assets"}
# A removed prim, or a subtree too large to read on a notice, changes everything.
assert resync("/removed") == ALL_CHANGES
for index in range(CHANGE_SCAN_LIMIT + 1):
    UsdGeom.Xform.Define(stage, f"/big/x{index}")
assert resync("/big") == ALL_CHANGES


# Cooperative jobs: run() completes a walk; Job.advance() stops at its budget and resumes.
def walk(count):
    total = 0
    for value in range(count):
        yield
        total += value
    return total


assert run(walk(10)) == 45
job = Job(walk(100000), "test")
assert not job.advance(0.0)  # one step, then the budget is spent
assert job.units == 1
while not job.advance(0.001):
    pass
assert job.done and job.result == sum(range(100000))


def failing():
    yield
    raise ValueError("scene error")


job = Job(failing())
try:
    while not job.advance(1):
        pass
    raise AssertionError("the walk's error must propagate")
except ValueError:
    assert job.done
print("shading changes: ok")
