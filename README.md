> [!IMPORTANT]
> This project is a Windows x64 proof of concept and subject to large changes.

<div align="center">

  <p align="center">
    <img src="https://raw.githubusercontent.com/healkeiser/hdUE/main/docs/images/banner.webp" alt="hdUE: Unreal Engine 5.8 as a Hydra renderer for Houdini Solaris, husk and USD" width="100%">
  </p>

  <p align="center">
    <!-- Maintenance status -->
    <img src="https://img.shields.io/badge/maintenance-actively--developed-brightgreen.svg?&label=Maintenance">&nbsp;&nbsp;
    <!-- License -->
    <img src="https://img.shields.io/badge/License-MIT-brightgreen.svg?&logo=open-source-initiative&logoColor=white" alt="License: MIT"/>&nbsp;&nbsp;
    <!-- GitHub build workflow -->
    <a href="https://github.com/healkeiser/hdUE/actions/workflows/ci.yml">
      <img src="https://img.shields.io/github/actions/workflow/status/healkeiser/hdUE/ci.yml?branch=main&label=CI&logo=github-actions&logoColor=white" alt="CI"/></a>&nbsp;&nbsp;
    <!-- Release -->
    <a href="https://github.com/healkeiser/hdUE/releases">
      <img src="https://img.shields.io/github/v/release/healkeiser/hdUE?logo=github&label=Release" alt="Release"/></a>&nbsp;&nbsp;
    <!-- Last Commit -->
    <img src="https://img.shields.io/github/last-commit/healkeiser/hdUE?logo=github&label=Last%20Commit" alt="Last Commit"/>&nbsp;&nbsp;
    <!-- Commit Activity -->
    <a href="https://github.com/healkeiser/hdUE/pulse" alt="Activity">
      <img src="https://img.shields.io/github/commit-activity/m/healkeiser/hdUE?&logo=github&label=Commit%20Activity"/></a>&nbsp;&nbsp;
    <!-- GitHub stars -->
    <img src="https://img.shields.io/github/stars/healkeiser/hdUE" alt="GitHub Stars"/>
  </p>

</div>

<!-- TABLE OF CONTENTS -->
## Table of Contents

- [About](#about)
- [Gallery](#gallery)
- [Features](#features)
- [Architecture](#architecture)
- [Installation](#installation)
- [Usage](#usage)
- [Environment Variables](#environment-variables)
- [Development](#development)
- [Credits](#credits)
- [Contact](#contact)
- [License](#license)

<!-- ABOUT -->
## About

**hdUE** is a Hydra render delegate that renders OpenUSD with [Unreal Engine 5.8](https://www.unrealengine.com/) inside the host process: the [SideFX Houdini](https://www.sidefx.com/) Solaris viewport, husk and the USD Render ROP, Live Render, usdview, or a standalone host. It renders with Lumen (realtime) or UE's Path Tracer, from the same USD and the same render settings as Karma.

Realtime mode is meant for final frames too: a simple 1080p frame takes about 0.25 s on an RTX 4090, after about 3 s of UE startup per husk process.

This is a fork of [K240/hdUE](https://github.com/K240/hdUE). See [Credits](#credits).

<!-- GALLERY -->
## Gallery

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/gallery/chart-times-dark.svg">
  <img src="docs/images/gallery/chart-times-light.svg" alt="Render time per frame for each gallery scene: hdUE Lumen, hdUE Path Tracer, Karma XPU and Karma CPU on a log scale" width="100%">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/gallery/chart-accuracy-dark.svg">
  <img src="docs/images/gallery/chart-accuracy-light.svg" alt="Mean brightness of hdUE Lumen, hdUE Path Tracer and Karma XPU relative to Karma CPU for each gallery scene" width="100%">
</picture>

The tables below give the numbers behind both charts; regenerate the charts with `uv run --with matplotlib python scripts/gallery_charts.py`.

<!-- gallery:start -->
Every scene is the [CC0 Cornell box](samples/cornell/) plus one small layer from [samples/cornell/variants](samples/cornell/variants), rendered with husk at 512 x 512:

- **hdUE Lumen**: realtime mode, 16 warmup frames.
- **hdUE Path Tracer**: 512 samples per pixel, up to 8 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 512 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the extra time of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Global illumination | 0.1 s | 2.0 s | 7.8 s | 49.7 s | 56x / 4x | 0.60 / 1.00 |
| Glossy materials | 0.1 s | 2.3 s | 6.2 s | 32.1 s | 41x / 3x | 0.72 / 1.20 |
| Depth of field | 0.1 s | 2.4 s | 7.9 s | 49.9 s | 57x / 3x | 0.60 / 1.00 |
| Motion blur | 0.2 s | 2.8 s | 22.6 s | 52.9 s | 119x / 8x | 0.59 / 1.00 |
| Subdivision surfaces | 0.1 s | 3.4 s | 7.9 s | 49.6 s | 87x / 2x | 0.60 / 1.00 |
| Instances, points, curves | 0.1 s | 2.1 s | 10.8 s | 51.2 s | 98x / 5x | 0.60 / 1.02 |
| Volumes (OpenVDB) | 0.1 s | 2.3 s | 28.9 s | 60.0 s | 262x / 13x | 0.71 / 1.29 |
| IES profile (experimental) | 0.2 s | 2.3 s | 9.8 s | 65.8 s | 49x / 4x | 0.00 / 0.00 |
| Lens shift | 0.2 s | 1.9 s | 5.8 s | 33.9 s | 29x / 3x | 0.54 / 1.02 |
| Orthographic camera | 0.2 s | 2.7 s | 7.5 s | 45.6 s | 47x / 3x | 0.56 / 1.03 |
| Camera exposure | 0.2 s | 2.3 s | 8.8 s | 57.4 s | 42x / 4x | 0.60 / 1.00 |
| Unreal post-processing | 0.1 s | 1.9 s | 18.3 s | 49.8 s | 141x / 9x | n/a |
| **All 12** | **1.8 s** | **28.6 s** | **142.3 s** | **598.1 s** | **78x / 5x** | |

| Scene | hdUE Lumen &#124; hdUE Path Tracer &#124; Karma XPU &#124; Karma CPU |
| --- | --- |
| **Global illumination**<br>The CC0 Cornell box with its measured camera and a RectLight matching the ceiling light. | ![Global illumination](docs/images/gallery/cornell.png) |
| **Glossy materials**<br>Glossy metal on the tall box and a glossy dielectric ball. | ![Glossy materials](docs/images/gallery/glossy.png) |
| **Depth of field**<br>Thin-lens depth of field focused on the tall box (USD lens units: tenths of a scene unit). | ![Depth of field](docs/images/gallery/depth_of_field.png) |
| **Motion blur**<br>A ball crossing the box during a half-frame shutter. | ![Motion blur](docs/images/gallery/motion_blur.png) |
| **Subdivision surfaces**<br>A Catmull-Clark cube (USD default scheme) with a red GeomSubset on two faces, refined to level 3 in hdUE. | ![Subdivision surfaces](docs/images/gallery/subdivision.png) |
| **Instances, points, curves**<br>A PointInstancer, Points and a cubic BasisCurves helix, with per-instance and per-curve colours. | ![Instances, points, curves](docs/images/gallery/instances.png) |
| **Volumes (OpenVDB)**<br>A fog sphere (OpenVDB, density field) in the middle of the box. | ![Volumes (OpenVDB)](docs/images/gallery/volume.png) |
| **IES profile (experimental)**<br>The ceiling light replaced by a point light with an IES profile (experimental in hdUE). | ![IES profile (experimental)](docs/images/gallery/ies.png) |
| **Lens shift**<br>Horizontal and vertical aperture offsets (lens shift). | ![Lens shift](docs/images/gallery/lens_shift.png) |
| **Orthographic camera**<br>Orthographic projection with a 6.2 m window. | ![Orthographic camera](docs/images/gallery/orthographic.png) |
| **Camera exposure**<br>Camera exposure of +1 stop. | ![Camera exposure](docs/images/gallery/exposure.png) |
| **Unreal post-processing**<br>UE's tonemapper (+2.5 stops), bloom, vignette, chromatic aberration, lens flares, film grain and a warm grade on the hdUE beauty; Karma renders the linear image. | ![Unreal post-processing](docs/images/gallery/post_processing.png) |
<!-- gallery:end -->

### Sponza

<!-- sponza:start -->
[Intel's Sponza 2022](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) (Frank Meinl and Anton Kaplanyan, CC BY 4.0; not included in this repository): 28 UsdPreviewSurface materials with 96 textures, normal maps and alpha cutouts, from its main camera. hdUE runs no denoiser, so its Path Tracer image shows more noise than Karma at the same sample count. The scene has no lights, so [samples/sponza/lighting.usda](samples/sponza/lighting.usda) adds a sun and a sky dome. Add-ons from the same package, sublayered unchanged: `NewSponza_CypressTree_USDA_YUp.usda`. Rendered with husk at 960 x 540:

- **hdUE Lumen**: realtime mode, 16 warmup frames.
- **hdUE Path Tracer**: 256 samples per pixel, up to 8 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 256 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the extra time of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py --set sponza --sponza <NewSponza_Main_USD_Yup_003.usda> --addon <add-on .usda>`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Main view | 0.1 s | 2.4 s | 84.1 s | 404.9 s | 700x / 36x | 0.79 / 1.03 |

| Scene | hdUE Lumen &#124; hdUE Path Tracer &#124; Karma XPU &#124; Karma CPU |
| --- | --- |
| **Main view**<br>`/root/PhysCamera001` (Intel's Render_Main_A view), exposure +4.5 stops as authored. | ![Main view](docs/images/gallery/sponza/PhysCamera001.png) |
<!-- sponza:end -->

<!-- FEATURES -->
## Features

| Area | What you get | Not yet |
|------|--------------|---------|
| **Houdini** | Solaris viewport renderer, husk, USD Render ROP, Render Gallery, Live Render LOP, an **Unreal Engine** tab on Render Settings LOPs, selection highlight, installable package | Houdini versions other than 22.0 |
| **Speed** | One UE per husk process for the whole frame range, only changed prims resent between frames, about 0.25 s per simple 1080p Lumen frame | |
| **Geometry** | Meshes and GeomSubsets, implicit prims (Cube, Sphere, ...), Points, BasisCurves, PointInstancers (nested), `displayColor` per face/vertex/point/curve/instance, Catmull-Clark and Loop subdivision (OpenSubdiv, uniform) | Displacement, adaptive subdivision, varying `displayOpacity` |
| **Volumes** | OpenVDB files, unsaved SOP volumes (`op:` paths, VDB and Houdini volumes), `.bgeo.sc` written by the USD Render ROP; `density` field | Temperature/emission, volume materials, frustum grids, volume shadows in realtime |
| **Materials** | UsdPreviewSurface with every texture input and cutouts, prepared MaterialX (Standard Surface, OpenPBR) and Substrate, COP textures | Arbitrary MaterialX at runtime |
| **Lights** | Distant, sphere, rect, disk, cylinder, dome (HDRI), UsdLux units, spot cones | Light linking, portals; IES profiles are experimental (applied and oriented, but brightness and falloff do not match Karma yet) |
| **Camera** | Perspective, orthographic, lens shift, depth of field, exposure, motion blur (transforms, camera, deforming meshes, point and instance `velocities`) | Lens distortion |
| **Output** | Beauty, depth, albedo, normal, roughness, metallic, specular, occlusion, `primId`, `instanceId`, multi-frame EXR | LPE AOVs, cryptomatte, deep output |
| **Post-processing** | UE tonemapper, bloom, grading, vignette, grain, any `FPostProcessSettings` property, post-process materials (beauty only, opt-in) | |

<!-- ARCHITECTURE -->
## Architecture

```mermaid
flowchart LR
    subgraph Houdini[" 🔶 Houdini / husk / usdview "]
        direction TB
        A1("Solaris viewport")
        A2("husk · USD Render ROP")
        A3("Live Render LOP")
    end

    subgraph Delegate[" ⚡ hdUE Hydra delegate "]
        direction TB
        B1("🔧 Rprims, lights, materials")
        B2("🎞️ Motion blur sub-frames")
        B3("📦 AOVs, render stats")
    end

    subgraph Unreal[" 🎮 Unreal Engine 5.8 "]
        direction TB
        C1("🌐 UELibrary backend DLL")
        C2("💡 Lumen · Path Tracer")
        C3("🖼️ GPU readback")
        C1 --> C2 --> C3
    end

    Houdini -. "Hydra 2 · same process" .-> Delegate
    Delegate -. "C ABI · versioned extensions" .-> Unreal

    classDef hostBox fill:#fff5f0,stroke:#e8c4a8,stroke-width:1px,color:#2d3748,rx:12,ry:12
    classDef delegateBox fill:#eef6f0,stroke:#a8d5b8,stroke-width:1px,color:#2d3748,rx:12,ry:12
    classDef unrealBox fill:#f0f4ff,stroke:#b8c9e8,stroke-width:1px,color:#2d3748,rx:12,ry:12

    classDef hostNode fill:#fde4d0,stroke:#e0a87c,stroke-width:1px,color:#2d3748,rx:8,ry:8
    classDef delegateNode fill:#d4edda,stroke:#82c896,stroke-width:1px,color:#2d3748,rx:8,ry:8
    classDef unrealNode fill:#dbe4f8,stroke:#96b0dc,stroke-width:1px,color:#2d3748,rx:8,ry:8

    class Houdini hostBox
    class Delegate delegateBox
    class Unreal unrealBox
    class A1,A2,A3 hostNode
    class B1,B2,B3 delegateNode
    class C1,C2,C3 unrealNode
```

The delegate and UE share the host process. UE only accepts calls from the thread that started it: in Houdini that is the UI thread, which hdUE's Python host ticks; under husk, hdUE starts UE on a thread of its own. Delegates running on other threads (Live Render) queue their UE work for that thread. See [Architecture and ABI](docs/architecture.md).

<!-- INSTALLATION -->
## Installation

Requires Houdini 22.0 on Windows x64 and a DirectX 12 GPU with hardware ray tracing. Unreal Engine, Visual Studio and OpenUSD are not needed.

1. Download `hdUE-<version>-houdini22.0-win64.zip` from [Releases](https://github.com/healkeiser/hdUE/releases).
2. Unzip it somewhere writable: UE writes its shader cache and logs next to the runtime.
3. Run `Install.cmd`. It writes `Documents\houdini22.0\packages\hdUE.json`.
4. Restart Houdini.

For a studio-wide install, use `Install.ps1 -PackagesDirectory <shared HOUDINI_PACKAGE_DIR>`. `Uninstall.cmd` removes the package file. See [Installing on another machine](docs/houdini.md#installing-on-another-machine).

<!-- USAGE -->
## Usage

Pick **Unreal Engine (hdUE)** in the Solaris viewport's renderer menu, or render to disk:

```powershell
husk --renderer HdUEHoudiniRendererPlugin -f 1 -n 24 -o render.$F4.exr scene.usd
```

Renderer settings live on any Render Settings LOP, on its **Unreal Engine** tab: render mode (Lumen or Path Tracer), samples, motion blur and motion samples, Lumen, environment resolution, and Unreal post-processing (tonemapper, bloom, grading, post-process materials). They are `hdUE:*` attributes on the UsdRenderSettings prim, so files rendered on a farm carry them. Render one frame range per husk call, and give farm tasks chunks of frames: each process starts UE once.

| Topic | Page |
|-------|------|
| Solaris, husk, Live Render, AOVs | [docs/houdini.md](docs/houdini.md) |
| Render settings, geometry, cameras, motion blur, post-processing, lights, volumes | [docs/rendering.md](docs/rendering.md) |
| MaterialX, Substrate, COP textures, cooked native assets | [docs/materials.md](docs/materials.md) |
| Architecture and the C ABI | [docs/architecture.md](docs/architecture.md) |
| What was measured | [VERIFICATION.md](VERIFICATION.md) |

<!-- ENVIRONMENT VARIABLES -->
## Environment Variables

The package file sets the first three; the rest are for development and debugging.

| Variable | Description |
|----------|-------------|
| `HDUE_RUNTIME_CONFIG` | Runtime profile (`backend_dll`, `runtime_project`) the delegate starts UE from |
| `HDUE_HOUDINI_ENABLE` | `1` starts hdUE's UI-thread host in Houdini |
| `HDUE_HOUDINI_PLUGIN` | Delegate DLL the Houdini host loads |
| `HDUE_BACKEND_DLL` | Backend DLL, overriding the runtime profile |
| `HDUE_HOUDINI_LOG_DIR` | Where the Houdini host writes its logs and state |
| `HDUE_TRACE_PASS` | `1` prints one line per render pass to stderr |
| `HDUE_TRACE_SETTINGS` | `1` prints every render setting the delegate receives |
| `HDUE_TRACE_SCENE` | `1` prints scene submissions |

<!-- DEVELOPMENT -->
## Development

Building needs the Unreal Engine 5.8.4 source, Visual Studio 2022, OpenUSD 26.03 for the standalone delegate and Houdini 22.0 for Solaris; see [Building from source](docs/building.md).

```powershell
.\scripts\Build-Backend.ps1            # UE backend DLL
.\scripts\Stage-Runtime.ps1            # cooked runtime under out/runtime
.\scripts\Build-Houdini.ps1            # Houdini delegate and its unit tests
.\scripts\Update-HoudiniRuntime.ps1 -RuntimeConfig .local\runtime.json -OutputConfig .local\runtime-houdini.json
.\scripts\Verify.ps1 -WithUsdview      # native, Hydra and usdview regression
.\scripts\Test-Houdini.ps1             # Solaris regression in a GUI Houdini
.\scripts\Package-Houdini.ps1 -RuntimeConfig .local\runtime-houdini.json -Zip
```

[CI](.github/workflows/ci.yml) builds the client library, runs the C ABI layout test, the repository scope tests, Python byte-compilation and a PowerShell parse check. GitHub's runners have no UE, Houdini or ray-tracing GPU, so the rendering suites above run locally.

<!-- CREDITS -->
## Credits

hdUE was started by [K240](https://github.com/K240/hdUE): the Hydra delegate, the UE backend, the Solaris host, HDRI lighting and MegaLights, MaterialX and Substrate preparation with the Substrate Builder, and the native asset bridge. This fork adds the Houdini package, render settings on Render Settings LOPs, husk and Live Render support, AOVs, instancing, points, curves, volumes, motion blur, camera projection, post-processing and the comparisons against Karma.

<!-- CONTACT -->
## Contact

Project Link: [hdUE](https://github.com/healkeiser/hdUE)

<p align='center'>
  <!-- GitHub profile -->
  <a href="https://github.com/healkeiser">
    <img src="https://img.shields.io/badge/healkeiser-181717?logo=github&style=social" alt="GitHub"/></a>&nbsp;&nbsp;
  <!-- LinkedIn -->
  <a href="https://www.linkedin.com/in/valentin-beaumont">
    <img src="https://img.shields.io/badge/Valentin%20Beaumont-0A66C2?logo=linkedin&style=social" alt="LinkedIn"/></a>&nbsp;&nbsp;
  <!-- Behance -->
  <a href="https://www.behance.net/el1ven">
    <img src="https://img.shields.io/badge/el1ven-1769FF?logo=behance&style=social" alt="Behance"/></a>&nbsp;&nbsp;
  <!-- X -->
  <a href="https://twitter.com/valentinbeaumon">
    <img src="https://img.shields.io/badge/@valentinbeaumon-1DA1F2?logo=x&style=social" alt="Twitter"/></a>&nbsp;&nbsp;
  <!-- Instagram -->
  <a href="https://www.instagram.com/val.beaumontart">
    <img src="https://img.shields.io/badge/@val.beaumontart-E4405F?logo=instagram&style=social" alt="Instagram"/></a>&nbsp;&nbsp;
  <!-- Gumroad -->
  <a href="https://healkeiser.gumroad.com/subscribe">
    <img src="https://img.shields.io/badge/healkeiser-36a9ae?logo=gumroad&style=social" alt="Gumroad"/></a>&nbsp;&nbsp;
  <!-- Gmail -->
  <a href="mailto:valentin.onze@gmail.com">
    <img src="https://img.shields.io/badge/valentin.onze@gmail.com-D14836?logo=gmail&style=social" alt="Email"/></a>&nbsp;&nbsp;
  <!-- Buy me a coffee -->
  <a href="https://www.buymeacoffee.com/healkeiser">
    <img src="https://img.shields.io/badge/Buy Me A Coffee-FFDD00?&logo=buy-me-a-coffee&logoColor=black" alt="Buy Me A Coffee"/></a>&nbsp;&nbsp;
</p>

<!-- LICENSE -->
## License

Original hdUE code, documentation, and project-generated synthetic samples are available under the [MIT License](LICENSE).

Unreal Engine, Houdini, third-party libraries, and separately supplied content retain their respective licenses. The MIT license does not grant rights to vendor SDKs, engine code, binaries, or third-party assets. Building, using, or distributing those components requires compliance with their applicable terms. See [Third-party notices](THIRD_PARTY_NOTICES.md).
