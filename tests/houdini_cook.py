"""Run with uv run -- <HFS>/bin/hython.exe tests/houdini_cook.py."""

import json
import os
import sys
import tempfile
from pathlib import Path

import hou
from pxr import UsdGeom, UsdRender

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "integrations/houdini/python3.13libs"))
_runtime_directory = tempfile.TemporaryDirectory(prefix="hdue-cook-test-")
_runtime_config = Path(_runtime_directory.name) / "runtime.json"
_runtime_config.write_text(json.dumps({"version": 1}), encoding="utf-8")
os.environ["HDUE_RUNTIME_CONFIG"] = str(_runtime_config)
import hdue_houdini as integration

ASSET = "/Game/HdUE/SM_Nanite.SM_Nanite"
network = hou.node("/stage")
source = network.createNode("pythonscript", "test_source")
source.parm("python").set("""from pxr import UsdGeom, Sdf
stage=hou.pwd().editableStage()
prim=UsdGeom.Xform.Define(stage,'/Native').GetPrim()
prim.CreateAttribute('unreal:asset',Sdf.ValueTypeNames.String).Set('%s')
parent=UsdGeom.Xform.Define(stage,'/Parent')
parent.AddTranslateOp().Set((1,2,3)); parent.AddScaleOp().Set((2,2,2))
mesh=UsdGeom.Mesh.Define(stage,'/Parent/MeshAsset')
mesh.CreatePointsAttr([(0,0,0),(1,0,0),(0,1,0)])
mesh.CreateFaceVertexCountsAttr([3]); mesh.CreateFaceVertexIndicesAttr([0,1,2])
mesh.GetPrim().CreateAttribute('unreal:asset',Sdf.ValueTypeNames.String).Set('%s')
asset=stage.DefinePrim('/Prototype/Asset','Xform')
asset.CreateAttribute('unreal:asset',Sdf.ValueTypeNames.String).Set('%s')
instance=stage.DefinePrim('/Instance','Xform')
instance.GetReferences().AddInternalReference('/Prototype'); instance.SetInstanceable(True)
""" % (ASSET, ASSET, ASSET))
before = source.stage().GetRootLayer().ExportToString()
# A Z-up centimetre input, as a scene file would bring it: a standard Configure Layer LOP.
units = network.createNode("configurelayer", "z_up_centimeters")
units.setInput(0, source)
for name, value in {"metersperunit": 0.01, "upaxis": "z"}.items():
    units.parm("set" + name).set(1)
    units.parm(name).set(value)

# hdUE_settings passes the input stage through: no Python LOPs, no normalization, no generated prims.
node = integration.settings_node(units)
stage = node.stage()
assert not node.errors(), node.errors()
assert not [
    c.path() for c in node.allSubChildren() if c.type().name() == "pythonscript"
]
assert (
    UsdGeom.GetStageMetersPerUnit(stage) == 0.01
    and UsdGeom.GetStageUpAxis(stage) == "Z"
)
assert stage.GetPrimAtPath(
    "/Instance"
).IsInstance(), "native instancing must reach Hydra"
# The delegate makes native assets' bounds meshes (HdUE_NativeAssetSceneIndex): none in the stage.
assert not stage.GetPrimAtPath("/Native/__HdUEAssetProxy")
assert not stage.GetPrimAtPath(
    "/Render/rendersettings"
), "hdUE settings must leave render settings to Render Settings LOPs"
assert integration.render_options(stage) == ("realtime", 64)
render = integration.render_settings_node(node)
assert not render.errors(), render.errors()
settings = render.stage().GetPrimAtPath("/Render/rendersettings")
assert (
    render.stage().GetMetadata("renderSettingsPrimPath")
    == "/Render/rendersettings"
)
assert integration.render_options(render.stage()) == ("realtime", 64)
assert not settings.GetAttribute(
    "unreal:lumenEnabled"
).HasAuthoredValue(), "settings at their defaults must not author"
assert not [
    p.name() for p in render.parms() if "_control" in p.name()
], "no USD control menus, as Karma Render Settings"
render.parm(hou.text.encode("unreal:lumenEnabled")).set(0)
assert (
    render.stage()
    .GetPrimAtPath("/Render/rendersettings")
    .GetAttribute("unreal:lumenEnabled")
    .Get()
    is False
), "a changed setting authors"
render.parm(hou.text.encode("unreal:lumenEnabled")).set(1)
integration._render_property(render, "xn__unrealrenderMode", "pathtraced")
assert integration.render_options(render.stage()) == ("pathtraced", 64)

# A scene with its own render settings: the node's prim becomes the default, with the scene's camera and hdUE values.
authored = Path(tempfile.mkdtemp()) / "authored_settings.usda"
authored.write_text(
    '#usda 1.0\ndef Camera "other" {}\ndef Camera "shot" {}\ndef Scope "Render" {\n'
    '    def RenderSettings "Settings" {\n        rel camera = </shot>\n        token unreal:renderMode = "pathtraced"\n'
    "        int unreal:samplesPerPixel = 256\n    }\n}\n",
    encoding="utf-8",
)
authored_source = network.createNode("sublayer")
authored_source.parm("filepath1").set(str(authored))
edited = integration.render_settings_node(authored_source)
assert not edited.errors(), edited.errors()
assert (
    edited.stage().GetMetadata("renderSettingsPrimPath")
    == "/Render/rendersettings"
)
assert integration.render_options(edited.stage()) == ("pathtraced", 256)
assert [
    str(t)
    for t in UsdRender.Settings(
        edited.stage().GetPrimAtPath("/Render/rendersettings")
    )
    .GetCameraRel()
    .GetForwardedTargets()
] == ["/shot"]
edited.parm(hou.text.encode("unreal:renderMode")).set("realtime")
assert integration.render_options(edited.stage()) == (
    "realtime",
    64,
), "the default value renders, not the scene prim (samples apply to the Path Tracer only)"

# Native assets need no viewport overrides: the delegate makes their bounds meshes (HdUE_NativeAssetSceneIndex), and
# assets in instances stay instanced, as in husk.
assets = integration.NativeAssets()
assets.sync(network, render, render.stage())
assert not assets.sync(
    network, render, render.stage()
), "unchanged stage must not re-author"


def overrides():
    with network.viewportOverrides(render) as edit:
        return edit.layer()


def instanceable_override(layer):
    spec = layer.GetPrimAtPath("/Instance")
    return bool(spec) and spec.HasInfo("instanceable")


layer = overrides()
for path in ("/Native", "/Parent/MeshAsset", "/Instance/Asset"):
    assert not layer.GetPrimAtPath(path + "/__HdUEAssetProxy"), path
assert not instanceable_override(layer), "an instance around an asset stays one"
assert (
    render.stage().GetPrimAtPath("/Instance").IsInstance()
), "the graph keeps its instancing"

# MaterialX inside an instance needs its stage path only with a cooked library.
look = network.createNode("pythonscript", "instanced_materialx")
look.setInput(0, render)
look.parm("python").set("""from pxr import Sdf, UsdShade
stage=hou.pwd().editableStage()
material=UsdShade.Material.Define(stage,'/Prototype/Asset/Looks/Paint')
shader=UsdShade.Shader.Define(stage,'/Prototype/Asset/Looks/Paint/Surface'); shader.CreateIdAttr('ND_standard_surface_surfaceshader')
material.CreateSurfaceOutput('mtlx').ConnectToSource(shader.CreateOutput('out',Sdf.ValueTypeNames.Token))
stage.OverridePrim('/Native').SetActive(False)
stage.OverridePrim('/Parent').SetActive(False)
stage.OverridePrim('/Prototype/Asset').GetAttribute('unreal:asset').Set('')""")
assert not look.errors(), look.errors()
assets.sync(network, look, look.stage())
assert not instanceable_override(overrides())
assert assets.sync(network, look, look.stage(), materials=True)
assert not overrides().GetPrimAtPath("/Instance").GetInfo("instanceable")
assert not assets.sync(
    network,
    look,
    look.stage(apply_viewport_overrides=True),
    materials=True,
), "the viewed stage, with the instance made editable, must not re-author (it alternated on every tick)"
assert not overrides().GetPrimAtPath("/Instance").GetInfo("instanceable")
assets.clear()
assert not instanceable_override(overrides())
assert source.stage().GetRootLayer().ExportToString() == before
print(
    "PASS: pass-through hdUE settings, render settings, instance overrides and source preservation"
)
