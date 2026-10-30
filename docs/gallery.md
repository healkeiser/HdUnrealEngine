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

Every scene is the [CC0 Cornell box](../samples/cornell/) plus one small layer from [samples/cornell/variants](../samples/cornell/variants), rendered with husk at 512 × 512:

- **hdUE Lumen**: realtime mode, 32 warmup frames.
- **hdUE Path Tracer**: 512 samples per pixel, up to 32 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 512 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the extra time of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Global illumination | 0.2 s | 2.4 s | 7.8 s | 49.7 s | 37x / 3x | 0.93 / 1.01 |
| Glossy materials | 0.2 s | 2.4 s | 6.2 s | 32.1 s | 30x / 3x | 1.03 / 1.22 |
| Depth of field | 0.2 s | 2.5 s | 7.9 s | 49.9 s | 38x / 3x | 0.93 / 1.01 |
| Motion blur | 0.3 s | 2.5 s | 22.6 s | 52.9 s | 87x / 9x | 0.94 / 1.01 |
| Subdivision surfaces | 0.2 s | 2.4 s | 7.9 s | 49.6 s | 37x / 3x | 0.93 / 1.01 |
| Instances, points, curves | 0.2 s | 2.5 s | 10.8 s | 51.2 s | 54x / 4x | 0.95 / 1.04 |
| Volumes (OpenVDB) | 0.2 s | 4.1 s | 28.9 s | 60.0 s | 131x / 7x | 1.05 / 1.31 |
| IES profile | 0.2 s | 2.0 s | 9.8 s | 65.8 s | 45x / 5x | 1.04 / 1.09 |
| Light linking | 0.2 s | 2.0 s | 9.7 s | 64.6 s | 46x / 5x | 0.95 / 1.00 |
| Clearcoat | 0.2 s | 2.0 s | 6.2 s | 38.8 s | 27x / 3x | 0.96 / 1.01 |
| Render region | 0.2 s | 1.9 s | 2.4 s | 14.2 s | 12x / 1x | 0.95 / 1.04 |
| Lens shift | 0.2 s | 1.9 s | 5.8 s | 33.9 s | 27x / 3x | 0.93 / 1.03 |
| Orthographic camera | 0.2 s | 2.0 s | 7.5 s | 45.6 s | 36x / 4x | 0.83 / 1.04 |
| Camera exposure | 0.2 s | 2.0 s | 8.8 s | 57.4 s | 42x / 4x | 0.93 / 1.01 |
| Unreal post-processing | 0.2 s | 2.0 s | 18.3 s | 49.8 s | 91x / 9x | n/a |
| **All 15** | **3.2 s** | **34.7 s** | **160.6 s** | **715.7 s** | **50x / 5x** | |

![Global illumination: The CC0 Cornell box with its measured camera and a RectLight matching the ceiling light.](images/gallery/cornell.png)

![Glossy materials: Glossy metal on the tall box and a glossy dielectric ball.](images/gallery/glossy.png)

![Depth of field: Thin-lens depth of field focused on the tall box (USD lens units: tenths of a scene unit).](images/gallery/depth_of_field.png)

![Motion blur: A ball crossing the box during a half-frame shutter.](images/gallery/motion_blur.png)

![Subdivision surfaces: A Catmull-Clark cube (USD default scheme) with a red GeomSubset on two faces, refined to level 3 in hdUE.](images/gallery/subdivision.png)

![Instances, points, curves: A PointInstancer, Points and a cubic BasisCurves helix, with per-instance and per-curve colours.](images/gallery/instances.png)

![Volumes (OpenVDB): A fog sphere (OpenVDB, density field) in the middle of the box.](images/gallery/volume.png)

![IES profile: The ceiling light replaced by a point light with an IES profile.](images/gallery/ies.png)

![Light linking: A warm light linked only to the tall box and a cool one only to the short box; the ceiling light lights everything. Lumen still bounces the linked lights onto every surface: UE’s lighting channels only restrict direct light.](images/gallery/light_linking.png)

![Clearcoat: Preview Surface clearcoat: red lacquer (clearcoat 1 over a rough red base, left) next to the same base without a coat (right).](images/gallery/clearcoat.png)

![Render region: A render region: dataWindowNDC (0.25, 0.15)-(0.75, 0.65) renders only that part of the 512 x 512 frame, as husk tiles do.](images/gallery/render_region.png)

![Lens shift: Horizontal and vertical aperture offsets (lens shift).](images/gallery/lens_shift.png)

![Orthographic camera: Orthographic projection with a 6.2 m window.](images/gallery/orthographic.png)

![Camera exposure: Camera exposure of +1 stop.](images/gallery/exposure.png)

![Unreal post-processing: UE's tonemapper (+2.5 stops), bloom, vignette, chromatic aberration, lens flares, film grain and a warm grade on the hdUE beauty; Karma renders the linear image.](images/gallery/post_processing.png)

## Sponza: a production-style scene

[Intel's Sponza 2022](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) (Frank Meinl and Anton Kaplanyan, CC BY 4.0; not included in this repository), from its main camera. Intel exports UsdPreviewSurface only; [samples/sponza/materials.usda](../samples/sponza/materials.usda) gives every material a MaterialX standard_surface network on Intel's textures (stone, metal, wood, sheen and translucent curtains, translucent leaves cut out by their alpha, glass), which hdUE and Karma render. The gallery renders without hdUE’s optional Open Image Denoise (Houdini build), so its Path Tracer image shows more noise than Karma at the same sample count. The scene has no lights, so [samples/sponza/lighting.usda](../samples/sponza/lighting.usda) adds a sun and a sky dome fitted to Intel's own renders of the scene, and levels the camera as Intel's render does; [samples/sponza/no_decals.usda](../samples/sponza/no_decals.usda) deactivates the dirt decal meshes. Add-ons from the same package, sublayered unchanged: `NewSponza_Curtains_USD_YUp.usda`, `NewSponza_CypressTree_USDA_YUp.usda`. Rendered with husk at 960 × 540:

- **hdUE Lumen**: realtime mode, 32 warmup frames.
- **hdUE Path Tracer**: 256 samples per pixel, up to 32 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 256 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the extra time of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py --set sponza`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Main view | 0.4 s | 11.9 s | 42.2 s | 474.6 s | 105x / 4x | 0.88 / 0.94 |

![Main view: `/root/PhysCamera001`, levelled as in Intel's Render_Main_A, exposure +4.5 stops as authored; sun and sky fitted to Intel's renders.](images/gallery/sponza/PhysCamera001.png)
