"""Build the hdUE Substrate VOPs (integrations/houdini/otls/hdue_substrate.hda) from scripts/substrate_graph.py.

They are locked leaf nodes, like MaterialX's VOPs. Like those, each carries a render mask (hdUE), by which a
Material Library LOP hands an hdUE Substrate Material Builder to integrations/houdini/husdplugins/shadertranslators/
hdue.py, and the TAB menu shows them under hdUE. The builder itself is no asset: like the Karma Material Builder,
it is a plain VOP subnet a tool sets up (hdue_substrate.tool, toolbar/hdUE.shelf). Run this after any edit of NODES
or the help below, and commit the result:

    python scripts/build_substrate_hda.py

It restarts itself in hython (HDUE_HOUDINI_ROOT, HFS or houdini_root).
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [
    str(ROOT / "scripts"),
    str(ROOT / "integrations/houdini/python3.13libs"),
]
ICON = "HDUE_hdue"
# Node summaries and help for each input and parameter, by node kind; HELP[None] holds what several kinds share.
HELP = {
    None: {
        "parameter_blending": "Blends both surfaces' parameters into one slab instead of evaluating both: cheaper, "
        "less accurate. Changing it recompiles the material.",
        "parameter_name": "Name of the material parameter, unique in the graph and not starting with hdUE_. "
        "Renaming it recompiles the material.",
        "value": "The value. Editing it updates the render without recompiling the material.",
    },
    "materialx": (
        "A MaterialX material on the stage, from the prepared MaterialX library, used as a Substrate surface.",
        {
            "material": "USD path of the MaterialX material, for example /Looks/Base. One per graph."
        },
    ),
    "slab": (
        "One Substrate slab: diffuse and specular reflection, with optional simple-volume subsurface scattering.",
        {
            "diffuse_albedo": "Diffuse color.",
            "f0": "Specular reflectance at normal incidence: about 0.04 for common dielectrics, 0.02 for water.",
            "roughness": "Specular roughness, from 0 (mirror) to 1.",
            "normal": "Shading normal from a Normal Map node. Unconnected, the mesh normal.",
            "mfp": "Mean free path of light in the slab, in scene units, used by Simple Volume subsurface.",
            "subsurface": "None: an opaque slab. Simple Volume: light scatters in the slab over its mean free path.",
        },
    ),
    "weight": (
        "Scales a surface's coverage.",
        {
            "surface": "Surface to weight.",
            "weight": "Coverage, from 0 (removed) to 1 (whole).",
        },
    ),
    "vertical": (
        "Layers one surface over another (Substrate vertical layering).",
        {
            "top": "Surface on top.",
            "base": "Surface underneath.",
            "thickness": "Thickness of the top layer, in UE centimeters, whatever the stage's units.",
        },
    ),
    "horizontal": (
        "Mixes two surfaces side by side (Substrate horizontal mixing).",
        {
            "background": "Surface where Mix is 0.",
            "foreground": "Surface where Mix is 1.",
            "mix": "Amount of the foreground, from 0 to 1.",
        },
    ),
    "scalar": (
        "A named float parameter of the material, editable on the material without recompiling.",
        {},
    ),
    "color": (
        "A named color parameter of the material, editable on the material without recompiling.",
        {},
    ),
    "constant": ("A constant float.", {}),
    "constant_color": ("A constant color.", {}),
    "multiply": (
        "Multiplies two floats.",
        {"a": "First factor.", "b": "Second factor."},
    ),
    "saturate": ("Clamps a float to [0, 1].", {"value": "Float to clamp."}),
    "primvar": (
        "Reads a float primvar of the mesh in [0, 1] as a mask, for example wetness. One per graph.",
        {
            "primvar": "Primvar name, without primvars:.",
            "default": "Value where the mesh has no such primvar, or for an empty name.",
        },
    ),
    "color_from_float": (
        "Copies a float into red, green and blue.",
        {"value": "Float to copy."},
    ),
    "unlit": (
        "An emissive surface without lighting.",
        {"emission": "Emitted color."},
    ),
    "uv": (
        "The mesh's UV set, scaled then offset.",
        {
            "scale": "UV scale.",
            "offset": "UV offset, added after the scale.",
            "index": "UV set. Only 0 (USD st) reaches UE.",
        },
    ),
    "texture": (
        "Samples an image file, or UDIM tiles from a <UDIM> pattern.",
        {
            "uv": "UV coordinates from a UV Coordinates node. Unconnected, UV set 0.",
            "file": "Image file, or a pattern with <UDIM>. Relative paths are relative to the HIP file.",
            "color_space": "sRGB for color images, Raw for data such as roughness, masks and normal maps.",
            "address": "How UVs outside [0, 1] sample the image.",
        },
    ),
    "channel": (
        "Extracts one channel of an RGBA value.",
        {
            "value": "RGBA value, usually a Texture Sample's RGBA output.",
            "channel": "Channel to extract.",
        },
    ),
    "normalmap": (
        "Decodes a tangent-space normal map for a Slab's Normal input.",
        {
            "color": "Encoded normal, from a Raw Texture Sample.",
            "strength": "Scales the normal's X and Y.",
            "flip_green": "On for OpenGL (+Y) maps such as MaterialX's, off for DirectX (-Y) maps.",
        },
    ),
}
MENUS = {
    "subsurface": {"none": "None", "simple_volume": "Simple Volume"},
    "color_space": {"srgb": "sRGB", "raw": "Raw"},
    "address": {"wrap": "Wrap", "clamp": "Clamp", "mirror": "Mirror"},
    "channel": {c: c.upper() for c in "rgba"},
}
TOOL = """<?xml version="1.0" encoding="UTF-8"?>
<shelfDocument>
  <tool name="$HDA_DEFAULT_TOOL" label="$HDA_LABEL" icon="$HDA_ICON">
    <toolMenuContext name="viewer">
      <contextNetType>VOP</contextNetType>
    </toolMenuContext>
    <toolMenuContext name="network">
      <contextOpType>$HDA_TABLE_AND_NAME</contextOpType>
    </toolMenuContext>
    <toolSubmenu>hdUE</toolSubmenu>
    <script scriptType="python"><![CDATA[import voptoolutils
voptoolutils.genericTool(kwargs, '$HDA_NAME')]]></script>
    <keywordList>
      <keyword>KEYWORD</keyword>
    </keywordList>
  </tool>
</shelfDocument>
"""


def help_text(kind, name):
    text = HELP[kind][1].get(name) or HELP[None].get(name)
    if not text:
        raise SystemExit(f"No help for {kind}.{name}: add it to HELP.")
    return text


def label(name):
    return name.replace("_", " ").title()


def parameter(kind, name, value_type, default):
    import hou

    args = dict(help=help_text(kind, name))
    if value_type in ("string", "asset"):
        template = hou.StringParmTemplate(
            name,
            label(name),
            1,
            default_value=(default,),
            **args,
            string_type=(
                hou.stringParmType.FileReference
                if value_type == "asset"
                else hou.stringParmType.Regular
            ),
        )
        if name in MENUS:
            template.setMenuItems(tuple(MENUS[name]))
            template.setMenuLabels(tuple(MENUS[name].values()))
            template.setMenuType(hou.menuType.Normal)
        return template
    if value_type == "bool":
        return hou.ToggleParmTemplate(
            name, label(name), default_value=default, **args
        )
    if value_type == "color3":
        return hou.FloatParmTemplate(
            name,
            label(name),
            3,
            default_value=default,
            naming_scheme=hou.parmNamingScheme.RGBA,
            look=hou.parmLook.ColorSquare,
            **args,
        )
    if value_type in ("vector2", "color4"):
        return hou.FloatParmTemplate(
            name, label(name), len(default), default_value=default, **args
        )
    return hou.FloatParmTemplate(
        name, label(name), 1, default_value=(default,), **args
    )


def connector(network, name, kind, value_type, default=None):
    node = network.createNode("subnetconnector", name)
    node.parm("connectorkind").set(kind)
    node.parm("parmname").set(name)
    node.parm("parmlabel").set(label(name))
    node.parm("parmtype").set(
        {
            "surface": "bsdf",
            "color3": "color",
            "color4": "coloralpha",
            "vector2": "float2",
        }.get(value_type, value_type)
    )
    if kind == "input" and default is not None:
        name = {
            "float": "floatdef",
            "color3": "colordef",
            "vector2": "float2def",
            "color4": "color4def",
        }[value_type]
        node.parmTuple(name).set(
            default if isinstance(default, tuple) else (default,)
        )
    return node


def help_page(kind, summary, group, inputs):
    """The node's help card (wiki markup): summary, inputs and parameters with their help."""
    from hdue_substrate import type_name

    lines = [
        f"= {NODES[kind]['label']} =",
        "",
        "#type: node",
        "#context: vop",
        f"#internal: {type_name(kind)}",
        f"#icon: {ICON.replace('_', '/', 1)}",
        "",
        f'"""{summary}"""',
        "",
        "It is part of an hdUE Substrate Material Builder graph, which hdUE prepares into a UE Substrate material; it does not run as a VEX or MaterialX shader.",
        "",
    ]
    if inputs:
        lines += ["@inputs", ""] + [
            line
            for name, text in inputs
            for line in (f"{label(name)}:", f"    {text}", "")
        ]
    lines += ["@parameters", ""]
    for template in group.entriesWithoutFolders():
        lines += [
            f"{template.label()}:",
            f"    #id: {template.name()}",
            f"    {template.help()}",
            "",
        ]
    return "\n".join(lines)


def finish(asset, kind, summary, group, inputs, keyword):
    from hdue_substrate import type_name

    definition = asset.type().definition()
    # VOP asset creation generates parameters from the connectors and resets numeric defaults.
    definition.setParmTemplateGroup(group)
    # An external shader, as Karma's kma_* and MaterialX's hmtlx* VOPs: no network inside, a render mask selecting
    # the hdUE shader translator and a shader type, in the dialog script as Type Properties > Node writes them.
    # Surface nodes are surface shaders, so a Material Library translates the one wired to a Material Builder's
    # surface output (with the whole builder graph); the others are generic, as kma_ao.
    shader_type = (
        "surface" if "surface" in outputs(NODES[kind]).values() else "generic"
    )
    script = definition.sections()["DialogScript"].contents()
    definition.addSection(
        "DialogScript",
        re.sub(
            r"(\n    label\s.*\n)",
            rf"\1    rendermask\thdUE\n    externalshader 1\n    shadertype\t{shader_type}\n",
            script,
            count=1,
        ),
    )
    definition.addSection("FunctionName", type_name(kind))
    # Type Properties' VopNet mask, which TAB menu masks test.
    definition.setExtraInfo(f"shadertype={shader_type} vopnetmask='{keyword}' ")
    definition.setIcon(ICON)
    definition.addSection("Tools.shelf", TOOL.replace("KEYWORD", keyword))
    definition.addSection("Help", help_page(kind, summary, group, inputs))
    definition.addSection(
        "HDUE.Substrate.json", json.dumps(dict(version=1, operation=kind))
    )


def build():
    import hou
    from hdue_substrate import HDA, type_name

    HDA.unlink(missing_ok=True)
    scratch = hou.node("/mat").createNode("subnet", "hdue_definition_build")
    for kind, definition in NODES.items():
        node = scratch.createNode("subnet", kind)
        for name, (value_type, default) in definition["inputs"].items():
            connector(node, name, "input", value_type, default)
        for port, value_type in outputs(definition).items():
            connector(node, port, "output", value_type)
        group = hou.ParmTemplateGroup()
        for source in ("inputs", "properties"):
            for name, (value_type, default) in definition[source].items():
                if default is not None:
                    group.append(parameter(kind, name, value_type, default))
        node.setParmTemplateGroup(group)
        for template in group.entries():
            node.parmTuple(template.name()).revertToDefaults()
        asset = node.createDigitalAsset(
            name=type_name(kind),
            hda_file_name=str(HDA),
            description=definition["label"],
            min_num_inputs=0,
            max_num_inputs=len(definition["inputs"]),
        )
        finish(
            asset,
            kind,
            HELP[kind][0],
            group,
            [(n, help_text(kind, n)) for n in definition["inputs"]],
            "hdUE",
        )
        asset.setMaterialFlag(False)
        asset.destroy()
    scratch.destroy()
    # Saving embeds a copy of a resolvable icon; keep the name only (config/Icons on HOUDINI_PATH resolves it).
    for saved in hou.hda.definitionsInFile(str(HDA)):
        if "IconSVG" in saved.sections():
            saved.removeSection("IconSVG")
            saved.setIcon(ICON)
        saved.removeSection("Contents.gz")  # external shaders have no network
    shutil.rmtree(HDA.parent / "backup", ignore_errors=True)
    # Assets made from subnets stay subnets in the library index; mark them as SideFX's shaderhda.py writes
    # external shader VOPs (Subnet: false), with hotl as it does, so they are leaf nodes, not networks to dive in.
    hotl = Path(os.environ["HFS"]) / "bin/hotl"
    with tempfile.TemporaryDirectory() as expanded:
        subprocess.run([hotl, "-x", expanded, HDA], check=True)
        index = Path(expanded) / "INDEX__SECTION"
        index.write_text(
            re.sub(r"(?m)^(Subnet:\s*)true$", r"\1false", index.read_text())
        )
        subprocess.run([hotl, "-l", expanded, HDA], check=True)
    print(f"{HDA}: {len(NODES)} node types")


if __name__ == "__main__":
    if os.environ.get("HDUE_BUILD_HDA"):
        from substrate_graph import NODES, outputs

        build()
    else:
        from local_paths import relaunch

        relaunch(__file__)
