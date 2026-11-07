# Post-processing

UE's post-processing turns the rendered light into a finished picture: exposure, tone curve, bloom, lens effects, vignette, grain and colour grading. In hdUE it is off by default, so Houdini gets a scene-linear beauty. Turn on **Unreal Tonemapper** to get UE's look.

Unreal Tonemapper off (default) and on.

<div class="hdue-figure" data-figure="tonemapper"></div>

## Using it in Solaris

=== "Houdini"

    1. Add **hdUE Render Settings** (TAB menu > hdUE > Rendering), or select the one in your scene.
    2. Turn on **Unreal Tonemapper** at the top of the parameters.
    3. Set the effects on the **Lens** and **Color Grading** tabs. Their parameters are greyed out while the tonemapper is off, and hidden in **Essential** mode.
    4. View the result with Houdini's default **Un-tone-mapped** view, not Raw. See [Colour management](color-management.md).

=== "USD"

    On a plain Render Settings LOP or in a USD file, author `unreal:postProcess:<Property>` on the render settings prim:

    ```usda
    bool unreal:tonemapper = 1
    float unreal:postProcess:BloomIntensity = 2
    float4 unreal:postProcess:ColorSaturation = (1, 1, 1, 0.8)
    ```

Every parameter is UE's own [Post Process Settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/post-process-effects-in-unreal-engine) property, with UE's name, range and default. The [render settings reference](../reference/render-settings/lens.md) lists all of them.

Post-processing applies to the beauty only. AOVs stay unprocessed. Changing a post-process setting restarts Path Tracer accumulation.

!!! note "What works with the tonemapper off"
    Depth of field, motion blur and post-process materials set to **Before Tonemapping** work with the tonemapper off. Everything else on this page needs it on.

## Tonemapper and exposure

### Unreal Tonemapper

**Unreal Tonemapper** (`unreal:tonemapper`) · Default Off · [reference](../reference/render-settings/index.md#unreal-tonemapper)

Runs UE's tonemapper pass, which holds bloom, colour grading, the film curve, vignette, grain, chromatic aberration and lens flares. The beauty becomes UE's tone-mapped image: middle grey (0.18) stays 0.18, 1.0 becomes 0.72, 4.0 becomes 0.94, and 0.03 drops to 0.011. Dark scenes therefore look darker than without it; raise **Exposure Compensation**.

### Disable Tone Curve

**Disable Tone Curve** (`unreal:disableToneCurve`) · Default Off · [reference](../reference/render-settings/index.md#unreal-disabletonecurve)

Keeps the tonemapper's grading, bloom, vignette and grain, without the film curve: the beauty stays scene-linear for an ACES or AgX view. See [Colour management](color-management.md#disable-tone-curve).

### Exposure Compensation

**Exposure Compensation** (`unreal:postProcess:AutoExposureBias`) · Default 0 · Range -15 to 15 · [reference](../reference/render-settings/index.md#unreal-postprocess-autoexposurebias)

Exposure in stops: +1 doubles the image, -1 halves it. The USD camera's **Exposure** adds to it. Only used with the tonemapper on.

<div class="hdue-figure" data-figure="post-exposure-compensation"></div>

### Metering Mode

**Metering Mode** (`unreal:postProcess:AutoExposureMethod`) · Default Manual · [reference](../reference/render-settings/lens.md#unreal-postprocess-autoexposuremethod)

Manual keeps exposure fixed, so every frame is exposed the same. **Auto Exposure Histogram** or **Auto Exposure Basic** turn on UE's eye adaptation, between **Min Brightness** (0.03) and **Max Brightness** (8).

Manual (default) and Auto Exposure Histogram.

<div class="hdue-figure" data-figure="post-metering-mode"></div>

!!! warning "Auto exposure in sequences"
    Auto exposure is measured per frame. Two identical Sponza renders differed by about 7%, and each frame of a sequence is exposed on its own. Use Manual for final sequences.

In husk and in Path Tracer mode, auto exposure reaches its target on every frame. In the realtime viewport it does too, unless you set **Speed Up** or **Speed Down**: then the viewport adapts gradually, as in UE.

UE's physical camera exposure (ISO, shutter speed, aperture) stays off unless you turn on **Apply Physical Camera Exposure**.

## Lens

### Bloom

**Intensity** (`unreal:postProcess:BloomIntensity`) · Default 0.675 · Range 0 to 8 · [reference](../reference/render-settings/lens.md#unreal-postprocess-bloomintensity)

Glow around bright pixels. 0 turns bloom off. **Threshold** (default -1) makes every pixel bloom in proportion to its brightness, as a real lens does. **Preferred Method** switches to **Convolution**, an FFT bloom shaped by a **Kernel** image, which costs more.

<div class="hdue-figure" data-figure="post-bloom"></div>

### Chromatic Aberration

**Intensity** (`unreal:postProcess:SceneFringeIntensity`) · Default 0 · Range 0 to 5 · [reference](../reference/render-settings/lens.md#unreal-postprocess-scenefringeintensity)

Colour fringes towards the frame's corners, in percent. **Start Offset** (0 to 1) moves where the fringes begin, from the centre outwards.

<div class="hdue-figure" data-figure="post-chromatic-aberration"></div>

### Lens Flares

**Intensity** (`unreal:postProcess:LensFlareIntensity`) · Default 1 · Range 0 to 16 · [reference](../reference/render-settings/lens.md#unreal-postprocess-lensflareintensity)

Image-based flares from pixels brighter than **Threshold** (default 8). Lower the threshold to get flares from dimmer highlights; each step down blurs more of the image and costs more. **BokehShape** takes an image for the flare's shape.

<div class="hdue-figure" data-figure="post-lens-flares"></div>

### Dirt Mask

**Dirt Mask Intensity** (`unreal:postProcess:BloomDirtMaskIntensity`) · Default 0 · Range 0 to 8 · [reference](../reference/render-settings/lens.md#unreal-postprocess-bloomdirtmaskintensity)

Lens dirt lit by bloom. Set **Dirt Mask Texture** to an image of the dirt, then raise the intensity.

### Vignette

**Vignette Intensity** (`unreal:postProcess:VignetteIntensity`) · Default 0.4 · Range 0 to 1 · [reference](../reference/render-settings/lens.md#unreal-postprocess-vignetteintensity)

Darkens the frame's corners. 0 turns it off. The Advanced parameters set its **Vignette Type** (cosine-fourth law, oval or a texture), centre, size, softness and colour.

<div class="hdue-figure" data-figure="post-vignette"></div>

### Sharpen

**Sharpen** (`unreal:postProcess:Sharpen`) · Default 0 · Range 0 to 10 · [reference](../reference/render-settings/lens.md#unreal-postprocess-sharpen)

Sharpens the image during tonemapping.

## Color Grading

All grading works on the scene-linear image before the film curve, as in UE. Saturation, Contrast, Gamma, Gain and Offset are four numbers: red, green, blue and an overall multiplier. Each exists for the whole image (**Global**) and for **Shadows**, **Midtones** and **Highlights**.

### Temperature

**Temp** (`unreal:postProcess:WhiteTemp`) · Default 6500 K · Range 1500 to 15000 · [reference](../reference/render-settings/color-grading.md#unreal-postprocess-whitetemp)

The white balance of a virtual camera: the light temperature that renders white. Below the scene's light temperature the image turns blue, above it yellow. **Temperature Type** set to **Color Temperature** inverts this. **Tint** (-1 to 1) shifts along the magenta-green axis.

<div class="hdue-figure" data-figure="post-white-temp"></div>

### Saturation

**Saturation** (`unreal:postProcess:ColorSaturation`) · Default (1, 1, 1, 1) · Range 0 to 2 · [reference](../reference/render-settings/color-grading.md#unreal-postprocess-colorsaturation)

0 is black and white, 1 leaves the image unchanged, above 1 strengthens colours. The captions give the fourth (overall) value.

<div class="hdue-figure" data-figure="post-saturation"></div>

### Contrast, Gamma, Gain, Offset

| Parameter | Attribute | Default | What it does |
|---|---|---|---|
| **Contrast** | `unreal:postProcess:ColorContrast` | (1, 1, 1, 1) | Spreads values away from middle grey |
| **Gamma** | `unreal:postProcess:ColorGamma` | (1, 1, 1, 1) | Brightens or darkens the midtones |
| **Gain** | `unreal:postProcess:ColorGain` | (1, 1, 1, 1) | Multiplies the image |
| **Offset** | `unreal:postProcess:ColorOffset` | (0, 0, 0, 0) | Adds to the image: lifts the blacks |

### Film

**Method** (`unreal:postProcess:TonemappingMethod`) · Default Filmic · [reference](../reference/render-settings/color-grading.md#unreal-postprocess-tonemappingmethod)

The film curve. **Slope** (0.88), **Toe** (0.55), **Shoulder** (0.26), **Black clip** (0) and **White clip** (0.04) shape it. **Tone Curve Amount** (1) fades it out: set it and **Expand Gamut** to 0 to remove the curve completely.

<div class="hdue-figure" data-figure="post-tonemapping-method"></div>

### Film Grain

**Film Grain Intensity** (`unreal:postProcess:FilmGrainIntensity`) · Default 0 · Range 0 to 1 · [reference](../reference/render-settings/color-grading.md#unreal-postprocess-filmgrainintensity)

Grain over the image. **Film Grain Intensity Shadows**, **Midtones** and **Highlights** weight it per range; **Film Grain Scale** sets its size.

<div class="hdue-figure" data-figure="post-film-grain"></div>

### Color Grading LUT

**Color Grading LUT** (`unreal:postProcess:ColorGradingLUT`) · Default empty · [reference](../reference/render-settings/color-grading.md#unreal-postprocess-colorgradinglut)

A lookup table in UE's format: a 256 x 16 image of 16 slices, red across a slice, green down, blue across the slices. **Color Grading LUT Intensity** (0 to 1) blends it in.

## Texture parameters

Parameters that take a texture (Color Grading LUT, Dirt Mask Texture, Film Grain Texture, Vignette Texture, Lens Flare BokehShape, the convolution bloom Kernel, Exposure Metering Mask, Ambient Cubemap) accept either:

- an image file: EXR, HDR, PNG, JPG, TIF, TGA or BMP. A relative path resolves against the USD layer that authors it. 8-bit files are read as sRGB, float files as linear. Editing the file updates the render.
- a cooked texture in the runtime project: `/Game/...` or `/Engine/...`.

A file that cannot be read is skipped with a warning in the UE log (`out/runtime/Windows/HdUEBackend/Saved/Logs/`).

## Post-process materials

**Post-Process Materials** (`unreal:postProcessMaterials`) · Default empty · [reference](../reference/render-settings/rendering-features.md#unreal-postprocessmaterials)

Cooked UE post-process materials, by path, separated by spaces. They are added to the beauty as UE blendables. Materials whose **Blendable Location** is **Before Tonemapping** also work with the tonemapper off.

## Post-process volumes

A post-process volume applies its settings only while the camera is inside it or within its blend radius, as UE's Post Process Volume does.

1. Add **hdUE Post Process Volume** (TAB menu > hdUE > Visual Effects). In Create mode it shows a guide box: the volume fills the cube from -1 to 1 of its transform.
2. Set **Priority**, **Blend Radius** and **Blend Weight**, or turn on **Infinite Extent (Unbound)** to cover the whole scene.
3. Turn on the settings it should change: each starts at **Do Nothing**, like UE's override checkboxes.

| Parameter | Attribute | Default | What it does |
|---|---|---|---|
| **Priority** | `unreal:postProcessVolume:Priority` | 0 | The higher priority wins where volumes overlap |
| **Blend Radius** | `unreal:postProcessVolume:BlendRadius` | 100 | Distance around the box over which the volume fades in, in UE centimetres |
| **Blend Weight** | `unreal:postProcessVolume:BlendWeight` | 1 | 0 no effect, 1 full effect |
| **Enabled** | `unreal:postProcessVolume:bEnabled` | On | |
| **Infinite Extent (Unbound)** | `unreal:postProcessVolume:bUnbound` | Off | Covers the whole world |

The render settings apply after every volume, as a camera's settings do in UE: a setting authored on hdUE Render Settings wins over all volumes. Volumes need the tonemapper on for exposure and grading, like the render settings. See [UnrealPostProcessVolume](../reference/usd-prims.md#unrealpostprocessvolume).

## Decals

A decal projects a material onto the surfaces inside a box, as UE's Decal Actor does.

1. Add **hdUE Decal** (TAB menu > hdUE > Visual Effects). In Create mode it shows a guide box: the decal fills the cube from -1 to 1 of its transform and projects down its local down axis.
2. Bind a decal material: a [Substrate Builder decal material](../substrate-builder.md#decal-materials), or set **Decal Material** (`unreal:decal:DecalMaterial`) to a cooked Deferred Decal material path.
3. Optionally set **Decal Color** (`unreal:decal:DecalColor`, default white), which the material reads through UE's Decal Color node. **Sort Order** (0): higher values draw on top of overlapping decals. **Fade Screen Size** (0.01) fades the decal when it is small on screen.

A decal with no material draws nothing. A material that is not a decal material is skipped with a warning. See [UnrealDecal](../reference/usd-prims.md#unrealdecal).

## Recipes

### How to match UE's editor look

1. **Unreal Tonemapper**: on.
2. **Lens > Exposure > Metering Mode**: Auto Exposure Histogram.
3. **Exposure Compensation**: 1 (UE's project default).
4. View with Un-tone-mapped.

Bloom 0.675, vignette 0.4 and the Filmic curve are already UE's defaults. The camera's exposure still adds to the compensation: set it to 0 for a scene exposed for Karma. On Sponza, this lands about 0.6 stops above the camera's authored exposure.

### How to get a filmic grade for a still

1. **Unreal Tonemapper**: on; **Metering Mode**: Manual.
2. **Exposure Compensation**: adjust until middle grey reads right, typically 0 to 2.
3. **Temp**: 7500 (warmer than neutral). **Saturation**: (1, 1, 1, 1.15).
4. **Bloom Intensity**: 1.5. **Vignette Intensity**: 0.5.
5. **Chromatic Aberration Intensity**: 4. **Film Grain Intensity**: 0.35.

`samples/cornell/variants/post_processing.usda` shows these effects on the Cornell box.

### How to grade but keep a linear EXR

1. **Unreal Tonemapper**: on; **Disable Tone Curve**: on.
2. Set the grade (Temp, Saturation, Contrast, ...). Leave grain and vignette off if comp will add them.

The EXR is scene-linear, graded, in your config's `scene_linear` space.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Bloom, grading or vignette have no effect | The Unreal Tonemapper is off | Turn on **Unreal Tonemapper**. |
| Exposure Compensation has no effect | The Unreal Tonemapper is off | Turn it on, or use the camera's **Exposure**, which works either way. |
| Image much darker with the tonemapper on | The film curve compresses values below middle grey (0.03 becomes 0.011) | Raise **Exposure Compensation**. |
| Image too dark and crushed | Viewed with Raw | Use Un-tone-mapped. |
| Exposure flickers between frames | Auto exposure | Set **Metering Mode** to Manual. |
| A post-process setting is ignored, with a warning in the UE log | The property name or value is unknown | Check the spelling against the [reference](../reference/render-settings/index.md). |
| Viewport shadows black, husk EXR fine | Houdini 22.0.368's viewport OCIO display shows linear values below about 0.004 as black | Judge dark images in the EXR. |

## Unreal Engine and Karma

- Every parameter is UE's `FPostProcessSettings` property, with UE's default, except exposure: hdUE renders with manual exposure and Exposure Compensation 0, where UE defaults to Auto Exposure Histogram with compensation 1. This keeps middle grey where the linear image has it and makes frames repeatable.
- With the tonemapper off, hdUE's beauty is scene-linear like Karma's. Karma has no equivalent of UE's tonemapper; grade Karma renders in comp or in Copernicus.
- For the same stage and settings, Houdini's viewport and husk give the same beauty: on the Cornell box with the tonemapper on, within 3% in mean and 1.5/255 in sRGB at the 10th, 50th and 90th percentiles.
