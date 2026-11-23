# Contributing

Thanks for taking the time. This page is the short version. [docs/building.md](docs/building.md) is the long one and stays the source of truth for the build, and [AGENTS.md](AGENTS.md) holds the project's rules.

## What you need

hdUE runs Unreal Engine 5.8.4 inside Houdini 22.0, so a full build needs:

- Windows x64 and a GPU with hardware ray tracing (D3D12).
- Houdini 22.0.
- Unreal Engine 5.8.4 **built from source**. The source comes from Epic's private repository: link your Epic Games account to GitHub first ([Epic's instructions](https://www.unrealengine.com/en-US/ue-on-github)). `scripts/Setup-Engine.ps1` fetches the pinned version with your own access.

Without the engine you can still change the docs, the Python and PowerShell scripts, and the client library (`src/client/`, `include/hdue/bridge.h`): CI builds and tests those without UE or Houdini.

## Before you push

CI runs these on every pull request. Running them locally saves a round trip:

```powershell
uvx black@24.10.0 --check scripts tests integrations
uvx ruff@0.7.4 check scripts tests integrations
uvx --from clang-format==19.1.5 clang-format --dry-run --Werror (git ls-files '*.cpp' '*.h' '*.hpp' '*.c')
.\scripts\Test.ps1 -Suite unit
```

PowerShell files also go through PSScriptAnalyzer 1.23.0 with `PSScriptAnalyzerSettings.psd1` (see `.github/workflows/ci.yml`).

With the engine built, a change to rendering also runs the conformance scenes it touches and a Cornell husk render compared with the previous build:

```powershell
hython scripts\run_conformance.py --runtime-config <absolute path> --only <area>/*
```

[docs/conformance.md](docs/conformance.md) lists the areas.

## What a good change looks like

- **One subject per pull request.** Six small ones review faster than one large one.
- **Unreal is the benchmark.** hdUE does what Unreal does. Call or configure UE's own feature rather than writing a parallel version, and never copy another renderer's behaviour (Karma, Storm, Arnold). For a translation choice, cite the source: Unreal first (its USD importer, Interchange, Movie Render Queue), then the USD or MaterialX spec, then Karma.
- **Never tune to one scene.** Defaults follow UE's own values, not what makes a sample look right.
- **Never hand-edit generated files.** The render settings come from `scripts/generate_render_settings.py`: edit it, run it, then `scripts/build_render_settings_hda.py`, and commit all of it.
- **An ABI change bumps the version.** A change to a struct or to the `HdueApi` table in `include/hdue/bridge.h` bumps the ABI version, and both sides are rebuilt.
- **Never commit local paths.** Engine, Houdini and OpenUSD locations live in `.local/paths.json`, which git ignores.
- **Comments explain why.** Docs follow the existing prose: plain and concrete, no marketing.

## Commit and pull request titles

A title is one plain sentence that says what now happens, in the present tense, so it reads well a year later:

> The colour-space test re-checks the conversion cache with the resolved image path
>
> Debug views are opaque, as UE's viewport shows them: Unlit no longer disappears in Houdini's viewport

Not `fix: colour bug`. Pull requests are squash-merged, so the title becomes the commit message.

## Licensing

The code is under the MIT license. The backend links UE Developer modules, so built packages go to Unreal Engine licensees only: never attach a built package (DLLs, the staged runtime, a zip) to an issue or a pull request.

## Reporting a bug

Open an issue with the bug report form. It asks for the hdUE version (the delegate DLL's file properties, Details tab), the Houdini build, the GPU and its driver, and the end of the backend log in `%LOCALAPPDATA%\hdUE\Saved\Logs`.
