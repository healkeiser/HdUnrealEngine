"""Run with uv run -- <HFS>/bin/hython.exe tests/substrate_builder_test.py: the hdUE Substrate Material Builder is a
plain VOP subnet, as SideFX's Karma Material Builder, made by its TAB menu / shelf tool, holding leaf Substrate VOPs,
and a Material Library authors it; builders saved as the earlier asset load, author and upgrade.
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = str(ROOT / "integrations/houdini")
# The Material Library finds hdUE's shader translator (husdplugins) and OPcustomize on HOUDINI_PATH.
if not os.environ.get("HDUE_SUBSTRATE_BUILDER_TEST"):
    sys.exit(
        subprocess.run(
            [str(Path(os.environ["HFS"]) / "bin/hython"), __file__],
            env=dict(
                os.environ,
                HOUDINI_PATH=INTEGRATION + ";&",
                HDUE_SUBSTRATE_BUILDER_TEST="1",
            ),
        ).returncode
    )

import hou  # noqa: E402
from pxr import Usd  # noqa: E402

sys.path.insert(0, str(ROOT / "integrations/houdini/python3.13libs"))
import hdue_substrate  # noqa: E402

hdue_substrate.install()
# The tool, from a shelf (no network editor): a builder in /mat.
shelf = ROOT / "integrations/houdini/toolbar/hdUE.shelf"
hou.shelves.loadFile(str(shelf))
tool = hou.shelves.tool("vop_hduesubstratebuilder")
assert "<contextOpType>Vop/materialbuilder</contextOpType>" in shelf.read_text()
exec(tool.script(), {"kwargs": {}})
builder = hou.node("/mat/substrate")
assert builder.type().name() == "subnet" and builder.type().definition() is None
assert builder.isMaterialFlagSet() and hdue_substrate.is_builder(builder)
assert builder.parm("tabmenumask").eval() == "hdUE subnetconnector"
assert builder.parm("shader_rendercontextname").eval() == "unreal"
assert sorted((c.name(), c.type().name()) for c in builder.children()) == [
    ("slab", hdue_substrate.type_name("slab")),
    ("surface_output", "subnetconnector"),
]
assert builder.node("surface_output").inputs() == (builder.node("slab"),)
# Leaf nodes are external shaders, as Karma's and MaterialX's VOPs: no network inside.
slab = builder.node("slab")
assert not slab.isSubNetwork() and slab.renderMask() == "hdUE"
assert slab.shaderType() == hou.shaderType.Surface

stage = hou.node("/stage")
library = stage.createNode("materiallibrary")
library.parm("materials").set(1)
library.parm("matnode1").set(builder.path())
library.parm("matpath1").set("/Looks/Builder")
slab.parm("roughness").set(0.25)


def authored(path):
    prim = library.stage().GetPrimAtPath(path)
    assert not library.errors(), library.errors()
    assert prim.GetAttribute("unreal:substrate:version").Get() == 1, path
    return {
        str(p.GetPath()): {
            a.GetName(): a.Get() for a in p.GetAuthoredAttributes()
        }
        for p in Usd.PrimRange(prim)
    }


graph = authored("/Looks/Builder")
assert abs(graph["/Looks/Builder/Graph/slab"]["inputs:roughness"] - 0.25) < 1e-6
# A Texture Sample's file has Houdini's image chooser and still authors an asset path.
texture = builder.createNode(hdue_substrate.type_name("texture"), "texture")
template = texture.parm("file").parmTemplate()
assert template.stringType() == hou.stringParmType.FileReference
assert template.fileType() == hou.fileType.Image
assert template.tags()["filechooser_mode"] == "read"
assert "*.exr" in template.tags()["filechooser_pattern"]
texture.parm("file").set("$HIP/tex/base.png")
slab.setNamedInput("diffuse_albedo", texture, "out")
file = (
    library.stage()
    .GetPrimAtPath("/Looks/Builder/Graph/texture")
    .GetAttribute("inputs:file")
)
assert str(file.GetTypeName()) == "asset" and file.Get().path.replace(
    "\\", "/"
).endswith("/tex/base.png"), file

# A builder saved as the earlier asset (otls/hdue_substrate_builder_legacy.hda): it loads, authors and upgrades.
legacy = library.createNode(hdue_substrate.type_name("builder"), "legacy")
assert hdue_substrate.is_builder(legacy)
legacy.node("slab").parm("roughness").set(0.6)
library.parm("matnode1").set("legacy")
library.parm("matpath1").set("/Looks/Legacy")
before = authored("/Looks/Legacy")
upgraded = hdue_substrate.upgrade(legacy)
assert upgraded.type().name() == "subnet" and hdue_substrate.is_builder(
    upgraded
)
assert authored("/Looks/Legacy") == before
print("substrate builder: ok")
