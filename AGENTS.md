# HdUnrealEngine: agent guide

HdUnrealEngine (hdUE) is a Hydra render delegate that runs Unreal Engine 5.8.4 as a library inside Houdini 22.0 (viewport and husk) and usdview. Windows x64, D3D12, a ray-tracing GPU. Plugin id `HdUnrealEngine`, display name "Unreal Engine" (`husk --renderer "Unreal Engine"`), USD attribute prefix `unreal:`. [docs/architecture.md](docs/architecture.md) is the runtime anatomy; read it before changing anything that crosses the delegate/backend boundary.

## Layout

- `src/hydra/`: the Hydra delegate (prims, materials, render pass, scene-index plugins). `delegate.cpp` turns Hydra state into backend calls.
- `include/hdue/bridge.h`: the C ABI between the delegate and the UE backend (one `HdueApi` table from `hdue_get_api`). Changing a struct or the table means bumping the ABI version and rebuilding both sides.
- `src/client/`: loads the backend DLL; `src/host/`: standalone hosts and smoke tests; `src/houdini/`: Houdini-only pieces.
- `unreal/HdUEBackend/`: the UE project and backend module (`Private/Bridge.cpp` is the ABI's implementation: sessions, requests, drawing, readback).
- `integrations/houdini/`: HDAs, `.ds` parameter files, Python (`hdue_houdini.py`), `UsdRenderers.json`.
- `scripts/`: build, stage, test, package (PowerShell) and generators (Python). `samples/`: test scenes, the conformance set, Sponza layers (Intel's files are a local download, not tracked).
- `docs/`: user and developer docs; `docs/audit/` tracks open audit items.

## Build and test

[docs/building.md](docs/building.md) has requirements and the full order. Local paths (engine, Houdini, OpenUSD) live in `.local/paths.json`, never in code.

```powershell
.\scripts\Build-Backend.ps1                      # UE backend DLL (incremental: ~40 s)
.\scripts\Update-HoudiniRuntime.ps1 -SetDefault  # stage it as a hashed DLL and point .local/runtime-houdini.json at it
.\scripts\Build-Houdini.ps1                      # Houdini delegate and its unit tests (~40 s); Houdini must be closed
.\scripts\Test.ps1 -Suite all
hython scripts\run_conformance.py --runtime-config <absolute path>   # add --only <area>/* (targeted); all 150+ scenes x 2 modes take over an hour
```

- The backend is staged under a content-hashed name, so a running Houdini keeps its DLL; stage to another config with `-OutputConfig` to test without touching the default.
- `HDUE_TRACE_TIMING=1` prints the delegate's timeline (request to frame, readback, setting changes); `HDUE_TRACE_SCENE=1` the scene uploads. The backend log is `out/runtime/Windows/HdUEBackend/Saved/Logs/` (timestamps in UTC).
- Render settings are generated: edit `scripts/generate_render_settings.py`, run it (writes `HdUnrealEngine_*.ds`), then `scripts/build_render_settings_hda.py` (rebuilds the HDAs). Never edit the `.ds` or the HDAs by hand.
- Gallery: `hython scripts/render_gallery.py --set cornell|sponza --dev [--renderers lumen pathtraced] [--docs-only]`, then `uv run --with matplotlib python scripts/gallery_charts.py`. Re-render only the columns that changed; Karma's renders are reused.
- Packages: `Package-Houdini.ps1 -Zip -UnrealLicenseesOnly` needs a clean tree and a delegate built from the current commit. The backend links UE Developer modules: packages go to Unreal Engine licensees only, never a public download.

## Rules

- **Unreal is the benchmark.** hdUE does what Unreal does: never reimplement a behaviour after another engine (Karma, Storm, Arnold, ...), and never reimplement what Unreal already has: call or configure UE's own feature (its renderer, importers, Movie Render Queue, DDC, ...) instead of writing a parallel version. Other engines are only comparisons.
- **Baseline order for any translation choice** (defaults, thresholds, how a USD/MaterialX feature maps to UE): what Unreal does first (its USDImporter plugin, Interchange, Movie Render Queue, the runtime), then the USD/MaterialX spec, then Karma. Cite the source. Never tune to one sample scene's values; per-engine look differences are the scene's job.
- Read the UE source (`Engine/Source`, `Engine/Plugins`) before assuming how UE behaves; the engine root is in `.local/paths.json`.
- Test on small scenes (Cornell, the conformance set, tiny repros). Sponza and Jungle Ruins renders take minutes: only for a gallery refresh someone asked for. No Karma renders unless asked.
- Lights in samples: intensity 1, tune with `inputs:exposure`. Never replace a scene's own lights or HDRI.
- **Verification is tiered.** Every change: build, unit tests, the conformance scenes it touches (`run_conformance.py --only <area>/*`) and a Cornell husk render compared with the previous build; changes to Hydra sync, scene upload or the ABI also run the in-Houdini test they touch (`Launch-Houdini.ps1 -TestKind surfaces|selection|...`). Before pushing, the targeted conformance runs of the group of commits (0 failing runs) and the Cornell gallery check are enough; the full conformance run (over an hour) runs only when the user asks for it.
- Work on `main` (the only branch); verified work is committed and pushed there. Never push to Epic's or the upstream fork's repositories.
- Comments explain why, at the density of the surrounding code; docs follow the existing prose style (plain, concrete, no marketing).
- C++: `.clang-format` (Google style, 4 spaces, 120 columns). Python: Black and Ruff (`pyproject.toml`). PowerShell: PSScriptAnalyzer. CI checks all of them.
- Disk: a backend build's `Intermediate` is ~13 GB; clean regenerable output (`out/package`, old gallery and test renders) rather than letting the drive fill.
