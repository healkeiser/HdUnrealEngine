"""Author a reversible wet override on selected composed USD prims: a Render Geometry Settings LOP, whose hdUE tab
holds the unreal:surface:wet:* attributes (soho/parameters/HdUnrealEngine_Geometry.ds), with its wet controls set.
"""

import hou
from pxr import Sdf

PREFIX = "unreal:surface:wet:"  # scripts/surface_effects.py, which reads them


def create(paths=None, input_node=None):
    import hdue_houdini

    viewer = hdue_houdini.scene_viewer()
    if paths is None:
        paths = list(viewer.currentSceneGraphSelection())
    paths = [str(p) for p in paths]
    if not paths:
        raise ValueError(
            "Select one or more Mesh, GeomSubset or ancestor prims in the Scene Graph Tree."
        )
    for path in paths:
        if (
            not Sdf.Path.IsValidPathString(path)
            or not Sdf.Path(path).IsAbsolutePath()
            or not Sdf.Path(path).IsPrimPath()
        ):
            raise ValueError("Expected an absolute prim path: " + path)
    input_node = input_node or viewer.pwd().displayNode()
    if not input_node:
        raise ValueError(
            "Display a Solaris input stage before creating a wet override."
        )
    with hou.undos.group("Create hdUE wet surface override"):
        node = input_node.parent().createNode(
            "rendergeometrysettings", "hdUE_wet_surface"
        )
        node.setInput(0, input_node)
        node.parm("primpattern").set(" ".join(paths))
        # Set or Create on the four wet attributes, the coating on, the rest at the tab's defaults.
        for name in ("enabled", "amount", "roughness", "maskPrimvar"):
            node.parm(hou.text.encode(PREFIX + name + "_control")).set("set")
        node.parm(hou.text.encode(PREFIX + "enabled")).set(True)
        node.setDisplayFlag(True)
        node.setCurrent(True, clear_all_selected=True)
        node.moveToGoodPosition()
    return node


def parm(node, name):
    """The override's parameter for unreal:surface:wet:<name> (enabled, amount, roughness, maskPrimvar)."""
    return node.parm(hou.text.encode(PREFIX + name))
