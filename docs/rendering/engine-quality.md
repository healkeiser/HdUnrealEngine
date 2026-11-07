# Engine quality

Engine quality settings trade render time for quality in UE's realtime renderer: UE's scalability levels, Nanite's triangle density, ray tracing and the render resolution. They are UE's console variables, set from the **Engine Quality** and **Rendering** tabs of your Render Settings LOP.

## Using it in Solaris

1. On your Render Settings LOP's **Unreal Engine** tab, open **Engine Quality**.
2. Set a **Scalability** group, or a Nanite or ray tracing setting.
3. For the viewport only, use **Display Options > Unreal Engine > Engine Quality**, which offers the main scalability groups.

Each setting is a `unreal:cvar:<console variable>` attribute, with `:` in place of `.`: **Shadows** is `unreal:cvar:sg:ShadowQuality`, UE's `sg.ShadowQuality`. Console variables apply to the whole Houdini or husk process, not to one viewport. Remove the attribute to restore the value UE had before.

Any UE console variable works as an attribute, not only the ones the LOP shows: `int unreal:cvar:r:Shadow:Virtual:Enable = 0`. Booleans are written as 1 and 0. Variables UE marks read-only, such as `r.RayTracing`, cannot change while UE runs; hdUE skips them with a warning.

## Scalability

UE's scalability groups, as in UE's **Engine Scalability Settings** menu. Each level (Low, Medium, High, Epic, Cinematic) applies a set of console variables from UE's `BaseScalability.ini`; a console variable you set yourself wins over its group.

| Group | Attribute | What the levels change |
| --- | --- | --- |
| View Distance | `unreal:cvar:sg:ViewDistanceQuality` | how far objects and details are drawn |
| Anti-Aliasing | `unreal:cvar:sg:AntiAliasingQuality` | the anti-aliasing pass's quality |
| Shadows | `unreal:cvar:sg:ShadowQuality` | shadow resolution and soft shadow rays: Low has no soft shadow rays, Epic 8 rays per pixel |
| Global Illumination | `unreal:cvar:sg:GlobalIlluminationQuality` | Lumen's bounce light: off at Low; Epic and Cinematic add mesh distance field tracing and allow hit lighting |
| Reflections | `unreal:cvar:sg:ReflectionQuality` | Lumen reflections: off at Low and Medium, half resolution at High, full resolution at Epic and Cinematic |
| Post-Processing | `unreal:cvar:sg:PostProcessQuality` | bloom, depth of field and other post-process quality |
| Textures | `unreal:cvar:sg:TextureQuality` | texture streaming and filtering |
| Effects | `unreal:cvar:sg:EffectsQuality` | translucency and effect quality |
| Foliage | `unreal:cvar:sg:FoliageQuality` | foliage density |
| Shading | `unreal:cvar:sg:ShadingQuality` | material shading quality |

The default is Epic in Houdini's viewport and Cinematic in husk and the USD Render ROP, as Movie Render Queue renders with Cinematic. The Path Tracer ignores the Anti-Aliasing, Shadows, Global Illumination and Reflections groups.

**Global Illumination** in the Bistro, lit by its lamps. Low turns Lumen's bounce light off.

<div class="hdue-figure" data-figure="engine-quality-gi"></div>


## Nanite

**Nanite** (`unreal:cvar:r:Nanite`), default on, Advanced.

Renders Nanite meshes with Nanite. Off, they render with their fallback meshes. See [Nanite](../geometry/nanite.md) for which meshes are Nanite.

**Max Pixels per Edge** (`unreal:cvar:r:Nanite:MaxPixelsPerEdge`), default 1, range 0.25 to 8. Realtime only, Advanced.

The triangle edge length Nanite aims for on screen, in pixels. Lower values draw more, smaller triangles and take more time.

## Ray tracing

**Ray Tracing** (`unreal:cvar:r:RayTracing:Enable`), default on, Advanced.

UE's hardware ray tracing, which the Path Tracer, Lumen's hardware ray tracing, ray traced shadows and ray traced translucency need.

**Nanite Ray Tracing** (`unreal:cvar:r:RayTracing:Nanite:Mode`), default Fallback Mesh, Advanced.

What rays hit on Nanite meshes. Fallback Mesh, UE's default, traces each mesh's simplified fallback: foliage simplified into its fallback looks thinner in the Path Tracer and in Lumen's reflections. Streamed Out Mesh traces the full Nanite geometry streamed in for the view. On Intel Sponza's tree, a 256-sample husk frame took 65 s with Streamed Out Mesh against 25 s with Fallback Mesh, both including scene load.

Other ray tracing settings are on the Lighting and Rendering Features tabs:

| Setting | Attribute | Default | What it does |
| --- | --- | --- | --- |
| [Hardware Ray Tracing](../reference/render-settings/lighting.md#unreal-cvar-r-lumen-hardwareraytracing) | `unreal:cvar:r:Lumen:HardwareRayTracing` | on | Lumen traces triangles instead of distance fields |
| [Ray Traced Shadows](../reference/render-settings/lighting.md#unreal-cvar-r-raytracing-shadows) | `unreal:cvar:r:RayTracing:Shadows` | off | ray traced shadows instead of shadow maps |
| [Translucency Method](../reference/render-settings/rendering-features.md#unreal-postprocess-translucencytype) | `unreal:postProcess:TranslucencyType` | Raster | Ray Traced traces every layer of glass |

!!! tip
    Glass seen through other glass is lit from Lumen's approximate radiance cache with Raster translucency, and can come out far too bright. In a dome-lit room, a bottle was 18 times the Path Tracer's brightness with Raster and within 2 times with `unreal:postProcess:TranslucencyType = "ETranslucencyType::RayTraced"`.

## Resolution and anti-aliasing

These settings are on the **Rendering** tab, under Anti-Aliasing. They apply to Realtime renders.

| Setting | Attribute | Default | What it does |
| --- | --- | --- | --- |
| [Anti-Aliasing Method](../reference/render-settings/rendering.md#unreal-cvar-r-antialiasingmethod) | `unreal:cvar:r:AntiAliasingMethod` | TSR | None, FXAA, TAA or TSR |
| [Screen Percentage](../reference/render-settings/rendering.md#unreal-cvar-r-screenpercentage) | `unreal:cvar:r:ScreenPercentage` | 100 | render resolution in percent of the output, upscaled by the anti-aliasing method; 50 renders a quarter of the pixels |
| [Lower Resolution While Moving](../reference/render-settings/rendering.md#unreal-cvar-r-hdue-interactiveresolution) | `unreal:cvar:r:HdUE:InteractiveResolution` | on | Houdini's viewport renders at Moving Screen Percentage (50) while the view changes |
| [NVIDIA DLSS](../reference/render-settings/rendering.md#unreal-cvar-r-ngx-dlss-enable) | `unreal:cvar:r:NGX:DLSS:Enable` | off | DLSS Super Resolution replaces TAA or TSR |

With **NVIDIA DLSS** on, Screen Percentage picks the DLSS mode: 50 is Performance, about 67 Quality, 100 DLAA. DLSS needs an NVIDIA RTX GPU and an hdUE build with NVIDIA's DLSS plugin; otherwise the anti-aliasing method renders. **DLSS Ray Reconstruction** (`unreal:cvar:r:HdUE:DLSS:RayReconstruction`, off) makes DLSS denoise Lumen reflections and ray traced shadows too, in place of UE's denoisers; it needs DLSS on. The Path Tracer does not use DLSS.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| No bounce light in Realtime | **Global Illumination** at Low | Set it to Epic or Cinematic. |
| No reflections of other objects | **Reflections** at Low or Medium | Set it to High or above. |
| Hard shadows from area lights | **Shadows** at Low | Set it to Medium or above. |
| Viewport and husk renders differ | the viewport defaults to Epic, husk to Cinematic | Author the scalability groups you want on the Render Settings LOP. |
| Thin foliage in the Path Tracer | rays hit Nanite fallback meshes | Set **Nanite Ray Tracing** to Streamed Out Mesh. |
| Glass behind glass is too bright | Raster translucency | Set **Translucency Method** to Ray Traced. |
| A console variable attribute has no effect, with a warning | UE marks it read-only | None at render time: read-only variables cannot change while UE runs. |

## Unreal Engine and Karma

Every setting here is UE's own console variable with UE's default, and husk renders with Movie Render Queue's offline levels. Karma has no equivalent of scalability; its quality comes from sample counts.
