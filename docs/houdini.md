# Houdini Solaris, husk and AOVs

## Installing on another machine

Like HtoA, Redshift or V-Ray, hdUE ships as one relocatable folder plus a Houdini package file. On the build machine:

```powershell
.\scripts\Package-Houdini.ps1 -RuntimeConfig .local\runtime-houdini.json -Zip
```

This writes `out\package\hdUE-<version>-houdini22.0-win64` (about 1.2 GB unpacked, 0.9 GB zipped; PDBs are left out) and the matching `.zip`. The folder holds the delegate (`plugin\`), the Solaris integration, the Python helpers, the samples, the staged UE runtime (`runtime\`) and a `runtime.json` with paths relative to itself.

On the target machine (Houdini 22.0, Windows x64, a DirectX 12 GPU with hardware ray tracing; no UE, Visual Studio or OpenUSD needed), unzip the folder to a writable location, because UE writes its shader cache and logs into `runtime\Windows\HdUEBackend\Saved`. Then run `Install.cmd`. It writes `Documents\houdini22.0\packages\hdUE.json`, which sets `HDUE_HOUDINI_ENABLE`, `HDUE_RUNTIME_CONFIG` and `HDUE_HOUDINI_PLUGIN`, appends the plugin to `PXR_PLUGINPATH_NAME`, and adds the integration to `HOUDINI_PATH`. For a studio-wide install, use `Install.ps1 -PackagesDirectory <shared HOUDINI_PACKAGE_DIR>`. `Uninstall.cmd` removes the package file. Houdini, hython and husk all pick it up.

The package's `uiready.py` replaces Houdini's own (which only pre-warms the info window), as with any package that ships one.

## Solaris viewport

Pick **Unreal Engine (hdUE)** in the viewport's renderer menu. Renderer settings are `hdUE:*` attributes on a UsdRenderSettings prim, so any Render Settings LOP drives hdUE: with the delegate registered, Render Settings LOPs show them on an **Unreal Engine** tab (`integrations/houdini/soho/parameters/HdUEHoudiniRendererPlugin_Global.ds`), in Rendering, Lighting, Motion Blur, Post-Processing and Geometry sections; settings for one render mode only (Path Tracer samples and denoiser, realtime warmup and Lumen) are hidden in the other, and the Unreal tonemapper's effects are disabled while it is off. The viewport's Display Options get the interactive subset (`HdUEHoudiniRendererPlugin_Viewport.ds`). Light LOPs get one for the per-light `hdue:*` settings (`HdUEHoudiniRendererPlugin_Light.ds`). As with Karma's properties, each control defaults to not authoring. `hdue_houdini.set_render_setting()` edits the displayed chain's Render Settings LOP, adding one when there is none. See [render settings](rendering.md#render-settings).

Color and real UE depth share a frame ID and generation. Solaris receives linear axial depth in stage units with `depthstyle: linear`. The delegate provides `primId` and `instanceId` AOVs, so Solaris draws its native selection highlight on hdUE images. Click picking through those AOVs has not been verified separately; the Scene Graph Tree is the reliable way to select prims. The current target is one active hdUE viewport.

### Performance

In the Houdini UI, UE runs on its own thread and ticks continuously; the viewport only posts camera and scene changes and picks up finished frames, so a slow UE tick never blocks Houdini. The host redraws the viewport when a frame arrives.

- **GPU frames.** Houdini's OpenGL viewport takes UE's frames without leaving the GPU (`include/hdue/gpu_api.h`, `src/hydra/gpu_frames.cpp`). UE writes each draw into a ring of three shared D3D12 textures (RGBA16F colour with straight alpha and the camera exposure applied, R32F linear depth in stage units, bottom row first) and signals a shared fence; on each redraw, GL waits for that fence on the GPU, copies the newest frame into HgiGL textures and hands them to Houdini's `HdxAovInputTask`, so nothing is read back or uploaded. A second fence tells UE when GL has finished reading a slot. The CPU render buffers then receive only converged frames (for `Map()`: the Python viewer, scripts). Motion blur, the denoiser and an OCIO config whose `scene_linear` is not linear Rec.709 keep the CPU path, as do husk, usdview and Live Render. It needs the GL driver's `GL_EXT_memory_object_win32` and `GL_EXT_semaphore_win32` and Houdini's GL on UE's GPU; otherwise, or with `HDUE_GPU_FRAMES=0`, frames are read back as before.

- **Image resolution.** Display Options > Render > Image Resolution set to *Render Settings Prim* renders at the render settings' resolution; the viewport size is cheaper while navigating.
- **Selection IDs.** Solaris asks for `primId` and `instanceId` for its selection highlight. Rendering them swaps every material twice per converged view, so the viewport leaves them out unless `HDUE_VIEWPORT_ID_AOVS=1`; selection then works from the Scene Graph Tree but is not highlighted in the hdUE image.
- **Loading.** Textures decode in parallel and meshes are assembled on all cores. Meshes are uploaded as static meshes (cached draw commands and static ray-tracing geometry), their render data built on all cores, and are swapped for Nanite meshes in the background, one per tick. Meshes with wet surface effects stay dynamic.
- **Re-created prims.** A prim deleted and re-created with identical data (Houdini rebuilds the Hydra scene when its LOP chain changes) is not re-uploaded.
- `HDUE_TRACE_TIMING=1` (with `HDUE_TRACE_FILE=<path>`, since Houdini's UI has no visible stderr) logs where each frame's and each scene upload's time goes.

`HDUE_UE_ARGS` appends arguments to UE's command line, for example `-csvGpuStats -csvCaptureFrames=600` for a CSV profile in `HdUEBackend/Saved/Profiling`.

### Development launcher

From a source checkout, `Launch-Houdini.ps1` registers `Unreal Engine (hdUE)` for its process without editing the user's Houdini environment file:

```powershell
.\scripts\Update-HoudiniRuntime.ps1 -RuntimeConfig .local/runtime.json -SetDefault
.\scripts\Launch-Houdini.ps1 -InputUsd samples/smoke.usda -RuntimeConfig .local\runtime-houdini.json
.\scripts\Test-Houdini.ps1
```

`.local/runtime.json` names the staged runtime (`backend_dll`, `runtime_project`, both under `out/runtime/Windows/HdUEBackend`). The update script places the current depth-capable DLL under a content-hash filename; `-SetDefault` records an ignored fallback profile. An explicit `-RuntimeConfig`, `HDUE_RUNTIME_CONFIG`, or the local `runtime_config` key takes priority over that fallback. Restart Houdini to change runtimes.

The launcher creates a source layer, an `hdUE_settings` subnet that prepares the stage, and a standard Render Settings LOP (`hdUE_render_settings`, `/Render/rendersettings`, which the stage's `renderSettingsPrimPath` names). Output uses **Y-up and one unit per meter**: geometry wired into `hdUE_settings` is rescaled from the source stage's units, so add test geometry in those units. Normalization preserves prim paths, original transform interpolation, reset stacks, and camera optical dimensions. The subnet's source-axis and source-unit controls handle inputs whose metadata was lost during composition.

For an existing LOP output, replace `YOUR_OUTPUT` with a node in the current network and run this in Houdini's Python Shell:

```python
import hdefereval, hdue_houdini

def connect_hdue():
    node = hdue_houdini.settings_node(hou.node('/stage/YOUR_OUTPUT'))
    node.setDisplayFlag(True)
    node.setCurrent(True, clear_all_selected=True)
    hdue_houdini.activate()

hdefereval.execute_deferred(connect_hdue)
```

When Reference or Edit LOPs are added after `hdUE_settings`, the host places an `hdUE_viewport_prepare` LOP after the displayed output. This final layer expands instanceable references and resolves embedded/COP images before material binding and Hydra translation. The artist's input layers and referenced files retain their original instancing and attributes. The generated LOP follows display-output changes and is removed when switching renderers or closing the host.

## Rendering to disk with husk

husk and the USD Render ROP load the same delegate. When no host has started UE, the first hdUE delegate in the process starts it from the runtime profile named by `HDUE_RUNTIME_CONFIG` (`backend_dll`, `runtime_project`), ticks it from the render pass, and shuts it down with the last delegate. UE allows one lifetime per process, which matches one husk invocation rendering a frame range.

```powershell
husk --renderer HdUEHoudiniRendererPlugin -f 1 -n 24 -o render.$F4.exr scene.usd
```

From a source checkout without the package, set the environment first:

```powershell
$env:HOUDINI_PATH = "$PWD\integrations\houdini;&"
$env:PXR_PLUGINPATH_NAME = "$PWD\build\houdini-22.0.466\resources"
$env:HDUE_HOUDINI_ENABLE = '1'
$env:HDUE_RUNTIME_CONFIG = "$PWD\.local\runtime-houdini.json"
```

UE renders offscreen (`-RenderOffscreen`), in husk, Live Render, the Solaris viewport and usdview alike: it opens no window, and frames come from GPU readback as before. Set `HDUE_SHOW_WINDOW=1` to get UE's window back for debugging. UE's log reaches the host console as UTF-8 (`-stdout`) and is written in full to `HdUEBackendSavedogs`.

UE starts once per husk process, and Hydra resends only what changes between frames, so render a frame range in one husk call (and give farm tasks chunks of frames) rather than one process per frame. On an RTX 4090, a 1080p Lumen frame of a simple scene with the default 16 warmup frames takes about 0.3 s including the EXR write, after about 3 s of UE startup per process. Realtime mode (`hdUE:renderMode = realtime`) is meant for final frames too; heavier scenes cost more, mostly on the GPU. When husk owns UE, the delegate ticks it continuously instead of once per husk poll, and only the first, the converged and (for progress) one draw per 250 ms are read back from the GPU.

husk's progress (`-V 2` and above, and the Render Gallery) follows the delegate's `percentDone` stat: Path Tracer samples, or 100 once the realtime warmup converges. If none of the requested render vars is one hdUE renders, the frame fails with an error instead of waiting forever.

A USD Render ROP passes its environment to husk. The delegate restores husk's working directory, which loading UE changes, so relative product names resolve as with other renderers. Unsaved SOP volumes are written next to the USD file by the ROP and render from there.

## Live Render and Copernicus

The Live Render LOP accepts **HdUEHoudiniRendererPlugin** as its render delegate. It renders inside the Houdini process on its own threads, while UE belongs to Houdini's UI thread (or, when nothing in the process has started UE, to a thread hdUE starts for it, as in husk). The delegate queues its UE work for that thread, which runs it after each UE tick; a render whose work is not picked up within 5 s fails with an error rather than blocking. A File COP with **Source: Live Render** can start and stop it for compositing in Copernicus.

The backend supports one render session per process, so a Live Render and an hdUE Solaris viewport cannot run at the same time (from the code; not tested). The Live Render has been checked to render and converge with its AOVs; the image reaching the File COP has not been verified by the automated tests (in the scripted test the File COP stayed empty with Karma too), so check it in the UI.

## AOVs

The beauty is the render var whose `sourceName` is `color`, `C`, `Ci` or `beauty` (`raw`), or any `lpe` source such as Karma's `C.*[LO]`. Besides the beauty and `depth`, render vars may request these `sourceName`s, rendered from the GBuffer (Substrate's first BSDF). The delegate matches a binding by its `sourceName`, so the render var and channel may be named anything.

| AOV | Format | Contents |
| --- | --- | --- |
| `albedo` (or `baseColor`) | color3f | Base color |
| `normal` | normal3f | World-space shading normal in USD axes, normalized |
| `roughness`, `metallic`, `specular`, `occlusion` | float | Material channels |
| `primId` | int | Hydra prim ID of the visible mesh, -1 for background |
| `instanceId` | int | Hydra instance index of an instanced prototype (nested instancers flattened as in hdEmbree), -1 elsewhere |

After the beauty converges, each bound AOV is rasterized from the same camera and time with Lumen, path tracing, anti-aliasing and post effects off, so the channels stay exact and pixel-aligned with a Path Tracer beauty. They describe the nearest opaque surface at the current time (without motion blur); translucent surfaces and emission are not separated. Each extra AOV costs one short raster render per frame. AOV renders use their own UE view state and frame slot (`include/hdue/aov_api.h`), so the beauty's Lumen or Path Tracer history is unaffected.

`primId` and `instanceId` come from unlit passes in which every mesh draws `M_HdUEId` with its prim ID in custom primitive data, and instanced meshes their Hydra instance index in per-instance custom data. Cutout opacity is ignored there, so masked foliage selects as its whole card, and meshes placed from the native asset catalog read as background. Because AOVs follow the converged beauty, a viewer that tints its selection from `primId` (usdview, Solaris) can show the previous frame's IDs for a moment after an edit.

## COP and embedded textures

Shader file inputs accept absolute Houdini node paths such as `op:/obj/geo1/copnet1/OUT[0]`. The suffix selects a Copernicus output port; the default is zero.

COP evaluation and EXR export run outside LOP cooks. Dirty and time-dependent outputs are checked approximately every 0.25 seconds. Changed images receive immutable content-hash paths and reset Path Tracer accumulation. Unchanged images are reused. Cache files and the helper ROP are removed on shutdown.

Copernicus color outputs use 32-bit float linear Rec.709 with the default OCIO configuration. Inputs marked `raw` bypass conversion. Custom configurations must supply the corresponding output colorspace. Legacy COP color-plane export requires integer frames.

MaterialX Image nodes and UsdUVTexture read these textures in every input hdUE translates (see [MaterialX surfaces](rendering.md#materialx-surfaces)); the basic Preview Surface fallback added to MaterialX-only materials is kept for other Hydra renderers. COP UDIM collections, alpha-to-opacity wiring, COP UDIM collections, and texture replacement inside cooked native materials are not implemented.

Embedded `opdef:` images are also supported. Houdini extracts the original image bytes into immutable content-hash files without changing the source definition or USD asset. MaterialX preparation automatically extracts these images when `houdini_root` is configured; `Import-UsdMaterialX.ps1 -HoudiniRoot` can select it explicitly.

```powershell
.\scripts\Test-HoudiniOpTextures.ps1
```

Light `op:` and `opdef:` texture inputs use the same preparation and cache path as material images.
