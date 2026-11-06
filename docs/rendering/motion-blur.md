# Motion blur

Motion blur smears what moves while the camera's shutter is open. hdUE renders it by averaging sub-frames spread over the shutter, as Movie Render Queue's temporal sampling does, in both render modes.

A ball crossing the Cornell box during a half-frame shutter, Realtime.

<div class="hdue-figure" data-figure="motion-blur"></div>

## Using it in Solaris

Motion blur needs three things:

1. **An open shutter.** On your Camera LOP, set the shutter so that `shutter:open` is before `shutter:close`, in frames relative to the rendered frame. -0.25 and 0.25 is a half-frame shutter centred on the frame.
2. **Something that moves** within the shutter (see [What moves](#what-moves)).
3. **Motion Blur** on in the render settings (the default).

## What moves

| Motion | From |
| --- | --- |
| Object transforms | transform time samples on meshes, curves, points and their parent prims |
| Deformation | time-sampled mesh `points` with a constant point count; otherwise per-point `velocities` (Houdini's `v`) |
| Points | per-point `velocities` |
| Instances | PointInstancer transform samples; per-instance `velocities`, `accelerations` and `angularVelocities`; otherwise time-sampled `positions`, `orientations`, `scales` or instance matrices |
| Camera | camera transform time samples |

- `velocities` are in scene units per second. hdUE converts them to frames with the frame rate Houdini passes; without one, with `unreal:framesPerSecond` (default 24).
- `motion:blurScale` on a prim scales its motion: 0.5 halves its blur, 0 removes it.
- Transforms are interpolated linearly between time samples. A fast rotation within one frame blurs along a slightly shorter arc.
- Time-sampled instance placements blur along one straight segment through the shutter, and animated instance scales do not blur. Author `velocities` and `angularVelocities` for curved instance paths.

## Motion Blur

**Motion Blur** (`unreal:motionBlur`), default on.

Averages sub-frames over the camera shutter when something moves. Off, each frame renders at the frame's time. A render settings prim's `disableMotionBlur` (or the deprecated `instantaneousShutter`) turns it off too.

## Motion Samples

**Motion Samples** (`unreal:motionSamples`), default 16, range 1 to 64.

Sub-frames per frame. The Path Tracer's samples and Realtime's warmup frames are divided between the sub-frames, so a frame with motion blur takes about the same time as one without. Too few sub-frames show separate copies of fast objects.

Path Tracer, 512 samples per pixel in total.

<div class="hdue-figure" data-figure="motion-blur-samples"></div>

!!! note
    A deforming mesh is uploaded again for each sub-frame. On dense meshes this adds time per sub-frame; lower **Motion Samples** if a deforming frame is slow.

## Shutter length

The shutter interval sets the length of the blur: a 1-frame shutter blurs twice as far as a half-frame shutter. Set it on the camera, not in the render settings.

<div class="hdue-figure" data-figure="motion-blur-shutter"></div>

## Realtime and Path Tracer

Both modes average the same sub-frames. In Realtime, UE's post-process motion blur also smears each sub-frame over its share of the shutter, which fills the gaps between sub-frames. Its strength is the shutter interval in frames (UE's shutter fraction, as Movie Render Queue reads it), divided by the motion samples, unless you author **Amount** (`unreal:postProcess:MotionBlurAmount`). The other Realtime motion blur settings are in [Rendering Features > Motion Blur](../reference/render-settings/rendering-features.md#unreal-postprocess-motionbluramount).

<div class="hdue-figure" data-figure="motion-blur-mode"></div>

## Recipes

**Film-style blur, 180-degree shutter:** camera `shutter:open` -0.25, `shutter:close` 0.25; **Motion Samples** 16.

**Fast spinning objects, Path Tracer:** **Motion Samples** 32 and **Path Tracer Samples** 1024, so each sub-frame keeps 32 samples.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| No blur at all | the shutter is closed: `shutter:open` equals `shutter:close` (USD's fallback is 0 and 0) | Set the camera's shutter, for example -0.25 and 0.25. |
| No blur at all | **Motion Blur** off, or `disableMotionBlur` on the render settings prim | Turn **Motion Blur** on and remove `disableMotionBlur`. |
| No blur on a simulation | its points have a changing point count and no velocities | Export `v` from the simulation as `velocities`. |
| Fast objects show separate copies | too few motion samples | Raise **Motion Samples**. |
| Instances blur in straight lines | time-sampled positions blur as one segment | Author `velocities` and `angularVelocities`. |
| Path Tracer blur is noisy | the samples are split between sub-frames | Raise **Path Tracer Samples**. |

## Unreal Engine and Karma

Averaging sub-frames is how Movie Render Queue renders motion blur with temporal samples. Karma reads the same USD camera shutter, so a scene blurs over the same interval in both.
