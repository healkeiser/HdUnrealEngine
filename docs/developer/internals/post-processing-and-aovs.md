# Post-processing, colour and AOVs: internals

Developer notes moved out of the user pages ([Post-processing](../../rendering/post-processing.md), [Colour management](../../rendering/color-management.md), [AOVs](../../output/aovs.md), [Alpha](../../output/alpha.md)).

## Colour management

`src/hydra/color_management.cpp` relates the host's OCIO config to OCIO's built-in `Linear Rec.709 (sRGB)` through the config's interchange role. The beauty goes through `HdUEToSceneLinear`; authored colours through `HdUEFromSceneLinear`; colour textures through `HdUELinearRec709Texture`, cached as EXRs in `%TEMP%/hdue_ocio`. `HDUE_COLOR_MANAGEMENT=0` or a config OCIO cannot relate leaves everything linear Rec.709.

`HdUEViewTransformsDisplay` decides whether the config's default view is a display transform of its own: it sends an sRGB-encoded ramp through the view's inverse and compares it with the plain conversion to `scene_linear`. When it is, the delegate sends tonemap mode 2 (no tone curve), as UE's OCIO viewport display and Movie Render Queue's Color Output force Disable Tone Curve (`MoviePipelineColorSetting.cpp`).

With the tone curve, the backend captures `SCS_FinalToneCurveHDR` (the film curve's output as linear Rec.709, as UE writes float back buffers); without it, `SCS_FinalColorHDR`.

## Post-process settings

The delegate sends every `unreal:postProcess:*` name and value as text; the backend imports them into an `FPostProcessSettings` by reflection and sets the override flags. Unknown names or values are skipped with a warning in the UE log. Texture settings take a cooked path or an image file, imported at render time with UE's texture defaults (8-bit sRGB, float linear, unfiltered mips) and re-imported when the file's time or size changes. Ambient Cubemap converts a lat-long into a cubemap with box-filtered mips.

Order of blending (`ApplyPostProcess` in `Bridge.cpp`): hdUE's defaults (manual exposure, bias 0), then the world's post-process volumes (`UWorld::AddPostProcessingSettings`), then what the request decides (render mode, samples, lighting components, the USD camera's depth of field), then the render settings, as Movie Render Queue blends a camera's settings after the volumes (`MoviePipelineUtils.cpp`, `DoPostProcessBlend`). With the tonemapper on, the USD camera's exposure is added to `AutoExposureBias`; with it off, the delegate multiplies the beauty by the camera's linear exposure scale.

The render settings generator (`scripts/generate_render_settings.py`) leaves out `WeightedBlendables` (use `unreal:postProcessMaterials`), `PathTracingSamplesPerPixel` (use `unreal:samplesPerPixel`), mobile depth of field, and the fixed-size arrays `LensFlareTints` and `DepthOfFieldMatteBoxFlags`.

UE 5.8.4's `AmbientCubemapComposite.usf` never defines `SUBSTRATE_LOAD_FROM_MATERIALCONTAINER`, so with Substrate's adaptive GBuffer it adds nothing; `Apply-ResidencyPatch.ps1` adds the define ([repro](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/repro/ambient_cubemap/README.md)).

## Viewport and husk parity

On the Cornell box with the tonemapper on, the viewport's converged frame is 1.03x husk's mean, its 10th, 50th and 90th percentiles within 1.5/255 in sRGB; husk with the viewport's quality profile renders 0.98x. Houdini 22.0.368's viewport OCIO display shows linear values below about 0.004 as black and lowers values up to about 0.05, although OCIO's CPU processor and OCIO 2.5's GPU shader for the same view are the exact sRGB curve. Reproduce with `Launch-Houdini.ps1 -TestKind parity -InputUsd <stage>` and `HDUE_TEST_CAMERA`: it writes the delegate's frame (`viewport.pfm`) and flipbooks with the viewport's OCIO on and off.

## AOVs

`src/hydra/aov_table.h` is the AOV table; `scripts/build_render_settings_hda.py` reads it for the Houdini assets. Requests go through `include/hdue/bridge.h` (`HDUE_AOV_*`), with their own UE view state and frame slot, so the beauty's Lumen or Path Tracer history is unaffected.

- Buffer AOVs: one raster render with Movie Render Queue's buffer visualization pipes (`FinalPostProcessSettings.BufferVisualizationPipes`, as `MoviePipelineDeferredPasses`), Lumen, anti-aliasing and post effects off. With `velocity`, the pass first renders the previous frame (unpublished) so UE's velocity buffer holds the motion since then.
- Path Tracer albedo and normal: read back from the converged beauty (`PathTracingBufferTexture`), used when the AOV request shows the same view; a motion-blurred beauty falls back to the raster buffers.
- Lighting components: the Path Tracer's `PathTracingInclude*` settings; realtime uses the Diffuse, Specular and Direct Lighting show flags and turns Lumen GI or reflections off. The tonemapper and eye adaptation show flags are off for every AOV.
- ID passes: generated materials switch to emitting the prim ID from custom primitive data 5 (`IdSwitch.cpp`), keeping their cutout opacity; meshes with cooked native materials draw `M_HdUEId`, which ignores cutout opacity. `unreal:cvar:r:HdUE:IdSwap = 1` swaps every mesh.
- Cryptomatte: 16 `primId` passes on a 4 x 4 sub-pixel grid; ids are MurmurHash3_32 of the prim or material path (`src/hydra/cryptomatte.h`). The metadata reaches husk through the render stats and `husk.metadata` in `UsdRenderers.json`.

## Alpha

`r.PostProcessing.PropagateAlpha=True` is set in `DefaultEngine.ini` (before TSR caches its alpha permutation). UE keeps 1 - coverage in scene colour alpha; the readback (`Bridge.cpp`, `SharedFrame.usf`) outputs coverage. The realtime HDRI background (`Environment.usf`) writes alpha 1 into scene colour, which is coverage 0.
