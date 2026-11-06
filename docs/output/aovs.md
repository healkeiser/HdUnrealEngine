# AOVs

AOVs (render vars) are extra images written next to the beauty: surface data such as albedo and normals, the beauty split into lighting components, object IDs and Cryptomatte mattes. hdUE uses Karma's render var names and light path expressions, so a Karma AOV setup selects the same outputs.

## Setting up AOVs in Solaris { #aovs-in-houdini }

**With hdUE Render Settings** (the quickest):

1. Open **Image Output > AOVs (Render Vars)** on hdUE Render Settings.
2. Turn on the AOVs you need. husk writes them as layers of the output picture.

**With hdUE Standard Render Vars** (the counterpart of Karma Standard Render Vars):

1. Add **hdUE Standard Render Vars** (TAB menu > hdUE > Rendering) above hdUE Render Settings or hdUE Render Products.
2. Turn on AOVs in its **Beauty**, **Surface**, **Lighting**, **Utility** and **Cryptomatte** sections. Beauty, Depth and Normal are on by default.
3. Set each AOV's **Precision**: 16 Bit (half) or 32 Bit (float).
4. On hdUE Render Settings, turn on **Import Upstream Render Vars** (or wire the node into the second input and turn on **Import Render Vars From Second Input**).

**With Render Var LOPs**: set **Source Name** to an AOV's name from the tables below (`sourceName`, source type `raw`), or to its light path expression (source type `lpe`). The render var and its channel may be named anything: hdUE matches the source name.

The beauty is the render var whose source name is `color`, `C`, `Ci` or `beauty`, or the light path expression `C.*[LO]` or `C.*`.

## Surface AOVs

Surface AOVs describe the nearest opaque surface. They are UE's buffer visualizations, the same buffers as UE's Buffer Visualization view mode.

| AOV (`sourceName`) | Karma name | Type | Holds |
|---|---|---|---|
| `albedo` | `baseColor`, `CDA` | color3f | Base colour. Path Tracer: the Path Tracer's albedo (see below) |
| `normal` | `N`, `ray:hitN` | normal3f | World-space shading normal, USD axes, normalized |
| `roughness` | | float | Roughness |
| `metallic` | | float | Metallic |
| `specular` | | float | UE's specular (0.5 is 4% reflectance) |
| `occlusion` | | float | Material ambient occlusion |
| `ambientOcclusion` | `AO` | float | Screen-space ambient occlusion (1 is unoccluded) |
| `opacity` | | float | UE's GBuffer opacity: subsurface and foliage data, 0 for other surfaces. Not the beauty's alpha |
| `subsurfaceColor` | | color3f | Subsurface colour, black for other surfaces |
| `anisotropy` | | float | Anisotropy |
| `tangent` | | normal3f | World-space tangent, USD axes, normalized |
| `shadingModel` | | color3f | UE's shading model, coloured as its Shading Model view |
| `velocity` | | color3f | Motion vectors, as Movie Render Queue writes them (below) |
| `customDepth`, `customStencil` | | float | UE's custom depth and stencil. hdUE meshes do not write them: far and 0 |

Surface AOVs have no anti-aliasing, motion blur, depth of field or transparency: one sample per pixel, at the frame's time. They line up pixel for pixel with the beauty.

**Path Tracer albedo and normal.** In Path Tracer mode, `albedo` and `normal` are the buffers the Path Tracer accumulates for its denoiser. They are anti-aliased, carry the beauty's depth of field, and cost nothing extra. The albedo is the denoiser albedo: diffuse and specular reflectance, followed through mirrors and glass, not the base colour. With motion blur on, they fall back to the unblurred surface buffers.

**Motion vectors.** `velocity` holds the screen motion since the previous frame, in UE's encoding: x in red, y in green, 0.5 for no motion. A motion of *v* in normalized device coordinates (2 is the full frame width) is stored as *v* × 0.2495 + 0.5. Deforming meshes and volumes show no motion of their own.

## Lighting AOVs

Lighting AOVs are the beauty restricted to one kind of light path, using UE's Lighting Components (the same switches Movie Render Queue's render layers use).

| AOV (`sourceName`) | Light path expression | Holds |
|---|---|---|
| `combineddiffuse` (`diffuse`) | `C<RD>.*L` | Diffuse reflection, direct and indirect |
| `directdiffuse` | `C<RD>L` | Diffuse reflection of direct light |
| `indirectdiffuse` | `C<RD>.+L` | Diffuse reflection of indirect light (realtime: Lumen GI) |
| `combinedglossyreflection` (`glossy`) | `C<RG>.*L` | Specular reflection and transmission, direct and indirect |
| `directglossyreflection` | `C<RG>L` | Specular reflection of direct light |
| `indirectglossyreflection` | `C<RG>.+L` | Specular reflection of indirect light (realtime: Lumen reflections) |
| `directemission` (`emission`) | `CO` | Emission seen directly |
| `combinedvolume` (`volume`) | `CV.*L` | Volume scattering, direct and indirect. Path Tracer only |
| `directvolume` | `CVL` | Volume scattering of direct light. Path Tracer only |
| `indirectvolume` | `CV.+L` | Volume scattering of indirect light. Path Tracer only |

All are color3f, in the config's `scene_linear` space, with the camera's exposure applied as on the beauty. They are not post-processed: no Unreal Tonemapper, bloom or grade.

In realtime mode, components are approximations: emission stays in every component, and volumes are not split (volume AOVs are cleared, with a warning). UE has no light path expressions, so other expressions are not supported.

## ID AOVs and depth

| AOV (`sourceName`) | Type | Holds |
|---|---|---|
| `depth` | float | Distance from the camera plane, in stage units |
| `primId` | int | Hydra prim ID of the visible mesh, -1 for background |
| `instanceId` | int | Instance index of an instanced prototype, -1 elsewhere |

ID AOVs are unlit raster passes: no anti-aliasing, motion blur, depth of field or transparency. Meshes placed from the native asset catalog read as background.

## Cryptomatte { #cryptomatte }

hdUE writes [Cryptomatte](https://github.com/Psyop/Cryptomatte) (specification 1.2), as Movie Render Queue's Object ID pass does.

1. On **hdUE Standard Render Vars**, open **Cryptomatte**.
2. Turn on **Crypto Object** (one ID per prim path), **Crypto Material** (one ID per bound material), or both.
3. Each adds three 32-bit `float4` render vars, `CryptoObject00` to `CryptoObject02` (or `CryptoMaterial00` to `02`): six ranks.

husk writes each layer's metadata and manifest (`cryptomatte/<key>/name`, `hash`, `conversion`, `manifest`) into every image, as the specification defines, so Cryptomatte tools in comp find the names.

Coverage comes from 16 samples per pixel on a 4 x 4 grid, so edges are anti-aliased. Like the ID AOVs, Cryptomatte ignores depth of field, motion blur and transparency. Geometry with no bound material has no Crypto Material ID.

!!! warning "Keep Cryptomatte at 32 bit"
    The IDs are float bit patterns: a 16-bit (half) channel destroys them. hdUE Standard Render Vars writes them as 32-bit float4.

Karma's Cryptomatte LOP authors Karma-only render vars, which hdUE does not read. Use hdUE Standard Render Vars instead.

## What each AOV costs

AOVs render after the beauty converges, from the same camera and time. The beauty itself is not slowed down.

| AOVs | Extra cost per frame |
|---|---|
| All surface AOVs together | One short raster render (two with `velocity`: it renders the previous frame first) |
| Path Tracer `albedo` and `normal` | Nothing |
| Each lighting AOV | One full render of the beauty: its mode, samples, warmup and exposure |
| Each ID AOV (`primId`, `instanceId`) | One short raster render |
| Cryptomatte, any number of layers | 16 short raster renders |
| `depth` | Nothing: read back with the beauty |

With three lighting AOVs, a frame takes about four times as long as the beauty alone. Pick the components you will use.

## Recipes

### How to rebuild the beauty in comp

Use Path Tracer mode with the Unreal Tonemapper off: the beauty and the lighting AOVs are then the same scene-linear radiance.

1. Turn on `combineddiffuse`, `combinedglossyreflection` and `directemission`, plus `combinedvolume` if the shot has volumes.
2. In comp, add them: diffuse + glossy + emission (+ volume).
3. Grade each layer, then compare the sum with the beauty using a Difference.

!!! note
    Conformance tests check that direct and indirect diffuse add up to combined diffuse. The full sum against the beauty is not tested: check the difference on your shot before relying on it. In realtime mode, emission is in every component, so the components do not add up to the beauty.

### How to set up a comp-ready AOV set

1. hdUE Standard Render Vars: Beauty (16 Bit), Depth (32 Bit), Normal (16 Bit), Albedo, Combined Diffuse, Combined Glossy Reflection, Direct Emission.
2. Cryptomatte: Crypto Object and Crypto Material.
3. hdUE Render Settings: **Unreal Tonemapper** off; **Import Upstream Render Vars** on.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| An AOV is black, with a warning naming it | The source name is not one hdUE renders, or a light path expression it does not support | Use a name or expression from the tables above. |
| Volume AOVs black in realtime | Realtime does not split volumes | Render in Path Tracer mode. |
| `customDepth` far, `customStencil` 0 | hdUE meshes do not write UE's custom depth or stencil | Use `depth` and `primId` instead. |
| Cryptomatte selects nothing in comp | Written as 16-bit half | Keep Cryptomatte at 32 bit (`float4`). |
| Cryptomatte from Karma's Cryptomatte LOP missing | Karma's render vars are Karma-only | Use hdUE Standard Render Vars' Cryptomatte section. |
| Lighting AOVs do not match the beauty | The Unreal Tonemapper is on: the beauty is tone-mapped, the AOVs are not | Turn the tonemapper off for comp renders. |
| Albedo and normal blurry or soft at the edges in Path Tracer mode | They are the Path Tracer's anti-aliased buffers, with depth of field | Expected. Use `depth` and Cryptomatte for hard mattes. |
| Viewport selection highlight lags one frame after an edit | AOVs follow the converged beauty, and Solaris tints its selection from `primId` | Expected. It catches up once the frame converges. |

## Unreal Engine and Karma

- Every output is one UE already provides: buffer visualizations, the Path Tracer's denoiser buffers, Lighting Components and Movie Render Queue's Object ID recipe for Cryptomatte.
- Karma's names and light path expressions select the same AOVs. Karma computes every light path expression in one render; UE has no light path expressions, so each lighting AOV is a render of its own.
- Karma's surface AOVs are anti-aliased; hdUE's realtime surface AOVs are single-sample, like UE's buffer visualizations.
- UE's buffer visualizations of the image itself (`SceneColor`, `PreTonemapHDRColor`, `FinalImage`, ...) are not offered: they are states of the beauty.
