# Opacity and glass

UE has three ways to let the background through a surface, and hdUE picks one from the material: a partly present surface (translucent), a cutout (masked), or glass that refracts (transmission). Only transmission bends light.

The figures on this page use the shader ball (`samples/docs/lookdev.usda`). Below: Preview Surface at opacity 0.2, then Standard Surface with transmission 1 (Realtime, Ray Traced translucency).

<div class="hdue-figure" data-figure="opacity-and-glass-models"></div>

## Which one you get

| You author | UE blend mode | Bends light | Use it for |
| --- | --- | --- | --- |
| Preview Surface **Opacity** below 1, **Opacity Threshold** 0 | Translucent | No | Thin films, holograms, fades |
| Preview Surface **Opacity Threshold** above 0 | Masked: cut at the threshold | No | Leaves, fences, cut-out cards |
| MaterialX **Opacity** (`opacity`, `geometry_opacity`) textured or below 1 | Masked: cut at 0.3333 | No | Foliage from MaterialX |
| MaterialX **Transmission** above 0 | Translucent Standard Surface, with refraction | Yes | Glass, water, liquids, plastics |
| `displayOpacity` on a prim with no material | Translucent | No | Quick previews |

A Standard Surface with transmission is never cut out by its `opacity`. An OpenPBR surface with a `geometry_opacity` cutout ignores its transmission.

See [Preview Surface](preview-surface.md#opacity) for the opacity inputs, with figures.

## Using it in Solaris

1. Build a **MaterialX Standard Surface** in a Karma Material Builder or Material Library.
2. Set **Transmission** to 1 and **Base** to 0. Glass gets its colour from transmission, not from the base.
3. Set **Specular IOR**: 1.5 for glass, 1.33 for water.
4. In Realtime, set **Translucency Method** (`unreal:postProcess:TranslucencyType`, hdUE Render Settings > Rendering Features > Translucency) to **Ray Traced**.

## Translucency Method

**Translucency Method** · [`unreal:postProcess:TranslucencyType`](../reference/render-settings/rendering-features.md#unreal-postprocess-translucencytype) · default Raster · Realtime only

How UE draws translucent surfaces in Realtime. **Raster** draws them over the scene; refraction is a screen-space offset. **Ray Traced** traces refraction and reflection rays through them and comes much closer to the Path Tracer. The Path Tracer always traces glass.

<div class="hdue-figure" data-figure="opacity-and-glass-translucency-method"></div>

With Ray Traced, its section shows UE's own settings: [Max Roughness](../reference/render-settings/rendering-features.md#unreal-postprocess-raytracingtranslucencymaxroughness) (default 0.6), [Max. Refraction Rays](../reference/render-settings/rendering-features.md#unreal-postprocess-raytracingtranslucencyrefractionrays) (default 3) and the shadow type. Ray Traced costs more GPU time than Raster.

## Specular Roughness

**Specular Roughness** · `specular_roughness` · default 0.2

On a transmissive surface, roughness blurs both the reflection and what is seen through the glass: frosted glass. With Ray Traced translucency, roughness above **Max Roughness** (0.6) fades out the traced refraction.

<div class="hdue-figure" data-figure="opacity-and-glass-roughness"></div>

## Transmission Color and Transmission Depth

**Transmission Color** · `transmission_color` · default (1, 1, 1)
**Transmission Depth** · `transmission_depth` · default 0, scene units

Light travelling through the glass reaches **Transmission Color** after **Transmission Depth**: thick parts are darker and more saturated than thin ones, as in a bottle. UE turns the pair into an absorption distance (its `Substrate-StandardSurface-Translucent` function, as Interchange imports it). Path Tracer below, on a 9 cm ball.

<div class="hdue-figure" data-figure="opacity-and-glass-transmission-depth"></div>

!!! note
    At **Transmission Depth** 0 (the Standard Surface default), **Transmission Color** tints the glass at its surface, without absorption inside: both render modes show the same tint whatever the object's thickness. Set a depth for the colour to deepen with thickness, as in a bottle.

## Alpha behind glass

The beauty's alpha behind glass is the alpha of what the glass shows: an opaque object seen through a pane keeps alpha 1, so you can composite it over a background. See [Alpha](../output/alpha.md).

## Recipes

How to get these looks on a MaterialX Standard Surface. Inputs not listed stay at their default. In Realtime, set **Translucency Method** to **Ray Traced**.

<div class="hdue-figure" data-figure="opacity-and-glass-recipes"></div>

| Look | Inputs |
| --- | --- |
| Clear glass | `base` 0, `specular_roughness` 0, `specular_IOR` 1.5, `transmission` 1 |
| Frosted glass | `base` 0, `specular_roughness` 0.3, `specular_IOR` 1.5, `transmission` 1 |
| Green bottle glass | `base` 0, `specular_roughness` 0, `specular_IOR` 1.5, `transmission` 1, `transmission_color` (0.3, 0.7, 0.45), `transmission_depth` 0.02 (2 cm in metres) |
| Water | `base` 0, `specular_roughness` 0, `specular_IOR` 1.33, `transmission` 1 |
| Thin window pane (a single quad) | Clear glass, plus `thin_walled` on |

[`samples/materialx/glass.usda`](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/materialx/glass.usda) shows an object behind clear glass and behind glass whose roughness follows a fractal smudge.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Glass shows bright rims or a flat overlay in Realtime, but looks right in the Path Tracer | **Translucency Method** is Raster | Set it to **Ray Traced** |
| A see-through surface does not refract | It uses Preview Surface **Opacity**, which is presence | Use MaterialX **Transmission** |
| Frosted glass loses its refraction above a roughness | Ray Traced translucency fades out above **Max Roughness** (0.6) | Raise **Max Roughness**, or check in the Path Tracer |
| A prim's `displayOpacity` varies per point but renders uniform, with a warning | A material carries one opacity | Bind a material with an opacity texture |
| Cutout foliage thins with distance in Realtime | Distant pixels read smaller mips | See [Preview Surface troubleshooting](preview-surface.md#troubleshooting) |

## Unreal Engine and Karma

- Transmission follows UE's Interchange import of MaterialX: the translucent Standard Surface function, coloured transmittance, refraction from the material's IOR. UE's Path Tracer refracts only with that refraction method, which hdUE uses.
- A Preview Surface with partial opacity keeps about 0.8 of Karma's Fresnel reflections at its rims, since UE's opacity fades the whole surface.
- MaterialX cutouts use UE's clip value 0.3333; Karma renders fractional coverage.
