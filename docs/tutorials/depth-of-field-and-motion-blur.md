# Depth of field and motion blur

A row of toys focused on the fourth one, at f/2.

<div class="hdue-figure" data-figure="tut-dof-mb-final"></div>

You will focus a camera on a row of toys and set its aperture, then blur a moving ball with the camera's shutter. Both effects come from the USD camera, so the same camera settings also drive Karma. It takes about 20 minutes.

## The starting scenes

- Depth of field: `$HDUE/samples/docs/row.usda`. Seven rubber toys stand 1.2 m apart in front of the camera `/scene/camera`, which is 2.4 m from the first toy. The fourth toy is 6 m away. The camera has **Focus Distance** 6 and **F-Stop** 0, which means no depth of field.
- Motion blur: `$HDUE/samples/cornell/variants/motion_blur.usda`. A blue ball crosses the Cornell box during frame 1; the camera's shutter opens a quarter frame before the frame and closes a quarter frame after.

1. In `/stage`, add a **Sublayer** LOP with **File** `$HDUE/samples/docs/row.usda` and set its display flag.
2. Render with **Unreal Engine** through `camera`, and choose **Unreal > Create hdUE Render Settings**. Keep **Render Mode** at **Realtime (Lumen)**.

## Aperture

The aperture sets how quickly objects blur away from the focus distance. A lower f-stop is a wider aperture and a shallower depth of field.

The row at **F-Stop** 0, 8 and 2, focused at 6 m.

<div class="hdue-figure" data-figure="tut-dof-mb-fstop"></div>

1. Add a **Camera** LOP. Set **Action** to **Edit** and **Primitive Path** to `/scene/camera`.
2. Set **F-Stop** (`fStop`) to 2.
    0 turns depth of field off. hdUE passes the f-stop, focus distance and focal length to UE's physical camera.

## Focus

The row at **F-Stop** 2, focused at 2.4 m (the first toy), 6 m (the fourth) and 9.6 m (the last).

<div class="hdue-figure" data-figure="tut-dof-mb-focus"></div>

1. Set **Focus Distance** (`focusDistance`) to the distance of the subject, in scene units: 6 for the fourth toy.
    Measure it from the camera, not from the origin.

## Realtime and the Path Tracer

Realtime uses UE's post-process depth of field, an approximation that clamps very large blur. The Path Tracer traces the lens itself (**Reference Depth Of Field**, `unreal:postProcess:PathTracingEnableReferenceDOF`, which hdUE turns on when the camera has depth of field).

The row at f/2 in Realtime and in the Path Tracer.

<div class="hdue-figure" data-figure="tut-dof-mb-dof-pt"></div>

1. For extreme blur, macro shots or bokeh that must be exact, render with the Path Tracer.
2. In Realtime, **Number of diaphragm blades** (`unreal:postProcess:DepthOfFieldBladeCount`, default 5, Lens tab) shapes the bokeh.

To turn depth of field off for a render without editing the camera, author `disableDepthOfField` = 1 on the render settings prim. See [Cameras](../cameras.md).

## Shutter

Motion blur needs two things: a camera whose **Shutter Open** is before its **Shutter Close**, and something that moves during that interval. **Motion Blur** (`unreal:motionBlur`) is on by default.

1. Replace the Sublayer's **File** with `$HDUE/samples/cornell/variants/motion_blur.usda` and look through `camera`.
2. On the hdUE Render Settings LOP, set **Render Mode** to **Realtime (Lumen)**.
3. Add a **Camera** LOP in **Edit** mode for `/cornell_box/camera`. Set **Shutter Open** to -0.25 and **Shutter Close** to 0.25.
    The shutter is in frames, relative to the frame: this is a half-frame shutter centred on the frame, a 180° shutter.

The ball with a quarter-frame, half-frame and full-frame shutter.

<div class="hdue-figure" data-figure="tut-dof-mb-shutter"></div>

A longer shutter makes longer streaks; the ball covers more distance while the shutter is open.

## Motion samples

hdUE renders the frame several times across the shutter and averages them: **Motion Samples** (`unreal:motionSamples`, default 16, Rendering Features tab) sets how many. In Realtime the warmup frames are divided between them; in the Path Tracer, the samples. The total render time stays about the same.

The ball with 1, 4 and 16 **Motion Samples**.

<div class="hdue-figure" data-figure="tut-dof-mb-samples"></div>

1. Keep **Motion Samples** at 16.
    With too few, a fast object shows as separate copies instead of a streak. Raise it for very fast motion or long shutters.

What moves: transforms of meshes, curves, points and their parents, camera transforms, deforming meshes (time-sampled points or `velocities`), and instancer and point `velocities`. See [Motion blur](../rendering/motion-blur.md) for the full list. Deforming meshes are uploaded again for each motion sample, which costs time on dense meshes.

The ball in Realtime and in the Path Tracer.

<div class="hdue-figure" data-figure="tut-dof-mb-pt"></div>

To render without motion blur, turn **Motion Blur** off, or author `disableMotionBlur` = 1 on the render settings prim.

## Final render settings

- Camera: **F-Stop** 2, **Focus Distance** the subject's distance; **Shutter Open** -0.25, **Shutter Close** 0.25.
- **Render Mode**: Realtime (Lumen); the Path Tracer for exact bokeh.
- **Motion Blur**: on; **Motion Samples**: 16.
