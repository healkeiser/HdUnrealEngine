# Lighting a room

The room at the end of this tutorial: sun and sky through the window, with a fill light in the window opening.

<div class="hdue-figure" data-figure="tut-room-final"></div>

You will light a 6 x 3 x 6 m room through its one window, add a fill light in the window, build a night version lit by an emissive ceiling panel, and compare Lumen with the Path Tracer. It takes about 30 minutes.

## The starting scene

The package ships the room in `$HDUE/samples/docs/room.usda`. Its left wall has a window 2 m wide and 1.55 m high. It holds:

- a normalized DistantLight `/scene/sun` (angle 0.53, exposure 4) aimed through the window;
- a sky dome `/scene/sky` with an HDRI;
- plaster walls, a wood floor and table, a fabric sofa;
- the camera `/scene/camera` and render settings: Realtime, **Unreal Tonemapper** on, 640 x 360.

1. In `/stage`, add a **Sublayer** LOP and set **File** to `$HDUE/samples/docs/room.usda`. Set its display flag.
2. Pick **Unreal Engine** in the viewport's renderer menu, and `camera` in its camera menu.
3. Choose **Unreal > Create hdUE Render Settings**.
    The new LOP picks up the scene's render mode, Realtime. Check that **Unreal Tonemapper** is on: the figures on this page show UE's tone-mapped image.

## Sun and sky

The room is lit only from outside. The sky enters through the window as soft light; the sun adds a hard patch on the floor that Lumen bounces around the room.

The room with the sky alone, then with the sun added.

<div class="hdue-figure" data-figure="tut-room-sun"></div>

1. Add a **Light** LOP. Set **Action** to **Edit** and **Primitive Path** to `/scene/sun`.
2. Leave **Intensity** at 1 and set **Exposure** to 4.
    Each stop doubles the light. Keep intensity at 1 and tune exposure, so values stay comparable between lights.
3. Keep **Normalize Power** on.
    A normalized distant light gives the same illuminance whatever its **Angle**, so widening the angle softens the shadow without darkening the room.

The bright patch on the floor is direct light. Everything else, the lit ceiling and the shaded walls, is global illumination from Lumen ([Lighting reference](../reference/render-settings/lighting.md#unreal-lumenenabled)).

## A fill light in the window

The back of the room stays dark: the light that reaches it has bounced several times, losing some at each bounce. A rect light filling the window opening adds soft sky-coloured light where the sky enters.

The room without a fill, then with the fill at exposure 2, 3 and 4.

<div class="hdue-figure" data-figure="tut-room-fill"></div>

1. Add a **Light** LOP with **Primitive Path** `/scene/fill` and **Type** **Rectangle**.
2. Set **Width** to 2 and **Height** to 1.55, the size of the window.
3. Set **Translate** to (-2.98, 1.625, 0) and **Rotate** to (0, -90, 0).
    A rect light emits along its -Z axis. The rotation turns it toward +X, into the room, and the translation puts it 2 cm inside the window.
4. Set **Color** to (0.85, 0.92, 1) and **Exposure** to 3.
    The slight blue matches the sky. Exposure 3 lifts the back wall without flattening the sun patch.

hdUE does not support USD light portals, so a rect light in the opening is how you fill a window. See [Lights](../lights.md).

## An emissive ceiling panel

For a night version, the sun and sky go off and a glowing panel under the ceiling lights the room. An emissive surface lights the scene in both render modes: in Realtime through Lumen's final gather, and in the Path Tracer while **Emissive Materials** (`unreal:postProcess:PathTracingEnableEmissiveMaterials`) is on, its default.

The night room lit by the panel at emissive 2, 4 and 8.

<div class="hdue-figure" data-figure="tut-room-panel"></div>

1. Deactivate `/scene/sun`, `/scene/sky` and `/scene/fill` with a **Configure Primitive** LOP: set **Primitives** to the three paths and **Activation** to deactivate them.
2. Add a 2 x 0.02 x 2 m box at (0, 2.99, 0), just under the ceiling.
3. Bind it a **USD Preview Surface** with **Diffuse Color** (0, 0, 0) and **Emissive Color** (4, 4, 4).
    Emissive colour is radiance: 4 is two stops above 1. A black diffuse colour keeps the panel from reflecting light it receives.

Keep emissive panels large. UE's documentation warns that small, bright emissive areas cause noise in Lumen, and that small emissive meshes light the scene inconsistently because Lumen culls small objects from its scene ([Lumen Technical Details](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine)). For a small source, use a light.

The same panel in Realtime and in the Path Tracer.

<div class="hdue-figure" data-figure="tut-room-panel-pt"></div>

## Lumen and the Path Tracer

Go back to the day setup: reactivate the sun, sky and fill, and deactivate the panel.

The day room in Realtime, in Realtime with **Ray Lighting Mode** set to **Hit Lighting**, and in the Path Tracer.

<div class="hdue-figure" data-figure="tut-room-lumen-pt"></div>

1. On the hdUE Render Settings LOP, set **Render Mode** to **Path Tracer** and **Path Tracer Samples** to 256.
    The Path Tracer is UE's reference: it traces up to 32 bounces and is the image Lumen aims for.
2. Compare, then switch back to **Realtime (Lumen)**.
3. On the **Lighting** tab, set **Ray Lighting Mode** (`unreal:postProcess:LumenRayLightingMode`) to **Hit Lighting**.
    Lumen then lights its rays where they hit instead of reading its surface cache. UE's tooltip calls it the higher-quality mode; it costs GPU time.

Directly lit surfaces match between the two modes; differences sit in the light of later bounces and in glossy reflections. See [Lumen or the Path Tracer](lumen-or-path-tracer.md).

## Final render settings

- **Render Mode**: Realtime (Lumen)
- **Warmup Frames**: 64 (default)
- **Unreal Tonemapper**: on
- **Ray Lighting Mode**: Hit Lighting, for stills where render time allows
- Sun: exposure 4, normalized; sky dome: exposure 0; window fill: rect light 2 x 1.55 m, exposure 3, colour (0.85, 0.92, 1)
