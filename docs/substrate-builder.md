# Substrate Builder

The **hdUE Substrate Material Builder** builds a UE Substrate material in a Houdini VOP network: slabs, layers, mixes and UE's special BSDFs (hair, eye, water, thin film), wired like a Karma Material Builder. UE's editor compiles the network into a native UE material in the background; you keep working in the viewport.

Use it for what UsdPreviewSurface and MaterialX cannot express: a water film over a material, an iridescent metal, layered paints, hair, eyes, Single Layer Water, decals and volume materials. For ordinary surfaces, a [Preview Surface](materials/preview-surface.md) or [MaterialX](materials/materialx-surfaces.md) material renders immediately and costs less.

!!! note
    The Substrate Builder is experimental. Preparing a graph needs UE's editor and the hdUE backend project on the machine; a package install without them renders only graphs prepared elsewhere.

## Using it in Solaris

1. Add a **Material Library** LOP.
2. Inside it, press TAB and choose **hdUE > hdUE Substrate Material Builder**. It starts with a **Substrate Slab** wired to the **surface** output.
3. Assign the material with the Material Library's **Geometry Path** or an **Assign Material** LOP.
4. Edit values: the viewport updates at once. Add or rewire nodes: the status bar shows the compile, then the new material replaces the old one, typically within 10 to 20 seconds.

The builder also works from `/mat`: point the Material Library's **Material VOP** at it. The **hdUE** shelf has the same tools:

| Shelf tool | What it does |
| --- | --- |
| **hdUE Substrate Material Builder** | Creates a builder in `/mat` |
| **Water Coating Example** | Creates a builder with a water layer over a MaterialX material ([recipe](#water-film-over-a-material)) |
| **Author Substrate USD** | Adds a Material Library after the displayed LOP that authors the selected builder at `/Looks/<name>` and binds it to the Scene Graph selection |

### What updates live

| Edit | Result |
| --- | --- |
| Any numeric or colour value, a Scalar or Color Parameter, UV scale and offset, Normal Map strength, Extract Channel selection | Live, no compile |
| A Float Mask Primvar's name or the primvar's values | Live, no compile |
| Adding, removing or rewiring nodes, a parameter's name, a Slab's subsurface mode, parameter blending, a texture's file, colour space or address mode | Compiled in the background (seconds) |
| A Float Ramp or Color Ramp's keys | Compiled in the background |
| An image file's content, changed on disk without a USD edit | Not detected: prepare the graph again |

While a graph compiles, the viewport keeps the last valid image. A graph with a missing connection shows an error in the status bar and resumes when you reconnect it.

## Nodes { #available-nodes }

Surface nodes:

| Node | Inputs | Use it for |
| --- | --- | --- |
| **Substrate Slab** | Diffuse Albedo (0.18), F0 (0.04), Roughness (0.5), Normal, Mean Free Path (0), Subsurface mode (`none`, `simple_volume`); optional F90, Second Roughness and its weight, Emission | Every ordinary surface: UE's Slab BSDF |
| **Substrate Vertical Layer** | Top, Base, Thickness (0.01 cm) | A coat over a surface: varnish, water, dust |
| **Substrate Horizontal Mix** | Background, Foreground, Mix (0.5) | Two materials side by side, by a mask |
| **Substrate Weight** | Surface, Weight (1) | Partial coverage of a layer |
| **Substrate Add** | Two surfaces | Light that adds, such as a glow over a surface |
| **Substrate Select** | Two surfaces, Select | One surface or the other, by a threshold |
| **Substrate Unlit** | Emission | Emission only, unlit |
| **Substrate Hair** | Base color, scatter, specular, roughness, backlit, tangent, emission | UE's Hair BSDF |
| **Substrate Eye** | Diffuse color, roughness, cornea, iris normals, iris mask and distance, emission | UE's Eye BSDF |
| **Substrate Single Layer Water** | Base color, metallic, specular, roughness, opacity, water albedo, extinction, phase, colour behind the water | UE's water surface |
| **MaterialX Surface** | A USD material path | An existing MaterialX material as a layer of the graph |

Value nodes:

| Node | What it does |
| --- | --- |
| **Scalar Parameter**, **Color Parameter** | A named value, shared by every prim bound to the material |
| **Float Constant**, **Color Constant** | A value, editable live |
| **Float Multiply**, **Float Saturate**, **Float to Color** | Arithmetic |
| **Float Mask Primvar [0..1]** | A float primvar of the mesh, per point or face, for masks painted in SOPs |
| **Texture Sample** | An image: outputs RGB, RGBA, R, G, B, A. Colour space `srgb` (colour) or `raw` (data); address `wrap`, `clamp` or `mirror`; `<UDIM>` patterns |
| **UV Coordinates** | UV0 with Scale and Offset |
| **Extract Channel** | One channel of an RGBA input |
| **Normal Map** | Decodes a raw normal texture; Strength; **Flip Green** (on: OpenGL +Y maps; off: DirectX -Y maps) |
| **Metalness to Albedo and F0** | Base color, metallic and specular, as in UE's Default Lit, into a Slab's Diffuse Albedo and F0 |
| **Haziness to Second Roughness** | A haze over the base roughness |
| **Transmittance to MFP** | A colour seen through a given thickness (cm), into a Slab's Mean Free Path |
| **Substrate Thin Film** | Thickness (micrometres) and IOR of a film, into a Slab's F0 and F90 |
| **Float Ramp**, **Color Ramp** | A Houdini ramp |
| **Blackbody** | A colour from a temperature in kelvin |

Volume and decal outputs: **Substrate Volumetric Fog Cloud**, **Volume Field** and **Substrate Convert To Decal** (see [below](#volume-materials)).

Unconnected inputs take UE's own defaults. Hair, Eye, Single Layer Water and Unlit must be the graph's output: UE's Substrate cannot layer or mix them.

## Recipes

### Water film over a material

The **Water Coating Example** shelf tool builds this graph. Set its MaterialX Surface to your material.

| Node | Values |
| --- | --- |
| **MaterialX Surface** | Material: the dry material, for example `/Looks/Base` |
| **Substrate Slab** (water) | Diffuse Albedo (0, 0, 0), F0 (0.0201, 0.0201, 0.0201) (IOR 1.33), Mean Free Path (1000000, 1000000, 1000000), Subsurface `simple_volume` |
| **Scalar Parameter** `WaterRoughness` | 0.08, into the water Slab's Roughness |
| **Scalar Parameter** `Coverage` → **Float Saturate** | 1 |
| **Float Mask Primvar** | Primvar `wetness` |
| **Float Multiply** | The saturated Coverage times the mask |
| **Substrate Weight** | Surface: the water Slab; Weight: the product |
| **Substrate Vertical Layer** | Top: the Weight; Base: the MaterialX Surface; Thickness 0.01 (cm) |

Paint `primvars:wetness` (0..1) in SOPs to place the water. For the same effect without a graph, see [Wet surfaces](wet-surface-layer.md).

### Iridescent metal

From `samples/substrate/bsdfs.usda`, material `Iridescent`:

| Node | Values |
| --- | --- |
| **Metalness to Albedo and F0** | Base color (0.9, 0.6, 0.2), Metallic 1, Specular 0.5 |
| **Substrate Thin Film** | F0 from the node above, F90 (1, 1, 1), IOR 1.44, Thickness 0.4 (µm) |
| **Haziness to Second Roughness** | Base roughness 0.2, Haziness 0.3 |
| **Substrate Slab** | Diffuse Albedo and F0, F90 from above; Roughness 0.2; Second Roughness and its weight from the haze |

### Wax

Material `Wax` in the same file:

| Node | Values |
| --- | --- |
| **Transmittance to MFP** | Transmittance (0.9, 0.6, 0.4), Thickness 1 (cm) |
| **Substrate Slab** | Diffuse Albedo (0.9, 0.8, 0.6), F0 (0.04, 0.04, 0.04), Roughness 0.5, Mean Free Path from above, Subsurface `simple_volume` |

### Textured slab

[`samples/substrate/textures.usda`](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/substrate/textures.usda): an sRGB checker into Diffuse Albedo, a packed texture's G (raw) through **Extract Channel** into Roughness, its A as a top layer's coverage, and a raw normal texture through **Normal Map** into the base Slab's Normal.

## Volume materials

A builder whose output is a **Substrate Volumetric Fog Cloud** is a volume material. Create one with `hdue_substrate.create(preset="volume")` in Houdini's Python shell, and bind it to a Volume prim. It replaces hdUE's default volume material in the viewport, husk and usdview. **Volume Field** reads the volume's `density` grid or its emission field. Left unconnected, the Fog Cloud renders as the default volume material does.

## Decal materials

A builder whose output is a **Substrate Convert To Decal** (`hdue_substrate.create(preset="decal")`) is a decal material: a UE Deferred Decal of the Slab wired to it, with Coverage as its opacity. Bind it to an `UnrealDecal` prim ([decals](rendering/post-processing.md#decals)).

## Rendering with husk and on a farm

**Prepare Edited Materials** ([`unreal:substrateAutoPrepare`](reference/render-settings/rendering.md#unreal-substrateautoprepare), hdUE Render Settings > Rendering > Materials, on by default) applies to husk too: husk prepares the graphs a frame needs before rendering it. A graph is prepared once per runtime folder and reused by every later render, viewport or husk; render nodes sharing a runtime prepare it once for the farm. Eight materials took 14 s to prepare and 8 s to render the first time, 8 s in all the second time.

For render nodes without UE's editor, prepare the materials on a workstation and set **Prepared Substrate Library** ([`unreal:substrateLibrary`](reference/render-settings/rendering.md#unreal-substratelibrary)) to the result:

```powershell
.\scripts\Prepare-Substrate.ps1 -InputUsd scene.usda -OutputDirectory out/prepared
```

With **Prepare Edited Materials** off, an unprepared graph shows the library's message in the viewport and fails a husk frame.

## Background preparation { #background-graph-preparation-in-solaris }

- The status bar shows each compile and its errors.
- Edits made while a graph compiles are kept: the newest graph is compiled next, and value edits apply to the result.
- Returning to a graph already compiled in this session does not compile again.
- **Retry material preparation** (on the hdUE settings subnet) retries a failed graph after you fix its cause, such as a missing file.

## Limits

- Opaque surfaces on USD meshes. No transmission or glass authoring, no displacement.
- Up to 256 connected nodes, one MaterialX Surface and one Float Mask Primvar per graph.
- Substrate's budget in hdUE's runtime: 4 closures and 128 bytes per pixel. The water example uses 3 closures and 92 bytes.
- Textures read UV0 only. `op:` and `opdef:` images must be written to files first.
- Parameters are shared by every prim bound to the material; use separate materials for separate values. Float masks vary per mesh.
- Native UE asset materials and arbitrary UE material expressions are out of scope.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The viewport keeps the old material after a rewire | The graph is compiling, or failed | Read the status bar; fix the error, then **Retry material preparation** |
| The status bar reports a disconnected surface | A required input or the output is not connected | Reconnect it; preparation resumes |
| A Normal Map connection is rejected | The Texture Sample is `srgb` | Set it to `raw` |
| A normal map lights bumps from the wrong side | A DirectX map with **Flip Green** on | Turn **Flip Green** off |
| A changed texture file does not update | File contents are not tracked | Prepare the graph again |
| husk fails a frame with a library message | The graph changed since the library was prepared, and **Prepare Edited Materials** is off | Turn it on, or prepare the library again |
| UE rejects a material at compile | Substrate's graph rules (Hair, Eye, Water and Unlit cannot be layered), or the closure budget | Make that BSDF the output; simplify the layering |

How the graph is stored in USD and compiled is described in [Substrate Builder internals](developer/internals/substrate-builder.md).
