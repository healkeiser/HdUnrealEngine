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
# File parameters have Houdini's file chooser; the cooked libraries still author strings.
for name, mode, pattern in (
    ("picture", "write", None),
    (hou.text.encode("unreal:materialxLibrary"), "read", "*.json"),
    (hou.text.encode("unreal:substrateLibrary"), "read", "*.json"),
):
    template = alone.parm(name).parmTemplate()
    assert template.stringType() == hou.stringParmType.FileReference, name
    assert template.tags().get("filechooser_mode") == mode, template.tags()
    assert (
        template.tags().get("filechooser_pattern") == pattern
    ), template.tags()
alone.parm(hou.text.encode("unreal:materialxLibrary")).set("$HIP/library.json")
library = (
    alone.stage()
    .GetPrimAtPath("/Render/rendersettings")
    .GetAttribute("unreal:materialxLibrary")
)
assert str(library.GetTypeName()) == "string" and library.Get().endswith(
    "/library.json"
), library
# Post-process texture settings take image files (USD assets, resolved against the layer) or cooked texture paths;
# curve settings stay cooked paths.
for prop in (
    "AmbientCubemap",
    "AutoExposureMeterMask",
    "BloomConvolutionTexture",
    "BloomDirtMask",
    "ColorGradingLUT",
    "FilmGrainTexture",
    "LensFlareBokehShape",
    "VignetteTexture",
):
    template = alone.parm(
        hou.text.encode("unreal:postProcess:" + prop)
    ).parmTemplate()
    tags = template.tags()
    assert template.stringType() == hou.stringParmType.FileReference, prop
    assert tags.get("filechooser_mode") == "read", (prop, tags)
    assert "*.png" in tags.get(
        "filechooser_pattern", ""
    ) and "*.exr" in tags.get("filechooser_pattern", ""), (prop, tags)
    assert tags.get("usdvaluetype") == "asset", (prop, tags)
assert (
    alone.parm(hou.text.encode("unreal:postProcess:AutoExposureBiasCurve"))
    .parmTemplate()
    .stringType()
    == hou.stringParmType.Regular
)
alone.parm(hou.text.encode("unreal:tonemapper")).set(
    1
)  # lens settings apply with it
alone.parm(hou.text.encode("unreal:postProcess:BloomDirtMask")).set(
    "$HIP/dirt.png"
)
dirt = (
    alone.stage()
    .GetPrimAtPath("/Render/rendersettings")
    .GetAttribute("unreal:postProcess:BloomDirtMask")
)
assert str(dirt.GetTypeName()) == "asset" and dirt.Get().path.endswith(
    "/dirt.png"
), dirt
products = network.createNode("hdue::hduerenderproducts::1.0")
products.parm("products").set(1)
assert (
    products.parm("productName_1").parmTemplate().tags()["filechooser_mode"]
    == "write"
)
# Hit Lighting (off by default, as in UE) authors Lumen's Ray Lighting Mode.
ray = "unreal:postProcess:LumenRayLightingMode"
settings_prim = lambda: alone.stage().GetPrimAtPath("/Render/rendersettings")
assert not alone.evalParm("hdue_lumenHitLighting")
assert not settings_prim().GetAttribute(ray), ray
alone.parm("hdue_lumenHitLighting").set(1)
attribute = settings_prim().GetAttribute(ray)
assert (
    str(attribute.GetTypeName()) == "token"
    and attribute.Get() == "ELumenRayLightingModeOverride::HitLighting"
), attribute
alone.parm("hdue_lumenHitLighting").set(0)
assert not settings_prim().GetAttribute(ray), ray
# Layout: the Essential | Advanced strip first, then UE's categories as tabs, each sub-category in one place.
group = alone.parmTemplateGroup()
strip = group.entries()[0]
assert strip.name() == "hdue_show" and strip.isButtonStrip(), strip
assert strip.menuItems() == ("essential", "advanced"), strip.menuItems()
tabs = [
    t.label()
    for t in group.entries()
    if t.type() == hou.parmTemplateType.Folder
    and t.folderType() == hou.folderType.Tabs
]
assert tabs == [
    "Rendering",
    "Lens",
    "Color Grading",
    "Lighting",
    "Fog",
    "Sky",
    "Rendering Features",
    "Image Output",
    "Engine Quality",
], tabs


def folders(templates):
    for t in templates:
        if t.type() == hou.parmTemplateType.Folder:
            yield t
            yield from folders(t.parmTemplates())


labels = [f.label() for f in folders(group.entries())]
for once in (
    "Motion Blur",
    "Path Tracing",
    "Global Illumination",
    "Reflections",
):
    assert labels.count(once) == 1, (once, labels)
assert "Lumen" not in labels and "Path Tracer" not in labels, labels
encode = lambda attribute: hou.text.encode(attribute)
# Advanced settings hide in Essential and are still authored (the internal LOP has no Essential | Advanced strip).
subdivision = group.find(encode("unreal:subdivisionLevel"))
assert "hdue_show == 0" in subdivision.conditionals().get(
    hou.parmCondType.HideWhen, ""
), subdivision.conditionals()
assert "hdue_show" not in group.find(
    encode("unreal:renderMode")
).conditionals().get(hou.parmCondType.HideWhen, "")
alone.parm("hdue_show").set(0)
alone.parm(encode("unreal:subdivisionLevel")).set(3)
alone.parm(encode("unreal:samplesPerPixel")).set(16)
assert settings_prim().GetAttribute("unreal:subdivisionLevel").Get() == 3
# As in a Render Settings LOP, a setting the render mode hides is not authored (samples: Path Tracer only).
assert not settings_prim().GetAttribute("unreal:samplesPerPixel")
alone.parm(encode("unreal:renderMode")).set("pathtraced")
assert settings_prim().GetAttribute("unreal:samplesPerPixel").Get() == 16
alone.parm(encode("unreal:renderMode")).set("realtime")
# Ranges lock where UE clamps (ClampMin/ClampMax) and only guide the slider where it does not (UIMin/UIMax).
for attribute, lo, hi, lo_locked, hi_locked in (
    ("unreal:postProcess:BloomIntensity", 0, 8, True, False),
    ("unreal:postProcess:PathTracingMaxBounces", 0, 100, True, True),
    ("unreal:postProcess:WhiteTemp", 1500, 15000, False, False),
    ("unreal:postProcess:LumenFinalGatherQuality", 0.25, 2, True, False),
    ("unreal:samplesPerPixel", 1, 65536, True, True),
    ("unreal:warmupFrames", 1, 256, True, False),
):
    t = group.find(encode(attribute))
    assert (t.minValue(), t.maxValue(), t.minIsStrict(), t.maxIsStrict()) == (
        lo,
        hi,
        lo_locked,
        hi_locked,
    ), (attribute, t.minValue(), t.maxValue(), t.minIsStrict(), t.maxIsStrict())
resolution = group.find("resolution")
assert (resolution.minValue(), resolution.maxValue()) == (1, 16384) and (
    resolution.minIsStrict() and not resolution.maxIsStrict()
)
# Essential names the changed settings it hides; Show in Advanced switches the strip.
notice = alone.parm("hdue_hiddenchanges")
alone.parm("hdue_show").set(0)
# Changed above: the subdivision level and the cooked MaterialX library.
assert notice.evalAsString().startswith(
    "2 changed settings hidden in Advanced: "
)
moving = next(
    p
    for p in alone.parms()
    if p.parmTemplate().label() == "Moving Screen Percentage"
)
moving.set(moving.eval() + 10)
assert notice.evalAsString().startswith(
    "3 changed settings hidden in Advanced: "
) and (
    "Moving Screen Percentage (Rendering > Anti-Aliasing)"
    in notice.evalAsString()
), notice.evalAsString()
alone.parm("hdue_showhidden").pressButton()
assert alone.evalParm("hdue_show") == 1 and notice.evalAsString() == ""
moving.revertToDefaults()
# Second input, as Karma Render Settings': Import Render Vars From Second Input appends its render vars to the
# built-in product's ordered vars, after its own.
assert alone.type().maxNumInputs() == 2 and alone.inputLabels() == (
    "Input Stage",
    "Additional Render Vars",
), alone.inputLabels()
extra = network.createNode("rendervar")
extra.parm("primpath").set(VARS + "extra")
extra.parm("sourceName").set("extra")
standard = network.createNode("hdue::hduestandardrendervars::1.0")
standard.setInput(0, extra)
standard.parm("aov_albedo").set(1)
alone.setInput(1, standard)
builtin = UsdRender.Product(
    alone.stage().GetPrimAtPath("/Render/Products/renderproduct")
).GetOrderedVarsRel()
assert builtin.GetTargets() == [VARS + "C"], builtin.GetTargets()
alone.parm("importsecondaryinputvars").set(1)
stage = alone.stage()
targets = [
    str(p)
    for p in UsdRender.Product(
        stage.GetPrimAtPath("/Render/Products/renderproduct")
    )
    .GetOrderedVarsRel()
    .GetTargets()
]
assert targets[0] == VARS + "C" and sorted(targets) == sorted(
    VARS + v for v in ("C", "extra", "depth", "normal", "albedo")
), targets
# As in Karma's merge, an additional var at a built-in var's path is stronger (here the 16-bit beauty).
assert (
    stage.GetPrimAtPath(VARS + "C")
    .GetAttribute("driver:parameters:aov:husk:format")
    .Get()
    == "color4h"
)
assert (
    stage.GetPrimAtPath(VARS + "extra").GetAttribute("sourceName").Get()
    == "extra"
)
print("render outputs: ok")
