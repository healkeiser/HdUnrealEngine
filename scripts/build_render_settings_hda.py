"""Build the hdUE Render Settings LOP (integrations/houdini/otls/hdue_rendersettings.hda).

Like Karma Render Settings, it wraps a Render Settings LOP, a Render Product and Render Vars, and shows the
camera, resolution, output picture and AOVs next to the hdUE settings, in the tabs of
soho/parameters/HdUEHoudiniRendererPlugin_Global.ds. The internal Render Settings LOP keeps those settings as
tagged spare parameters referencing the asset's, so it authors the same hdUE:* attributes, also where the
hdUE delegate is not registered. Like Karma Standard Render Vars and Karma Render Products, hdUE Standard Render
Vars (hdue_standardrendervars.hda) and hdUE Render Products (hdue_renderproducts.hda) author render vars and
products upstream, which hdUE Render Settings can render. Run it after scripts/generate_render_settings.py or any
edit of the .ds, and commit the result:

    python scripts/build_render_settings_hda.py

It restarts itself in hython, with the integration and the delegate's plugInfo.json registered
(HDUE_HOUDINI_ROOT, HFS or houdini_root; HDUE_HOUDINI_BUILD_ROOT, houdini_build_root or build/houdini).
"""

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
NAME, LABEL, ICON = (
    f"hdue::rendersettings::{VERSION}",
    "hdUE Render Settings",
    "HDUE_hduerenderproperties",
)
VARS = "/Render/Products/Vars/"
# AOVs the delegate renders besides the beauty (HdUEAovId in src/hydra/delegate.cpp):
# (render var and channel name, label, USD data type, husk format).
AOVS = [
    ("depth", "Depth", "float", "float"),
    ("albedo", "Albedo", "color3f", "color3f"),
    ("normal", "Normal", "normal3f", "normal3f"),
    ("roughness", "Roughness", "float", "float"),
    ("metallic", "Metallic", "float", "float"),
    ("specular", "Specular", "float", "float"),
    ("occlusion", "Occlusion", "float", "float"),
    ("primId", "Prim ID", "int", "int"),
    ("instanceId", "Instance ID", "int", "int"),
]
AOV_HELP = (
    "Rendered after the beauty converges, from the same camera without Lumen, path tracing, anti-aliasing "
    "or post effects: one short raster render per AOV and frame."
)
# hdUE Standard Render Vars: what each var holds, its sections, the husk formats of its 16 and 32 bit precisions.
AOV_CONTENTS = {
    "C": "The beauty: the rendered image with alpha (color4f, sourceName color).",
    "depth": "Linear axial depth from the camera in stage units (float).",
    "albedo": "Base color of the nearest opaque surface (color3f, Substrate's first BSDF).",
    "normal": "World-space shading normal in USD axes, normalized (normal3f).",
    "roughness": "Roughness of the nearest opaque surface (float).",
    "metallic": "Metallic of the nearest opaque surface (float).",
    "specular": "Specular of the nearest opaque surface (float).",
    "occlusion": "Material ambient occlusion of the nearest opaque surface (float).",
    "primId": "Hydra prim ID of the visible mesh, -1 for background (int).",
    "instanceId": "Hydra instance index of an instanced prototype, -1 elsewhere (int).",
}
VAR_SECTIONS = [
    ("Beauty", ["C"]),
    (
        "Surface",
        ["albedo", "normal", "roughness", "metallic", "specular", "occlusion"],
    ),
    ("Utility", ["depth", "primId", "instanceId"]),
]
PRECISIONS = {
    "color4f": ("color4h", "color4f"),
    "color3f": ("color3h", "color3f"),
    "normal3f": ("half3", "float3"),
    "float": ("half", "float"),
}
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
TOOL = """<?xml version="1.0" encoding="UTF-8"?>
<shelfDocument>
  <tool name="$HDA_DEFAULT_TOOL" label="$HDA_LABEL" icon="$HDA_ICON">
    <toolMenuContext name="viewer">
      <contextNetType>LOP</contextNetType>
    </toolMenuContext>
    <toolMenuContext name="network">
      <contextOpType>$HDA_TABLE_AND_NAME</contextOpType>
    </toolMenuContext>
    <toolSubmenu>Rendering</toolSubmenu>
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
    summary="Renders the stage with Unreal Engine (hdUE): render settings, camera, resolution, output and AOVs.",
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
    default = subnet.createNode("configurelayer", "default_settings")
    default.setInput(0, settings)
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

    group = hou.ParmTemplateGroup()
    group.append(
        hou.StringParmTemplate(
            "primpath",
            "Render Settings Prim",
            1,
            default_value=("/Render/rendersettings",),
            help="Path of the UsdRenderSettings prim this node creates or edits.",
        )
    )
    group.append(
        hou.ToggleParmTemplate(
            "setlayerrendersettings",
            "Set as Default Render Settings Prim",
            default_value=True,
            help="Makes this prim the stage's default render settings (renderSettingsPrimPath), which the viewport and husk "
            "render with, also where the input stage brings render settings of its own.",
        )
    )
    for name, text in TOP_HELP.items():
        template = settings.parmTemplateGroup().find(name)
        template.setHelp(text)
        group.append(template)
    output = hou.FolderParmTemplate(
        "hdUE_ImageOutputTab", "Image Output", folder_type=hou.folderType.Tabs
    )
    no_builtin = {hou.parmCondType.HideWhen: "{ outputproducts == upstream }"}
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
            help="Which render products the render settings render: this node's own product (Output Picture and AOVs below), "
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
    output.addParmTemplate(
        hou.StringParmTemplate(
            "picture",
            "Output Picture",
            1,
            default_value=("$HIP/render/$HIPNAME.$OS.$F4.exr",),
            string_type=hou.stringParmType.FileReference,
            conditionals=no_builtin,
            help="Image the built-in render product writes (husk, USD Render ROP).",
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
            hou.ToggleParmTemplate("aov_" + name, label, help=AOV_HELP)
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
    output.addParmTemplate(aovs)
    for tab in renderer.parmTemplates():
        if tab.label() == "Engine Quality":
            group.append(output)
        group.append(without_controls(tab))
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
            if template.name().startswith("xn__hdUE")
            else None
        )
        if control:
            control.setExpression(
                control_expression(template, subnet.parmTuple(template.name()))
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
    shutil.rmtree(hda.parent / "backup", ignore_errors=True)
    print(
        f"{hda}: {len(definition.parmTemplateGroup().entriesWithoutFolders())} parameters"
    )


def parent_path(default, what):
    """The Parent Primitive Path parameter of the nodes creating render vars or products."""
    import hou

    return hou.StringParmTemplate(
        "parentprimpath",
        "Parent Primitive Path",
        1,
        default_value=(default,),
        tags={"sidefx::usdpathtype": "prim"},
        help=f"Path of the prim under which this node creates its {what}.",
    )


def build_standard_render_vars():
    """HdUE Standard Render Vars: a render var per output hdUE renders, as Karma Standard Render Vars does for Karma."""
    import hou

    network = hou.node("/stage")
    name, label = (
        f"hdue::standardrendervars::{VERSION}",
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
                    + ("" if var == "C" else " " + AOV_HELP),
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
            "is rendered as converged; the other AOVs are read from the GBuffer of the nearest opaque surface "
            "(see AOVs in docs/houdini.md in the hdUE repository).",
        ),
    )


def build_render_products():
    """HdUE Render Products: render products from a multiparm, as Karma Render Products, with a For Each loop."""
    import hou

    network = hou.node("/stage")
    name, label = (
        f"hdue::renderproducts::{VERSION}",
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


if __name__ == "__main__":
    if os.environ.get("HDUE_BUILD_HDA"):
        build()
        build_standard_render_vars()
        build_render_products()
    else:
        relaunch()
