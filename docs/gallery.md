# Gallery

<div class="hdue-chart" data-chart="times" role="img" aria-label="Render time per frame per scene and renderer, log scale"></div>

<div class="hdue-chart" data-chart="accuracy" role="img" aria-label="Mean brightness relative to Karma CPU per scene and renderer"></div>

Point at a bar or a mark for its value; click a renderer in the legend to hide or show it.

## Cornell box: one feature per scene

Every scene is the [CC0 Cornell box](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/cornell) plus one small layer from [samples/cornell/variants](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/cornell/variants), rendered with husk at 512 × 512:

- **hdUE Lumen**: realtime mode, 64 warmup frames on a sequence's first frame, then one draw per frame (as Movie Render Queue).
- **hdUE Path Tracer**: 512 samples per pixel, up to 32 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 512 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the delegate's timing trace of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, after its first frame (a Lumen time is a sequence frame's, after the first frame's warm-up),Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Global illumination | 48 ms | 2.34 s | 8.08 s | 49.70 s | 168x / 3x | 0.98 / 1.01 |
| Glossy materials | 49 ms | 2.29 s | 5.88 s | 30.30 s | 121x / 3x | 1.06 / 1.22 |
| Depth of field | 48 ms | 2.36 s | 8.40 s | 51.03 s | 173x / 4x | 0.98 / 1.01 |
| Motion blur | 49 ms | 2.53 s | 7.92 s | 50.24 s | 162x / 3x | 0.98 / 1.01 |
| Subdivision surfaces | 46 ms | 2.37 s | 7.60 s | 48.27 s | 163x / 3x | 0.98 / 1.01 |
| Instances, points, curves | 49 ms | 2.40 s | 7.63 s | 50.05 s | 155x / 3x | 0.98 / 1.03 |
| Volumes (OpenVDB) | 49 ms | 4.04 s | 8.21 s | 58.66 s | 167x / 2x | 1.10 / 1.31 |
| IES profile | 48 ms | 1.99 s | 9.81 s | 68.45 s | 205x / 5x | 1.02 / 1.09 |
| Light linking | 48 ms | 2.37 s | 9.74 s | 64.69 s | 203x / 4x | 0.99 / 1.00 |
| Clearcoat | 49 ms | 1.97 s | 6.34 s | 37.10 s | 129x / 3x | 0.99 / 1.00 |
| Render region | 34 ms | 2.25 s | 2.33 s | 13.71 s | 70x / 1x | 0.98 / 1.04 |
| Lens shift | 45 ms | 2.25 s | 5.63 s | 34.78 s | 125x / 3x | 0.97 / 1.03 |
| Orthographic camera | 49 ms | 2.29 s | 7.23 s | 44.68 s | 149x / 3x | 0.85 / 1.04 |
| Camera exposure | 49 ms | 2.34 s | 8.98 s | 57.14 s | 182x / 4x | 0.97 / 1.01 |
| Unreal post-processing | 50 ms | 2.40 s | 7.77 s | 47.66 s | 154x / 3x | n/a |
| **All 15** | **711 ms** | **36.19 s** | **111.57 s** | **706.49 s** | **157x / 3x** | |

### Global illumination

The CC0 Cornell box with its measured camera and a RectLight matching the ceiling light.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/cornell/lumen.jpg" alt="Global illumination, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">48 ms</span><span class="hdue-note">64 warmup, then 1 · 0.98× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/cornell/pathtraced.jpg" alt="Global illumination, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.34 s</span><span class="hdue-note">512 spp · 1.01× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/cornell/xpu.jpg" alt="Global illumination, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">8.08 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/cornell/karma.jpg" alt="Global illumination, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">49.70 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Glossy materials

Glossy metal on the tall box and a glossy dielectric ball.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/glossy/lumen.jpg" alt="Glossy materials, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">49 ms</span><span class="hdue-note">64 warmup, then 1 · 1.06× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/glossy/pathtraced.jpg" alt="Glossy materials, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.29 s</span><span class="hdue-note">512 spp · 1.22× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/glossy/xpu.jpg" alt="Glossy materials, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">5.88 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/glossy/karma.jpg" alt="Glossy materials, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">30.30 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Depth of field

Thin-lens depth of field focused on the tall box (USD lens units: tenths of a scene unit).

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/depth_of_field/lumen.jpg" alt="Depth of field, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">48 ms</span><span class="hdue-note">64 warmup, then 1 · 0.98× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/depth_of_field/pathtraced.jpg" alt="Depth of field, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.36 s</span><span class="hdue-note">512 spp · 1.01× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/depth_of_field/xpu.jpg" alt="Depth of field, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">8.40 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/depth_of_field/karma.jpg" alt="Depth of field, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">51.03 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Motion blur

A ball crossing the box during a half-frame shutter.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/motion_blur/lumen.jpg" alt="Motion blur, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">49 ms</span><span class="hdue-note">64 warmup, then 1 · 0.98× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/motion_blur/pathtraced.jpg" alt="Motion blur, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.53 s</span><span class="hdue-note">512 spp · 1.01× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/motion_blur/xpu.jpg" alt="Motion blur, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">7.92 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/motion_blur/karma.jpg" alt="Motion blur, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">50.24 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Subdivision surfaces

A Catmull-Clark cube (USD default scheme) with a red GeomSubset on two faces, refined to level 3 in hdUE.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/subdivision/lumen.jpg" alt="Subdivision surfaces, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">46 ms</span><span class="hdue-note">64 warmup, then 1 · 0.98× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/subdivision/pathtraced.jpg" alt="Subdivision surfaces, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.37 s</span><span class="hdue-note">512 spp · 1.01× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/subdivision/xpu.jpg" alt="Subdivision surfaces, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">7.60 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/subdivision/karma.jpg" alt="Subdivision surfaces, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">48.27 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Instances, points, curves

A PointInstancer, Points and a cubic BasisCurves helix, with per-instance and per-curve colours.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/instances/lumen.jpg" alt="Instances, points, curves, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">49 ms</span><span class="hdue-note">64 warmup, then 1 · 0.98× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/instances/pathtraced.jpg" alt="Instances, points, curves, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.40 s</span><span class="hdue-note">512 spp · 1.03× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/instances/xpu.jpg" alt="Instances, points, curves, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">7.63 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/instances/karma.jpg" alt="Instances, points, curves, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">50.05 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Volumes (OpenVDB)

A fog sphere (OpenVDB, density field) in the middle of the box.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/volume/lumen.jpg" alt="Volumes (OpenVDB), hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">49 ms</span><span class="hdue-note">64 warmup, then 1 · 1.10× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/volume/pathtraced.jpg" alt="Volumes (OpenVDB), hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">4.04 s</span><span class="hdue-note">512 spp · 1.31× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/volume/xpu.jpg" alt="Volumes (OpenVDB), Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">8.21 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/volume/karma.jpg" alt="Volumes (OpenVDB), Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">58.66 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### IES profile

The ceiling light replaced by a point light with an IES profile.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/ies/lumen.jpg" alt="IES profile, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">48 ms</span><span class="hdue-note">64 warmup, then 1 · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/ies/pathtraced.jpg" alt="IES profile, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">1.99 s</span><span class="hdue-note">512 spp · 1.09× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/ies/xpu.jpg" alt="IES profile, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">9.81 s</span><span class="hdue-note">512 spp · 1.07× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/ies/karma.jpg" alt="IES profile, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">68.45 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Light linking

A warm light linked only to the tall box and a cool one only to the short box; the ceiling light lights everything. Lumen still bounces the linked lights onto every surface: UE’s lighting channels only restrict direct light.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/light_linking/lumen.jpg" alt="Light linking, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">48 ms</span><span class="hdue-note">64 warmup, then 1 · 0.99× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/light_linking/pathtraced.jpg" alt="Light linking, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.37 s</span><span class="hdue-note">512 spp · 1.00× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/light_linking/xpu.jpg" alt="Light linking, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">9.74 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/light_linking/karma.jpg" alt="Light linking, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">64.69 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Clearcoat

Preview Surface clearcoat: red lacquer (clearcoat 1 over a rough red base, left) next to the same base without a coat (right).

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/clearcoat/lumen.jpg" alt="Clearcoat, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">49 ms</span><span class="hdue-note">64 warmup, then 1 · 0.99× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/clearcoat/pathtraced.jpg" alt="Clearcoat, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">1.97 s</span><span class="hdue-note">512 spp · 1.00× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/clearcoat/xpu.jpg" alt="Clearcoat, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">6.34 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/clearcoat/karma.jpg" alt="Clearcoat, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">37.10 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Render region

A render region: dataWindowNDC (0.25, 0.15)-(0.75, 0.65) renders only that part of the 512 x 512 frame, as husk tiles do.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/render_region/lumen.jpg" alt="Render region, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">34 ms</span><span class="hdue-note">64 warmup, then 1 · 0.98× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/render_region/pathtraced.jpg" alt="Render region, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.25 s</span><span class="hdue-note">512 spp · 1.04× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/render_region/xpu.jpg" alt="Render region, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">2.33 s</span><span class="hdue-note">512 spp · 1.01× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/render_region/karma.jpg" alt="Render region, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">13.71 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Lens shift

Horizontal and vertical aperture offsets (lens shift).

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/lens_shift/lumen.jpg" alt="Lens shift, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">45 ms</span><span class="hdue-note">64 warmup, then 1 · 0.97× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/lens_shift/pathtraced.jpg" alt="Lens shift, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.25 s</span><span class="hdue-note">512 spp · 1.03× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/lens_shift/xpu.jpg" alt="Lens shift, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">5.63 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/lens_shift/karma.jpg" alt="Lens shift, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">34.78 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Orthographic camera

Orthographic projection with a 6.2 m window.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/orthographic/lumen.jpg" alt="Orthographic camera, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">49 ms</span><span class="hdue-note">64 warmup, then 1 · 0.85× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/orthographic/pathtraced.jpg" alt="Orthographic camera, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.29 s</span><span class="hdue-note">512 spp · 1.04× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/orthographic/xpu.jpg" alt="Orthographic camera, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">7.23 s</span><span class="hdue-note">512 spp · 1.01× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/orthographic/karma.jpg" alt="Orthographic camera, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">44.68 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Camera exposure

Camera exposure of +1 stop.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/exposure/lumen.jpg" alt="Camera exposure, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">49 ms</span><span class="hdue-note">64 warmup, then 1 · 0.97× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/exposure/pathtraced.jpg" alt="Camera exposure, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.34 s</span><span class="hdue-note">512 spp · 1.01× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/exposure/xpu.jpg" alt="Camera exposure, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">8.98 s</span><span class="hdue-note">512 spp · 1.02× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/exposure/karma.jpg" alt="Camera exposure, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">57.14 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

### Unreal post-processing

UE's tonemapper (+2.5 stops), bloom, vignette, chromatic aberration, lens flares, film grain and a warm grade on the hdUE beauty; Karma renders the linear image.

<div class="hdue-band">
<figure class="hdue-shot">
<img src="images/gallery/post_processing/lumen.jpg" alt="Unreal post-processing, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">50 ms</span><span class="hdue-note">64 warmup, then 1</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/post_processing/pathtraced.jpg" alt="Unreal post-processing, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">2.40 s</span><span class="hdue-note">512 spp</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/post_processing/xpu.jpg" alt="Unreal post-processing, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">7.77 s</span><span class="hdue-note">512 spp</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/post_processing/karma.jpg" alt="Unreal post-processing, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">47.66 s</span><span class="hdue-note">512 spp · brightness reference</span></figcaption>
</figure>
</div>

## Sponza: a production-style scene

[Intel's Sponza 2022](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) (Frank Meinl and Anton Kaplanyan, CC BY 4.0; not included in this repository), from its main camera. Intel exports UsdPreviewSurface only; [samples/sponza/materials.usda](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/sponza/materials.usda) gives every material a MaterialX standard_surface network on Intel's textures (stone, metal, wood, sheen and translucent curtains, translucent leaves cut out by their alpha, glass), which hdUE and Karma render. hdUE's Path Tracer image is denoised by UE's default denoiser (NNEDenoiser, PathTracingEnableDenoiser on); Karma's is not. Ray tracing uses Nanite's fallback meshes, as in UE (r.RayTracing.Nanite.Mode 0): the tree and the curtains are simplified in the Path Tracer, and Nanite Ray Tracing (Streamed Out Mesh) traces them in full. The scene has no lights, so [samples/sponza/lighting.usda](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/sponza/lighting.usda) adds a sun and a sky dome fitted to Intel's own renders of the scene, and levels the camera as Intel's render does; [samples/sponza/no_decals.usda](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/sponza/no_decals.usda) deactivates the dirt decal meshes. Add-ons from the same package, sublayered unchanged: `NewSponza_Curtains_USD_YUp.usda`, `NewSponza_CypressTree_USDA_YUp.usda`. Rendered with husk at 960 × 540:

- **hdUE Lumen**: realtime mode, 64 warmup frames on a sequence's first frame, then one draw per frame (as Movie Render Queue).
- **hdUE Path Tracer**: 256 samples per pixel, up to 32 bounces.
- **Karma XPU** (GPU and CPU) and **Karma CPU**: path traced, 256 samples per pixel, 8 diffuse bounces.
- Machine: NVIDIA GeForce RTX 4090 and AMD Ryzen 9 5950X 16-Core Processor.
- Times are per frame without process or UE startup: hdUE from the delegate's timing trace of a 16-frame (Lumen) or 4-frame (Path Tracer) husk run, after its first frame (a Lumen time is a sequence frame's, after the first frame's warm-up),Karma as husk's wall time minus its startup on an empty stage.
- Speed-ups compare with Karma XPU, the GPU renderer, when it was rendered; brightness is the mean luminance relative to Karma CPU, the reference (1.00 = same).

Regenerate with `hython scripts/render_gallery.py --set sponza`.

| Scene | hdUE Lumen | hdUE Path Tracer | Karma XPU | Karma CPU | Faster than Karma XPU (Lumen / Path Tracer) | Brightness vs Karma CPU (Lumen / Path Tracer) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Main view | 84 ms | 6.64 s | 42.19 s | 474.65 s | 504x / 6x | 0.91 / 1.06 |

### Main view

`/root/PhysCamera001`, levelled as in Intel's Render_Main_A, exposure +4.5 stops as authored; sun and sky fitted to Intel's renders.

<div class="hdue-band hdue-band--wide">
<figure class="hdue-shot">
<img src="images/gallery/sponza/PhysCamera001/lumen.jpg" alt="Main view, hdUE Lumen" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--lumen">hdUE Lumen</span><span class="hdue-time">84 ms</span><span class="hdue-note">64 warmup, then 1 · 0.91× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/sponza/PhysCamera001/pathtraced.jpg" alt="Main view, hdUE Path Tracer" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--pathtraced">hdUE Path Tracer</span><span class="hdue-time">6.64 s</span><span class="hdue-note">256 spp · 1.06× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/sponza/PhysCamera001/xpu.jpg" alt="Main view, Karma XPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--xpu">Karma XPU</span><span class="hdue-time">42.19 s</span><span class="hdue-note">256 spp · 0.98× brightness</span></figcaption>
</figure>
<figure class="hdue-shot">
<img src="images/gallery/sponza/PhysCamera001/karma.jpg" alt="Main view, Karma CPU" loading="lazy">
<figcaption><span class="hdue-renderer hdue-renderer--karma">Karma CPU</span><span class="hdue-time">474.65 s</span><span class="hdue-note">256 spp · brightness reference</span></figcaption>
</figure>
</div>
