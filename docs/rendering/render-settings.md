# Render settings

Render settings choose how Unreal Engine renders your stage: the render mode, its sample counts and UE's post-processing. They are `unreal:*` attributes on the stage's UsdRenderSettings prim, so Houdini's viewport and husk render with the same settings.

The Cornell box in both render modes.

<div class="hdue-figure" data-figure="render-mode"></div>

## Realtime or Path Tracer

hdUE has two render modes, the two renderers of UE itself.

| | Realtime (Lumen) | Path Tracer |
| --- | --- | --- |
| What it is | UE's deferred renderer, with Lumen global illumination and reflections, MegaLights and Virtual Shadow Maps | UE's reference path tracer, which accumulates samples per pixel |
| Cornell box, 512 x 512 | 48 ms per frame | 2.34 s per frame (512 samples) |
| Intel Sponza, 960 x 540 | 84 ms per frame | 6.64 s per frame (256 samples) |
| Noise | none; Lumen settles over its warmup frames | falls with the sample count, then the denoiser removes the rest |
| Use it for | look development, layout, animation previews, final frames that must render in milliseconds | reference images, glossy and refractive detail, final frames where accuracy matters more than time |

Times are per frame in husk on an NVIDIA GeForce RTX 4090, from the [gallery](../gallery.md). A Realtime time is a sequence frame after the first frame's warmup.

Lumen approximates glossy reflections of small, bright lights. On SideFX's Veach planks, the Path Tracer resolves each light on each plank.

<div class="hdue-figure" data-figure="render-settings-veach"></div>

Lumen also approximates bounce light. In the Bistro, lit by its lamps, compare Realtime without global illumination, Realtime with Lumen and the Path Tracer.

<div class="hdue-figure" data-figure="global-illumination"></div>

## Using render settings in Solaris

1. Add an **hdUE Render Settings** LOP (TAB menu > hdUE > Rendering), or use any Render Settings LOP: with hdUE installed, it gets an **Unreal Engine** tab.
2. Set the essential settings above the tabs: **Render Mode**, **Path Tracer Samples** or **Warmup Frames**, **Unreal Tonemapper** and **Exposure Compensation**.
3. Find the rest in the tabs, laid out as UE's editor lays them out: Rendering, Lens, Color Grading, Lighting, Fog, Sky, Rendering Features, Engine Quality.
4. On hdUE Render Settings, click **Show: Advanced** to see the settings UE's editor keeps behind its Advanced expanders. A notice under the strip names the changed settings that Essential hides.

Each control authors nothing until you change it, as on Karma's render settings. Settings that one render mode ignores are hidden in that mode.

The viewport's **Display Options** have an **Unreal Engine** tab with the settings you change while working: render mode, samples, warmup, debug view, motion blur, Lumen, subdivision, the tonemapper and the main scalability groups. The **Unreal** menu in Houdini's main menu bar switches **Render Mode** too.

Outside Houdini, author the attributes on the render settings prim:

```usda
def RenderSettings "Settings"
{
    token unreal:renderMode = "pathtraced"
    int unreal:samplesPerPixel = 256
    bool unreal:tonemapper = 1
    float unreal:postProcess:AutoExposureBias = 1
}
```

Three families cover all 371 settings:

- hdUE's own settings: `unreal:renderMode`, `unreal:samplesPerPixel`, `unreal:motionBlur`, ...
- `unreal:postProcess:<Property>`: any property of UE's post-process settings (`FPostProcessSettings`), with UE's name.
- `unreal:cvar:<console variable>`: a UE console variable, with `:` in place of `.` because USD attribute names cannot hold dots. `int unreal:cvar:r:Shadow:Virtual:Enable = 0` sets `r.Shadow.Virtual.Enable 0`. See [Engine quality](engine-quality.md).

## Render Mode

**Render Mode** (`unreal:renderMode`), default Realtime (Lumen).

Realtime renders with UE's deferred renderer and Lumen. Path Tracer renders with UE's path tracer. Path Tracer frames take 40 to 80 times longer than Realtime frames in the gallery scenes above.

## Path Tracer Samples

**Path Tracer Samples** (`unreal:samplesPerPixel`), default 2048, range 1 to 65536. Path Tracer only.

Samples per pixel. Render time grows in proportion to the samples; noise falls with their square root, so 4 times the samples halves the noise. With motion blur, the samples are divided between the motion sub-frames.

The Cornell box with the denoiser off.

<div class="hdue-figure" data-figure="samples"></div>

!!! tip
    With the [denoiser](denoising.md) on, you need far fewer samples: the gallery's Sponza renders with 256. Raise the samples when the denoised image smears fine texture or contact shadows.

## Warmup Frames

**Warmup Frames** (`unreal:warmupFrames`), default 64, range 1 to 256. Realtime only.

The frames UE renders before a frame counts as finished, so that Lumen's lighting and UE's temporal effects settle. The default is the warmup of Movie Render Queue's default configuration. In husk, only the first frame of a sequence warms up; each later frame continues from the previous one with one draw, as in Movie Render Queue.

Lumen gathers its bounce light over frames: too few warmup frames leave it dark and blotchy.

<div class="hdue-figure" data-figure="warmup"></div>

## Unreal Tonemapper

**Unreal Tonemapper** (`unreal:tonemapper`), default off.

Runs UE's tonemapper with its film curve, bloom, colour grading, vignette and film grain. On, the beauty is UE's display image: view it in Houdini with an sRGB view that only encodes (Un-tone-mapped), not Raw. Off, the beauty stays scene-linear for your OCIO view. See [Post-processing](post-processing.md) and [Colour management](color-management.md).

<div class="hdue-figure" data-figure="render-settings-tonemapper"></div>

**Disable Tone Curve** (`unreal:disableToneCurve`, default off) keeps the tonemapper's effects without the film curve, as Movie Render Queue's setting of the same name. hdUE turns it on when your OCIO config's default view is ACES or AgX.

## Exposure Compensation

**Exposure Compensation** (`unreal:postProcess:AutoExposureBias`), default 0, range -15 to 15, in stops. Needs the Unreal Tonemapper.

Each step doubles or halves the image's brightness before the tone curve. The USD camera's `exposure` adds to it. Exposure stays manual, so frames are repeatable; UE's auto exposure is under Lens > Exposure ([Metering Mode](../reference/render-settings/lens.md#unreal-postprocess-autoexposuremethod)).

<div class="hdue-figure" data-figure="render-settings-exposure"></div>

## Viewport and husk

The same settings render differently while you work in the viewport, so that it stays responsive:

- While you tumble, scrub or play, the Path Tracer renders at most 8 samples. It renders your full sample count once the view is still.
- While the camera or scene changes, Realtime renders at 50% screen percentage and upscales ([Lower Resolution While Moving](../reference/render-settings/rendering.md#unreal-cvar-r-hdue-interactiveresolution)). Full resolution returns 250 ms after the last change.
- The selection highlight's ID passes render only once the view is still.

husk and the USD Render ROP render with an offline profile, like Movie Render Queue's Game Overrides:

- every scalability group at Cinematic (the viewport uses Epic);
- every texture fully streamed in before the frame renders;
- no GPU timeout, so long Path Tracer frames are not reset by Windows.

Settings you author win over both profiles.

## All settings

The [render settings reference](../reference/render-settings/index.md) lists every setting with its attribute, default, range and UE's tooltip, tab by tab: [Rendering](../reference/render-settings/rendering.md), [Lens](../reference/render-settings/lens.md), [Color Grading](../reference/render-settings/color-grading.md), [Lighting](../reference/render-settings/lighting.md), [Fog](../reference/render-settings/fog.md), [Sky](../reference/render-settings/sky.md), [Rendering Features](../reference/render-settings/rendering-features.md), [Engine Quality](../reference/render-settings/engine-quality.md).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Realtime bounce light is dark or blotchy in husk | too few warmup frames | Raise **Warmup Frames** to the default 64. |
| Path Tracer image is noisy in the viewport while you move | the viewport caps samples at 8 while moving | Stop moving; the full sample count renders. |
| Path Tracer image keeps its noise in the final frame | the denoiser is off, or too few samples | Turn **Denoiser** on, or raise **Path Tracer Samples**. See [Denoising](denoising.md). |
| Exposure Compensation does nothing | the Unreal Tonemapper is off | Turn **Unreal Tonemapper** on, or use the camera's `exposure`. |
| Colours look wrong with the tonemapper on | the viewer applies its own display transform, or none (Raw) | Set the viewer to Un-tone-mapped sRGB. |
| A setting is missing from the LOP | it applies to the other render mode, or it is Advanced | Switch **Render Mode**, or click **Show: Advanced**. |

## Unreal Engine and Karma

The defaults are UE's: Realtime with Lumen, 64 warmup frames as Movie Render Queue, and the Path Tracer's own settings from UE's post-process settings. Karma has no Realtime mode. Karma's **Diffuse Limit** N matches the Path Tracer's [Max. Bounces](../reference/render-settings/rendering.md#unreal-postprocess-pathtracingmaxbounces) N + 1.
