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

- **hdUE Lumen**: realtime mode, 32 warmup frames on a sequence's first frame, then one draw per frame (as Movie Render Queue).
- **hdUE Path Tracer**: 512 samples per pixel, up to 32 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 512 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the delegate's timing trace of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, after its first frame (a Lumen time is a sequence frame's, after the first frame's warm-up),Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Global illumination | 45 ms | 1.99 s | 8.08 s | 49.70 s | 178x / 4x | 0.93 / 1.01 |
| Glossy materials | 47 ms | 2.01 s | 5.88 s | 30.30 s | 124x / 3x | 1.03 / 1.22 |
| Depth of field | 48 ms | 2.01 s | 8.40 s | 51.03 s | 174x / 4x | 0.93 / 1.01 |
| Motion blur | 49 ms | 2.16 s | 7.92 s | 50.24 s | 162x / 4x | 0.97 / 1.01 |
| Subdivision surfaces | 47 ms | 1.98 s | 7.60 s | 48.27 s | 161x / 4x | 0.93 / 1.01 |
| Instances, points, curves | 47 ms | 2.03 s | 7.63 s | 50.05 s | 161x / 4x | 0.95 / 1.04 |
| Volumes (OpenVDB) | 49 ms | 4.00 s | 8.21 s | 58.66 s | 169x / 2x | 1.05 / 1.32 |
| IES profile | 46 ms | 2.03 s | 9.81 s | 68.45 s | 212x / 5x | 1.04 / 1.09 |
| Light linking | 48 ms | 1.98 s | 9.74 s | 64.69 s | 205x / 5x | 0.95 / 1.00 |
| Clearcoat | 49 ms | 2.15 s | 6.34 s | 37.10 s | 130x / 3x | 0.96 / 1.01 |
| Render region | 33 ms | 2.28 s | 2.33 s | 13.71 s | 70x / 1x | 0.95 / 1.04 |
| Lens shift | 46 ms | 1.99 s | 5.63 s | 34.78 s | 124x / 3x | 0.93 / 1.03 |
| Orthographic camera | 46 ms | 2.02 s | 7.23 s | 44.68 s | 158x / 4x | 0.83 / 1.05 |
| Camera exposure | 48 ms | 2.02 s | 8.98 s | 57.14 s | 188x / 4x | 0.93 / 1.01 |
| Unreal post-processing | 49 ms | 2.03 s | 7.77 s | 47.66 s | 159x / 4x | n/a |
| **All 15** | **697 ms** | **32.66 s** | **111.57 s** | **706.49 s** | **160x / 3x** | |

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

- **hdUE Lumen**: realtime mode, 32 warmup frames on a sequence's first frame, then one draw per frame (as Movie Render Queue).
- **hdUE Path Tracer**: 256 samples per pixel, up to 32 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 256 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the delegate's timing trace of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, after its first frame (a Lumen time is a sequence frame's, after the first frame's warm-up),Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py --set sponza`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Main view | 93 ms | 14.45 s | 42.19 s | 474.65 s | 453x / 3x | 0.88 / 0.94 |

![Main view: `/root/PhysCamera001`, levelled as in Intel's Render_Main_A, exposure +4.5 stops as authored; sun and sky fitted to Intel's renders.](images/gallery/sponza/PhysCamera001.png)
