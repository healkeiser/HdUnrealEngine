# Your first render

You will render the Cornell box with Unreal Engine: first in the Solaris viewport, then to an EXR on disk with husk. It takes about ten minutes once hdUE is [installed](installation.md).

The result, in Unreal Engine's two render modes:

<div class="hdue-figure" data-figure="render-mode"></div>

## The starting scene

The package ships the Cornell box in `$HDUE/samples/cornell/cornell.usda`. It holds a camera, one rect light and a render settings prim set to the Path Tracer at 512 samples, 512 x 512 pixels.

1. In a new Houdini session, go to the `/stage` network.
2. Add a **Sublayer** LOP. Set **File** to `$HDUE/samples/cornell/cornell.usda`.
3. Set its display flag.

A Sublayer LOP with no input copies the file's metadata to the stage, so the scene keeps its Z-up axis and metre units.

## Render in the viewport

1. In the Scene Viewer's renderer menu, pick **Unreal Engine**.
    The first time in a session, Unreal Engine starts inside Houdini. The image appears a few seconds later; later switches are immediate.
2. In the viewport's camera menu, pick `camera` to look through the Cornell camera.
3. Choose **Unreal > Create hdUE Render Settings** from the main menu bar.
    This adds an **hdUE Render Settings** LOP after the Sublayer and displays it. It takes the camera, the render mode and the sample count from the scene's own render settings.

The viewport now renders with the Path Tracer and refines the image up to 512 samples. While you tumble, scrub or play, it uses 8 samples per pixel, then refines again once the view is still.

## Switch to Realtime

On the hdUE Render Settings LOP, set **Render Mode** (`unreal:renderMode`) to **Realtime (Lumen)**.

Realtime is Unreal Engine's deferred renderer with Lumen global illumination and reflections, the renderer of a game or a Movie Render Queue shot. It is meant for final frames too. The Path Tracer is UE's reference renderer: it traces up to 32 bounces and takes about 50 times longer on this scene.

In Realtime, a still frame counts as finished after **Warmup Frames** (`unreal:warmupFrames`, default 64), the frames Lumen needs to gather its lighting. You can also switch with **Unreal > Render Mode > Realtime** or **Path Traced**.

## Turn on the Unreal Tonemapper

The Cornell box renders scene-linear, like Karma: Houdini's viewer applies the display transform.

To see the image Unreal's editor would show, turn on **Unreal Tonemapper** (`unreal:tonemapper`) above the tabs. UE then applies its film curve, bloom and colour grading. The beauty becomes display-linear: view it with an sRGB view that only encodes, not Raw or ACES.

**Unreal Tonemapper**, in Realtime:

<div class="hdue-figure" data-figure="first-render-tonemapper"></div>

Turn it off again for this tutorial's EXR, so the image stays scene-linear for compositing.

## Render to disk with husk

1. Save the scene, for example as `cornell.hip`.
2. Add a **USD Render ROP** LOP after the hdUE Render Settings LOP.
3. Set **Render Delegate** to **Unreal Engine**.
4. Click **Render to Disk**.

husk writes the image to the **Output Picture** of the hdUE Render Settings LOP, `$HIP/render/$HIPNAME.$OS.$F4.exr` by default: a numbered EXR in the `render` folder next to `cornell.hip`.

You can also render the sample file directly from a terminal:

```powershell
husk --renderer "Unreal Engine" -o cornell.exr "$env:HDUE\samples\cornell\cornell.usda"
```

On an RTX 4090, the Cornell box takes about 2.3 s per frame in the Path Tracer and about 50 ms in Realtime, after about 3 s of Unreal Engine startup per husk process ([Gallery](../gallery.md)).

## Final render settings

- **Render Mode**: Path Tracer (Realtime (Lumen) for the fast version)
- **Path Tracer Samples**: 512
- **Unreal Tonemapper**: off
- **Camera**: `/cornell_box/camera`
- **Resolution**: 512 x 512

## Next steps

- [Solaris viewport](../houdini/viewport.md): the Unreal Engine tab, the Unreal menu and viewport speed.
- [Rendering with husk](../houdini/husk.md): frame ranges, farms and render products.
- [Render settings reference](../reference/render-settings/index.md): every setting with its default.
