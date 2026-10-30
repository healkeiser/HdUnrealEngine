"""Run with uv run -- <HFS>/bin/hython.exe tests/render_outputs_test.py: hdUE Standard Render Vars -> hdUE Render Products -> hdUE Render Settings."""

from pathlib import Path

import hou
from pxr import UsdRender

for name, type_name, icon in (
    (
        "hdue_rendersettings",
        "hdue::hduerendersettings::1.0",
        "hduerenderproperties",
    ),
    (
        "hdue_standardrendervars",
        "hdue::hduestandardrendervars::1.0",
        "hduestandardrendervars",
    ),
    (
        "hdue_renderproducts",
        "hdue::hduerenderproducts::1.0",
        "hduerenderproducts",
    ),
):
    hda = str(
        Path(__file__).resolve().parents[1]
        / f"integrations/houdini/otls/{name}.hda"
    )
    hou.hda.installFile(hda)
    definition = hou.hda.definitionsInFile(hda)[0]
    assert definition.nodeTypeName() == type_name, definition.nodeTypeName()
    assert (
        definition.icon() == "HDUE_" + icon
        and "IconSVG" not in definition.sections()
    ), (
        name,
        definition.icon(),
    )  # by name only
    # TAB menu > Rendering > hdUE, as Karma's LOPs are under Rendering > Karma.
    assert (
        "<toolSubmenu>Rendering/hdUE</toolSubmenu>"
        in definition.sections()["Tools.shelf"].contents()
    ), name
# Scenes saved with the unversioned names load the namespaced types (integrations/houdini/OPcustomize).
for line in (
    (Path(__file__).resolve().parents[1] / "integrations/houdini/OPcustomize")
    .read_text()
    .splitlines()
):
    if line.startswith("opalias"):
        hou.hscript(line)
        assert (
            hou.node("/stage").createNode(line.split()[-1]).type().name()
            == line.split()[-2]
        ), line
network = hou.node("/stage")
camera = network.createNode("camera")
rendervars = network.createNode("hdue::hduestandardrendervars::1.0")
rendervars.setInput(0, camera)
rendervars.parm("aov_primId").set(1)
products = network.createNode("hdue::hduerenderproducts::1.0")
products.setInput(0, rendervars)
products.parm("products").set(2)
products.parm("primname_2").set("depthonly")
products.parm("doorderedVars_2").set(1)
products.parm("orderedVars_2").set("/Render/Products/Vars/depth")
settings = network.createNode("hdue::hduerendersettings::1.0")
settings.setInput(0, products)
VARS = "/Render/Products/Vars/"


def render(mode, importvars=0):
    settings.parm("outputproducts").set(mode)
    settings.parm("importvars").set(importvars)
    stage = settings.stage()
    assert not settings.errors(), settings.errors()
    result = {}
    for path in (
        UsdRender.Settings(stage.GetPrimAtPath("/Render/rendersettings"))
        .GetProductsRel()
        .GetTargets()
    ):
        product = UsdRender.Product(stage.GetPrimAtPath(path))
        assert product, path
        result[str(path)] = [
            str(var) for var in product.GetOrderedVarsRel().GetTargets()
        ]
    return stage, result


stage, _ = render("upstream")
var = stage.GetPrimAtPath(VARS + "C")
assert (
    var.GetAttribute("dataType").Get() == "color4f"
    and var.GetAttribute("sourceName").Get() == "color"
)
assert var.GetAttribute("driver:parameters:aov:husk:format").Get() == "color4h"
assert (
    stage.GetPrimAtPath(VARS + "depth")
    .GetAttribute("driver:parameters:aov:husk:format")
    .Get()
    == "float"
)
assert (
    stage.GetPrimAtPath(VARS + "primId")
    .GetAttribute("driver:parameters:aov:husk:format")
    .Get()
    == "int"
)
assert not stage.GetPrimAtPath(VARS + "albedo")
upstream = {
    "/Render/Products/product1": [
        VARS + v for v in ("C", "normal", "depth", "primId")
    ],
    "/Render/Products/depthonly": [VARS + "depth"],
}
_, result = render("upstream")
assert {k: sorted(v) for k, v in result.items()} == {
    k: sorted(v) for k, v in upstream.items()
}, result
assert (
    stage.GetPrimAtPath("/Render/Products/product1")
    .GetAttribute("productName")
    .Get(1)
    .endswith(".product1.0001.exr")
)
_, result = render("both")
assert list(result)[0] == "/Render/Products/renderproduct" and sorted(
    result
) == sorted(["/Render/Products/renderproduct", *upstream]), result
assert result["/Render/Products/renderproduct"] == [VARS + "C"], result
_, result = render("builtin", importvars=1)
assert list(result) == ["/Render/Products/renderproduct"] and sorted(
    result["/Render/Products/renderproduct"]
) == sorted(upstream["/Render/Products/product1"]), result
alone = network.createNode("hdue::hduerendersettings::1.0")
alone.setInput(0, camera)
# Output Picture above the camera, as in Karma Render Settings, is the built-in product's productName.
top = [t.name() for t in alone.parmTemplateGroup().entries()]
assert top.index("picture") < top.index("camera"), top
alone.parm("picture").set("$HIP/elsewhere/shot.$F4.exr")
hou.setFrame(7)
stage = alone.stage()
assert (
    stage.GetPrimAtPath("/Render/Products/renderproduct")
    .GetAttribute("productName")
    .Get(7)
    .endswith("/elsewhere/shot.0007.exr")
)
assert UsdRender.Settings(
    stage.GetPrimAtPath("/Render/rendersettings")
).GetProductsRel().GetTargets() == ["/Render/Products/renderproduct"]
assert UsdRender.Product(
    stage.GetPrimAtPath("/Render/Products/renderproduct")
).GetOrderedVarsRel().GetTargets() == [VARS + "C"]
print("render outputs: ok")
