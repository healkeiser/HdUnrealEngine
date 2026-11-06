# Lumen or the Path Tracer

hdUE renders in one of UE's two modes. **Realtime (Lumen)** is UE's deferred renderer with Lumen global illumination and reflections, the renderer of a game or a Movie Render Queue shot. **Path Tracer** is UE's reference renderer. This page shows both on four scenes, names where Lumen differs and how to close the gap.

The Cornell box in Realtime and in the Path Tracer.

<div class="hdue-figure" data-figure="tut-lumen-pt-cornell"></div>

Switch with **Render Mode** (`unreal:renderMode`) on the hdUE Render Settings LOP, or **Unreal > Render Mode**. Every scene below is in `samples/` and renders unchanged in both modes.

## Choosing

| | Realtime (Lumen) | Path Tracer |
|---|---|---|
| A frame | 64 warmup frames for a still; one draw per frame after the first in a husk sequence | **Path Tracer Samples** (default 2048), denoised |
| Noise | Converges over frames; can leave blotches in indirect light | Grain that falls with samples; the denoiser removes the rest |
| Bounces | Lumen's multi-bounce approximation | Up to **Max. Bounces** (default 32) |
| Reflections | One bounce; traced up to roughness 0.4 | Every bounce, every roughness |
| Use it for | Look development, animation, final frames | Reference images, glass and mirrors, stills where render time allows |

On the gallery's Global illumination Cornell box at 512 x 512 on an RTX 4090, a Lumen sequence frame takes 48 ms and the Path Tracer 2.34 s at 512 samples ([Gallery](../gallery.md)). Times are measured on the figures below.

Realtime is not a preview mode: UE renders final frames with it, and so can you. Use the Path Tracer to check what Lumen approximates, or when a shot depends on what Lumen does not do.

## Four scenes side by side

The room (`samples/docs/room.usda`), lit only through its window: almost all the light is bounced.

<div class="hdue-figure" data-figure="tut-lumen-pt-room"></div>

The Veach planks (`samples/docs/veach.usda`): four sphere lights of increasing size over four planks of increasing roughness.

<div class="hdue-figure" data-figure="tut-lumen-pt-veach"></div>

The shader ball studio (`samples/docs/lookdev.usda`).

<div class="hdue-figure" data-figure="tut-lumen-pt-lookdev"></div>

Surfaces lit directly by a light match closely between the two modes. The differences are in bounced light and reflections.

## What Lumen approximates

Each item comes from UE's Lumen documentation ([Lumen Global Illumination and Reflections](https://dev.epicgames.com/documentation/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine), [Lumen Technical Details](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine)).

| Symptom in Realtime | Cause | Fix |
|---|---|---|
| Blotches in mirror reflections indoors | Lumen lights its own scene at low quality for reflections | Raise **Lumen Scene Lighting Quality**, or set **Ray Lighting Mode** to **Hit Lighting for Reflections** |
| Small objects black in mirror reflections | Lumen culls small objects from its scene | Raise **Lumen Scene Detail** |
| Rough metal looks flat or blurry | Above **Max Roughness To Trace** (0.4), Lumen traces no dedicated reflection rays | Raise **Max Roughness To Trace** |
| No reflections inside reflections | **Max Reflection Bounces** is 1 | Raise it; UE supports more than 1 only with hardware ray tracing and hit lighting (**Ray Lighting Mode**) |
| A small emissive object lights the room unevenly, or noisily | Small, bright emissive areas are hard for the final gather, and small objects are culled | Use a light for a small source; keep emissive surfaces large |
| A smooth base under a clear coat reflects as rough | Lumen traces one reflection ray per pixel; only the top layer can be smooth | Use the Path Tracer for smooth clear-coated surfaces |
| Indirect light from a linked light reaches every object | UE's lighting channels restrict direct light only | The Path Tracer honours light links exactly ([Lights](../lights.md)) |

## Glossy reflections

The ball as polished metal (roughness 0.1): Realtime with the default **Ray Lighting Mode**, with **Hit Lighting for Reflections**, and the Path Tracer.

<div class="hdue-figure" data-figure="tut-lumen-pt-reflections"></div>

1. On the **Lighting** tab, set **Ray Lighting Mode** (`unreal:postProcess:LumenRayLightingMode`) to **Hit Lighting for Reflections**.
    Reflection rays are lit where they hit instead of from Lumen's surface cache. UE recommends it for the highest-quality mirror reflections; it costs GPU time.
2. For many smooth surfaces, also raise **Quality** under Lumen Reflections (`unreal:postProcess:LumenReflectionQuality`) from 1 to 2.
    It reduces noise in reflections.

The ball as rough metal (roughness 0.55), with **Max Roughness To Trace** at 0.4 and 1.

<div class="hdue-figure" data-figure="tut-lumen-pt-roughness"></div>

3. For rough metals, raise **Max Roughness To Trace** (`unreal:postProcess:LumenMaxRoughnessToTraceReflections`) above the material's roughness.
    UE's tooltip: higher values improve reflection quality but greatly increase GPU cost.

## Bounced light

1. Set **Ray Lighting Mode** to **Hit Lighting**.
    Lumen's diffuse rays are then lit at their hit point too. See [Lighting a room](lighting-a-room.md#lumen-and-the-path-tracer) for the room in this mode.
2. If indirect light stays blotchy, raise **Final Gather Quality** to 2. See [Removing noise](removing-noise.md#realtime-blotches-in-indirect-light).
3. Do not raise **Diffuse Color Boost** or **Indirect Lighting Intensity** to match the Path Tracer.
    UE's tooltip says values of Diffuse Color Boost above 1 are not physically correct; they brighten bounced light for art direction, not accuracy.

## The Path Tracer's own limits

- Grain needs samples; see [Removing noise](removing-noise.md).
- **Ambient Cubemap** lights the scene in Realtime only.
- Fireflies are clamped by **Max Path Intensity** (24), which removes some energy from bright indirect paths.

## Final render settings

- Animation and look development: **Render Mode** Realtime (Lumen), **Warmup Frames** 64, **Ray Lighting Mode** Hit Lighting for Reflections when the shot has mirrors.
- Reference stills: **Render Mode** Path Tracer, **Path Tracer Samples** 2048, **Denoiser** on.
- All settings: [Render settings](../rendering/render-settings.md) and the [Lighting reference](../reference/render-settings/lighting.md).
