# Substrate materials

Substrate is Unreal Engine's material model: surfaces built from slabs, layered and mixed. In Houdini you build a Substrate material in a VOP network, the way you build a Karma material with the Karma Material Builder. hdUE then turns it into a native UE material.

!!! warning "Needs the Unreal Engine editor"
    A changed Substrate network is compiled by Unreal Engine's editor, which the hdUE package does not include. Preparing a material needs a machine with the UE editor and an hdUE source checkout ([Building from source](../building.md)). On a machine with the package only, an unprepared Substrate material fails the husk frame. Prepared materials render everywhere.

## Creating a material

1. Add a **Material Library** LOP.
2. Inside it, press TAB and choose **hdUE > hdUE Substrate Material Builder**.
    It starts with a **Substrate Slab** wired to the `surface` output.
3. Dive into the builder and add hdUE Substrate nodes from the TAB menu (**hdUE** submenu), filed as UE's Material Editor palette files them: Substrate, Parameters, Constants, Math, Coordinates, Texture, Utility and Volume.
4. On the Material Library LOP, assign the material to your geometry, as with any material.

You can also create the builder in `/mat` and point the Material Library's **Material VOP** at it.

The **hdUE** shelf has the same builder, plus:

- **Water Coating Example**: a builder with a clear water slab layered over a MaterialX base, with coverage and roughness parameters.
- **Author Substrate USD**: adds a Material Library LOP after the displayed LOP that authors the selected builder at `/Looks/<builder name>` and binds it to the current Scene Graph selection.

See [Substrate Builder](../substrate-builder.md#available-nodes) for every node and its inputs.

## Editing

| You change | What happens |
| --- | --- |
| A value: colour, roughness, thickness, a parameter | The viewport updates at once. |
| A node type, a wire, a parameter name, a blending option | hdUE prepares the material again in the background. Houdini's status bar shows the progress; the viewport keeps the last valid image until it is ready. |
| A texture's file contents only | Nothing: re-prepare the material by hand. |

Background preparation is on by default: **Prepare Edited Materials** (`unreal:substrateAutoPrepare`, [reference](../reference/render-settings/rendering.md#unreal-substrateautoprepare)) on hdUE Render Settings > Rendering > Materials. husk reads the same setting and prepares the materials a frame needs before rendering it. A material is prepared once per runtime: later renders reuse it.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The material stops updating after you disconnect a node. | A required surface or output is disconnected. | Reconnect it; preparation resumes. |
| A husk frame fails with a message about an unprepared material. | **Prepare Edited Materials** is off, or the machine has no UE editor. | Turn the setting on, or prepare the material on a machine with the editor first. |
| Preparation failed and stays failed after you fixed the cause. | A failed network is retried only when its nodes or wires change. | Change a node or a wire in the network. |

## Unreal Engine and Karma

- The builder is made the way SideFX makes the Karma Material Builder: an editable VOP subnet with a material flag.
- Its nodes are UE's Substrate expressions, not Karma shaders: Karma does not render these materials.
