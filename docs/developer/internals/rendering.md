# Rendering internals

Implementation notes behind the user pages on render settings, denoising, motion blur, debug views and engine quality.

## Render settings

`unreal:*` attributes on the UsdRenderSettings prim reach the delegate as render settings with the `unreal:` prefix stripped. String settings accept `string` or `token` (Houdini's menus author tokens); numbers accept any numeric type. In Houdini's viewport, `houdini:interactive` (tumbling, scrubbing, playing) caps Path Tracer samples at 8 (`HdUEState::samples` in `src/hydra/delegate.h`) and skips the ID AOV passes; a change of `houdini:interactive` is a new request.

## Offline profile

`Bridge.cpp` applies Movie Render Queue's Game Overrides when the host is not interactive (husk), before any session, so authored `unreal:cvar:*` values override them: Cinematic scalability keeping the resolution quality, `r.Streaming.FullyLoadUsedTextures 1` (with `FramesForFullUpdate 0` and `SyncStatesWhenBlocking 1`), `r.D3D12.GPUTimeout 0`, Lumen surface cache updates every frame (`r.LumenScene.Radiosity.UpdateFactor 2`, `DirectLighting.UpdateFactor 2`, `SurfaceCache.CardCapturesPerFrame 100000`, `CardCaptureFactor 4`, as Movie Render Graph's layer warm-ups raise them, `DeferredLayerWarmUpHelpers.cpp`), and the heterogeneous volume settings UE's docs recommend for cinematic renders (shading rate 1, 512 MB). Each change is logged with UE's value before it; `scripts/Test-RenderSettings.ps1` compares the generated defaults with those values.

## Denoiser

NNEDenoiser registers its denoiser from a view extension. The backend runs UE's active view extensions on every view family, as Movie Render Queue does (`MoviePipelineImagePassBase.cpp`).

A host that exits with UE running (husk) ends in the backend's `DllMain`, before the backend's static destructors. UE's denoiser registry would otherwise release NNEDenoiser's ONNX Runtime session into DirectML and the GPU driver after the exit had ended UE's threads (a fail-fast, 0xC0000409). `onnxruntime.dll` is loaded before the backend (`src/client/backend.cpp`) so that it detaches after it; loaded later, it detached first and husk never exited.

UE does not stage the files the OptiX plugin loads from its `Source` folder (`OptiXDenoiseBase.dll` and its CUDA kernels, `OptiXDenoiserFunctionList.ptx`); `Stage-Runtime.ps1` copies them. Without the kernels the image stayed black. In UE 5.8.4 the plugin's default 256 x 256 tiles fail in the prebuilt library (`OPTIX_ERROR_INVALID_VALUE`) and leave a garbled 384-pixel strip, so the runtime sets `r.OptixDenoise.TileWidth` and `TileHeight` to 0.

Measurements on the glossy Cornell box at 1920 x 1080 and 32 samples (noise as mean absolute Laplacian): none 0.00303, NNEDenoiser 0.00015, OptiX 0.00009; mean brightness 0.0291, 0.0290, 0.0291.

## Motion blur

The delegate renders `unreal:motionSamples` sub-frames when the camera shutter is open, `disableMotionBlur` and `instantaneousShutter` are off, and the camera or a mesh moves (`delegate.cpp`). Lumen keeps its history between sub-frames, so the warmup and the Path Tracer samples are divided between them. Deforming meshes are uploaded again for each sub-frame. Time-sampled PointInstancer placements without `velocities` and `angularVelocities` become a velocity and an angular velocity per instance, interpolated between the first and last sample around the frame, as `UsdGeomPointInstancer::ComputeInstanceTransformsAtTime` does. Velocities are converted with `houdini:fps`, else `framesPerSecond` (24) (`HdUEFramesPerSecond` in `prims.cpp`). In Realtime, UE's `MotionBlurAmount` is the shutter interval in frames divided by the motion samples, unless authored.

## Debug views

`unreal:debugView` sets the view mode the way UE's game viewport sets it: view mode show flags, the view's visualization name and material overrides. Buffer views are linear; the others are drawn through UE's tonemapper, as with `unreal:tonemapper`.

Not offered in the cooked runtime: Wireframe (UE composites it in the editor-primitives pass, compiled into editor builds only), Shader Complexity, Quad Overdraw, LOD coloration and the other material debug modes (their shaders are cooked only with `r.ForceDebugViewModes=1`, for every material), and the Lightmap modes. Cooked runtimes leave UE's buffer visualization data uninitialized, so hdUE blends the buffer materials (`/Engine/BufferVisualization`, cooked through `DirectoriesToAlwaysCook`) as post-process materials itself.

## DLSS

The DLSS plugin enables itself only for game and editor viewports, so hdUE adds its view extension to each realtime frame, as the plugin's Movie Render Queue support does. Ray Reconstruction is switched with the plugin's `EnableDLSSRR`, which turns off the denoisers it replaces (`r.Shadow.Denoiser`, Lumen reflections' temporal and screen-space reconstruction and bilateral filter) and restores them. With DLSS on, the viewport's moving frames size their buffers to their own percentage, as UE's dynamic resolution keeps to the upscaler's range. `Launch-Houdini.ps1 -TestKind dlss -InputUsd samples\smoke.usda -RuntimeConfig <a DLSS runtime>` checks it in Houdini's viewport.
