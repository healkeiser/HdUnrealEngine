# Building from source

## Requirements

- Unreal Engine **5.8.4** source: tag `5.8.4-release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`.
- Windows x64, Visual Studio 2022 C++ tools, a Windows SDK, DirectX 12, and hardware ray tracing. The CMake scripts use the `Visual Studio 17 2022` generator.
- MSVC **14.44.35211 or newer** (a current Visual Studio 2022 17.14 update) or **14.50.35723 or newer**. UE 5.8.4 rejects MSVC 14.39-14.43, 14.44 before 35211, and 14.50 before 35723; `Engine/Config/Windows/Windows_SDK.json` in the engine source holds the authoritative list.
- .NET Framework **4.8 SDK** and **4.6.2 targeting pack** (Visual Studio Installer components `Microsoft.Net.Component.4.8.SDK` and `Microsoft.Net.Component.4.6.2.TargetingPack`). Without them, UnrealBuildTool stops with `Could not find NetFxSDK install dir`.
- PowerShell 7, Git, and CMake **3.24 or newer**, with Git and CMake available on `PATH`.
- OpenUSD **26.03** C++ SDK with Hydra/usdImaging, usdview, and Python **3.11** bindings for the standalone host and usdview. Build it with a moderate job count (`-j 8`); 28 parallel MSVC jobs ran out of compiler heap on a 64 GB machine.
- Houdini **22.0** with its bundled USD **26.05** and Python **3.13**, for Solaris; see [Supported Houdini builds](#supported-houdini-builds).
- `uv` for all Python execution and dependency management.

The standalone and Houdini delegates use separate builds because their USD ABIs differ. The UE backend disables UE's USD and Python plugins. Dependencies retain their own licensing requirements.

## Local configuration

Run commands from the repository root. Store installation locations in the ignored `.local/paths.json` file, environment variables, or explicit script parameters. Fill in the values locally; do not commit them.

```json
{
  "engine_root": "",
  "editor_root": "",
  "openusd_root": "",
  "houdini_root": ""
}
```

| Local key | Environment variable | Script parameter |
| --- | --- | --- |
| `engine_root` | `HDUE_ENGINE_ROOT` | `-EngineRoot` |
| `editor_root` | `HDUE_EDITOR_ROOT` | `-EditorRoot` |
| `openusd_root` | `HDUE_OPENUSD_ROOT` | `-OpenUsdRoot` |
| `houdini_root` | `HDUE_HOUDINI_ROOT` or `HFS` | `-HoudiniRoot` |
| `houdini_build_root` | `HDUE_HOUDINI_BUILD_ROOT` | `-BuildDirectory` (Houdini build/launch scripts) |
| `runtime_config` | `HDUE_RUNTIME_CONFIG` | `-RuntimeConfig` |
| `review_scene` | `HDUE_REVIEW_SCENE` | `-InputUsd` |

Explicit parameters take priority over environment variables and local configuration. `HDUE_LOCAL_CONFIG` selects an alternative local configuration file. Relative values stored in that file are resolved against its directory; relative explicit parameters and environment values use the current working directory.

`runtime_config` is read by `Launch-Houdini.ps1` and `Update-HoudiniRuntime.ps1`. Pass `-RuntimeConfig` explicitly to `Launch-Usdview.ps1`; it otherwise uses the repository's staged runtime. `review_scene` supplies the input for `Test-Usdview.ps1 -Check opacity` and `-Check glass`.

Keep private projects, scene-specific helpers, catalogs, runtime profiles, and review evidence in `.local/`, `external/`, or `out/`. These directories are ignored. The source under `unreal/HdUEBackend` remains version controlled; its generated Content and build products do not.

## Build and validation

```powershell
.\scripts\Setup-Engine.ps1
.\scripts\Verify.ps1 -WithUsdview
```

`Setup-Engine.ps1` obtains the pinned source using your own engine access. `Verify.ps1` builds the host, prepares synthetic UE content, builds the backend DLL, stages a runtime, runs rendering checks, and compares native UE and Hydra HDR images. `-WithUsdview` adds GUI, camera, and buffer checks. `-SkipBuild` reuses compatible build and cook outputs.

Individual steps:

```powershell
.\scripts\Build-Host.ps1        # standalone delegate and hosts (OpenUSD)
.\scripts\Prepare-Content.ps1   # synthetic UE content: Preview Surface, ID, volume materials
.\scripts\Build-Backend.ps1     # UE backend DLL
.\scripts\Stage-Runtime.ps1     # cook and stage the runtime
.\scripts\Build-Houdini.ps1     # Houdini delegate (no OpenUSD needed)
.\scripts\Update-HoudiniRuntime.ps1 -RuntimeConfig .local\runtime.json -OutputConfig .local\runtime-houdini.json
.\scripts\Package-Houdini.ps1 -RuntimeConfig .local\runtime-houdini.json -Zip -UnrealLicenseesOnly  # licensees only: see docs/getting-started/installation.md
```

`Stage-Runtime.ps1` replaces the staged binaries, including the hashed Houdini copy of the backend, so run `Update-HoudiniRuntime.ps1` after every stage.

### Runtime profile

No script writes `.local/runtime.json`: create it once, after the first `Stage-Runtime.ps1`. It names the staged runtime (by default under `out\runtime`):

```json
{
  "version": 1,
  "runtime_project": "../out/runtime/Windows/HdUEBackend/HdUEBackend.uproject",
  "backend_dll": "../out/runtime/Windows/HdUEBackend/Binaries/Win64/HdUEBackend.dll"
}
```

`version` must be 1. Relative paths resolve against the profile's folder, so these start with `../`; absolute paths work too. With `Stage-Runtime.ps1 -OutputDirectory`, point both paths into that folder. Two optional keys: `asset_catalog`, a catalog of a project's assets (`Prepare-ProjectAssets.ps1` writes a complete profile with one), and `materialx_library`, a prepared MaterialX library ([materials](developer/internals/materials.md)).

`Update-HoudiniRuntime.ps1` reads this profile and writes the Houdini one (`runtime-houdini.json`): the same keys, with `backend_dll` replaced by the hashed copy. Do not edit the Houdini profile by hand.

### Test tiers

`.\scripts\Test.ps1 -Suite <name>` runs one tier:

- `unit` (the default): needs only `uv`, and is what CI runs. Repository scope, the Python tests, the committed gallery's brightness bands.
- `native`: the C++ tests (`ctest`) of the standalone and Houdini builds that exist. It does not rebuild; it fails if neither build exists.
- `hydra`: `Verify.ps1 -SkipBuild -WithUsdview`: native UE and Hydra renders, usdview, camera and buffer checks.
- `houdini`: the in-Houdini tests, the prim LOP tests and husk's streaming test.
- `conformance`: `run_conformance.py` on the scenes named by `-Only <area>/*` (for example `-Only materialx/*`). It refuses to run without `-Only`, because the full set takes over an hour.
- `extended`: the in-Houdini kinds no other tier starts, each on its own scene, and husk's prepared-material test with every check required. It prints what it does not run. Its kinds and husk bind prepared libraries under `out\` (`-LibraryRoot` for another folder), each prepared from this checkout's samples (a library records its source files); a missing one fails the kind that needs it:

    ```powershell
    .\scripts\Import-MaterialX.ps1 -Source samples\materialx\standard_surface.mtlx,samples\materialx\open_pbr_surface.mtlx -OutputDirectory out\materialx
    .\scripts\Import-MaterialX.ps1 -Source samples\materialx\standard_surface.mtlx -WetProfile -OutputDirectory out\materialx-wet
    .\scripts\Prepare-Substrate.ps1 -InputUsd samples\decal\decal.usda -OutputDirectory out\substrate\decal-<date>
    .\scripts\Prepare-Substrate.ps1 -InputUsd samples\volume_material\volume_material.usda -OutputDirectory out\substrate\volume-<date>
    .\scripts\Prepare-Substrate.ps1 -InputUsd samples\substrate\showcase.usda -MaterialXLibrary out\materialx\library.json -OutputDirectory out\substrate\showcase-<date>
    .\scripts\Stage-Runtime.ps1; .\scripts\Update-HoudiniRuntime.ps1 -SetDefault   # the cooked packages reach the runtime
    ```
- `all`: `unit`, `native` (when a build exists), `hydra` and `houdini`. It says which of them did not run, and that conformance and extended were not run.

Dev runs must test the dev build, not an installed hdUE package. `Launch-Houdini.ps1`, `Test-Houdini.ps1`, `run_conformance.py` and `render_gallery.py` add `hdUE` to `HOUDINI_PACKAGE_SKIPLIST` (comma-separated, so existing entries stay). `render_gallery.py --dev` also uses the runtime profile in `.local`, even when an installed package set `HDUE_RUNTIME_CONFIG`.

### PSOs

UE compiles a pipeline state object (PSO) the first time a draw or dispatch needs one. hdUE ships no recorded PSO cache: material PSOs are precached when meshes are created (UE's runtime PSO precaching; a mesh appears once its PSOs are ready, UE's default `r.PSOPrecache.ProxyCreationStrategy`), and the GPU driver keeps every compiled PSO in its own disk cache across runs. A bundled cache, recorded by rendering representative scenes, only covered the passes the recording happened to render and needed recording again after every shader change; with a warm driver cache it changed nothing measurable (`texture/udim`, two runs per mode: realtime 8.2/7.2 s with it, 6.9/6.8 s without; Path Tracer 6.9/7.0 s with, 7.2/7.2 s without).

The slowest first frame is not a missed PSO. In UE's CSV profile of a Cornell Path Tracer render, `PSO/PSOMisses` stays 0. (`HDUE_UE_ARGS=-csvCaptureFrames=<n>` writes it to `%LOCALAPPDATA%\hdUE\Saved\Profiling\CSV`; choose `n` below the frames the render draws: the file is written when the capture ends, and the process exits without UE's shutdown.) The first Path Tracer frame then spends about 340 ms after `Recreating Persistent SBTs`: UE builds the ray tracing pipeline from the material hit shaders, and offline renders wait for it rather than drawing samples with the opaque black fallback that `r.RayTracing.NonBlockingPipelineCreation` uses meanwhile in interactive views (`RayTracingMaterialHitShaders.cpp`). A bundled cache can precompile those shaders, but the pipeline is still assembled from them on that frame.

Global and post-processing passes that UE does not precache (bloom and blur filters, depth of field, lens flares, the tonemapper, virtual texture page table updates) log `Encountered a new graphics PSO (PSOPrecacheState: Missed)` the first time each process binds them, 20-35 lines per run. They are UE's Display-level notice that a PSO was created on first use; the driver serves it from its cache. Only the first run after a driver update or cache wipe compiles them for real, a one-off hitch in the first frames. `r.PSOPrecache.Validation=2` logs the pass and material of each miss in a material pass, which would be an hdUE precaching gap to fix.

At the end of its pre-initialization UE precaches the compute PSOs of the global shaders (`PrecacheComputePipelineStatesForGlobalShaders`, `r.PSOPrecache.GlobalShaders`, 5,404 shaders), one task per shader on the worker threads. Their code is already in memory: loading a shader map reads all of its shaders' code (`FShaderLibraryInstance::AddResource`), and creating a shader releases its map's hold on that code (`FShaderMapResource::CreateShaderOrCrash`). Shaders with identical code share one library shader, and `FShaderLibraryInstance::GetOrCreateShader` creates outside its lock, so two tasks could create it at once: the second, having missed the cache, found the code released by the first and read it again on its worker. Each such read logged "Spent N ms in a blocking wait for shader preload" (18-35 per Houdini session or husk run, NumRefs 1, frame 0); the default material's Nanite raster PSO precache hit the same race. `Apply-ResidencyPatch.ps1` makes `GetOrCreateShader` check its cache again under the lock it already takes, and otherwise hold the code until the shader is created. Turning the global precache off (`r.PSOPrecache.GlobalShaders=0`) also removed the waits, but left 222 PSOs to compile on the render thread.

The monolithic DLL requires a source engine. A matching editor installation can prepare content. `Apply-ResidencyPatch.ps1` makes small engine edits, validating the engine version and each insertion point: one friend declaration for virtual-texture residency, an editor-only guard around NaniteBuilder's displacement-texture read (`NaniteDisplace.cpp`) so the runtime can build Nanite meshes, and the `SUBSTRATE_LOAD_FROM_MATERIALCONTAINER` define UE 5.8.4's `AmbientCubemapComposite.usf` lacks (without it the Ambient Cubemap adds nothing with Substrate's adaptive GBuffer), and, in `NaniteStreamingManager`, the editor's path for streaming Nanite pages from memory in non-editor builds too (runtime-built meshes keep their pages in memory; cooked ones are never loaded there and stream from disk as before), the same memory path for virtual texture chunks in `UploadingVirtualTexture.cpp` (runtime UDIM sets keep their chunks in memory), the cache check in `ShaderLibraryInstance.cpp` described above, and alpha in rough refraction's copy of the scene (`DistortFiltering.usf` wrote alpha 1 into a `PF_FloatR11G11B10` mip chain, `DistortionRendering.cpp`, so with `r.PostProcessing.PropagateAlpha` everything seen through glass came out uncovered: alpha 0.06 over opaque objects). `ReflectionEnvironmentShaders.usf` gets one line in the cube filter (`FilterPS`): a 512-texel sky light filters its top mip with GGX at roughness 2^(-7/1.2), where `ImportanceSampleGGX`'s `sqrt(1 - CosTheta^2)` is NaN on the GPU for 5 of its 32 samples; each still passed `NoL > 0` and read the cube in a NaN direction, so a dome's pole colour leaked 5/32 into every direction. A NaN half vector becomes (0, 0, 1), its exact limit; `MonteCarlo.ush` itself stays unchanged, since material shaders include it. The Path Tracer's solid glass (`PathTracingMaterialHitShader.usf`, `PathTracingSubstrateSolidGlass.ush`) tints the surface of a non-scattering glass slab whose mean free path is under 1 cm, the 0.01 cm layer UE's Standard Surface function makes of transmission depth 0, instead of absorbing inside the object; the tint travels in `SpecularEdgeColor`, which solid glass does not read, flagged by the payload's free output bit. Those are material shaders: re-cook the content after applying it, then stage the runtime (`Stage-Runtime.ps1`): `Update-HoudiniRuntime.ps1` alone stages the DLL, not the cooked shaders. The shader edits go into both the source engine and the editor installation (`editor_root`), since the editor's cook compiles the runtime's global shaders; prepare content and stage the runtime after applying it. The global shaders are a pair, `Engine/GlobalShaderCache-PCD3D_SM6.bin` and `ShaderArchive-Global-PCD3D_SM6-PCD3D_SM6.ushaderbytecode`: a runtime given one without the other fails at boot ("Could not load section ... of the global shadermap"). UELibrary and Launch are otherwise unchanged.

### NVIDIA DLSS

DLSS is optional and comes from NVIDIA's plugin, which is never committed: the NVIDIA RTX SDKs license does not allow its source to be redistributed or put under this repository's license. Download the UE 5.8 package from [NVIDIA's DLSS page](https://developer.nvidia.com/rtx/dlss/get-started) (an NVIDIA Developer login; tested with `DLSS4.5Plugin_v8.9.0`) and copy two of its plugins into `unreal/HdUEBackend/Plugins/` (ignored by git):

- `Plugins/DLSS`: `DLSS.uplugin`, `Source`, `Shaders`, `Config`, `Resources`, `Content`, `Binaries/ThirdParty` (the NGX DLLs) and `Binaries/Win64` (the prebuilt editor modules, which the cook loads; their `BuildId` must match `editor_root`'s).
- `Plugins/StreamlineNGXCommon`: `StreamlineNGXCommon.uplugin`, `Source`, `Resources` and `Binaries/Win64`.

The Streamline, Frame Generation, NIS and Movie Pipeline plugins are not needed. `HdUEBackend.uproject` lists both plugins as optional, and `HdUEBackend.Build.cs` links DLSS only when `Plugins/DLSS/DLSS.uplugin` exists. After adding them, run `Build-Backend.ps1`, `Prepare-Content.ps1` (the editor's cook compiles the plugin's global shaders) and `Stage-Runtime.ps1`; staging copies the NGX DLLs. [Engine quality](rendering/engine-quality.md) describes the setting.

Fresh verification evidence is written under `out/`. A successful exit code alone is insufficient: tests require new images, metadata, and clean engine shutdown.

C++ follows Google style with 4-space indents and 120 columns (`.clang-format`); format what you change with `git clang-format` or your editor's format-on-save. `.clang-tidy` enables the bug-prone and performance checks; CI runs it on the client library, and locally it needs a `compile_commands.json` (`cmake -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` from a Visual Studio developer prompt). Both tools ship with Visual Studio's C++ Clang tools. Python uses Black and Ruff (`pyproject.toml`: 80 columns, Python 3.11+, Google docstrings): `uvx black@24.10.0 scripts tests integrations` and `uvx ruff@0.7.4 check --fix scripts tests integrations` (CI pins these versions; newer ones format differently). PowerShell uses `Invoke-Formatter` and PSScriptAnalyzer (`PSScriptAnalyzerSettings.psd1`). `.editorconfig` sets whitespace for everything else. CI checks all of them, and lints the workflow with actionlint.

## Supported Houdini builds

The HDK only guarantees its ABI within one Houdini daily build. `Build-Houdini.ps1` compiles against the build in `houdini_root` and stamps it into the delegate's version resource, `runtime.json` and the package name. The installed package loads from that build up to the last 22.0 build; in any build other than the one it was compiled against, the delegate prints a warning when it starts.

| hdUE | Compiled against | Tested on |
| --- | --- | --- |
| v0.2.0 and later | Houdini 22.0.368 | 22.0.368 |

## Unreal Engine versions

`main` follows one Unreal Engine version at a time (`Build-Backend.ps1` pins it: 5.8.4 today). Moving to a new engine is ordinary work on `main`, released as a new minor version; the package name (`-ue<engine>-`) and the release notes say which engine a package carries, and earlier packages stay downloadable from earlier releases. A `release/ue<x.y>` branch is made from the last tag on an engine only when a fix must ship on that engine after `main` has moved on.

### Release checklist

GitHub runners cannot build Unreal Engine, so releases are built locally:

1. Set `project(hdUE VERSION x.y.z)` in `CMakeLists.txt`, commit, and tag `vx.y.z` on that commit.
2. `.\scripts\Verify.ps1 -WithUsdview` and `.\scripts\Test.ps1 -Suite all` on a clean tree.
3. Render the gallery (`hython scripts/render_gallery.py --dev`). It fails when a scene's brightness against Karma CPU leaves its band (`scripts/check_gallery.py`, which CI also runs on the committed `docs/gallery.md`) or a Cornell hdUE render departs from its tile baseline (`tests/gallery_baselines.json`); after an intended change, check the images and store new baselines with `--bless`.
4. `.\scripts\Build-Houdini.ps1`, `.\scripts\Build-Backend.ps1 -Configuration Test` and `.\scripts\Update-HoudiniRuntime.ps1 -Configuration Test -RuntimeConfig .local\runtime.json -OutputConfig .local\runtime-release.json`, then `.\scripts\Package-Houdini.ps1 -RuntimeConfig .local\runtime-release.json -Zip -UnrealLicenseesOnly`. It refuses uncommitted changes, a delegate built from another commit, and a backend that is not the current Test build (packages ship Test; see [houdini](getting-started/installation.md)).
5. Add the build to the table above. Keep the package zip and its `-symbols.zip` together; distribute the package only as [Installation](getting-started/installation.md) describes.
6. Publish the package zip (never the `-symbols.zip`) as a release of the licensee fork, on its README-only `hdue` branch: `gh release create vx.y.z --repo healkeiser/UnrealEngine --target hdue --title "hdUE x.y.z" --notes-file <notes> --latest <zip>`. The notes open with what the package carries (`hdUE x.y.z for Houdini <build> (and later 22.0 builds), Windows x64. Unreal Engine <engine> runtime.`), say so first when the Unreal Engine version changed since the last release, and end with the zip's SHA-256.
7. Publish the same notes as a release of this repository, with no file attached and a download link to the fork's release at the top, so the repository's Releases list shows the version: `gh release create vx.y.z --verify-tag --title "hdUE x.y.z" --notes-file <notes> --latest`. Never attach a package to a release of this repository.

## Standalone rendering and usdview

```powershell
.\scripts\Test-Hydra.ps1 -InputUsd samples/smoke.usda -Mode both -Cycles 1
.\scripts\Launch-Usdview.ps1 -InputUsd samples/smoke.usda
.\scripts\Launch-Usdview.ps1 -InputUsd samples/smoke.usda -Mode pathtraced
```

The launcher owns UE initialization, Tick, and shutdown on the UI thread. Use it rather than an unmanaged usdview process. Asynchronous scene processing is unsupported.

Camera controls include orbit, pan, and dolly. Continuous updates are coalesced. Lumen retains its ViewState during camera motion; scene, material, time, resolution, and Path Tracer request changes reset accumulation.

Picking in usdview uses the delegate's `unreal:pickRay` command, separate from the Hydra ID AOVs. The delegate tests generic USD meshes itself. Native assets are tested by the backend on UE's thread, the same in every host. A native asset needs readable, resident LOD0 data (Allow CPU Access and Never Stream on the cooked mesh) and opaque materials without deformation. Nanite meshes are picked through their LOD0 fallback geometry; Nanite override materials are not supported.

- An asset that cannot be picked (a material that is not opaque or deforms, or LOD0 data that is not readable) fails the pick only when the ray reaches its bounds nearer than the hit. An unpickable asset behind the hit, or off the ray, changes nothing.
- Instanced native assets are tested where they are drawn, at each instance's transform.
- If the scene changes while the pick runs, the pick fails with "The scene changed during the pick". Pick again.

`hdue_usd_render` renders a USD file to PFM without a GUI (`--prim-id` also writes the `primId` AOV).
