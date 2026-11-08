# hdUE user guide

hdUE (HdUnrealEngine) is a Hydra render delegate that renders OpenUSD with Unreal Engine 5.8 inside the host process: the Houdini Solaris viewport, husk and the USD Render ROP, Live Render, usdview, or a standalone host. It renders with Lumen (Realtime) or UE's Path Tracer, from the same USD and render settings as Karma.

Amazon Lumberyard Bistro (NVIDIA ORCA, CC BY 4.0) rendered by hdUE with MaterialX surfaces: inside at night, and the street outside in the rain. Drag to compare Realtime (Lumen) with UE's Path Tracer.

<div class="hdue-figure" data-figure="home-bistro-interior"></div>

<div class="hdue-figure" data-figure="home-bistro-exterior"></div>

hdUE follows Unreal Engine and Movie Render Queue: a setting means what it means in UE, with UE's name, range and default, and a scene renders as it would in UE's editor. Where hdUE departs from a UE default, the page says so.

## Where to start

<div class="grid cards" markdown>

-   :lucide-download:{ .lg .middle } **Installation**

    ---

    Build the package, install it in Houdini, check that it loads.

    [:lucide-arrow-right: Install hdUE](getting-started/installation.md)

-   :lucide-camera:{ .lg .middle } **Your first render**

    ---

    The Solaris viewport, the render settings, a husk render.

    [:lucide-arrow-right: First render](getting-started/first-render.md)

-   :lucide-graduation-cap:{ .lg .middle } **Tutorials**

    ---

    Lighting a room, removing noise, Lumen or the Path Tracer, exposure, depth of field.

    [:lucide-arrow-right: Tutorials](tutorials/index.md)

-   :lucide-sliders-horizontal:{ .lg .middle } **Render settings**

    ---

    Realtime and Path Tracer, samples, warmup, profiles.

    [:lucide-arrow-right: Render settings](rendering/render-settings.md)

-   :lucide-book-open:{ .lg .middle } **Render settings reference**

    ---

    Every `unreal:*` setting with its default, UE's tooltip and a picture of what it does.

    [:lucide-arrow-right: Reference](reference/render-settings/index.md)

-   :lucide-blocks:{ .lg .middle } **Houdini nodes**

    ---

    The hdUE LOPs and Substrate VOPs, with every parameter.

    [:lucide-arrow-right: Node reference](reference/nodes/index.md)

</div>

## Sections

| Section | What it covers |
|---|---|
| [Getting started](getting-started/installation.md) | Requirements, installing the package, a first render, environment variables |
| [Tutorials](tutorials/index.md) | Step-by-step results: lighting a room, removing noise, choosing Lumen or the Path Tracer, exposure and tone, depth of field and motion blur |
| [Houdini](houdini/index.md) | The Solaris viewport and its Unreal menu, husk, Live Render, Substrate materials |
| [Render output](output/aovs.md) | AOVs, Cryptomatte, alpha |
| [Rendering](rendering/render-settings.md) | Render modes, denoising, motion blur, colour management, post-processing, engine quality, debug views |
| [Cameras](cameras.md) and [Lights](lights.md) | UsdGeomCamera and UsdLux in UE: depth of field, lens distortion, units, IES, HDR environments |
| [Materials](materials.md) | UsdPreviewSurface, MaterialX, Substrate, textures, glass, cooked UE assets |
| [Geometry](geometry/meshes.md) | Meshes, curves, points, subdivision, displacement, Nanite |
| [Environment](environment/volumes.md) | Volumes, fog, sky and clouds, game effects (Niagara) |
| [Reference](reference/render-settings/index.md) | Generated from the engine source: render settings, light properties, USD prim types, USD conformance |
| [Developer](architecture.md) | Architecture, building from source, specifications |
