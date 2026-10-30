"""Substrate authoring in native Houdini VOP networks; no VEX evaluation."""

import json
from pathlib import Path

import hou
import voptoolutils
from pxr import Sdf, UsdShade

ROOT = Path(__file__).resolve().parents[3]
import sys

if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))
from substrate_graph import ERROR_ATTRIBUTE, NODES, author, outputs, validate

PREFIX = "hdue::substrate_"
HDA_VERSION = "1.3"
REVISION_PARM = "hdue_graph_revision"
HDA = Path(__file__).resolve().parents[1] / "otls/hdue_substrate.hda"
# The earlier builder asset (hdue::substrate_builder::1.3), kept so scenes saved with it load; see upgrade().
LEGACY_HDA = (
    Path(__file__).resolve().parents[1]
    / "otls/hdue_substrate_builder_legacy.hda"
)
# A builder is a plain VOP subnet, as SideFX's Karma Material Builder (voptoolutils.createMaskedMtlXSubnet): its
# render context marks it, and its TAB menu offers the hdUE Substrate nodes and the subnet output connector only.
RENDER_CONTEXT_PARM = "shader_rendercontextname"
RENDER_CONTEXT = "hdue"
TAB_MASK = "hdUE subnetconnector"


def _revision_parameter():
    parm = hou.IntParmTemplate(
        REVISION_PARM, "Graph revision", 1, default_value=(0,)
    )
    parm.hide(True)
    return parm


def type_name(kind):
    return PREFIX + kind + "::" + HDA_VERSION


def install():
    """Load the shipped Substrate VOPs where the integration is not on HOUDINI_PATH (plain hython)."""
    if not hou.nodeType(hou.vopNodeTypeCategory(), type_name("slab")):
        hou.hda.installFile(str(HDA))
    if not hou.nodeType(hou.vopNodeTypeCategory(), type_name("builder")):
        hou.hda.installFile(str(LEGACY_HDA))


def is_builder(node):
    """An hdUE Substrate Material Builder: a VOP subnet whose render context is hdue (set up by builder()), or a
    builder saved as the earlier hdue::substrate_builder::1.3 asset.
    """
    if node.type().name() == type_name("builder"):
        return True
    parm = node.parm(RENDER_CONTEXT_PARM)
    return (
        node.type().name() == "subnet"
        and parm is not None
        and parm.evalAsString() == RENDER_CONTEXT
    )


def builder(subnet):
    """Make a VOP subnet an hdUE Substrate Material Builder, as voptoolutils.createMaskedMtlXSubnet makes the
    Karma Material Builder: the same Material Builder parameters (TAB menu mask limited to the hdUE Substrate
    nodes, render context hdue), the material flag, and a Slab wired to a surface output.
    """
    voptoolutils._addMtlSubnetParms(
        subnet, TAB_MASK, RENDER_CONTEXT, "hdUE Substrate Material Builder"
    )
    for child in subnet.children():
        child.destroy()
    output = subnet.createNode("subnetconnector", "surface_output")
    output.setParms(
        dict(
            parmname="surface",
            parmlabel="Surface",
            parmtype="surface",
            connectorkind="output",
            useasparmdefiner=0,
        )
    )
    output.setColor(hou.Color((0.89, 0.69, 0.6)))  # as Karma's surface output
    output.setInput(0, subnet.createNode(type_name("slab"), "slab"))
    subnet.layoutChildren()
    subnet.setMaterialFlag(True)
    return subnet


def tool(kwargs):
    """TAB menu and shelf tool: an hdUE Substrate Material Builder where the user places it, or in /mat from a
    shelf when no network editor shows a material network.
    """
    install()
    pane = kwargs.get("pane")
    if not (
        isinstance(pane, hou.NetworkEditor)
        and pane.pwd().childTypeCategory() == hou.vopNodeTypeCategory()
    ):
        return create()
    with hou.undos.group("Create Substrate Builder"):
        return builder(voptoolutils.genericTool(kwargs, "subnet", "substrate"))


def upgrade(node):
    """Turn an hdue::substrate_builder::1.3 asset (scenes before the Material Builder subnet) into an hdUE Substrate
    Material Builder subnet, keeping its name, network and wiring; returns the subnet.
    """
    subnet = node.changeNodeType(
        "subnet", keep_network_contents=True, keep_parms=False
    )
    voptoolutils._addMtlSubnetParms(
        subnet, TAB_MASK, RENDER_CONTEXT, "hdUE Substrate Material Builder"
    )
    subnet.setMaterialFlag(True)
    return subnet


def create(parent=None, preset="slab", material="/Looks/Base"):
    install()
    parent = parent or hou.node("/mat")
    with hou.undos.group("Create Substrate Builder"):
        network = builder(parent.createNode("subnet", "substrate"))
        output = network.node("surface_output")
        if preset == "wet":
            network.node("slab").destroy()
            base = network.createNode(type_name("materialx"), "base")
            base.parm("material").set(material)
            water = network.createNode(type_name("slab"), "water")
            water.parmTuple("diffuse_albedo").set((0, 0, 0))
            f0 = ((1.33 - 1) / (1.33 + 1)) ** 2
            water.parmTuple("f0").set((f0, f0, f0))
            water.parmTuple("mfp").set((1e6, 1e6, 1e6))
            water.parm("subsurface").set("simple_volume")
            rough = network.createNode(type_name("scalar"), "roughness")
            rough.parm("parameter_name").set("WaterRoughness")
            rough.parm("value").set(0.08)
            water.setInput(2, rough)
            amount = network.createNode(type_name("scalar"), "amount")
            amount.parm("parameter_name").set("Coverage")
            amount.parm("value").set(1)
            saturate = network.createNode(type_name("saturate"), "clamp_amount")
            saturate.setInput(0, amount)
            mask = network.createNode(type_name("primvar"), "mask")
            mask.parm("primvar").set("wetness")
            multiply = network.createNode(type_name("multiply"), "coverage")
            multiply.setInput(0, saturate)
            multiply.setInput(1, mask)
            weight = network.createNode(type_name("weight"), "weight")
            weight.setInput(0, water)
            weight.setInput(1, multiply)
            layer = network.createNode(type_name("vertical"), "layer")
            layer.setInput(0, weight)
            layer.setInput(1, base)
            layer.parm("thickness").set(0.01)
            output.setInput(0, layer)
        elif preset != "slab":
            network.destroy()
            raise ValueError("Unknown Substrate preset: " + preset)
        network.layoutChildren()
        network.moveToGoodPosition()
        network.setCurrent(True, clear_all_selected=True)
    return network


def graph(network):
    output_nodes = [
        n
        for n in network.children()
        if n.type().name() == "subnetconnector"
        and n.parm("connectorkind").evalAsString() == "output"
    ]
    if (
        len(output_nodes) != 1
        or not output_nodes[0].inputs()
        or not output_nodes[0].inputs()[0]
    ):
        raise ValueError("Substrate Builder needs one connected surface output")
    nodes = {}
    active = set()

    def visit(node):
        name = node.name()
        if name in active:
            raise ValueError("Substrate VOP cycle at " + node.path())
        if name in nodes:
            return name
        definition = node.type().definition()
        section = (
            definition.sections().get("HDUE.Substrate.json")
            if definition
            else None
        )
        if not section:
            raise ValueError(
                "Unsupported VOP in Substrate Builder: " + node.path()
            )
        kind = json.loads(section.contents())["operation"]
        if kind not in NODES:
            raise ValueError("Nested Substrate Builders are not supported yet")
        schema = NODES[kind]
        result = dict(type=kind, inputs={}, properties={})
        active.add(name)
        upstream = node.inputs()
        connections = {c.inputIndex(): c for c in node.inputConnections()}
        for index, (port, (value_type, default)) in enumerate(
            schema["inputs"].items()
        ):
            source = upstream[index] if index < len(upstream) else None
            if source:
                source_name = visit(source)
                connection = connections[index]
                output_index = connection.outputIndex()
                source_definition = json.loads(
                    source.type()
                    .definition()
                    .sections()["HDUE.Substrate.json"]
                    .contents()
                )["operation"]
                source_outputs = list(outputs(NODES[source_definition]))
                if output_index >= len(source_outputs):
                    raise ValueError(
                        "Unsupported source output: " + source.path()
                    )
                result["inputs"][port] = dict(node=source_name)
                if source_outputs[output_index] != "out":
                    result["inputs"][port]["output"] = source_outputs[
                        output_index
                    ]
            elif default is not None:
                result["inputs"][port] = (
                    list(node.evalParmTuple(port))
                    if value_type in ("color3", "vector2", "color4")
                    else node.evalParm(port)
                )
        for prop, (value_type, default) in schema["properties"].items():
            value = (
                list(node.evalParmTuple(prop))
                if value_type in ("color3", "vector2", "color4")
                else (
                    bool(node.evalParm(prop))
                    if value_type == "bool"
                    else node.evalParm(prop)
                )
            )
            if (
                value_type == "asset"
                and value
                and not value.startswith(("op:", "opdef:"))
            ):
                value = str(
                    (Path(hou.hipFile.path()).parent / Path(value)).resolve()
                )
            result["properties"][prop] = value
        nodes[name] = result
        active.remove(name)
        return name

    root = visit(output_nodes[0].inputs()[0])
    return validate(dict(version=1, output=root, nodes=nodes))


def translate(stage, path, network, replace=True):
    """Author the builder's graph as the material at path. An incomplete graph, an ordinary editing state,
    authors its error on the material instead, so the stage stays valid and later repairs are observed.
    """
    try:
        return author(stage, path, graph(network), replace)
    except ValueError as error:
        existing = stage.GetPrimAtPath(path)
        if (
            replace
            and existing
            and existing.GetAttribute("hdue:substrate:version").Get() != 1
        ):
            raise
        material = UsdShade.Material.Define(stage, path)
        material.GetPrim().CreateAttribute(
            "hdue:substrate:version", Sdf.ValueTypeNames.Int
        ).Set(1)
        material.GetPrim().CreateAttribute(
            ERROR_ATTRIBUTE, Sdf.ValueTypeNames.String
        ).Set(str(error))
        return material


def author_lop(
    network, material_path="/Looks/Substrate", input_node=None, prim_paths=()
):
    """A Material Library LOP authoring the builder at material_path, bound to prim_paths: the builder's own
    library when it is in one, else a new one after input_node that references it.
    """
    if (
        not Sdf.Path.IsValidPathString(material_path)
        or not Sdf.Path(material_path).IsAbsolutePath()
        or not Sdf.Path(material_path).IsPrimPath()
    ):
        raise ValueError("Expected an absolute USD material prim path")
    validate(graph(network))
    library = network.parent()
    with hou.undos.group("Author Substrate material"):
        if library.type().name() == "materiallibrary":
            node = network.name()
        else:
            parent = input_node.parent() if input_node else hou.node("/stage")
            library = parent.createNode(
                "materiallibrary", "hdUE_substrate_material"
            )
            node = network.path()
            if input_node:
                library.setInput(0, input_node)
            library.moveToGoodPosition()
        library.parm("materials").set(1)
        library.setParms(
            dict(
                matnode1=node,
                matpath1=material_path,
                assign1=bool(prim_paths),
                geopath1=" ".join(prim_paths),
            )
        )
        library.setDisplayFlag(True)
        library.setCurrent(True, clear_all_selected=True)
    return library


def cook(node):
    """Legacy: scenes saved before Material Library translation author through a Python LOP calling this."""
    network = hou.node(node.evalParm("hdue_builder"))
    if not network:
        raise ValueError("Substrate VOP network not found")
    # Wiring notifications dirty a hidden revision parm; they never force a cook from an event handler.
    _watch_structure(network, node)
    node.evalParm(REVISION_PARM)
    stage = node.editableStage()
    material = translate(stage, node.evalParm("hdue_material"), network)
    if material.GetPrim().GetAttribute(ERROR_ATTRIBUTE).Get():
        return
    for path in node.evalParm("hdue_bindings").split():
        prim = stage.GetPrimAtPath(path)
        if not prim:
            raise ValueError("Substrate binding target not found: " + path)
        UsdShade.MaterialBindingAPI.Apply(prim).Bind(material)


_structure_watchers = {}


def _watch_structure(network, target):
    identity = (network.sessionId(), target.sessionId())
    if identity in _structure_watchers:
        return
    if not target.parm(REVISION_PARM):
        with hou.undos.disabler():
            target.addSpareParmTuple(_revision_parameter())
    events = (
        hou.nodeEventType.InputRewired,
        hou.nodeEventType.ChildCreated,
        hou.nodeEventType.ChildDeleted,
    )
    watched = {}
    pending = [False]
    alive = [True]

    def notify():
        pending[0] = False
        if not alive[0]:
            return
        try:
            register()
            with hou.undos.disabler():
                parm = target.parm(REVISION_PARM)
                parm.set((parm.eval() + 1) % 2000000000)
        except (hou.ObjectWasDeleted, hou.OperationFailed):
            pass

    def changed(**kwargs):
        if pending[0] or not alive[0]:
            return
        if hou.isUIAvailable():
            # InputRewired can fire before the native wire edit has committed.
            # Defer publication so a viewport cook cannot read the old topology
            # inside the event callback and then miss the completed disconnect.
            from PySide6 import QtCore

            pending[0] = True
            QtCore.QTimer.singleShot(50, notify)
        else:
            notify()

    def register():
        sources = (network, *network.children())
        current = {s.sessionId() for s in sources}
        for key in set(watched) - current:
            source = watched.pop(key)
            try:
                source.removeEventCallback(events, changed)
            except hou.Error:
                pass
        for source in sources:
            if source.sessionId() in watched:
                continue
            source.addEventCallback(events, changed)
            watched[source.sessionId()] = source

    def cleanup(**kwargs):
        alive[0] = False
        for source in watched.values():
            try:
                source.removeEventCallback(events, changed)
            except hou.Error:
                pass
        for endpoint in (network, target):
            try:
                endpoint.removeEventCallback(
                    (hou.nodeEventType.BeingDeleted,), cleanup
                )
            except hou.Error:
                pass
        _structure_watchers.pop(identity, None)

    register()
    for endpoint in (network, target):
        endpoint.addEventCallback((hou.nodeEventType.BeingDeleted,), cleanup)
    _structure_watchers[identity] = changed


def publish(network=None):
    import hdue_houdini

    if network is None:
        selected = [n for n in hou.selectedNodes() if is_builder(n)]
        if len(selected) != 1:
            raise ValueError(
                "Select one Substrate Builder VOP network to author."
            )
        network = selected[0]
    viewer = hdue_houdini.scene_viewer()
    parent = viewer.pwd()
    if not isinstance(parent, hou.LopNetwork):
        parent = hou.node("/stage")
    return author_lop(
        network,
        "/Looks/" + network.name(),
        parent.displayNode(),
        list(viewer.currentSceneGraphSelection()),
    )
