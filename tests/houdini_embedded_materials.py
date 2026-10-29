"""Owned HDA image, composed MaterialX binding and source-preservation checks."""

import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

import hou
from pxr import Sdf, Usd, UsdGeom, UsdLux, UsdShade

root = Path(__file__).resolve().parents[1]
sys.path[:0] = [
    str(root / "scripts"),
    str(root / "integrations/houdini/python3.13libs"),
]
from hdue_materialx import PreparedMaterials
from hdue_op_textures import Cache
from houdini_resources import embedded_image
from usd_materialx_graph import (
    graph_data,
    graph_signature,
    rebase_graph,
    value_data,
)

with tempfile.TemporaryDirectory(prefix="hdue-embedded-test-") as directory:
    directory = Path(directory)
    geo = hou.node("/obj").createNode("geo")
    node = geo.createNode("subnet")
    node = node.createDigitalAsset(
        name="hdue_embedded_fixture",
        hda_file_name=str(directory / "fixture.hda"),
    )
    definition = node.type().definition()
    first = (root / "samples/materialx/textures/warm.png").read_bytes()
    second = (root / "samples/materialx/textures/cool.png").read_bytes()
    definition.addSection("image.png", first)
    uri = "opdef:/Sop/hdue_embedded_fixture?image.png"
    assert value_data(Sdf.AssetPath(uri)) == value_data(Sdf.AssetPath(uri, uri))
    resolved = embedded_image(uri, directory / "images")
    assert Path(resolved["path"]).read_bytes() == first
    cache = Cache()
    assert Path(cache.resolve(uri)).read_bytes() == first
    assert Path(cache.resolve(uri, True)).read_bytes() == first
    cache.bindings["test"] = {uri}
    assert not cache.dirty("test")
    definition.addSection("image.png", second)
    assert cache.dirty("test")
    assert cache.resolve(uri) != resolved["path"]
    assert not cache.dirty("test")
    definition.addSection("image.png", first)
    source = directory / "source.usda"
    stage = Usd.Stage.CreateNew(str(source))
    asset = UsdGeom.Xform.Define(stage, "/Asset")
    stage.SetDefaultPrim(asset.GetPrim())
    material = UsdShade.Material.Define(stage, "/Asset/Looks/Paint")
    shader = UsdShade.Shader.Define(stage, "/Asset/Looks/Paint/Surface")
    shader.CreateIdAttr("ND_standard_surface_surfaceshader")
    image = UsdShade.Shader.Define(stage, "/Asset/Looks/Paint/Image")
    image.CreateIdAttr("ND_image_color3")
    image.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath(uri))
    image.CreateOutput("out", Sdf.ValueTypeNames.Color3f)
    shader.CreateInput(
        "base_color", Sdf.ValueTypeNames.Color3f
    ).ConnectToSource(image.ConnectableAPI(), "out")
    shader.CreateOutput("out", Sdf.ValueTypeNames.Token)
    material.CreateSurfaceOutput("mtlx").ConnectToSource(
        shader.ConnectableAPI(), "out"
    )
    mesh = UsdGeom.Mesh.Define(stage, "/Asset/Geometry")
    mesh.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (0, 1, 0)])
    mesh.CreateFaceVertexCountsAttr([3])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2])
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
    environment = (root / "samples/lighting/environment.hdr").read_bytes()
    definition.addSection("environment.hdr", environment)
    sky_uri = "opdef:/Sop/hdue_embedded_fixture?environment.hdr"
    dome = UsdLux.DomeLight.Define(stage, "/Asset/Dome")
    dome.CreateTextureFileAttr(Sdf.AssetPath(sky_uri))
    stage.GetRootLayer().Save()
    from resolve_houdini_textures import resolve

    resolve(source, directory / "resolved")
    extracted = json.loads((directory / "resolved/textures.json").read_text())[
        "textures"
    ][sky_uri]
    assert Path(extracted["path"]).read_bytes() == environment
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    registry = {
        "version": 1,
        "dependencies": {str(source): digest},
        "embedded_textures": {uri: resolved},
        "materials": [
            {
                "source": str(source),
                "material": "Paint",
                "asset": "/Game/HdUE/Test.Test",
                "model": "standard_surface",
                "usd_stage": str(source),
                "usd_material": str(material.GetPath()),
                "usd_shader": str(shader.GetPath()),
                "usd_bindings": [],
                "bindings": {},
                "usd_graph_signature": graph_signature(graph_data(material)),
            }
        ],
    }
    file = directory / "library.json"
    file.write_text(json.dumps(registry))
    composed = Usd.Stage.CreateInMemory()
    composed.DefinePrim("/Referenced").GetReferences().AddReference(str(source))
    from hdue_op_textures import prepare_stage

    files = prepare_stage(composed, "test-dome")
    assert (
        Path(files[sky_uri]).read_bytes() == environment
        and Path(files[uri]).read_bytes() == first
    )
    assert (
        composed.GetAttributeAtPath("/Referenced/Dome.inputs:texture:file")
        .Get()
        .path
        == sky_uri
    ), "the stage keeps its opdef: paths"
    calls = []
    api = SimpleNamespace(
        materials=lambda session, assets, overrides: calls.append(
            (session, assets, overrides)
        )
        or True,
        surfaces=lambda *args, **kwargs: True,
    )
    bridge = PreparedMaterials()
    assert bridge.sync(composed, Usd.TimeCode.Default(), str(file), 1, api)
    assert calls[-1][1] == {"/Referenced/Looks/Paint": "/Game/HdUE/Test.Test"}
    definition.addSection("image.png", second)
    try:
        bridge.sync(composed, Usd.TimeCode.Default(), str(file), 1, api)
    except ValueError:
        pass
    else:
        raise AssertionError(
            "Changed embedded bytes used stale cooked materials"
        )
    definition.addSection("image.png", first)
    assert not bridge.sync(composed, Usd.TimeCode.Default(), str(file), 1, api)
    assert bridge.sync(composed, Usd.TimeCode.Default(), str(file), 2, api)
    entry = bridge.library.inline.pop(next(iter(bridge.library.inline)))
    entry["usd_graph_signature"] = graph_signature(
        rebase_graph(
            graph_data(material), entry["usd_material"], "/Public/Looks/Paint"
        )
    )
    entry["usd_material"] = "/Public/Looks/Paint"
    bridge.library.inline[("composed-asset", entry["usd_material"])] = entry
    assert not bridge.sync(composed, Usd.TimeCode.Default(), str(file), 2, api)
    composed.GetAttributeAtPath("/Referenced/Looks/Paint/Surface.info:id").Set(
        "ND_open_pbr_surface_surfaceshader"
    )
    try:
        bridge.sync(composed, Usd.TimeCode.Default(), str(file), 2, api)
    except ValueError:
        pass
    else:
        raise AssertionError("Changed graph accepted")
    assert bridge.sync(composed, Usd.TimeCode.Default(), "", 2, api)
    assert calls[-1][1] == {}
    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
    reference = Usd.Stage.CreateNew(str(directory / "reference.usda"))
    instance = reference.DefinePrim("/Referenced")
    instance.GetReferences().AddReference(str(source))
    instance.SetInstanceable(True)
    reference.GetRootLayer().Save()
    runtime = directory / "runtime.json"
    runtime.write_text(json.dumps({"version": 1}))
    os.environ["HDUE_RUNTIME_CONFIG"] = str(runtime)
    import hdue_houdini as integration

    lop = hou.node("/stage").createNode("sublayer")
    lop.parm("filepath1").set(str(directory / "reference.usda"))
    assert lop.stage().GetPrimAtPath("/Referenced").IsInstance()
    prepared = integration.settings_node(lop)
    assert not prepared.errors(), prepared.errors()
    # Instancing reaches Hydra unchanged; the host still finds images inside instances.
    assert prepared.stage().GetPrimAtPath("/Referenced").IsInstance()
    assert set(prepare_stage(prepared.stage(), "instanced")) == {uri, sky_uri}
    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
    cache.close()
    from hdue_op_textures import close

    close()
    node.destroy()
    geo.destroy()
print(
    "PASS embedded image updates, reference rebasing, signatures, session recreation, clearing and source preservation"
)
