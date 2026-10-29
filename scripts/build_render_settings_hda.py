"""Build the hdUE Render Settings LOP (integrations/houdini/otls/hdue_rendersettings.hda).

Like Karma Render Settings, it wraps a Render Settings LOP, a Render Product and Render Vars, and shows the
camera, resolution, output picture and AOVs next to the hdUE settings, in the tabs of
soho/parameters/HdUEHoudiniRendererPlugin_Global.ds. The internal Render Settings LOP keeps those settings as
tagged spare parameters referencing the asset's, so it authors the same hdUE:* attributes, also where the
hdUE delegate is not registered. Run it after scripts/generate_render_settings.py or any edit of the .ds,
and commit the result:

    python scripts/build_render_settings_hda.py

It restarts itself in hython, with the integration and the delegate's plugInfo.json registered
(HDUE_HOUDINI_ROOT, HFS or houdini_root; HDUE_HOUDINI_BUILD_ROOT, houdini_build_root or build/houdini-22.0.466).
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / "integrations/houdini"
HDA = INTEGRATION / "otls/hdue_rendersettings.hda"
NAME, LABEL = "hdue_rendersettings", "hdUE Render Settings"
VARS = "/Render/Products/Vars/"
# AOVs the delegate renders besides the beauty (HdUEAovId in src/hydra/delegate.cpp):
# (render var and channel name, label, USD data type, husk format).
AOVS = [("depth", "Depth", "float", "float"), ("albedo", "Albedo", "color3f", "color3f"),
        ("normal", "Normal", "normal3f", "normal3f"), ("roughness", "Roughness", "float", "float"),
        ("metallic", "Metallic", "float", "float"), ("specular", "Specular", "float", "float"),
        ("occlusion", "Occlusion", "float", "float"), ("primId", "Prim ID", "int", "int"),
        ("instanceId", "Instance ID", "int", "int")]
AOV_HELP = ("Rendered after the beauty converges, from the same camera without Lumen, path tracing, anti-aliasing "
            "or post effects: one short raster render per AOV and frame.")
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
    paths = ROOT / ".local/paths.json"
    local = json.loads(paths.read_text()) if paths.exists() else {}
    houdini = Path(os.environ.get("HDUE_HOUDINI_ROOT") or os.environ.get("HFS") or local["houdini_root"])
    build = Path(os.environ.get("HDUE_HOUDINI_BUILD_ROOT") or local.get("houdini_build_root") or ROOT / "build/houdini-22.0.466")
    env = dict(os.environ, HOUDINI_PATH=f"{INTEGRATION};&", HDUE_HOUDINI_ENABLE="1", HDUE_BUILD_HDA="1",
               PXR_PLUGINPATH_NAME=os.pathsep.join([str(houdini / "bin/usd_plugins"), str(houdini / "houdini/dso/usd_plugins"),
                                                    str(build / "resources")]))
    sys.exit(subprocess.run([str(houdini / "bin/hython.exe"), __file__], env=env).returncode)


def help_page(group):
    """The node's help card (wiki markup), one entry per parameter from its help text, under its tabs and sections."""
    import hou

    lines = [f"= {LABEL} =", "", '#type: node', '#context: lop', f'#internal: {NAME}', '#icon: HDUE/hduerenderproperties', "",
             '"""Renders the stage with Unreal Engine (hdUE): render settings, camera, resolution, output and AOVs."""', "",
             "Like Karma Render Settings, it creates the render settings prim, its render product and render vars. "
             "Each setting's menu button chooses whether this node authors it; unauthored settings keep hdUE's defaults "
             "(see docs/rendering.md in the hdUE repository).", "", "@parameters", ""]

    def walk(templates, depth):
        for template in templates:
            if template.type() == hou.parmTemplateType.Folder:
                if template.parmTemplates():
                    lines.extend([("==" if depth == 0 else "===") + f" {template.label()} " + ("==" if depth == 0 else "==="), ""])
                    walk(template.parmTemplates(), depth + 1)
            elif template.help() and "_control" not in template.name():
                lines.extend([f"{template.label()}:", f"    #id: {template.name()}", f"    {template.help()}", ""])

    walk(group.entries(), 0)
    return "\n".join(lines)


def build():
    import hou

    network = hou.node("/stage")
    subnet = network.createNode("subnet", NAME)
    settings = subnet.createNode("rendersettings", "rendersettings")
    renderer = settings.parmTemplateGroup().findFolder("Unreal Engine")
    if renderer is None:
        raise SystemExit("The Render Settings LOP has no Unreal Engine tab: is the hdUE delegate registered?")
    settings.setInput(0, subnet.indirectInputs()[0])
    settings.parm("products").set("/Render/Products/renderproduct")
    product = subnet.createNode("renderproduct", "renderproduct")
    product.setInput(0, settings)
    previous = product
    for name, data_type, husk_format, source in [("C", "color4f", "color4f", "color")] + [(a[0], a[2], a[3], a[0]) for a in AOVS]:
        var = subnet.createNode("rendervar", name)
        var.setInput(0, previous)
        var.parm("sourceName").set(source)
        var.parm("dataType").set(data_type)
        var.parm(hou.text.encode("driver:parameters:aov:husk:format")).set(husk_format)
        if name == "C":
            previous = var
            continue
        switch = subnet.createNode("switch", "enable_" + name)  # the AOV's var exists only while it is on
        switch.setInput(0, previous)
        switch.setInput(1, var)
        switch.parm("input").setExpression(f'ch("../aov_{name}")')
        previous = switch
    subnet.node("output0").setInput(0, previous)
    product.parm("orderedVars").set(VARS + "C" + "".join(f' `ifs(ch("../aov_{a[0]}"), "{VARS}{a[0]}", "")`' for a in AOVS))
    product.parm("productName").set('`chs("../picture")`')
    subnet.layoutChildren()

    group = hou.ParmTemplateGroup()
    group.append(hou.StringParmTemplate("primpath", "Render Settings Prim", 1, default_value=("/Render/rendersettings",),
                                        help="Path of the UsdRenderSettings prim this node creates or edits."))
    for name, text in TOP_HELP.items():
        template = settings.parmTemplateGroup().find(name)
        template.setHelp(text)
        group.append(template)
    output = hou.FolderParmTemplate("hdUE_ImageOutputTab", "Image Output", folder_type=hou.folderType.Tabs)
    output.addParmTemplate(hou.StringParmTemplate("picture", "Output Picture", 1, default_value=("$HIP/render/$HIPNAME.$OS.$F4.exr",),
                                                  string_type=hou.stringParmType.FileReference,
                                                  help="Image the render product writes (husk, USD Render ROP)."))
    aovs = hou.FolderParmTemplate("hdUE_Aovs", "AOVs (Render Vars)", folder_type=hou.folderType.Simple)
    for name, label, _, _ in AOVS:
        aovs.addParmTemplate(hou.ToggleParmTemplate("aov_" + name, label, help=AOV_HELP))
    output.addParmTemplate(aovs)
    for tab in renderer.parmTemplates():
        if tab.label() == "Engine Quality":
            group.append(output)
        group.append(tab)
    subnet.setParmTemplateGroup(group)
    # The internal Render Settings LOP follows the asset's parameters.
    settings.parm("primpath").set('`chs("../primpath")`')
    # (The internal resolution's locked height expression computes from the referenced mode, camera and width.)
    for template in group.entriesWithoutFolders():
        if template.name() not in ("primpath", "resolutionMenu") and settings.parmTuple(template.name()):
            for parm, source in zip(settings.parmTuple(template.name()), subnet.parmTuple(template.name())):
                if not parm.isLocked():
                    parm.set(source)

    HDA.unlink(missing_ok=True)
    asset = subnet.createDigitalAsset(NAME, str(HDA), LABEL, min_num_inputs=0, max_num_inputs=1, ignore_external_references=True)
    definition = asset.type().definition()
    definition.setParmTemplateGroup(group)  # the subnet's spare parameters are not carried over
    definition.setIcon("HDUE_hduerenderproperties")
    definition.addSection("Tools.shelf", TOOL)
    definition.addSection("Help", help_page(group))
    definition.addSection("OnCreated", "import loputils\nloputils.updateResolutionParameters(kwargs['node'])\n")
    definition.setExtraFileOption("OnCreated/IsPython", True)
    definition.save(str(HDA), asset)
    shutil.rmtree(HDA.parent / "backup", ignore_errors=True)
    print(f"{HDA}: {len(definition.parmTemplateGroup().entriesWithoutFolders())} parameters")


if __name__ == "__main__":
    build() if os.environ.get("HDUE_BUILD_HDA") else relaunch()
