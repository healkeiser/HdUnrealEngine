"""Run with uv run -- <HFS>/bin/hython.exe tests/houdini_cook.py."""
import os
from pathlib import Path
import sys
import tempfile
import json
import hou
from pxr import Sdf,UsdGeom,UsdRender

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'integrations/houdini/python3.13libs'))
_runtime_directory=tempfile.TemporaryDirectory(prefix='hdue-cook-test-')
_runtime_config=Path(_runtime_directory.name)/'runtime.json'
_runtime_config.write_text(json.dumps({'version':1}),encoding='utf-8')
os.environ['HDUE_RUNTIME_CONFIG']=str(_runtime_config)
import hdue_houdini as integration

ASSET='/Game/HdUE/SM_Nanite.SM_Nanite'
network=hou.node('/stage')
source=network.createNode('pythonscript','test_source')
source.parm('python').set('''from pxr import UsdGeom, Sdf
stage=hou.pwd().editableStage()
prim=UsdGeom.Xform.Define(stage,'/Native').GetPrim()
prim.CreateAttribute('hdue:asset',Sdf.ValueTypeNames.String).Set('%s')
parent=UsdGeom.Xform.Define(stage,'/Parent')
parent.AddTranslateOp().Set((1,2,3)); parent.AddScaleOp().Set((2,2,2))
mesh=UsdGeom.Mesh.Define(stage,'/Parent/MeshAsset')
mesh.CreatePointsAttr([(0,0,0),(1,0,0),(0,1,0)])
mesh.CreateFaceVertexCountsAttr([3]); mesh.CreateFaceVertexIndicesAttr([0,1,2])
mesh.GetPrim().CreateAttribute('hdue:asset',Sdf.ValueTypeNames.String).Set('%s')
asset=stage.DefinePrim('/Prototype/Asset','Xform')
asset.CreateAttribute('hdue:asset',Sdf.ValueTypeNames.String).Set('%s')
instance=stage.DefinePrim('/Instance','Xform')
instance.GetReferences().AddInternalReference('/Prototype'); instance.SetInstanceable(True)
''' % (ASSET,ASSET,ASSET))
before=source.stage().GetRootLayer().ExportToString()
# A Z-up centimetre input, as a scene file would bring it: a standard Configure Layer LOP.
units=network.createNode('configurelayer','z_up_centimeters'); units.setInput(0,source)
for name,value in {'metersperunit':.01,'upaxis':'z'}.items():
    units.parm('set'+name).set(1); units.parm(name).set(value)

# hdUE_settings passes the input stage through: no Python LOPs, no normalization, no generated prims.
node=integration.settings_node(units)
stage=node.stage()
assert not node.errors(),node.errors()
assert not [c.path() for c in node.allSubChildren() if c.type().name()=='pythonscript']
assert UsdGeom.GetStageMetersPerUnit(stage)==.01 and UsdGeom.GetStageUpAxis(stage)=='Z'
assert stage.GetPrimAtPath('/Instance').IsInstance(),'native instancing must reach Hydra'
assert not stage.GetPrimAtPath('/Native/'+integration.NativeAssets.PROXY)
assert not stage.GetPrimAtPath('/Render/rendersettings'),'hdUE settings must leave render settings to Render Settings LOPs'
assert integration.render_options(stage)==('realtime',64)
render=integration.render_settings_node(node)
assert not render.errors(),render.errors()
settings=render.stage().GetPrimAtPath('/Render/rendersettings')
assert render.stage().GetMetadata('renderSettingsPrimPath')=='/Render/rendersettings'
assert integration.render_options(render.stage())==('realtime',64)
assert not settings.GetAttribute('hdUE:lumenEnabled').HasAuthoredValue(),'settings at their defaults must not author'
assert not [p.name() for p in render.parms() if '_control' in p.name()],'no USD control menus, as Karma Render Settings'
render.parm(hou.text.encode('hdUE:lumenEnabled')).set(0)
assert render.stage().GetPrimAtPath('/Render/rendersettings').GetAttribute('hdUE:lumenEnabled').Get() is False,'a changed setting authors'
render.parm(hou.text.encode('hdUE:lumenEnabled')).set(1)
integration._render_property(render,'xn__hdUErenderMode','pathtraced')
assert integration.render_options(render.stage())==('pathtraced',64)

# A scene with its own render settings: the node's prim becomes the default, with the scene's camera and hdUE values.
authored=Path(tempfile.mkdtemp())/'authored_settings.usda'
authored.write_text('#usda 1.0\ndef Camera "other" {}\ndef Camera "shot" {}\ndef Scope "Render" {\n'
    '    def RenderSettings "Settings" {\n        rel camera = </shot>\n        token hdUE:renderMode = "pathtraced"\n'
    '        int hdUE:samplesPerPixel = 256\n    }\n}\n',encoding='utf-8')
authored_source=network.createNode('sublayer'); authored_source.parm('filepath1').set(str(authored))
edited=integration.render_settings_node(authored_source)
assert not edited.errors(),edited.errors()
assert edited.stage().GetMetadata('renderSettingsPrimPath')=='/Render/rendersettings'
assert integration.render_options(edited.stage())==('pathtraced',256)
assert [str(t) for t in UsdRender.Settings(edited.stage().GetPrimAtPath('/Render/rendersettings')).GetCameraRel().GetForwardedTargets()]==['/shot']
edited.parm(hou.text.encode('hdUE:renderMode')).set('realtime')
assert integration.render_options(edited.stage())==('realtime',64),'the default value renders, not the scene prim (samples apply to the Path Tracer only)'

# Native asset bounds live in the viewport overrides (session-only), sized in the stage's own units.
assets=integration.NativeAssets()
assert assets.sync(network,render,render.stage())
assert not assets.sync(network,render,render.stage()),'unchanged stage must not re-author'
def overrides():
    with network.viewportOverrides(render) as edit:
        return edit.layer()
layer=overrides()
for path in ('/Native','/Parent/MeshAsset','/Instance/Asset'):
    proxy=layer.GetPrimAtPath(path+'/'+integration.NativeAssets.PROXY)
    assert proxy and proxy.specifier==Sdf.SpecifierDef and proxy.typeName=='Mesh',path
    assert proxy.attributes['primvars:hdue:asset'].default==ASSET
    assert proxy.attributes['purpose'].default=='render'
    points=proxy.attributes['points'].default
    assert len(points)==8 and abs(max(p[0] for p in points)-min(p[0] for p in points)-100)<1e-4,'100 cm in a centimetre stage'
assert layer.GetPrimAtPath('/Parent/MeshAsset').attributes['purpose'].default=='guide'
assert layer.GetPrimAtPath('/Instance').HasInfo('instanceable') and not layer.GetPrimAtPath('/Instance').GetInfo('instanceable')
assert render.stage().GetPrimAtPath('/Instance').IsInstance(),'the graph keeps its instancing'

# MaterialX inside an instance needs its stage path only with a cooked library.
look=network.createNode('pythonscript','instanced_materialx'); look.setInput(0,render)
look.parm('python').set('''from pxr import Sdf, UsdShade
stage=hou.pwd().editableStage()
material=UsdShade.Material.Define(stage,'/Prototype/Asset/Looks/Paint')
shader=UsdShade.Shader.Define(stage,'/Prototype/Asset/Looks/Paint/Surface'); shader.CreateIdAttr('ND_standard_surface_surfaceshader')
material.CreateSurfaceOutput('mtlx').ConnectToSource(shader.CreateOutput('out',Sdf.ValueTypeNames.Token))
stage.OverridePrim('/Native').SetActive(False)
stage.OverridePrim('/Parent').SetActive(False)
stage.OverridePrim('/Prototype/Asset').GetAttribute('hdue:asset').Set('')''')
assert not look.errors(),look.errors()
assert assets.sync(network,look,look.stage())
assert not overrides().GetPrimAtPath('/Instance') or not overrides().GetPrimAtPath('/Instance').HasInfo('instanceable')
assert assets.sync(network,look,look.stage(),materials=True)
assert not overrides().GetPrimAtPath('/Instance').GetInfo('instanceable')
assert not overrides().GetPrimAtPath('/Instance/Asset/'+integration.NativeAssets.PROXY)
# A downstream edit clearing the asset removes its proxy; restoring it brings it back.
clear=network.createNode('pythonscript','clear_asset_reference'); clear.setInput(0,render)
clear.parm('python').set('stage=hou.pwd().editableStage()\nstage.GetPrimAtPath("/Parent/MeshAsset").GetAttribute("hdue:asset").Set("")')
assert assets.sync(network,clear,clear.stage())
layer=overrides()
assert not layer.GetPrimAtPath('/Parent/MeshAsset/'+integration.NativeAssets.PROXY)
assert not layer.GetPropertyAtPath('/Parent/MeshAsset.purpose')
assert layer.GetPrimAtPath('/Native/'+integration.NativeAssets.PROXY)
assets.clear()
layer=overrides()
assert not layer.GetPrimAtPath('/Native/'+integration.NativeAssets.PROXY)
assert not layer.GetPrimAtPath('/Instance').HasInfo('instanceable')
assert source.stage().GetRootLayer().ExportToString()==before
print('PASS: pass-through hdUE settings, render settings, native asset overrides and source preservation')
