# Preview Surface

`UsdPreviewSurface` is USD's portable material. hdUE renders every input of it, constant or textured, on UE's Preview Surface reference material: one Substrate slab, the cheapest material UE has.

The figures on this page use SideFX's shader ball (`samples/docs/lookdev.usda`) in Realtime, with the ball's `diffuseColor` (0.8, 0.25, 0.08) and `roughness` 0.3 unless a figure changes them.

<div class="hdue-figure" data-figure="preview-surface-recipes"></div>

## Using it in Solaris

1. In a **Material Library** LOP, create a **USD Preview Surface** VOP.
2. For a texture, wire a **USD UV Texture** VOP into the input, with a **USD Primvar Reader** (`st`) into its **ST**.
3. Assign the material with **Assign Material** or the library's **Geometry Path**.

Every input updates live in the viewport and the Path Tracer restarts its accumulation. Defaults below are the [UsdPreviewSurface specification](https://openusd.org/release/spec_usdpreviewsurface.html)'s.

## Diffuse Color

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Diffuse Color</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:diffuseColor`</dd></div><div markdown><dt>Default</dt><dd markdown="span">(0.18, 0.18, 0.18)</dd></div></dl>
</div>
</div>

</div>

The surface colour: UE's Base Color. With **Metallic** 1 it is the metal's reflectance colour instead.

<div class="hdue-figure" data-figure="preview-surface-diffuse-color"></div>

## Emissive Color

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Emissive Color</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:emissiveColor`</dd></div><div markdown><dt>Default</dt><dd markdown="span">(0, 0, 0)</dd></div></dl>
</div>
</div>

</div>

Light the surface emits: UE's Emissive Color. Values above 1 are allowed and needed for a surface that reads as a light source.

<div class="hdue-figure" data-figure="preview-surface-emissive-color"></div>

!!! note
    In the Path Tracer, emissive surfaces light the scene ([Emissive Materials](../reference/render-settings/rendering.md#unreal-postprocess-pathtracingenableemissivematerials), on by default). In Realtime, Lumen picks up their light only through its global illumination, much weaker for small emitters: on the conformance mesh-light scene a wall gets about 1% of the Path Tracer's light. Light a scene with UsdLux lights, and use emission for what the camera sees.

## Use Specular Workflow and Specular Color

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Use Specular Workflow</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:useSpecularWorkflow`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Specular Color</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:specularColor`</dd></div><div markdown><dt>Default</dt><dd markdown="span">(0, 0, 0)</dd></div></dl>
</div>
</div>

</div>

At 1, **Diffuse Color** is the diffuse colour and **Specular Color** the reflectance at normal incidence (F0), as in UE's specular reflection. **Metallic** and **IOR** are then ignored. At 0 (the metalness workflow), **Specular Color** is ignored.

<div class="hdue-figure" data-figure="preview-surface-specular-workflow"></div>

## Metallic

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Metallic</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:metallic`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

0 is a dielectric (plastic, wood, paint), 1 a metal. A metal has no diffuse lobe: **Diffuse Color** becomes its reflection colour. Use 0 or 1; values between are for texture transitions such as rust edges.

<div class="hdue-figure" data-figure="preview-surface-metallic"></div>

## Roughness

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Roughness</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:roughness`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0.5</dd></div></dl>
</div>
</div>

</div>

Microfacet roughness of the specular reflection, UE's Roughness: 0 is a mirror, 1 fully diffuse-looking.

<div class="hdue-figure" data-figure="preview-surface-roughness"></div>

!!! tip
    In Realtime, Lumen traces dedicated reflection rays only up to [Max Roughness To Trace](../reference/render-settings/lighting.md#unreal-postprocess-lumenmaxroughnesstotracereflections) (default 0.4). Rougher surfaces reflect Lumen's cheaper radiance cache, as in UE.

## Clearcoat

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Clearcoat</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:clearcoat`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

A clear, white varnish layer over the surface, with an IOR of 1.5. Above 0, the material renders with UE's layered Standard Surface material: the coat is a second Substrate slab over the base, which costs more per pixel than a plain Preview Surface.

<div class="hdue-figure" data-figure="preview-surface-clearcoat"></div>

!!! note
    With **Use Specular Workflow** 1, or with **Opacity** below 1, the coat is not layered: it becomes a second specular lobe of the base (Substrate's haziness), which reads like a coat on most surfaces.

## Clearcoat Roughness

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Clearcoat Roughness</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:clearcoatRoughness`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0.01</dd></div></dl>
</div>
</div>

</div>

Roughness of the coat alone; the base keeps **Roughness**. A rough base under a sharp coat is the usual varnished look.

<div class="hdue-figure" data-figure="preview-surface-clearcoat-roughness"></div>

## Opacity

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Opacity</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:opacity`</dd></div><div markdown><dt>Default</dt><dd markdown="span">1</dd></div></dl>
</div>
</div>

</div>

Below 1 (with **Opacity Threshold** 0) the surface is partly present, on UE's translucent material: what is behind it shows through, unbent. Opacity scales the whole surface, its reflections and emission too, so it reads as a thin film or a ghost, not as glass. For glass that refracts, use MaterialX `transmission` ([Opacity and glass](opacity-and-glass.md)).

<div class="hdue-figure" data-figure="preview-surface-opacity"></div>

## Opacity Threshold

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Opacity Threshold</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:opacityThreshold`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

Above 0, **Opacity** becomes a cutout mask (UE's Masked blend mode): pixels with an opacity below the threshold are removed, the rest are fully opaque. Use it with a texture's alpha for leaves, fences and decals. Here the ball's opacity is 0.4 throughout: at 0.3 it is opaque, at 0.5 it is cut out entirely.

<div class="hdue-figure" data-figure="preview-surface-opacity-threshold"></div>

## IOR

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">IOR</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:ior`</dd></div><div markdown><dt>Default</dt><dd markdown="span">1.5</dd></div></dl>
</div>
</div>

</div>

Index of refraction of a dielectric. It sets the reflectance facing the camera, F0 = ((ior - 1) / (ior + 1))²: 0.02 for water (1.33), 0.04 for glass and plastics (1.5), 0.17 for diamond (2.4). It does not refract. Ignored with **Use Specular Workflow** 1.

<div class="hdue-figure" data-figure="preview-surface-ior"></div>

## Normal

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Normal</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:normal`</dd></div><div markdown><dt>Default</dt><dd markdown="span">(0, 0, 1)</dd></div></dl>
</div>
</div>

</div>

A tangent-space normal map. Read the texture with **Source Color Space** `raw`, **Scale** (2, 2, 2, 1) and **Bias** (-1, -1, -1, 0), which turn texel values from 0..1 into -1..1. The map must be OpenGL-style (+Y up), as USD specifies.

## Displacement

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Displacement</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:displacement`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

Moves the surface along its normals, in scene units, before rendering, so Lumen, the Path Tracer and Nanite all see the displaced shape. A textured displacement first subdivides the mesh to about one triangle per two texels (at most about a million triangles per mesh). See [Displacement](../geometry/displacement.md).

## Occlusion

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Occlusion</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`inputs:occlusion`</dd></div><div markdown><dt>Default</dt><dd markdown="span">1</dd></div></dl>
</div>
</div>

</div>

Baked ambient occlusion, UE's Ambient Occlusion input: below 1 it darkens the ambient light reaching the surface. Checked in Realtime.

## Textures

A **USD UV Texture** (`UsdUVTexture`) can drive every input above. hdUE reads:

| Input | What it does |
| --- | --- |
| `file` | The image. `<UDIM>` patterns stream as UE virtual textures ([UDIMs](../materials.md#normal-maps-and-udims)); `op:` paths read Copernicus images ([COP textures](cop-textures.md)) |
| `st` | The UV primvar, usually through a **USD Primvar Reader** (`st`). A material reads up to 4 UV primvars |
| `wrapS`, `wrapT` | `repeat`, `clamp`, `mirror` or `black`. The default, `useMetadata`, falls back to `black` |
| `sourceColorSpace` | `raw` (data), `sRGB` (8-bit colour), `auto` (default: sRGB for colour inputs, raw for the others) |
| `scale`, `bias` | Multiply then add to the texel |
| `fallback` | The value when the file cannot be read; (0, 0, 0, 1) by default |
| `outputs:rgb`, `r`, `g`, `b`, `a` | The colour, or one channel for a float input such as roughness or opacity |

A **USD Transform 2D** (`UsdTransform2d`) between the primvar reader and the texture scales, rotates and offsets the UVs. A material uses one UV transform: the first textured input's.

Floating-point images (EXR, HDR) in colour inputs are read in the OCIO `scene_linear` space and converted to UE's linear Rec.709; with Houdini's default OCIO config this changes nothing. See [Colour management](../rendering/color-management.md).

## Recipes

How to get these looks on a Preview Surface. Inputs not listed stay at their default.

| Look | Diffuse Color | Metallic | Roughness | Other |
| --- | --- | --- | --- | --- |
| Mirror chrome | (0.9, 0.9, 0.9) | 1 | 0 | |
| Rough metal (aluminium) | (0.91, 0.92, 0.92) | 1 | 0.35 | |
| Gold | (1, 0.71, 0.29) | 1 | 0.2 | |
| Plastic | (0.05, 0.3, 0.8) | 0 | 0.25 | |
| Car paint | (0.4, 0.01, 0.02) | 0 | 0.4 | Clearcoat 1, Clearcoat Roughness 0.02 |
| Emissive panel | (0, 0, 0) | 0 | 0.5 | Emissive Color (5, 4.5, 4) |
| Cutout leaf | texture `rgb` | 0 | 0.5 | Opacity from the texture's `a`, Opacity Threshold 0.5 |

- **Brushed metal**: Preview Surface and the MaterialX translation have no anisotropy. Use a rough metal with a roughness texture of fine streaks, or a normal map.
- **Emissive panel**: raise **Emissive Color** until the panel reads as a light at the scene's exposure; add a RectLight in front of it to light the scene in Realtime.
- **Glass**: Preview Surface does not refract. Use the [glass recipes](opacity-and-glass.md#recipes).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| A texture is black outside the 0..1 UV square (tiling UVs show one tile) | `wrapS`/`wrapT` default to `useMetadata`, which falls back to `black` | Set **Wrap S** and **Wrap T** to `repeat` |
| A colour texture looks washed out | Read as `raw` | Set **Source Color Space** to `sRGB` or `auto` |
| A roughness or mask texture reads too dark (surface too glossy) | Read as sRGB | Set **Source Color Space** to `raw` |
| A texture input renders black or flat | The file does not resolve, so `fallback` is used | Fix the path |
| A normal map lights bumps from the wrong side | A DirectX-style map (-Y) | Use **Scale** (2, -2, 2, 1) and **Bias** (-1, 1, -1, 0) to flip green |
| Cutout leaves thin out with distance in Realtime | Distant pixels read smaller mips, where alpha averages below the threshold | hdUE rescales each mip to keep the alpha coverage, as UE's "Scale Mips for Alpha Coverage"; a branch 9 times further keeps 0.42 of its near coverage (0.05 with plain mips). The Path Tracer reads the full-resolution mip and does not thin |
| A clear coat looks like a hazy second highlight, not a layer | **Use Specular Workflow** 1 or **Opacity** below 1 | Use the metalness workflow and full opacity |
| A see-through surface looks like a ghost, without refraction | Opacity is presence, not transmission | Use MaterialX `transmission` for glass |

## Unreal Engine and Karma

- hdUE maps Preview Surface as UE's USD importer does onto its Preview Surface reference materials: opaque, masked (with **Opacity Threshold**) or translucent.
- `opacityMode` is not read. Partial opacity is presence in hdUE, so it fades reflections too: the rims of a thin see-through pane render at about 0.8 of Karma's.
- **Occlusion**: the specification notes that path tracers may ignore it.
