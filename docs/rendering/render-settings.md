# Render settings

Renderer settings are `unreal:*` attributes on the UsdRenderSettings prim (the **Unreal Engine** tab of Render Settings LOPs, or the hdUE Render Settings LOP, in Houdini; see [Houdini](../houdini/viewport.md)). `unreal:*`, the spelling of hdUE's light and prim attributes, is accepted too. String settings accept `string` or `token` (Houdini's menus author tokens), numbers accept any numeric type. Houdini's viewport and husk read the same prim, so they render the same settings.

| Setting | Default | Meaning |
| --- | --- | --- |
| `unreal:renderMode` | `realtime` | `realtime` (Lumen) or `pathtraced` |
| `unreal:samplesPerPixel` | 2048 | Path Tracer samples (UE's `PathTracingSamplesPerPixel`) |
| `unreal:lumenEnabled` | 1 | Lumen GI and reflections in realtime mode |
| `unreal:warmupFrames` | 64 | Realtime frames before a frame counts as converged (the warm-up frame count of Movie Render Queue's default Basic configuration, `UMoviePipelineBasicConfig::NumWarmUpFrames`; Movie Render Graph renders its warm-up frames and discards them). In husk, only a sequence's first frame warms up: each later frame keeps the temporal history and draws once, as Movie Render Queue does after its warm-up |
| `unreal:motionBlur` | 1 | Motion blur when the camera shutter is open and something moves |
| `unreal:motionSamples` | 16 | Motion-blur sub-frames; Path Tracer samples are divided between them |
| `unreal:debugView` | `off` | One of UE's viewport view modes instead of the realtime beauty; see [debug views](debug-views.md) |
| `unreal:subdivisionLevel` | 0 | Uniform refinement of catmullClark and loop meshes (a prim's `primvars:unreal:subdivisionLevel` replaces it); see [subdivision](../geometry/subdivision.md) |
| `unreal:cullBackface` | 1 | Meshes whose `doubleSided` is off render their front faces only, as UE's USD importer does; 0 renders both sides, as Karma; see [geometry](../geometry/meshes.md) |
| `unreal:tonemapper` | 0 | Run UE's tonemapper and the effects inside it; see [post-processing](post-processing.md) |
| `unreal:disableToneCurve` | 0 | Movie Render Queue's Disable Tone Curve: the tonemapper's effects without UE's film curve, a scene-linear beauty; forced on when the OCIO default view is ACES or AgX ([colour management](color-management.md)) |
| `unreal:postProcess:<Property>` | | Any `FPostProcessSettings` property, for the beauty; see [post-processing](post-processing.md) |
| `unreal:cvar:<console variable>` | | A UE console variable, with `:` for `.` (`int unreal:cvar:r:Shadow:Virtual:Enable = 0`); see [engine quality](engine-quality.md) |
| `unreal:postProcessMaterials` | | Cooked post-process materials added to the beauty |
| `unreal:domeResolution` | 128 | Environment cubemap face size (UE's sky light `CubemapResolution`) |
| `unreal:autoViewerLight`, `unreal:viewerLightIntensity` | | Review light for scenes without lights |
| `unreal:upAxis` | stage | Up axis override |
| `unreal:substrateAutoPrepare` | on | **Prepare Edited Materials**: Substrate Builder graphs missing from the library, and MaterialX graphs the render-time translation cannot express, are prepared in the viewport as you edit, in husk before the frame ([background preparation](../substrate-builder.md#background-graph-preparation-in-solaris), [MaterialX](../materials.md#automatic-import-of-untranslatable-graphs)) |
| `unreal:materialxLibrary`, `unreal:substrateLibrary` | runtime profile | Cooked MaterialX and prepared Substrate libraries (`library.json`). Applied alike in Houdini's viewport, husk and usdview (`--hdue-substrate-library`); a stale or unprepared material fails a husk frame |
| `unreal:viewportIdAovs` | 1 | Houdini's viewport renders the `primId`/`instanceId` AOVs for its selection highlight once the view settles (skipped while tumbling, scrubbing or playing); 0 never renders them (no highlight) |

In Houdini's viewport, `houdini:interactive` (tumbling, scrubbing, playing) caps Path Tracer samples at 8 and skips the AOV passes; the full image renders once the viewport is still. Flipbooks render in full.

## Examples

The Cornell box (`samples/cornell`) rendered by hdUE with one setting changed; drag the slider.

**Render Mode**: Realtime (Lumen) against the Path Tracer at 512 samples.

<div class="hdue-figure" data-figure="render-mode"></div>

**Path Tracer Samples**, with the denoiser off: noise falls with the square root of the sample count.

<div class="hdue-figure" data-figure="samples"></div>

**Warmup Frames**: Lumen gathers its lighting over frames; the first frames of a still are not converged.

<div class="hdue-figure" data-figure="warmup"></div>

**Global Illumination Method**: no global illumination against Lumen. Without it, only direct light reaches the walls.

<div class="hdue-figure" data-figure="global-illumination"></div>
