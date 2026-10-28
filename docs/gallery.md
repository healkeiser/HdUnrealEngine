# Gallery

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/gallery/chart-times-dark.svg">
  <img src="images/gallery/chart-times-light.svg" alt="Render time per frame per scene and renderer, log scale" width="100%">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/gallery/chart-accuracy-dark.svg">
  <img src="images/gallery/chart-accuracy-light.svg" alt="Mean brightness relative to Karma CPU per scene and renderer" width="100%">
</picture>

Charts: `uv run --with matplotlib python scripts/gallery_charts.py`.

## Cornell box: one feature per scene

Every scene is the [CC0 Cornell box](../samples/cornell/) plus one small layer from [samples/cornell/variants](../samples/cornell/variants), rendered with husk at 512 x 512:

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
| **Global illumination**<br>The CC0 Cornell box with its measured camera and a RectLight matching the ceiling light. | ![Global illumination](images/gallery/cornell.png) |
| **Glossy materials**<br>Glossy metal on the tall box and a glossy dielectric ball. | ![Glossy materials](images/gallery/glossy.png) |
| **Depth of field**<br>Thin-lens depth of field focused on the tall box (USD lens units: tenths of a scene unit). | ![Depth of field](images/gallery/depth_of_field.png) |
| **Motion blur**<br>A ball crossing the box during a half-frame shutter. | ![Motion blur](images/gallery/motion_blur.png) |
| **Subdivision surfaces**<br>A Catmull-Clark cube (USD default scheme) with a red GeomSubset on two faces, refined to level 3 in hdUE. | ![Subdivision surfaces](images/gallery/subdivision.png) |
| **Instances, points, curves**<br>A PointInstancer, Points and a cubic BasisCurves helix, with per-instance and per-curve colours. | ![Instances, points, curves](images/gallery/instances.png) |
| **Volumes (OpenVDB)**<br>A fog sphere (OpenVDB, density field) in the middle of the box. | ![Volumes (OpenVDB)](images/gallery/volume.png) |
| **IES profile (experimental)**<br>The ceiling light replaced by a point light with an IES profile (experimental in hdUE). | ![IES profile (experimental)](images/gallery/ies.png) |
| **Lens shift**<br>Horizontal and vertical aperture offsets (lens shift). | ![Lens shift](images/gallery/lens_shift.png) |
| **Orthographic camera**<br>Orthographic projection with a 6.2 m window. | ![Orthographic camera](images/gallery/orthographic.png) |
| **Camera exposure**<br>Camera exposure of +1 stop. | ![Camera exposure](images/gallery/exposure.png) |
| **Unreal post-processing**<br>UE's tonemapper (+2.5 stops), bloom, vignette, chromatic aberration, lens flares, film grain and a warm grade on the hdUE beauty; Karma renders the linear image. | ![Unreal post-processing](images/gallery/post_processing.png) |

## Sponza: a production-style scene

[Intel's Sponza 2022](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) (Frank Meinl and Anton Kaplanyan, CC BY 4.0; not included in this repository): 28 UsdPreviewSurface materials with 96 textures, normal maps and alpha cutouts, from its main camera. hdUE runs no denoiser, so its Path Tracer image shows more noise than Karma at the same sample count. The scene has no lights, so [samples/sponza/lighting.usda](../samples/sponza/lighting.usda) adds a sun and a sky dome. Add-ons from the same package, sublayered unchanged: `NewSponza_CypressTree_USDA_YUp.usda`. Rendered with husk at 960 x 540:

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
| **Main view**<br>`/root/PhysCamera001` (Intel's Render_Main_A view), exposure +4.5 stops as authored. | ![Main view](images/gallery/sponza/PhysCamera001.png) |
