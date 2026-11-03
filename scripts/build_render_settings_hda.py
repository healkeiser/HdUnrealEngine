"""Build the hdUE Render Settings LOP (integrations/houdini/otls/hdue_rendersettings.hda).

Like Karma Render Settings, it wraps a Render Settings LOP, a Render Product and Render Vars, and shows the
camera, resolution, output picture and AOVs next to the hdUE settings, in the tabs of
soho/parameters/HdUnrealEngine_Global.ds. The internal Render Settings LOP keeps those settings as
tagged spare parameters referencing the asset's, so it authors the same unreal:* attributes, also where the
hdUE delegate is not registered. Like Karma Standard Render Vars and Karma Render Products, hdUE Standard Render
Vars (hdue_standardrendervars.hda) and hdUE Render Products (hdue_renderproducts.hda) author render vars and
products upstream, which hdUE Render Settings can render. Run it after scripts/generate_render_settings.py or any
edit of the .ds, and commit the result:

    python scripts/build_render_settings_hda.py

It restarts itself in hython, with the integration and the delegate's plugInfo.json registered
(HDUE_HOUDINI_ROOT, HFS or houdini_root; HDUE_HOUDINI_BUILD_ROOT, houdini_build_root or build/houdini).
"""

import itertools
import os
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / "integrations/houdini"
OTLS = INTEGRATION / "otls"
HDA = OTLS / "hdue_rendersettings.hda"
# Namespaced and versioned type names (hdue::<name>::<version>, as the Substrate HDAs). Bump the version when a
# parameter is removed or renamed, and keep the previous definition, so saved scenes keep theirs. OPcustomize aliases
# the names of the unversioned types these replace, so scenes saved with them load these.
VERSION = "1.0"
# hdUE Local Fog Volume 2.0 creates or edits its prim; 1.0 (create only) is otls/hdue_localfogvolume_legacy.hda.
LOCAL_FOG_VERSION = "2.0"
# The input stage's cameras as a menu, as Karma's render LOPs list them (their getCameras: loputils.globPrims).
CAMERA_MENU = (
    "import loputils\n"
    "node = kwargs['node']\n"
    "source = node.inputs()[0] if node.inputs() and node.inputs()[0] else node\n"
    "return [p.GetPrimPath().pathString for p in loputils.globPrims(source, '%type:Camera') for _ in range(2)]"
)
NAME, LABEL, ICON = (
    f"hdue::hduerendersettings::{VERSION}",
    "hdUE Render Settings",
    "HDUE_hduerenderproperties",
)
VARS = "/Render/Products/Vars/"
# The asset's Hit Lighting toggle drives this generated menu (unreal:postProcess:LumenRayLightingMode).
RAY_LIGHTING = "xn__unrealpostProcessLumenRayLightingMode_c1bl"
HIT_LIGHTING = "hdue_lumenHitLighting"
# AOVs the delegate renders besides the beauty, read from its table (src/hydra/aov_table.h; depth is the delegate's
# own): (render var and channel name, label, USD data type, husk format).
TABLE_ROW = re.compile(
    r'\{"(\w+)",\s*"[^"]*",\s*HdUEAovKind::(\w+),.*?HdUEAovFormat::(\w+),\s*\w+,\s*\w+,\s*((?:"[^"]*"\s*)+)\}',
    re.S,
)
DATA_TYPES = {
    "Float": "float",
    "Color": "color3f",
    "Vector": "normal3f",
    "Int": "int",
    "Beauty": "color3f",
}


def aov_table():
    """(name, kind, USD data type, description) per row of the delegate's AOV table."""
    text = (ROOT / "src/hydra/aov_table.h").read_text(encoding="utf-8")
    text = text[text.index("HdUEAovTable[]") :]
    return [
        (name, kind, DATA_TYPES[fmt], "".join(re.findall(r'"([^"]*)"', doc)))
        for name, kind, fmt, doc in TABLE_ROW.findall(text)
    ]


def aov_label(name):
    """Label of a render var: combineddiffuse -> Combined Diffuse, primId -> Prim ID."""
    words = re.sub(r"^(combined|direct|indirect)", r"\1 ", name)
    words = words.replace("glossyreflection", "glossy reflection")
    words = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", words)
    return " ".join(w[0].upper() + w[1:] for w in words.split()).replace(
        " Id", " ID"
    )


TABLE = aov_table()
AOVS = [("depth", "Depth", "float", "float")] + [
    (name, aov_label(name), data_type, data_type)
    for name, _, data_type, _ in TABLE
]
AOV_KIND = {"depth": "Depth", **{name: kind for name, kind, _, _ in TABLE}}
AOV_HELP = {
    "Depth": "Read back with the beauty.",
    "Buffer": "One of UE's buffer visualizations: all of a frame's render in one raster pass after the beauty "
    "converges (Movie Render Queue's buffer visualization pipes), from the same camera without Lumen, anti-aliasing "
    "or post effects. In path-traced mode albedo and normal are the Path Tracer's own buffers of the beauty instead "
    "(anti-aliased, with its depth of field).",
    "Lighting": "The beauty restricted to this lighting component (UE's Path Tracer Lighting Components; realtime, "
    "its Diffuse, Specular, Direct Lighting and Global Illumination show flags, with emission in every component): "
    "one full render per AOV and frame after the beauty converges.",
    "Id": "An unlit ID pass after the beauty converges: one short raster render per AOV and frame.",
}
# hdUE Standard Render Vars: what each var holds, its sections, the husk formats of its 16 and 32 bit precisions.
AOV_CONTENTS = {
    "C": "The beauty: the rendered image with alpha (color4f, sourceName color).",
    "depth": "Linear axial depth from the camera in stage units (float).",
    **{name: f"{doc} ({data_type})" for name, _, data_type, doc in TABLE},
}
VAR_SECTIONS = [
    ("Beauty", ["C"]),
    ("Surface", [name for name, kind, _, _ in TABLE if kind == "Buffer"]),
    ("Lighting", [name for name, kind, _, _ in TABLE if kind == "Lighting"]),
    (
        "Utility",
        ["depth"] + [name for name, kind, _, _ in TABLE if kind == "Id"],
    ),
]
PRECISIONS = {
    "color4f": ("color4h", "color4f"),
    "color3f": ("color3h", "color3f"),
    "normal3f": ("half3", "float3"),
    "float": ("half", "float"),
}
# Cryptomatte layers (src/hydra/cryptomatte.h): (name, label, what it names). Each adds the render vars <name>00 to
# <name>02 (ranks 0 to 5), 32-bit float4: the ids are float bit patterns.
CRYPTOMATTES = [
    ("CryptoObject", "Crypto Object", "prim paths"),
    ("CryptoMaterial", "Crypto Material", "bound material paths"),
]
CRYPTOMATTE_LAYERS = 3
VARS_ON = (
    "C",
    "depth",
    "normal",
)  # on by default, as Karma's beauty, depth and N
RENDER_PRIM_MENU = """import loputils
lop = kwargs['node']
if len(lop.inputs()) > 0:
    lop = lop.inputs()[0]
stage = lop.stage() if lop else None
return loputils.createRender{}PrimMenu(stage)"""
# Render Settings LOP parameters the asset shows above its tabs (they have no help of their own).
TOP_HELP = {
    "camera": "Camera prim the render looks through (husk, USD Render ROP; the viewport uses its own camera unless you look through this one).",
    "res_mode": "How the resolution is chosen: set both sizes, or set one and compute the other from the camera's aperture.",
    "resolution": "Image size in pixels.",
    "resolutionMenu": "Common image sizes.",
    "pixelAspectRatio": "Width of a pixel relative to its height.",
}
# The second input's render vars, after the built-in product's own (as Karma Render Settings appends them).
APPEND_VARS = """from pxr import UsdRender

node = hou.pwd()
asset = node.parent()
product = node.editableStage().GetPrimAtPath("/Render/Products/renderproduct")
if product and asset.evalParm("importsecondaryinputvars"):
    ordered = UsdRender.Product(product).GetOrderedVarsRel()
    targets = ordered.GetTargets()
    for prim in asset.node("secondary_vars").stage().Traverse():
        if prim.IsA(UsdRender.Var) and prim.GetPath() not in targets:
            targets.append(prim.GetPath())
    ordered.SetTargets(targets)
"""
TOOL = """<?xml version="1.0" encoding="UTF-8"?>
<shelfDocument>
  <tool name="$HDA_DEFAULT_TOOL" label="$HDA_LABEL" icon="$HDA_ICON">
    <toolMenuContext name="viewer">
      <contextNetType>LOP</contextNetType>
    </toolMenuContext>
    <toolMenuContext name="network">
      <contextOpType>$HDA_TABLE_AND_NAME</contextOpType>
    </toolMenuContext>
    <toolSubmenu>Rendering/hdUE</toolSubmenu>
    <script scriptType="python"><![CDATA[import loptoolutils
loptoolutils.genericTool(kwargs, '$HDA_NAME')]]></script>
  </tool>
</shelfDocument>
"""


def relaunch():
    """Run this file in hython with the hdUE integration and delegate registered."""
    from local_paths import local_path
    from local_paths import relaunch as run_in_hython

    houdini = local_path("houdini_root")
    run_in_hython(
        __file__,
        HOUDINI_PATH=f"{INTEGRATION};&",
        HDUE_HOUDINI_ENABLE="1",
        # UsdRenderers.json only lists the delegate when a runtime is configured.
        HDUE_RUNTIME_CONFIG=str(local_path("runtime_config")),
        PXR_PLUGINPATH_NAME=os.pathsep.join(
            [
                str(houdini / "bin/usd_plugins"),
                str(houdini / "houdini/dso/usd_plugins"),
                str(local_path("houdini_build_root") / "resources"),
            ]
        ),
    )


def help_page(
    group,
    name=NAME,
    label=LABEL,
    icon=ICON,
    summary="Renders the stage with Unreal Engine: render settings, camera, resolution, output and AOVs.",
    overview="Like Karma Render Settings, it creates the render settings prim, its render product and render vars, and "
    "makes that prim the stage's default render settings. It authors the hdUE settings changed from their "
    "defaults; the others keep hdUE's defaults (see docs/rendering.md in the hdUE repository). In Image Output, it can also render the render products "
    "and add the render vars authored upstream, by hdUE Render Products and hdUE Standard Render Vars.",
):
    """The node's help card (wiki markup), one entry per parameter from its help text, under its tabs and sections."""
    import hou

    lines = [
        f"= {label} =",
        "",
        "#type: node",
        "#context: lop",
        f"#internal: {name}",
        f'#icon: {icon.replace("_", "/", 1)}',
        "",
        f'"""{summary}"""',
        "",
        overview,
        "",
        "@parameters",
        "",
    ]

    def walk(templates, depth):
        for template in templates:
            if template.type() == hou.parmTemplateType.Folder:
                if template.parmTemplates():
                    lines.extend(
                        [
                            ("==" if depth == 0 else "===")
                            + f" {template.label()} "
                            + ("==" if depth == 0 else "==="),
                            "",
                        ]
                    )
                    if template.help():  # a multiparm's count
                        lines.extend(
                            [
                                f"{template.label()}:",
                                f"    #id: {template.name()}",
                                f"    {template.help()}",
                                "",
                            ]
                        )
                    walk(template.parmTemplates(), depth + 1)
            elif template.help() and "_control" not in template.name():
                ids = (
                    []
                    if "#" in template.name()
                    else [f"    #id: {template.name()}"]
                )  # multiparm instances have none
                lines.extend(
                    [f"{template.label()}:", *ids, f"    {template.help()}", ""]
                )

    walk(group.entries(), 0)
    return "\n".join(lines)


def build():
    import hou
    from generate_render_settings import JOINS, SHOW, SHOW_HELP

    encode = hou.text.encode

    network = hou.node("/stage")
    subnet = network.createNode("subnet", "rendersettings")
    settings = subnet.createNode("rendersettings", "rendersettings")
    renderer = settings.parmTemplateGroup().findFolder("Unreal Engine")
    if renderer is None:
        raise SystemExit(
            "The Render Settings LOP has no Unreal Engine tab: is the hdUE delegate registered?"
        )
    # Vars, then product, then settings: relationship patterns only match prims of their node's input.
    source = subnet.indirectInputs()[0]
    last = add_vars(
        subnet,
        source,
        [("C", "color", "color4f", "color4f", None)]
        + [(a[0], a[0], a[2], a[3], "aov_" + a[0]) for a in AOVS],
    )
    product = subnet.createNode("renderproduct", "renderproduct")
    product.setInput(0, last)
    settings.setInput(
        0,
        switch(
            subnet,
            "enable_builtin",
            source,
            product,
            'strcmp(chs("../outputproducts"), "upstream") != 0',
        ),
    )
    # As Karma Render Settings: the viewport and husk render with this prim even where the input brings its own.
    # As Karma Render Settings' second input: Import Render Vars From Second Input merges its render vars (its other
    # prims deactivated) and appends them to the built-in product's ordered vars.
    prune = subnet.createNode("prune", "additional_vars")
    prune.setInput(0, subnet.indirectInputs()[1])
    for parm, value in (
        ("pruneunselected", 1),
        ("method", "deactivate"),
        ("pruneancestors", 1),
        ("targetmethod1", "primtype"),
        ("primtype1", "UsdRenderVar"),
    ):
        prune.parm(parm).set(value)
    secondary = switch(
        subnet,
        "secondary_vars",
        subnet.createNode("null", "no_secondary_vars"),
        prune,
        'ch("../importsecondaryinputvars")',
    )
    merge = subnet.createNode("merge", "merge_secondary_vars")
    merge.parm("mergestyle").set("separate")
    merge.setInput(0, settings)
    merge.setInput(1, secondary)
    append = subnet.createNode("pythonscript", "append_secondary_vars")
    append.setInput(0, merge)
    append.parm("python").set(APPEND_VARS)
    default = subnet.createNode("configurelayer", "default_settings")
    default.setInput(0, append)
    default.parm("setrendersettings").setExpression(
        'ch("../setlayerrendersettings")'
    )
    default.parm("rendersettings").set('`chs("../primpath")`')
    subnet.node("output0").setInput(0, default)
    product.parm("orderedVars").set(
        VARS
        + "C"
        + "".join(
            f' `ifs(ch("../aov_{a[0]}"), "{VARS}{a[0]}", "")`' for a in AOVS
        )
        + ' `ifs(ch("../importvars"), "(" + chs("../upstreamvars") + ")", "")`'
    )
    product.parm("productName").set('`chs("../picture")`')
    settings.parm("products").set(
        '`ifs(strcmp(chs("../outputproducts"), "upstream"), "/Render/Products/renderproduct", "")` '
        '`ifs(strcmp(chs("../outputproducts"), "builtin"), "(" + chs("../upstreamproducts") + ")", "")`'
    )
    subnet.layoutChildren()

    # The renderer tab: the Essential | Advanced strip, the main settings, a separator, then the tabs.
    renderer = [
        with_level(without_controls(t))
        for t in renderer.parmTemplates()
        if "_control" not in t.name()
    ]
    show = hou.MenuParmTemplate(
        SHOW,
        "Show",
        ("essential", "advanced"),
        ("Essential", "Advanced"),
        is_button_strip=True,
        help=SHOW_HELP,
    )
    main = list(
        itertools.takewhile(
            lambda t: t.type() != hou.parmTemplateType.Folder, renderer
        )
    )
    tabs = renderer[len(main) :]
    # In Essential, the changed settings it hides (hdue_houdini.hidden_changes, evaluated by the parameter
    # dialog's refresh, never cooked) with a button showing them: the notice and button hide while there are none.
    hidden = {
        hou.parmCondType.HideWhen: '{ hdue_show == 1 } { hdue_hiddenchanges == "" }'
    }
    notice = hou.LabelParmTemplate(
        "hdue_hiddenchanges",
        "",
        column_labels=(
            "`pythonexprs(\"__import__('hdue_houdini').hidden_changes()\")`",
        ),
        is_label_hidden=True,
        conditionals=hidden,
        help="Settings changed from their default that Essential hides: they are still rendered.",
    )
    reveal = hou.ButtonParmTemplate(
        "hdue_showhidden",
        "Show in Advanced",
        script_callback="hou.pwd().parm('hdue_show').set(1)",
        script_callback_language=hou.scriptLanguage.Python,
        join_with_next=True,
        conditionals=hidden,
        help="Switches Show to Advanced, which shows the changed settings Essential hides.",
    )
    group = hou.ParmTemplateGroup([show, reveal, notice])
    # As Karma Render Settings: the prim path in the header of a closed section.
    group.append(
        hou.FolderParmTemplate(
            "hdue_primfolder",
            "Render Settings Prim",
            [
                prim_path(
                    "primpath",
                    "Render Settings Prim",
                    "/Render/rendersettings",
                    "Path of the UsdRenderSettings prim this node creates or edits.",
                ),
                hou.ToggleParmTemplate(
                    "setlayerrendersettings",
                    "Set as Default Render Settings Prim",
                    default_value=True,
                    help="Makes this prim the stage's default render settings (renderSettingsPrimPath), which the "
                    "viewport and husk render with, also where the input stage brings render settings of its own.",
                ),
            ],
            folder_type=hou.folderType.Collapsible,
            tags={"sidefx::header_parm": "primpath", "group_default": "0"},
        )
    )
    no_builtin = {hou.parmCondType.HideWhen: "{ outputproducts == upstream }"}
    # Above the camera, as Karma Render Settings' Output Picture.
    group.append(
        hou.StringParmTemplate(
            "picture",
            "Output Picture",
            1,
            default_value=("$HIP/render/$HIPNAME.$OS.$F4.exr",),
            string_type=hou.stringParmType.FileReference,
            conditionals={
                hou.parmCondType.DisableWhen: "{ outputproducts == upstream }"
            },
            tags={"filechooser_mode": "write"},
            help="Image the built-in render product writes (husk, USD Render ROP): its productName.",
        )
    )
    for name, text in TOP_HELP.items():
        template = settings.parmTemplateGroup().find(name)
        template.setHelp(text)
        # UE renders at most 16384 pixels on a side (D3D12's largest texture); larger
        # images render in husk tiles, so the slider stops there but the value does not.
        if name == "resolution":
            template.setMinValue(1)
            template.setMinIsStrict(True)
            template.setMaxValue(16384)
            template.setMaxIsStrict(False)
        if name == "camera":
            template.setMenuType(hou.menuType.StringReplace)
            template.setItemGeneratorScript(CAMERA_MENU)
            template.setItemGeneratorScriptLanguage(hou.scriptLanguage.Python)
        group.append(template)
    # The Unreal settings above the tabs (render mode, samples, tonemapper, ...) in a group of their own, apart from
    # the renderer-independent output, camera and resolution above them. Its box replaces the .ds separator.
    group.append(
        hou.FolderParmTemplate(
            "hdue_enginefolder",
            "Unreal Engine",
            [t for t in main if t.type() != hou.parmTemplateType.Separator],
            folder_type=hou.folderType.Simple,
        )
    )
    output = hou.FolderParmTemplate(
        "hdUE_ImageOutputTab", "Image Output", folder_type=hou.folderType.Tabs
    )
    output.addParmTemplate(
        hou.StringParmTemplate(
            "outputproducts",
            "Render Products",
            1,
            default_value=("builtin",),
            menu_items=("builtin", "upstream", "both"),
            menu_labels=(
                "Built-in Product",
                "Upstream Products",
                "Built-in and Upstream Products",
            ),
            help="Which render products the render settings render: this node's own product (Output Picture above, AOVs below), "
            "the render products authored upstream (hdUE Render Products, Render Product LOPs), or both.",
        )
    )
    output.addParmTemplate(
        primlist(
            "upstreamproducts",
            "Upstream Products",
            "/Render/** & %type:RenderProduct",
            "Product",
            "Render products of the input stage the render settings also render.",
            {hou.parmCondType.HideWhen: "{ outputproducts == builtin }"},
        )
    )
    aovs = hou.FolderParmTemplate(
        "hdUE_Aovs",
        "AOVs (Render Vars)",
        folder_type=hou.folderType.Simple,
        conditionals=no_builtin,
    )
    for name, label, _, _ in AOVS:
        aovs.addParmTemplate(
            hou.ToggleParmTemplate(
                "aov_" + name, label, help=AOV_HELP[AOV_KIND[name]]
            )
        )
    aovs.addParmTemplate(
        hou.ToggleParmTemplate(
            "importvars",
            "Import Upstream Render Vars",
            join_with_next=True,
            help="Also writes the render vars authored upstream (hdUE Standard Render Vars, Render Var LOPs) to the built-in "
            "product, after the beauty and the AOVs above. An upstream var at the path of a built-in one is replaced by it.",
        )
    )
    aovs.addParmTemplate(
        primlist(
            "upstreamvars",
            "Upstream Render Vars",
            "/Render/** & %type:RenderVar",
            "Var",
            "Render vars of the input stage the built-in product writes.",
            {hou.parmCondType.DisableWhen: "{ importvars == 0 }"},
            show_label=False,
        )
    )
    aovs.addParmTemplate(
        hou.ToggleParmTemplate(
            "importsecondaryinputvars",
            "Import Render Vars From Second Input",
            help="As Karma Render Settings: merges the render vars of the second input (Additional Render Vars: hdUE "
            "Standard Render Vars, Render Var LOPs; its other prims are deactivated) and appends them to the built-in "
            "product's ordered vars, after the vars above. As in Karma's merge, a var at the path of a built-in one "
            "is stronger (its format, for example).",
        )
    )
    output.addParmTemplate(aovs)
    for tab in tabs:
        if tab.label() == "Engine Quality":
            group.append(output)
        group.append(tab)
    for first, _ in JOINS:  # a range's minimum and maximum on one row
        template = group.find(encode("unreal:postProcess:" + first))
        template.setJoinWithNext(True)
        group.replace(template.name(), template)
    group.insertBefore(
        RAY_LIGHTING,
        hou.ToggleParmTemplate(
            HIT_LIGHTING,
            "Hit Lighting",
            default_value=False,
            help="Lights Lumen's hardware rays where they hit, instead of from the surface cache: brighter, more "
            "accurate indirect light and reflections, at a cost in render time. Off by default, as in UE. On, it "
            "sets the Ray Lighting Mode below to Hit Lighting (unreal:postProcess:LumenRayLightingMode).",
        ),
    )
    ray_lighting = group.find(RAY_LIGHTING)
    ray_lighting.setConditional(
        hou.parmCondType.DisableWhen, f"{{ {HIT_LIGHTING} == 1 }}"
    )
    group.replace(RAY_LIGHTING, ray_lighting)
    subnet.setParmTemplateGroup(group)
    # The internal Render Settings LOP follows the asset's parameters.
    settings.parm("primpath").set('`chs("../primpath")`')
    # (The internal resolution's locked height expression computes from the referenced mode, camera and width.)
    for template in group.entriesWithoutFolders():
        if template.name() not in (
            "primpath",
            "resolutionMenu",
        ) and settings.parmTuple(template.name()):
            for parm, source in zip(
                settings.parmTuple(template.name()),
                subnet.parmTuple(template.name()),
                strict=False,
            ):
                if not parm.isLocked():
                    parm.set(source)
    # No USD control menus, as Karma Render Settings: the internal controls author what differs from its default.
    for template in group.entriesWithoutFolders():
        control = (
            settings.parm(
                hou.text.encode(hou.text.decode(template.name()) + "_control")
            )
            if template.name().startswith("xn__unreal")
            else None
        )
        if control:
            control.setExpression(
                control_expression(template, subnet.parmTuple(template.name()))
            )
    # Hit Lighting authors the Ray Lighting Mode menu's Hit Lighting; off, the menu's own value.
    mode = settings.parm(RAY_LIGHTING)
    mode.setExpression(
        f'ifs(ch("../{HIT_LIGHTING}"), "ELumenRayLightingModeOverride::HitLighting", chs("../{RAY_LIGHTING}"))'
    )
    settings.parm(
        hou.text.encode(hou.text.decode(RAY_LIGHTING) + "_control")
    ).setExpression(
        control_expression(
            group.find(RAY_LIGHTING), subnet.parmTuple(RAY_LIGHTING)
        ).replace("ifs(", f'ifs(ch("../{HIT_LIGHTING}") || ', 1)
    )

    save(
        subnet,
        HDA,
        NAME,
        LABEL,
        ICON,
        group,
        help_page(group),
        "import loputils\nloputils.updateResolutionParameters(kwargs['node'])\n",
        max_inputs=2,
        input_labels=("Input Stage", "Additional Render Vars"),
    )


def without_controls(template):
    """The renderer tab without its USD control menus, and without the conditions on them."""
    import hou

    if template.type() == hou.parmTemplateType.Folder:
        template.setParmTemplates(
            [
                without_controls(t)
                for t in template.parmTemplates()
                if "_control" not in t.name()
            ]
        )
    elif "_control" in template.conditionals().get(
        hou.parmCondType.DisableWhen, ""
    ):
        rest = re.sub(
            r"\{[^}]*_control[^}]*\}",
            "",
            template.conditionals()[hou.parmCondType.DisableWhen],
        ).strip()
        template.setConditional(hou.parmCondType.DisableWhen, rest)
    return template


def with_level(template):
    """The template with its Essential | Advanced hide-when (the generator's tag) added, and its folders' contents."""
    import hou
    from generate_render_settings import LEVEL_TAG

    folder = template.type() == hou.parmTemplateType.Folder
    if folder:
        template.setParmTemplates(
            [with_level(t) for t in template.parmTemplates()]
        )
    level = template.tags().get(LEVEL_TAG)
    if level:  # a folder's own hide-when is its tab conditional (hidewhentab)
        conditions = (
            template.tabConditionals() if folder else template.conditionals()
        )
        hide = (
            f"{conditions.get(hou.parmCondType.HideWhen, '')} {level}".strip()
        )
        (template.setTabConditional if folder else template.setConditional)(
            hou.parmCondType.HideWhen, hide
        )
    return template


def control_expression(template, parms):
    """An internal control expression: Set while the asset's value differs from its default, else Do Nothing, so
    unchanged settings keep hdUE's defaults (UE's post-process and engine defaults are not all fixed values).
    """
    import hou

    defaults = (
        template.defaultValue()
        if isinstance(template.defaultValue(), tuple)
        else (template.defaultValue(),)
    )
    if isinstance(template, hou.StringParmTemplate):
        quoted = [d.replace("\\", "\\\\").replace('"', '\\"') for d in defaults]
        checks = [
            f'strcmp(chs("../{p.name()}"), "{d}") != 0'
            for p, d in zip(parms, quoted, strict=False)
        ]
    else:
        checks = [
            f'ch("../{p.name()}") != {int(d) if isinstance(d, bool) else d}'
            for p, d in zip(parms, defaults, strict=False)
        ]
    return "ifs(" + " || ".join(checks) + ', "set", "none")'


def primlist(
    name, label, default, kind, text, conditionals=None, show_label=True
):
    """A prim pattern parameter with a menu of the input stage's render vars or products (kind Var or Product)."""
    import hou

    return hou.StringParmTemplate(
        name,
        label,
        1,
        default_value=(default,),
        menu_type=hou.menuType.StringToggle,
        item_generator_script=RENDER_PRIM_MENU.format(kind),
        item_generator_script_language=hou.scriptLanguage.Python,
        conditionals=conditionals or {},
        is_label_hidden=not show_label,
        tags={"sidefx::usdpathtype": "primlist"},
        help=text,
    )


def switch(subnet, name, off, on, expression):
    """A Switch LOP passing on while the expression is nonzero, otherwise off."""
    node = subnet.createNode("switch", name)
    node.setInput(0, off)
    node.setInput(1, on)
    node.parm("input").setExpression(expression)
    return node


def add_vars(subnet, previous, entries, parent=None):
    """Chain a Render Var LOP per (name, sourceName, data type, husk format, toggle) after previous, under parent
    (an expression; the Render Var LOP's default path otherwise); a var with a toggle exists only while it is on.
    """
    import hou

    for name, source, data_type, husk_format, toggle in entries:
        var = subnet.createNode("rendervar", name)
        var.setInput(0, previous)
        if parent:
            var.parm("primpath").set(f"{parent}/{name}")
        var.parm("sourceName").set(source)
        var.parm("dataType").set(data_type)
        var.parm(hou.text.encode("driver:parameters:aov:husk:format")).set(
            husk_format
        )
        previous = (
            switch(
                subnet, "enable_" + name, previous, var, f'ch("../{toggle}")'
            )
            if toggle
            else var
        )
    return previous


def save(
    subnet,
    hda,
    name,
    label,
    icon,
    group,
    help_text,
    on_created=None,
    max_inputs=1,
    input_labels=(),
):
    """Turn the subnet into the asset saved to hda, with the parameters, icon, TAB menu entry and help card."""
    import hou

    hda.unlink(missing_ok=True)
    asset = subnet.createDigitalAsset(
        name,
        str(hda),
        label,
        min_num_inputs=0,
        max_num_inputs=max_inputs,
        ignore_external_references=True,
    )
    definition = asset.type().definition()
    definition.setParmTemplateGroup(
        group
    )  # the subnet's spare parameters are not carried over
    definition.setIcon(icon)
    definition.addSection("Tools.shelf", TOOL)
    definition.addSection("Help", help_text)
    if on_created:
        definition.addSection("OnCreated", on_created)
        definition.setExtraFileOption("OnCreated/IsPython", True)
    definition.save(str(hda), asset)
    # Saving embeds a copy of a resolvable icon; keep the name only (config/Icons on HOUDINI_PATH resolves it).
    saved = hou.hda.definitionsInFile(str(hda))[0]
    if "IconSVG" in saved.sections():
        saved.removeSection("IconSVG")
        saved.setIcon(icon)
    if (
        input_labels
    ):  # the Type Properties' input labels (inputlabel lines of the dialog script)
        script = saved.sections()["DialogScript"]
        contents = script.contents()
        for i, text in enumerate(input_labels, 1):
            contents = re.sub(
                rf'^(\s*inputlabel\s+{i}\s+)"[^"]*"',
                lambda m, text=text: f'{m.group(1)}"{text}"',
                contents,
                flags=re.M,
            )
        script.setContents(contents)
    shutil.rmtree(hda.parent / "backup", ignore_errors=True)
    print(
        f"{hda}: {len(definition.parmTemplateGroup().entriesWithoutFolders())} parameters"
    )


def prim_path(name, label, default, help):
    """A prim path parameter as Karma's render LOPs have it: the prim picker button and the stage's prim paths as a menu
    (loputils.createPrimPathMenu).
    """
    import hou

    return hou.StringParmTemplate(
        name,
        label,
        1,
        default_value=(default,),
        menu_type=hou.menuType.StringReplace,
        item_generator_script="import loputils\nreturn loputils.createPrimPathMenu()",
        item_generator_script_language=hou.scriptLanguage.Python,
        tags={
            "sidefx::usdpathtype": "prim",
            "script_action": "import loputils\nloputils.selectPrimsInParm(kwargs, False)",
            "script_action_icon": "BUTTONS_reselect",
        },
        help=help,
    )


def parent_path(default, what):
    """The Parent Primitive Path parameter of the nodes creating render vars or products."""
    return prim_path(
        "parentprimpath",
        "Parent Primitive Path",
        default,
        f"Path of the prim under which this node creates its {what}.",
    )


def build_standard_render_vars():
    """HdUE Standard Render Vars: a render var per output hdUE renders, as Karma Standard Render Vars does for Karma."""
    import hou

    network = hou.node("/stage")
    name, label = (
        f"hdue::hduestandardrendervars::{VERSION}",
        "hdUE Standard Render Vars",
    )
    subnet = network.createNode("subnet", "standardrendervars")
    types = {"C": "color4f", **{a[0]: a[2] for a in AOVS}}
    labels = {"C": "Beauty", **{a[0]: a[1] for a in AOVS}}
    group = hou.ParmTemplateGroup(
        [parent_path("/Render/Products/Vars", "render vars")]
    )
    for section, names in VAR_SECTIONS:
        folder = hou.FolderParmTemplate(
            "section_" + section.lower(),
            section,
            folder_type=hou.folderType.Collapsible,
        )
        for var in names:
            folder.addParmTemplate(
                hou.ToggleParmTemplate(
                    "aov_" + var,
                    labels[var],
                    default_value=var in VARS_ON,
                    help=f"Adds the render var {var}. {AOV_CONTENTS[var]}"
                    + ("" if var == "C" else " " + AOV_HELP[AOV_KIND[var]]),
                )
            )
            if types[var] in PRECISIONS:
                folder.addParmTemplate(
                    hou.StringParmTemplate(
                        f"aov_{var}_format",
                        "Precision",
                        1,
                        default_value=(PRECISIONS[types[var]][var == "depth"],),
                        menu_items=PRECISIONS[types[var]],
                        menu_labels=("16 Bit", "32 Bit"),
                        conditionals={
                            hou.parmCondType.DisableWhen: f"{{ aov_{var} == 0 }}"
                        },
                        help="Bit depth of the channels husk writes to the image (driver:parameters:aov:husk:format).",
                    )
                )
        group.append(folder)
    folder = hou.FolderParmTemplate(
        "section_cryptomatte",
        "Cryptomatte",
        folder_type=hou.folderType.Collapsible,
    )
    for layer, layer_label, names in CRYPTOMATTES:
        folder.addParmTemplate(
            hou.ToggleParmTemplate(
                "aov_" + layer,
                layer_label,
                help=f"Adds the Cryptomatte layer {layer} of {names}: the render vars {layer}00 to "
                f"{layer}{CRYPTOMATTE_LAYERS - 1:02d} (float4, ranks 0 to {2 * CRYPTOMATTE_LAYERS - 1}), with "
                "coverage from 16 sub-pixel ID passes after the beauty converges. husk writes the layer's "
                "metadata and manifest into each image.",
            )
        )
    group.append(folder)
    entries = [
        (
            var,
            "color" if var == "C" else var,
            types[var],
            (
                f'`chs("../aov_{var}_format")`'
                if types[var] in PRECISIONS
                else types[var]
            ),
            "aov_" + var,
        )
        for _, names in VAR_SECTIONS
        for var in names
    ] + [
        (
            f"{layer}{i:02d}",
            f"{layer}{i:02d}",
            "float4",
            "float4",
            "aov_" + layer,
        )
        for layer, _, _ in CRYPTOMATTES
        for i in range(CRYPTOMATTE_LAYERS)
    ]
    subnet.node("output0").setInput(
        0,
        add_vars(
            subnet,
            subnet.indirectInputs()[0],
            entries,
            '`chs("../parentprimpath")`',
        ),
    )
    subnet.layoutChildren()
    save(
        subnet,
        OTLS / "hdue_standardrendervars.hda",
        name,
        label,
        "HDUE_hduestandardrendervars",
        group,
        help_page(
            group,
            name,
            label,
            "HDUE_hduestandardrendervars",
            "Creates the render vars (AOVs) hdUE renders.",
            "Like Karma Standard Render Vars, it creates a render var prim per enabled output under the parent primitive path, "
            "for hdUE Render Products or hdUE Render Settings (Image Output > Import Upstream Render Vars) to write. The beauty "
            "is rendered as converged; the other AOVs are read from the GBuffer of the nearest opaque surface (path traced: "
            "albedo and normal from the Path Tracer), Cryptomatte from sub-pixel ID passes "
            "(see AOVs in docs/houdini.md in the hdUE repository).",
        ),
    )


def build_render_products():
    """HdUE Render Products: render products from a multiparm, as Karma Render Products, with a For Each loop."""
    import hou

    network = hou.node("/stage")
    name, label = (
        f"hdue::hduerenderproducts::{VERSION}",
        "hdUE Render Products",
    )
    subnet = network.createNode("subnet", "renderproducts")
    source = subnet.indirectInputs()[0]
    begin = subnet.createNode("begincontextoptionsblock", "foreach_begin")
    begin.setInput(0, source)
    product = subnet.createNode("renderproduct", "product")
    product.setInput(0, begin)
    index = "int(@ITERATION + 1)"
    product.parm("primpath").set(
        f'`chs("../parentprimpath")`/`chs("../primname_" + {index})`'
    )
    product.parm("productName").set(f'`chs("../productName_" + {index})`')
    # The Default Camera is authored only when set: products without one render through the settings' camera.
    product.parm("camera").set('`chs("../camera")`')
    product.parm("camera_control").setExpression(
        '"set" if hou.pwd().parent().evalParm("camera") else "none"',
        hou.exprLanguage.Python,
    )
    product.parm("orderedVars").set(
        f'`ifs(ch("../doorderedVars_" + {index}), chs("../orderedVars_" + {index}), chs("../orderedVars"))`'
    )
    enabled = switch(
        subnet, "enable_product", begin, product, f'ch("../enable_" + {index})'
    )
    loop = subnet.createNode("foreach", "foreach_end")
    loop.setInput(0, source)
    loop.setInput(2, enabled)
    loop.parm("iterations").setExpression('ch("../products")')
    subnet.node("output0").setInput(0, loop)
    subnet.layoutChildren()

    common = hou.FolderParmTemplate(
        "common", "Common Settings", folder_type=hou.folderType.Simple
    )
    common.addParmTemplate(
        hou.StringParmTemplate(
            "camera",
            "Default Camera",
            1,
            menu_type=hou.menuType.StringReplace,
            item_generator_script=CAMERA_MENU,
            item_generator_script_language=hou.scriptLanguage.Python,
            tags={
                "sidefx::usdpathtype": "prim",
                "script_action": "import loputils\nloputils.selectPrimsInParm(kwargs, False)",
                "script_action_icon": "BUTTONS_reselect",
            },
            help="Camera the products render through, as Karma Render Products' Default Camera. Empty: the render "
            "settings' camera.",
        )
    )
    common.addParmTemplate(
        primlist(
            "orderedVars",
            "Default Ordered Render Vars",
            "/Render/Products/Vars/*",
            "Var",
            "Render vars (AOVs) of the input stage the products write, in this order, unless a product sets its own.",
        )
    )
    products = hou.FolderParmTemplate(
        "products",
        "Render Products",
        folder_type=hou.folderType.MultiparmBlock,
        default_value=1,
    )
    products.setHelp("Number of render products.")
    disabled = {hou.parmCondType.DisableWhen: "{ enable_# == 0 }"}
    products.addParmTemplate(
        hou.ToggleParmTemplate(
            "enable_#",
            "Enable",
            default_value=True,
            help="Creates this render product.",
        )
    )
    products.addParmTemplate(
        hou.StringParmTemplate(
            "primname_#",
            "Name",
            1,
            default_value=("product#",),
            conditionals=disabled,
            help="Name of the render product prim, under the parent primitive path.",
        )
    )
    products.addParmTemplate(
        hou.StringParmTemplate(
            "productName_#",
            "Product Name",
            1,
            default_value=('$HIP/render/$HIPNAME.`chs("primname_#")`.$F4.exr',),
            string_type=hou.stringParmType.FileReference,
            conditionals=disabled,
            tags={"filechooser_mode": "write"},
            help="Image the render product writes (husk, USD Render ROP), with one channel set per render var.",
        )
    )
    products.addParmTemplate(
        hou.ToggleParmTemplate(
            "doorderedVars_#",
            "Ordered Render Vars",
            join_with_next=True,
            conditionals=disabled,
            help="Writes the render vars set here instead of the default ordered render vars.",
        )
    )
    products.addParmTemplate(
        primlist(
            "orderedVars_#",
            "Ordered Render Vars",
            "",
            "Var",
            "Render vars this product writes, in this order.",
            {
                hou.parmCondType.DisableWhen: "{ enable_# == 0 } { doorderedVars_# == 0 }"
            },
            show_label=False,
        )
    )
    folder = hou.FolderParmTemplate(
        "folder_products", "Render Products", folder_type=hou.folderType.Simple
    )
    folder.addParmTemplate(products)
    group = hou.ParmTemplateGroup(
        [parent_path("/Render/Products", "render products"), common, folder]
    )
    save(
        subnet,
        OTLS / "hdue_renderproducts.hda",
        name,
        label,
        "HDUE_hduerenderproducts",
        group,
        help_page(
            group,
            name,
            label,
            "HDUE_hduerenderproducts",
            "Creates render products sharing common settings.",
            "Like Karma Render Products, it creates render products, each writing render vars (from hdUE Standard Render Vars "
            "or Render Var LOPs upstream) to its own image. hdUE Render Settings renders them with Image Output > Render "
            "Products set to Upstream Products (or Built-in and Upstream Products).",
        ),
    )


# hdUE's prim LOPs create or edit their prim as Houdini's own prim LOPs do (Karma Sky Atmosphere, Sphere): the header of
# an Edit Properties LOP (Action: Create, Edit or Force Edit; Primitives; Initialize Parameters) and a control menu per
# property. The asset's parameters drive an internal Edit Properties LOP, as Karma Physical Sky's drive its internal
# nodes, so Create mode can also add a guide under the prim.
EDIT_HEADER = (
    "sample_group",
    "primpattern",
    "allowedprimtypes",
    "handledisallowedtypes",
    "primpath",
    "createprims",
    "initforedit",
    "createprimsgroup",
)
# The transform section of SideFX's Sphere LOP (Edit Properties' xform value type, XformCommonAPI by default).
XFORM_PARMS = (
    "xn__xformOptransform_control_6fb",
    "xn__xformOptransform_51a",
    "xOrd",
    "rOrd",
    "t",
    "r",
    "s",
    "shear",
    "scale",
    "pivotxform",
)
CONTROL_MENU = "import loputils\nreturn loputils.createEditPropertiesControlMenu(kwargs, '{}')"


def property_templates(setting):
    """A generated UE property as an Edit Properties parameter pair: its control menu (Set or Create by default, as
    Houdini's prim LOPs) and its value, named after the USD attribute and tagged with its USD type.
    """
    import hou
    from generate_render_settings import USD_TYPES, houdini_name

    usd_type = USD_TYPES[setting["kind"]]
    name = houdini_name(setting["attribute"])
    control = houdini_name(setting["attribute"] + "_control")
    common = dict(
        help=setting["help"],
        tags={"usdvaluetype": usd_type},
        disable_when=f"{{ {control} == block }} {{ {control} == animationblock }} {{ {control} == none }}",
    )
    if setting["kind"] == "toggle":
        value = hou.ToggleParmTemplate(
            name,
            setting["label"],
            default_value=bool(setting["default"][0]),
            **common,
        )
    elif setting["kind"] == "string":
        value = hou.StringParmTemplate(
            name,
            setting["label"],
            1,
            default_value=tuple(setting["default"]),
            **common,
        )
    elif setting["kind"] == "color":
        value = hou.FloatParmTemplate(
            name,
            setting["label"],
            3,
            default_value=tuple(setting["default"]),
            look=hou.parmLook.ColorSquare,
            naming_scheme=hou.parmNamingScheme.RGBA,
            **common,
        )
    else:
        lo, hi, lock_lo, lock_hi = setting["range"] or (0, 10, False, False)
        number = int if setting["kind"] == "int" else float
        cls = (
            hou.IntParmTemplate
            if setting["kind"] == "int"
            else hou.FloatParmTemplate
        )
        value = cls(
            name,
            setting["label"],
            1,
            default_value=tuple(setting["default"]),
            min=number(lo),
            max=number(hi),
            min_is_strict=lock_lo,
            max_is_strict=lock_hi,
            **common,
        )
    menu = hou.StringParmTemplate(
        control,
        setting["label"],
        1,
        default_value=("set",),
        menu_type=hou.menuType.StringReplace,
        item_generator_script=CONTROL_MENU.format(usd_type),
        item_generator_script_language=hou.scriptLanguage.Python,
        tags={"sidefx::look": "icon"},
    )
    menu.setJoinWithNext(True)
    return [menu, value]


def build_prim_lop(
    hda,
    name,
    label,
    icon,
    prim_type,
    prim_path,
    folders,
    guide,
    summary,
    overview,
    extra=(),
):
    """A create/edit LOP for one prim type: Edit Properties' header (the prim type fixed, only that type editable),
    Sphere's transform section and folders of (label, settings); in Create mode,
    guide(subnet, previous), if given, adds the prim's viewport guide after the internal Edit Properties LOP.
    """
    import hou

    subnet = hou.node("/stage").createNode("subnet", name.split("::")[1])
    props = subnet.createNode("editproperties", "properties")
    props.setInput(0, subnet.indirectInputs()[0])

    header = props.parmTemplateGroup()
    entries = [header.find(n) for n in EDIT_HEADER]
    for entry in entries:
        if entry.name() == "createprims":
            entry.setDefaultValue(1)  # Create, as Houdini's prim LOPs
        elif entry.name() == "primpath":
            entry.setDefaultValue((prim_path,))
        elif entry.name() == "allowedprimtypes":
            entry.setDefaultValue((prim_type,))
        elif entry.name() == "createprimsgroup":
            # One prim of the asset's type, so the guide follows the one path.
            kept = []
            for template in entry.parmTemplates():
                if template.name() == "primtype":
                    template.setDefaultValue((prim_type,))
                    template.hide(True)
                elif template.name() == "primcount":
                    template.hide(True)
                kept.append(template)
            entry.setParmTemplates(kept)
    sphere = hou.nodeType(
        hou.lopNodeTypeCategory(), "sphere"
    ).parmTemplateGroup()
    transform = hou.FolderParmTemplate(
        "folder_transform", "Transform", folder_type=hou.folderType.Simple
    )
    for parm in XFORM_PARMS:
        # Sphere's identity defaults: in Edit mode the transform is applied on top of the prim's, so any other default
        # would move or grow the prim on every edit node.
        transform.addParmTemplate(sphere.find(parm))
    properties = [transform]
    for folder_label, settings in folders:
        folder = hou.FolderParmTemplate(
            "folder_" + re.sub(r"\W", "", folder_label.lower()),
            folder_label,
            folder_type=hou.folderType.Simple,
        )
        for setting in settings:
            for template in property_templates(setting):
                folder.addParmTemplate(template)
        properties.append(folder)
    properties += list(extra)  # folders of ready-made control/value parameters

    inner = props.parmTemplateGroup()
    for template in properties:
        inner.append(template)
    props.setParmTemplateGroup(inner)
    group = hou.ParmTemplateGroup(entries + properties)
    subnet.setParmTemplateGroup(group)
    # The internal Edit Properties LOP follows the asset's parameters; Initialize Parameters runs on the asset.
    for template in group.entriesWithoutFolders():
        if (
            template.type() == hou.parmTemplateType.Button
            or template.name() == "initforedit"
        ):
            continue
        for target, source in zip(
            props.parmTuple(template.name()),
            subnet.parmTuple(template.name()),
            strict=True,
        ):
            target.set(source)

    last = (
        switch(
            subnet,
            "create",
            props,
            guide(subnet, props),
            'ch("../createprims") == 1',
        )
        if guide
        else props
    )
    subnet.node("output0").setInput(0, last)
    subnet.layoutChildren()
    save(
        subnet,
        hda,
        name,
        label,
        icon,
        group,
        help_page(group, name, label, icon, summary, overview),
    )
    # The transform handles of SideFX's Sphere LOP, bound to the same parameters.
    sphere_script = (
        hou.nodeType(hou.lopNodeTypeCategory(), "sphere")
        .definition()
        .sections()["DialogScript"]
        .contents()
    )
    handles = "".join(re.findall(r"^\s*bindhandle .*\n", sphere_script, re.M))
    script = hou.hda.definitionsInFile(str(hda))[0].sections()["DialogScript"]
    script.setContents(
        re.sub(
            r"^(\s*label\s+.*\n)",
            lambda m: m.group(1) + handles,
            script.contents(),
            count=1,
            flags=re.M,
        )
    )
    shutil.rmtree(hda.parent / "backup", ignore_errors=True)


def build_local_fog_volume():
    """HdUE Local Fog Volume: creates or edits an UnrealLocalFogVolume prim (UE's Local Fog Volume), as Karma Sky
    Atmosphere does Karma's sky, with ULocalFogVolumeComponent's properties as generated by generate_render_settings.py.
    Version 2.0; otls/hdue_localfogvolume_legacy.hda keeps 1.0 (create only) loading in saved scenes.
    """
    from generate_render_settings import local_fog_volume_settings
    from local_paths import local_path

    def guide(subnet, previous):
        # The volume's extent in the viewport: the unit sphere UE's base volume fills, a guide under the prim.
        sphere = subnet.createNode("sphere", "guide")
        sphere.setInput(0, previous)
        sphere.parm("primpath").set('`chs("../primpath")`/guide')
        sphere.parm("radius").set(1.0)
        purpose = subnet.createNode("configureprimitive", "guide_purpose")
        purpose.setInput(0, sphere)
        purpose.parm("primpattern").set('`chs("../primpath")`/guide')
        purpose.parm("setpurpose").set(True)
        purpose.parm("purpose").set("guide")
        return purpose

    build_prim_lop(
        OTLS / "hdue_localfogvolume.hda",
        f"hdue::hduelocalfogvolume::{LOCAL_FOG_VERSION}",
        "hdUE Local Fog Volume",
        "HDUE_hdue",
        "UnrealLocalFogVolume",
        "/fog/$OS",
        [
            (
                "Local Fog Volume",
                local_fog_volume_settings(local_path("engine_root")),
            )
        ],
        guide,
        "Creates or edits a UE Local Fog Volume: a sphere of fog that fades out radially, rendered by hdUE.",
        "Like Karma Sky Atmosphere for Karma, it creates or edits a prim hdUE renders: an UnrealLocalFogVolume, UE's "
        "Local Fog Volume (realtime only; UE's Path Tracer does not render local fog volumes). Uniform Scale is the "
        "radius in scene units (UE's volume is a sphere of radius 500 cm). In Create mode it authors every property, "
        "at UE's defaults too, and a guide sphere. In Edit mode every control starts at Set or Create, as on Houdini's "
        "prim LOPs: Initialize Parameters sets them to Do Nothing and reads the prim's property values (not its "
        "transform). UE "
        "draws no local fog nearer than 10 m to the camera unless r.LocalFogVolume.GlobalStartDistance is lowered "
        "(Engine Quality console variables). Height Fog Density 0 hides the whole volume: UE multiplies its radial "
        "and height coverage.",
    )


def render_settings_folders(*prefixes):
    """The parameters of the Render Settings LOP's Unreal Engine tab (HdUnrealEngine_Global.ds) whose attributes start
    with one of prefixes, in its tabs and sections, for a prim that carries the same UE properties (a post-process
    volume's settings, a height fog's, ...). Each keeps its control menu (Do Nothing by default: the prim's fallback,
    UE's default, or for a post-process volume, as UE's bOverride_ flags, no override), without the render settings'
    Essential | Advanced and render-mode conditions. Texture files are left out: these prims take cooked paths, which
    the backend does not import from image files.
    """
    import hou

    source = hou.node("/stage").createNode(
        "rendersettings", "post_process_source"
    )
    renderer = source.parmTemplateGroup().findFolder("Unreal Engine")
    source.destroy()

    def flatten(template):
        if template.type() == hou.parmTemplateType.Folder:
            for child in template.parmTemplates():
                yield from flatten(child)
        else:
            yield template

    values = {
        t.name()
        for t in flatten(renderer)
        if hou.text.decode(t.name()).startswith(prefixes)
        and not hou.text.decode(t.name()).endswith("_control")
        and not (
            t.type() == hou.parmTemplateType.String
            and t.stringType() == hou.stringParmType.FileReference
        )
    }

    def kept(template):
        decoded = hou.text.decode(template.name())
        if decoded.endswith("_control"):
            return hou.text.encode(decoded[: -len("_control")]) in values
        return template.name() in values

    def strip(template):
        if template.type() == hou.parmTemplateType.Folder:
            children = [
                strip(t)
                for t in template.parmTemplates()
                if t.type() == hou.parmTemplateType.Folder or kept(t)
            ]
            template.setParmTemplates([t for t in children if t is not None])
            for kind in (
                hou.parmCondType.HideWhen,
                hou.parmCondType.DisableWhen,
            ):
                template.setTabConditional(kind, "")
            return template if template.parmTemplates() else None
        conditionals = template.conditionals()
        template.setConditional(hou.parmCondType.HideWhen, "")
        disable = " ".join(
            term
            for term in re.findall(
                r"\{[^}]*\}", conditionals.get(hou.parmCondType.DisableWhen, "")
            )
            if "_control" in term
        )
        template.setConditional(hou.parmCondType.DisableWhen, disable)
        tags = dict(template.tags())
        tags.pop("hdue::essential_hidewhen", None)
        template.setTags(tags)
        return template

    # The tab's main settings above its tabs (Exposure Compensation, ...) in a first tab of their own.
    main = hou.FolderParmTemplate(
        "folder_main", "Main", folder_type=hou.folderType.Tabs
    )
    main.setParmTemplates(
        [
            t
            for t in renderer.parmTemplates()
            if t.type() != hou.parmTemplateType.Folder
        ]
    )
    folders = [
        strip(t)
        for t in [main, *renderer.parmTemplates()]
        if t.type() == hou.parmTemplateType.Folder
    ]
    return [f for f in folders if f is not None]


def build_post_process_volume():
    """HdUE Post Process Volume: creates or edits an UnrealPostProcessVolume prim (UE's APostProcessVolume), a box
    whose post-process settings UE blends into the view by priority, blend radius and weight.
    """
    from generate_render_settings import post_process_volume_settings
    from local_paths import local_path

    def guide(subnet, previous):
        # The volume's box in the viewport: the cube from -1 to 1 it fills, a guide under the prim.
        cube = subnet.createNode("cube", "guide")
        cube.setInput(0, previous)
        cube.parm("primpath").set('`chs("../primpath")`/guide')
        cube.parm("size").set(2.0)
        purpose = subnet.createNode("configureprimitive", "guide_purpose")
        purpose.setInput(0, cube)
        purpose.parm("primpattern").set('`chs("../primpath")`/guide')
        purpose.parm("setpurpose").set(True)
        purpose.parm("purpose").set("guide")
        return purpose

    build_prim_lop(
        OTLS / "hdue_postprocessvolume.hda",
        f"hdue::hduepostprocessvolume::{VERSION}",
        "hdUE Post Process Volume",
        "HDUE_hdue",
        "UnrealPostProcessVolume",
        "/postprocess/$OS",
        [
            (
                "Post Process Volume",
                post_process_volume_settings(local_path("engine_root")),
            )
        ],
        guide,
        "Creates or edits a UE Post Process Volume: a box whose post-process settings blend in while the camera is "
        "inside or near it.",
        "Like a Post Process Volume placed in a UE level, it creates or edits an UnrealPostProcessVolume prim: a box "
        "filling the cube from -1 to 1 of its local space (Uniform Scale 1 is a 2-unit box), with UE's Priority, Blend "
        "Radius (UE centimetres), Blend Weight, Enabled and Infinite Extent (Unbound). Its other tabs are the render "
        "settings' post-process settings: turn a setting's control to Set or Create to override it, as UE's override "
        "checkboxes. UE blends the volumes the camera is in or near, by priority, before the render settings' own "
        "post-process settings (which act as the camera's). Textures take cooked paths here, not image files.",
        extra=render_settings_folders(
            "unreal:postProcess:", "unreal:postProcessMaterials"
        ),
    )


def build_decal():
    """HdUE Decal: creates or edits an UnrealDecal prim (UE's decal actor): a box projecting its material along the
    prim's local down axis, with a guide box in Create mode.
    """
    from generate_render_settings import decal_settings
    from local_paths import local_path

    def guide(subnet, previous):
        # The decal's box in the viewport: the cube from -1 to 1 it fills, a guide under the prim.
        cube = subnet.createNode("cube", "guide")
        cube.setInput(0, previous)
        cube.parm("primpath").set('`chs("../primpath")`/guide')
        cube.parm("size").set(2.0)
        purpose = subnet.createNode("configureprimitive", "guide_purpose")
        purpose.setInput(0, cube)
        purpose.parm("primpattern").set('`chs("../primpath")`/guide')
        purpose.parm("setpurpose").set(True)
        purpose.parm("purpose").set("guide")
        return purpose

    build_prim_lop(
        OTLS / "hdue_decal.hda",
        f"hdue::hduedecal::{VERSION}",
        "hdUE Decal",
        "HDUE_hdue",
        "UnrealDecal",
        "/decals/$OS",
        [("Decal", decal_settings(local_path("engine_root")))],
        guide,
        "Creates or edits a UE decal: a box projecting its material onto the surfaces it covers.",
        "Like a Decal Actor placed in a UE level, it creates or edits an UnrealDecal prim: a box filling the cube from "
        "-1 to 1 of its local space (Uniform Scale 1 is a 2-unit box), projecting along the prim's local down axis. "
        "Its material is the material bound to the prim (Assign Material LOP), a Substrate Builder decal material "
        "(hdUE Substrate Material Builder, decal preset) prepared for hdUE, applied in Houdini's viewport; else Decal "
        "Material, a cooked Deferred Decal material path. Decal Color tints it, Sort Order orders overlapping decals, "
        "and Fade Screen Size hides it below that screen size.",
    )


# The scene components a UE level places once (UE draws one of each): (type name, label, prim type, attribute prefix,
# summary, overview).
SCENE_COMPONENTS = (
    (
        "hdueheightfog",
        "hdUE Height Fog",
        "UnrealExponentialHeightFog",
        "unreal:heightFog:",
        "Creates or edits a UE Exponential Height Fog: fog thickest below the prim's height.",
        "Like an Exponential Height Fog placed in a UE level, it creates or edits an UnrealExponentialHeightFog prim: "
        "the fog's density is Fog Density at the prim's height (its world position along the up axis), with UE's "
        "properties (Volumetric Fog included). A prim takes the place of hdUE Render Settings' Fog tab; UE draws one "
        "height fog, the first prim by path. Each property's control starts at Do Nothing: the prim's fallback is UE's "
        "default.",
    ),
    (
        "hdueskyatmosphere",
        "hdUE Sky Atmosphere",
        "UnrealSkyAtmosphere",
        "unreal:skyAtmosphere:",
        "Creates or edits a UE Sky Atmosphere: UE's physical sky, lit by the distant lights.",
        "Like a Sky Atmosphere placed in a UE level, it creates or edits an UnrealSkyAtmosphere prim with UE's "
        "properties (kilometres, as in UE). Distant lights light it as atmosphere suns. A prim takes the place of hdUE "
        "Render Settings' Sky Atmosphere; UE draws one, the first prim by path. Its position counts only when Transform "
        "Mode places the planet at the component.",
    ),
    (
        "hduevolumetriccloud",
        "hdUE Volumetric Cloud",
        "UnrealVolumetricCloud",
        "unreal:volumetricCloud:",
        "Creates or edits a UE Volumetric Cloud layer, drawn with a Sky Atmosphere.",
        "Like a Volumetric Cloud placed in a UE level, it creates or edits an UnrealVolumetricCloud prim with UE's "
        "properties; with no Material it uses UE's default cloud material. It needs a Sky Atmosphere (prim or render "
        "setting). A prim takes the place of hdUE Render Settings' Volumetric Cloud; UE draws one, the first prim by "
        "path.",
    ),
)


def build_scene_components():
    """HdUE Height Fog, Sky Atmosphere and Volumetric Cloud: create/edit LOPs for the prims that place UE's scene
    components, with the render settings' Fog and Sky properties.
    """
    for name, label, prim_type, prefix, summary, overview in SCENE_COMPONENTS:
        build_prim_lop(
            OTLS / f"hdue_{name[4:]}.hda",
            f"hdue::{name}::{VERSION}",
            label,
            "HDUE_hdue",
            prim_type,
            "/environment/$OS",
            [],
            None,
            summary,
            overview,
            extra=render_settings_folders(prefix),
        )


if __name__ == "__main__":
    if os.environ.get("HDUE_BUILD_HDA"):
        build()
        build_standard_render_vars()
        build_render_products()
        build_local_fog_volume()
        build_post_process_volume()
        build_scene_components()
        build_decal()
    else:
        relaunch()
