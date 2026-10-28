# Building from source

## Requirements

- Unreal Engine **5.8.4** source: tag `5.8.4-release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`.
- Windows x64, Visual Studio 2022 C++ tools, a Windows SDK, DirectX 12, and hardware ray tracing. The CMake scripts use the `Visual Studio 17 2022` generator.
- MSVC **14.44.35211 or newer** (a current Visual Studio 2022 17.14 update) or **14.50.35723 or newer**. UE 5.8.4 rejects MSVC 14.39-14.43, 14.44 before 35211, and 14.50 before 35723; `Engine/Config/Windows/Windows_SDK.json` in the engine source holds the authoritative list.
- .NET Framework **4.8 SDK** and **4.6.2 targeting pack** (Visual Studio Installer components `Microsoft.Net.Component.4.8.SDK` and `Microsoft.Net.Component.4.6.2.TargetingPack`). Without them, UnrealBuildTool stops with `Could not find NetFxSDK install dir`.
- PowerShell 7, Git, and CMake **3.24 or newer**, with Git and CMake available on `PATH`.
- OpenUSD **26.03** C++ SDK with Hydra/usdImaging, usdview, and Python **3.11** bindings for the standalone host and usdview. Build it with a moderate job count (`-j 8`); 28 parallel MSVC jobs ran out of compiler heap on a 64 GB machine.
- Houdini **22.0** with its bundled USD **26.05** and Python **3.13**, for Solaris. Builds bundling USD 26.05 are accepted; upstream verified 22.0.466, and the features in this fork were tested on 22.0.368.
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

`runtime_config` is read by `Launch-Houdini.ps1` and `Update-HoudiniRuntime.ps1`. Pass `-RuntimeConfig` explicitly to `Launch-Usdview.ps1`; it otherwise uses the repository's staged runtime. `review_scene` supplies the input for `Test-Opacity.ps1` and `Test-Glass.ps1`.

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
.\scripts\Package-Houdini.ps1 -RuntimeConfig .local\runtime-houdini.json -Zip
```

`Stage-Runtime.ps1` replaces the staged binaries, including the hashed Houdini copy of the backend, so run `Update-HoudiniRuntime.ps1` after every stage.

The monolithic DLL requires a source engine. A matching editor installation can prepare content. Virtual-texture residency support adds one friend declaration through `Apply-ResidencyPatch.ps1`, which validates the engine version and insertion point. UELibrary and Launch are otherwise unchanged.

Fresh verification evidence is written under `out/`. A successful exit code alone is insufficient: tests require new images, metadata, and clean engine shutdown.

## Standalone rendering and usdview

```powershell
.\scripts\Test-Hydra.ps1 -InputUsd samples/smoke.usda -Mode both -Cycles 1
.\scripts\Launch-Usdview.ps1 -InputUsd samples/smoke.usda
.\scripts\Launch-Usdview.ps1 -InputUsd samples/smoke.usda -Mode pathtraced
```

The launcher owns UE initialization, Tick, and shutdown on the UI thread. Use it rather than an unmanaged usdview process. Asynchronous scene processing is unsupported.

Camera controls include orbit, pan, and dolly. Continuous updates are coalesced. Lumen retains its ViewState during camera motion; scene, material, time, resolution, and Path Tracer request changes reset accumulation.

The launcher supplies its own CPU picking path for generic meshes and supported native geometry, separate from the Hydra ID AOVs. Native CPU picking requires readable, resident LOD0 data and rejects unsupported material deformation or Nanite overrides.

`hdue_usd_render` renders a USD file to PFM without a GUI (`--prim-id` also writes the `primId` AOV).
