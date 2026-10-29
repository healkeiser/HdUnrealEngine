# Materials, lights and color audit

## Scope & sources

Audited at `feature/package` @ `e8fe1f4`. Code read: `src/hydra/materials.cpp`, `src/hydra/color_management.{h,cpp}`, `src/hydra/prims.cpp` (Light sprim), `src/hydra/delegate.cpp` (light/material packing, output colour), `include/hdue/{lighting,material,postprocess,bridge}_api.h`, `unreal/HdUEBackend/Source/HdUEBackend/Private/{Lighting,Scene,Bridge}.cpp`, `unreal/HdUEBackend/Source/HdUEContent/Private/HdUEPrepareCommandlet.cpp` (the `M_PreviewSurface*` parents), the MaterialX/Substrate commandlets (only skimmed for colour-space handling), `unreal/HdUEBackend/Config/DefaultEngine.ini`, `samples/cornell/**`, `scripts/render_gallery.py`.

Engine and USD sources were read locally rather than quoted from memory: UE 5.8.4 at the engine source (`engine_root`) and OpenUSD 26.03 at the OpenUSD build (`openusd_root`) (paths below marked `UE:` / `USD:`).

Specs and docs:
- UsdPreviewSurface spec: https://openusd.org/release/spec_usdpreviewsurface.html (the page header says 2.5 and the changelog says 2.6 is head).
- UsdLux schema docs: https://openusd.org/release/api/usd_lux_page_front.html, plus the class pages (LightAPI, ShapingAPI, DomeLight_1). The quotes below were checked against the shipped schema, `USD: lib/usd/usdLux/resources/usdLux/schema.usda`, and `USD: include/pxr/usd/usdLux/domeLight_1.h`.
- Lumen technical details: https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
- UE physical light units: https://dev.epicgames.com/documentation/en-us/unreal-engine/using-physical-lighting-units-in-unreal-engine. This page was not fetched. Every unit claim below comes from UE source.
- UE's own USD importer: `UE: Engine/Plugins/Runtime/USDCore/Source/USDUtilities/Private/USDLightConversion.cpp`.

## Summary table

| ID | Severity | Finding | Effort |
|---|---|---|---|
| LGT-01 | Critical | IES lights drop the profile's absolute candela scale (UE normalises the profile to max = 1), so they render about 1000x darker than spec and Karma. This is the gallery's 0.00. | S |
| LGT-02 | High | Lumen renders about 0.6x Karma while the Path Tracer matches. The deficit is in indirect light. Most likely cause: Lumen's surface cache gets little or no direct light from the rect light, which sits 1 mm under a zero-thickness ceiling quad. | M |
| LGT-03 | High | `shaping:ies:normalize` does nothing: it multiplies by `TextureMultiplier`, which UE always sets to 1. | S |
| MAT-01 | High | One missing or unreadable texture fails the whole material batch, instead of using `UsdUVTexture.inputs:fallback`. | S |
| LGT-04 | High | Light linking and shadow linking (`collection:lightLink` / `shadowLink`) are ignored. | L |
| LGT-05 | Medium | One unsupported light attribute rejects the entire light batch: dome `diffuse`/`specular` ≠ 1, a non-latlong dome format, IES on a distant light, or a texture on a sphere light. | S |
| LGT-06 | Medium | `DomeLight_1.poleAxis` is ignored, so domes on Z-up stages are rotated 90°. | S |
| LGT-07 | Medium | ShapingAPI is only partly honoured: cone only on SphereLight below 90°, the schema default of 90° (hemisphere) is ignored, no cone on rect/disk/cylinder, `focus`/`focusTint` ignored, and the softness curve differs. | M |
| MAT-02 | Medium | `sourceColorSpace` does not drive sRGB decoding; the slot does. A raw-tagged colour texture is still sRGB-decoded. | S |
| MAT-03 | Medium | `wrapS`/`wrapT` are ignored (always repeat). The spec default `useMetadata` falls back to `black`. | S |
| MAT-04 | Medium | Preview Surface clearcoat is Substrate "haziness" on the base lobe, not a white coat lobe with ior-derived F0. | M |
| MAT-05 | Medium | Partial opacity weights the whole slab (specular and emission too). That is `presence` semantics applied to the default `transparent` mode. `opacityMode` is not read. | M |
| COL-01 | Medium | With UE's tonemapper on, display-referred pixels are converted as if they were scene_linear, so the host's view transform applies a second tone curve. | S |
| COL-02 | Medium | Light textures (dome HDRI, rect image) skip the OCIO scene_linear→Rec.709 conversion that material textures and light colours get. | S |
| MAT-06 | Low | Dielectric F0 is clamped at 0.08 (ior ≈ 1.8). Higher-ior materials are under-reflective. | S |
| MAT-07 | Low | One UV transform and one UV primvar per material. Other textures silently reuse them. | M |
| MAT-08 | Low | Displacement is World Position Offset on existing vertices, with no tessellation (Karma displaces at render time). | L |
| COL-03 | Low | Texture colour spaces are binary (raw / not raw). MaterialX `colorspace` names such as `acescg`, `srgb_texture` and `lin_rec709` are ignored, and every EXR/HDR is assumed scene_linear. | S |
| LGT-08 | Low | `shaping:ies:angleScale` is unsupported (only a warning). | S |
| LGT-09 | Low | Light filters, PortalLight, ShadowAPI `color`/`distance`/`falloff`, and dome `guideRadius`/portals are ignored. | M–L |

---

## LGT-01 — IES lights lose absolute candela (Critical)

**What we do.** `Lighting.cpp:214` sets `Local->bUseIESBrightness=false`, then applies only the user intensity (`Lighting.cpp:146-162`) and, when normalize is on, `TextureMultiplier` (`Lighting.cpp:216`). The profile texture is built with `FIESConverter` (`Lighting.cpp:227-241`). UE normalises its texels to the profile maximum: `UE: Engine/Source/Runtime/IESFile/Private/IESLoader.cpp:381-396` (`FloatValue = InvMaxValue * Interpolate2D(...)`). The absolute maximum candela is kept only in `Brightness` (`IESLoader.cpp:333`, "Use max candela as the brightness"), and `ULightComponent::ComputeLightBrightness` reads it only when `bUseIESBrightness` is true (`UE: Runtime/Engine/Private/Components/LightComponent.cpp:569-573`).

**What the spec says.** The UsdLux `shaping:ies:file` doc (https://openusd.org/release/api/class_usd_lux_shaping_a_p_i.html; `USD: schema.usda:790-812`) says the sampled luminous intensity "is then used as a scaling factor on the returned luminance: … iesSample = sampleIES(iesFile, θ, φ) … L_Color = iesSample ⋅ L_Color". The raw candela values in the file scale the light.

**Impact.** The gallery scene `samples/cornell/variants/ies.usda` uses `beam.ies`, whose peak is 1000 cd (`samples/cornell/beam.ies:8`). hdUE renders it about 1000x darker than Karma in both modes, which is the 0.00 / 0.00 in the README table. `docs/images/gallery/ies.png` shows both hdUE panels nearly black while Karma's are fully lit. This is a unit error, not a shading bug: the IES shape is applied, so the README's "applied and oriented" holds.

**Recommendation.**
1. With `ies:normalize=false`, multiply `Intensity` by `Converter.GetBrightness()` (max cd), since UE's texture already holds `cd/max`. Do not switch to `bUseIESBrightness=true`, which would discard the USD intensity.
2. Re-render the IES gallery row. A ratio near 1 confirms the fix; anything else points to a second issue such as orientation of the vertical angle.

## LGT-02 — Lumen is about 40% darker than Karma, the Path Tracer matches (High)

**Code that rules out a global exposure cause.** Both modes go through the same view setup:
- `Bridge.cpp:553-554` sets manual exposure with bias 0.
- `Bridge.cpp:484-485` turns the tonemapper and eye adaptation off unless requested.
- `DefaultEngine.ini:12` sets `r.UsePreExposure=False`.
- Camera exposure is applied on the host for both modes (`delegate.cpp:1280-1281`).

The light intensities are identical too: `Lighting.cpp` does not branch on render mode. The difference therefore comes from Lumen's light transport, not from units, exposure or the tonemapper. The "Camera exposure" row (0.60 / 1.00) agrees: its ratio is the same as the base scene.

**Evidence that the missing energy is indirect.** I linearised `docs/images/gallery/cornell.png` (sRGB PNG, 4 panels of 512 px) and took luminance ratios of each panel to Karma CPU (approximate):

| Region | Lumen | Path Tracer | Karma XPU |
|---|---:|---:|---:|
| Ceiling (indirect only) | **0.20** | 1.00 | 1.00 |
| Back wall | 0.74 | 1.04 | 1.01 |
| Lit floor (mostly direct) | 0.85 | 0.91 | 1.01 |
| Whole frame | 0.66 | 1.00 | 1.02 |

Directly lit surfaces are close to the Path Tracer. The ceiling, which only sees bounced light, keeps 20%. Lumen delivers roughly one screen-space bounce and loses multi-bounce energy.

**Grounded hypothesis (unverified, needs one test).** The Lumen surface cache is not receiving the rect light's direct lighting:
- In `samples/cornell/cornell.usda:22-30` the RectLight is at y = 5.487 m. The ceiling quad is at y = 5.488 m (`cornell-box.usda:363`), 1 mm away.
- Every wall is a single zero-thickness quad (`cornell-box.usda:73-263`). Their Lumen cards are built from a degenerate (flat) bounding box (`MeshCards.cpp:24-36`, "opaque, one-sided").
- Lumen's surface-cache direct lighting traces shadow rays with a surface bias of several cm (`UE: Runtime/Renderer/Private/Lumen/LumenRadiosity.cpp:100`, `GLumenRadiosityDistanceFieldSurfaceBias = 10.0f`; the direct-lighting bias is analogous, unverified). Those rays would end inside the ceiling's 1 mm gap, so the floor's cached radiance is near zero.
- Screen traces still read this frame's deferred direct light from the visible floor. That explains why the ceiling gets 20% rather than 0%.

Epic documents both conditions: "Walls should be no thinner than 10 centimeters (cm) to avoid light leaking", and areas without surface-cache coverage "will not bounce light" (https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine).

Secondary contributors, all unverified:
- 16 warmup frames with `r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated=4` (`LumenRadiosity.cpp:147`). The `Bridge.cpp:465-466` comment admits that 16 frames are about 15% short of 128 frames in the shade.
- Lumen's short-range AO (`r.Lumen.ScreenProbeGather.ShortRangeAO`) darkens the corners, which are visibly darker in the Lumen panel.

The surface-cache explanation is also consistent with the other rows: Sponza (sun plus dome, no ceiling-flush light) shows a smaller deficit (0.83).

**Recommendation.** Run these as tests before changing any code:
1. Render the Lumen "Surface Cache" / Lumen Scene debug view (already exposed, `Bridge.cpp:886`) on the Cornell scene. A black floor in the Lumen Scene confirms the hypothesis.
2. Move the RectLight 5 cm down, or make the ceiling 10 cm thick, and re-measure the ceiling ratio.
3. Render Lumen with `lumenEnabled=false` and the Path Tracer with `PathTracingMaxBounces=1` to compare direct light alone.

If the hypothesis holds:
- Give runtime cards a minimum thickness for planar meshes.
- Offer an `r.Lumen.HardwareRayTracing.LightingMode=2` (hit lighting) option for offline Lumen. Hit lighting evaluates lights at ray hits instead of reading the surface cache. Its cost is unmeasured.
- Raise warmup frames or `MaxFramesAccumulated` for husk.

Finally, publish the per-region breakdown in the gallery, so the next regression is diagnosable.

## LGT-03 — `ies:normalize` is a no-op (High)

**What we do.** `Lighting.cpp:215-216` does `if(D.ies_normalize && Texture->TextureMultiplier>0) Intensity*=Texture->TextureMultiplier;`, with a comment calling it "the profile's sphere-integration multiplier". `FIESConverter::GetMultiplier()` returns what `ExtractInRGBA16F` returns, which is a constant `1.f` (`UE: IESLoader.cpp:412`, `IESConverter.cpp`/`IESLoader.cpp:652`). UE also multiplies `TextureMultiplier` itself in `ComputeLightBrightness` (`LightComponent.cpp:573`), so a real multiplier would be applied twice.

**What the spec says.** `shaping:ies:normalize` "Normalizes the IES profile so that it affects the shaping of the light while preserving the overall energy output … the total power is calculated by integrating the luminous intensity over all solid angle patches" (`USD: schema.usda:898-907`; https://openusd.org/release/api/class_usd_lux_shaping_a_p_i.html).

**Impact.** Normalised and unnormalised IES lights render the same, both wrong after LGT-01.

**Recommendation.** Integrate the profile once when the texture is built: Σ I(θ,φ)·dΩ over the 256² texels, giving the profile's power in lumens. When `ies_normalize` is set, scale by `max / (power / 4π)`, so the profile's mean intensity equals the unshaped light's. Remove the `TextureMultiplier` line.

## MAT-01 — A missing texture fails the material batch (High)

**What we do.** `FHdueScene::PreviewTexture` returns `nullptr` with "Failed to import texture" when a file did not decode (`Scene.cpp:396`). The caller aborts the whole `Apply` (`Scene.cpp:753`: `if (!Image) return false;`). Decoding failures are silently skipped earlier (`Scene.cpp:699`). `materials.cpp` never reads `UsdUVTexture.inputs:fallback`.

**What the spec says.** "fallback … Fallback value used when texture can not be read", default (0,0,0,1) (https://openusd.org/release/spec_usdpreviewsurface.html).

**Impact.** One broken path in a 100-material asset blocks every material in that batch. Karma and Storm render the fallback colour.

**Recommendation.** On a decode failure, set the slot's `Scale`=0 and `Bias`=`fallback` (already the untextured code path, `Scene.cpp:747-750`), and warn once.

## LGT-04 — No light or shadow linking (High)

**What we do.** `prims.cpp:778-846` reads no collection data, and `HdueLight` (`lighting_api.h:323-339`) has no field for it.

**What the spec says.** LightAPI applies `CollectionAPI:lightLink` and `CollectionAPI:shadowLink` with `includeRoot = 1` (`USD: schema.usda:14-21,117-124`; https://openusd.org/release/api/class_usd_lux_light_a_p_i.html). Karma honours both. UE's importer has none, but UE has 3 lighting channels and per-primitive "affect lighting / cast shadow" flags.

**Impact.** Any lookdev or lighting setup that relies on linking renders differently from Karma, with no warning.

**Recommendation.** Hydra already exposes `HdTokens->lightLink` / `shadowLink` category IDs on rprims and lights (unverified for 26.03 scene-index emulation). Map up to 3 distinct light-link categories to `LightingChannels`. Map a shadow-link exclusion to a per-component `bCastShadow` off for that light only where UE allows it (UE cannot do per-light shadow exclusion; document that limit). Warn when more than 3 categories exist.

## LGT-05 — One unsupported attribute rejects every light (Medium)

**What we do.** `FHdueLighting::Apply` validates the whole batch first and returns false on the first problem (`Lighting.cpp:71-118`). The problems are:
- dome `diffuse`/`specular` ≠ 1 (`:85-87`)
- IES on a distant or dome light (`:91-93`)
- an image on anything but a dome or rect light (`:115`)
- dome `texture:format` of `angular`, `mirroredBall` or `cubeMapVerticalCross` (`prims.cpp:810` maps these to 99, rejected at `Lighting.cpp:74`)

**What the spec says.** All of these are valid UsdLux: `diffuse`/`specular` are "non-physical" multipliers (`schema.usda:232-244`), and `texture:format` allows five tokens (`schema.usda:1308-1311`).

**Impact.** A dome with diffuse 0.5 turns off the sun too. The error surfaces in the UE log, not as a clear Hydra warning.

**Recommendation.** Degrade per light: warn and ignore the attribute (dome multipliers → `SkyLight` `IndirectLightingIntensity`/`VolumetricScatteringIntensity` cannot split diffuse from specular, so ignore them), or skip only the offending light.

## LGT-06 — `DomeLight_1.poleAxis` ignored (Medium)

**What we do.** `prims.cpp` never reads `poleAxis`. `lighting_api.h:328-329` says "Dome maps always use local +Y as the pole and +Z at longitude zero", and `Lighting.cpp:36-38` samples that way. The +Y convention itself is correct; see "What is already right".

**What the spec says.** "scene: The dome light's top pole is aligned with the stage's up axis … Z: … latitude 0, longitude 0 will instead point into the negative Y direction", and the rotation "is intended to be applied by a renderer to only the dome itself" (`USD: domeLight_1.h:38-75`; https://openusd.org/release/api/class_usd_lux_dome_light__1.html). UsdImaging maps `DomeLight_1` onto the same Hd `domeLight` (`USD: usdImaging plugInfo.json:254-259`), so the renderer must read the attribute.

**Impact.** On Z-up stages (Houdini's default USD up axis is Y, but many studio and Blender stages are Z), an HDRI's horizon stands vertical. It also differs from Karma.

**Recommendation.** Read `poleAxis` with `GetLightParamValue`. When it resolves to Z (or "scene" on a Z-up stage), pre-rotate the dome frame by +90° about X before `Frame(D).Inverse()`.

## LGT-07 — Partial ShapingAPI (Medium)

**What we do.**
- A spot is made only for `SphereLight` with `cone_angle < 90` (`Lighting.cpp:136`).
- The fallback when the attribute is absent is 180 (`prims.cpp:797`). With ShapingAPI applied, Hydra returns the schema default of 90, which is not below 90, so the light becomes a full-sphere point light.
- Rect, disk and cylinder lights ignore the cone.
- The inner cone is `angle·(1-softness)` (`Lighting.cpp:177`).
- `shaping:focus` / `focusTint` are never read.

**What the spec says.** `cone:angle` defaults to 90, and emission beyond it "are guaranteed to be zero". Softness is `1 - smoothStep(θ, lerp(softness, cutoff, 0), cutoff)` (`USD: schema.usda:717-812`; https://openusd.org/release/api/class_usd_lux_shaping_a_p_i.html). UE's spot falloff is a squared ramp in cos θ, not a smoothstep in θ (unverified against `SpotLightSceneProxy`). UE's importer uses the same SphereLight-only spot mapping (`USDLightConversion.cpp:253-275`).

**Impact.** A shaped sphere light with the default cone lights the ceiling in hdUE and not in Karma. Rect lights with cones or focus spill.

**Recommendation.**
- Treat `cone_angle <= 90` (not `< 90`) as a spot with an outer angle of 89.9°.
- Map `focus` on rect lights to UE barn doors as an approximation, or warn.
- Warn once when a cone is set on rect, disk or cylinder lights.

## MAT-02 — `sourceColorSpace` does not select sRGB decoding (Medium)

**What we do.** sRGB sampling is decided by the slot: `hdue_texture_is_color(Slot)` (`bridge.h:143-145`) drives `TextureKey(File, SRGB)` and `Created->SRGB` (`Scene.cpp:680,387`). `sourceColorSpace` is read only to skip OCIO (`materials.cpp:134-135,92`).

**What the spec says.** `sourceColorSpace` is raw / sRGB / auto. With auto, the reader checks file metadata, else marks the texture sRGB "when it is 8-bit with 3 or 4 channels" (https://openusd.org/release/spec_usdpreviewsurface.html).

**Impact.**
- A `diffuseColor` texture tagged `raw` (linear-encoded 8-bit albedo, or packed ORM used as colour) is gamma-decoded.
- A roughness PNG tagged `sRGB` is read linear.
Both differ from Karma and Storm.

**Recommendation.** Carry a per-texture `srgb` tri-state (raw / sRGB / auto) through `HdUETextureData`. Use it instead of the slot rule, keeping the slot rule only for `auto` on 8-bit files.

## MAT-03 — Wrap modes ignored (Medium)

**What we do.** `materials.cpp` never reads `wrapS`/`wrapT`. The parents' samplers use UE defaults (wrap) (`HdUEPrepareCommandlet.cpp:159-163`).

**What the spec says.** wrapS/T are black / clamp / repeat / mirror / useMetadata, defaulting to useMetadata, which falls back to black when the file has none (https://openusd.org/release/spec_usdpreviewsurface.html).

**Impact.** Decals and labels authored with clamp or black tile across the surface. This is the most visible PreviewSurface mismatch on product assets.

**Recommendation.** Set `AddressX`/`AddressY` on the transient `UTexture2D` (`Scene.cpp:384-394`). Wrap mode is a texture property in UE, so make it part of `TextureKey`. For black, use clamp plus a UV-range mask in the parent material.

## MAT-04 — Clearcoat is haziness, not a coat (Medium)

**What we do.** `Slab->SecondRoughness = ClearcoatRoughness; SecondRoughnessWeight = Clearcoat` (`HdUEPrepareCommandlet.cpp:276-279`). This is documented in `docs/rendering.md` ("no coat absorption").

**What the spec says.** "clearcoat: Second specular lobe amount. The color is white." The ior applies to the clearcoat when clearcoat > 0 (https://openusd.org/release/spec_usdpreviewsurface.html).

**Impact.** Haziness splits the base lobe's energy between two roughnesses using the base F0. On a red metal, the "clearcoat" is red and adds no reflectance. On a dielectric it adds no energy where Karma adds an F0 ≈ 0.04 white lobe.

**Recommendation.** Use a vertical layer, `SubstrateVerticalLayering(Slab(coat: F0 from ior, roughness = clearcoatRoughness, thickness≈0), base)`, weighted by `clearcoat`. Only when `clearcoat > 0` (static switch / separate parent) to keep closure cost down.

## MAT-05 — Opacity semantics (Medium)

**What we do.** Translucent parents wrap the whole Front in `SubstrateWeight(Front, Opacity)` (`HdUEPrepareCommandlet.cpp:296-299`). Specular and emission are scaled by opacity. `opacityMode` is never read (`materials.cpp:181-199`).

**What the spec says.** Default `opacityMode = transparent`: "opacity reduces the contribution of the diffuse lighting component in favor of transparency". `presence` scales the whole response including specular and emission (https://openusd.org/release/spec_usdpreviewsurface.html).

**Impact.** Glass-like PreviewSurface materials (opacity 0.1) lose their reflections. Fresnel highlights on windows vanish compared with Karma.

**Recommendation.** For `transparent`, weight only the diffuse albedo: Substrate coverage of a diffuse-only slab under a full-coverage specular slab, or `TranslucentColoredTransmittance` with `DiffuseAlbedo *= opacity`. Keep the current behaviour for `presence`.

## COL-01 — Tone-mapped beauty tagged as scene_linear (Medium)

**What we do.** `postprocess_api.h:374-376` states that with `tonemapper=1` the beauty is "UE's tone-mapped image as display-linear Rec.709". `delegate.cpp:1284-1285` and `delegate.cpp:932` still run `HdUEToSceneLinear` on it, and hand it to the host as the scene_linear `color` AOV.

**What the docs say.** OCIO roles: `scene_linear` is scene-referred, and the host's display/view transform is applied on top of it (https://opencolorio.readthedocs.io/en/latest/guides/authoring/rules.html; not fetched, general OCIO convention).

**Impact.** In Solaris with ACES or AgX views, the UE tone curve is followed by a second tone curve: crushed highlights and desaturation. The gallery handles this by special-casing (`render_gallery.py:134-135`), but users cannot.

**Recommendation.** When the tonemapper is on, either:
- emit the image in an OCIO display-referred space (and document that the viewer should use Raw), or
- publish it under a separate AOV name.
At minimum, warn in the viewport HUD.

## COL-02 — Light textures bypass OCIO (Medium)

**What we do.** Material colour textures go through `HdUELinearRec709Texture` (`materials.cpp:92`, `delegate.cpp:432`). Light textures call `textureFile(state, source->texture)` without the `sceneLinear` flag (`delegate.cpp:576`), and `Lighting.cpp:101-106` loads them as-is.

**Impact.** Under an ACEScg scene_linear config, a dome HDRI authored in ACEScg is interpreted as Rec.709. That shifts hue, and the light colour, which is converted (`prims.cpp:784`), no longer matches its texture.

**Recommendation.** Pass `sceneLinear=true` for dome and rect light textures (one argument at `delegate.cpp:576`).

## MAT-06 — Dielectric F0 clamps at ior ≈ 1.79 (Low)

**What we do.** `SpecularLevel = clamp(F0/0.08, 0, 1)` (`Scene.cpp:774-777`), so F0 cannot exceed 0.08.

**What the spec says.** ior defaults to 1.5 and is used "for translucent objects and objects with specular components" (https://openusd.org/release/spec_usdpreviewsurface.html).

**Impact.** Diamond (ior 2.42, F0 0.17) and gems are about 2x under-reflective.

**Recommendation.** Feed F0 directly to the slab (bypass `MetalnessToDiffuseAlbedoF0`'s Specular input), as the specular-workflow branch already does.

## MAT-07 — One UV set and transform per material (Low)

**What we do.** `materials.cpp:94-95` has the `ponytail:` note "one UV transform per material, from the first textured input". `HdUEMaterialUvPrimvar` returns the first `UsdPrimvarReader_float2` (`materials.cpp:315-323`).

**What the spec says.** Every `UsdUVTexture` has its own `st` input, driven by any `UsdPrimvarReader_float2` / `UsdTransform2d` (https://openusd.org/release/spec_usdpreviewsurface.html).

**Impact.** Detail maps tiled differently from base colour, and lightmap-style second UVs, render with the wrong coordinates and only a one-time warning.

**Recommendation.** Leave it until an asset needs it, as the ponytail note says. Add a per-slot `UVRow` pair when one does.

## MAT-08 — Displacement is WPO only (Low)

**What we do.** `HdUEPrepareCommandlet.cpp:196-213` displaces along the vertex normal in the vertex shader at mip 0.

**What the spec says.** "displacement: Displacement in the direction of the normal" (https://openusd.org/release/spec_usdpreviewsurface.html). The spec does not require tessellation, but Karma dices.

**Impact.** Displacement maps on low-poly meshes have no visible effect.

**Recommendation.** Nanite tessellation / displacement (UE 5.4+) for Nanite-built meshes. Document the current limit.

## COL-03 — Texture colour spaces are binary (Low)

**What we do.**
- `materials.cpp:134-135` only checks whether the colour-space string contains `"raw"`.
- `color_management.cpp:80` converts only `.exr`/`.hdr`, and always from scene_linear.
- 8-bit sRGB files are assumed to have Rec.709 primaries.

**Impact.** A MaterialX image tagged `lin_rec709` inside an ACES config is converted twice. One tagged `acescg` is fine only by accident. A `srgb_texture` EXR is treated as linear.

**Recommendation.** Resolve the authored colour-space name through OCIO (`config->getColorSpace(name)`) and convert from that space, not from the scene_linear role.

## LGT-08 — `ies:angleScale` unsupported (Low)

`Lighting.cpp:210-211` logs a warning. The spec gives a closed-form remap of θ (`USD: schema.usda:821-897`). Because the IES texture is built locally from `FIESConverter`, θ can be resampled while copying `GetRawData()` (`Lighting.cpp:230-237`). The texture's x axis is the vertical angle 0..180° (`IESLoader.cpp:391-396`).

## LGT-09 — Filters, portals, ShadowAPI extras (Low)

`rel light:filters` (`schema.usda:424`), PortalLight, and ShadowAPI `shadow:color` / `distance` / `falloff` are not read (`prims.cpp:778-846`). Only `shadow:enable` is honoured (`prims.cpp:791`). UE has light functions (filters) and no portal concept. The README already lists portals as missing.

---

## What is already right

- **Rect/disk luminance.** UE renders a rect light at 2x its nits: `RectLightComponent.cpp:189-192` multiplies by area, and `RectLightSceneProxy.cpp:50` divides by `0.5 * SourceWidth * SourceHeight`. hdUE's `Intensity*=.5f` (`Lighting.cpp:155-158`) is therefore exactly correct, and matches the Path Tracer's 1.00. The code comment says it was "measured 2.05x"; cite the proxy line instead. UE's own USD importer passes nits straight through (`USDLightConversion.cpp:188-193`), so imported UE rect lights render twice as bright as UsdLux.
- **Sphere luminance.** hdUE uses candela = L·πr² (`Lighting.cpp:159-162`), the correct intensity of a Lambertian sphere seen from outside. UE's importer uses Nits, which UE multiplies by the full capsule area 4πr² (`PointLightComponent.cpp:137-142`, `USDLightConversion.cpp:244-248`), making imported spheres 4x too bright.
- **Exposure and normalize.** `intensity·2^exposure` (`prims.cpp:785`). `normalize` divides by world-space area for area lights (`Lighting.cpp:149-154`) and makes intensity mean illuminance for distant lights (`Lighting.cpp:164-170`), exactly the `sizeFactor` rules in `schema.usda:250-300`.
- **Colour temperature.** `UsdLuxBlackbodyTemperatureAsRgb` is applied in Rec.709, after the authored colour is converted to Rec.709 (`prims.cpp:784-789`).
- **Dome latlong mapping.** It matches the OpenEXR convention in the spec: +Z at the image centre, +X at u = 0.25, +Y at the top (`Lighting.cpp:36-38` vs `domeLight_1.h:53-67`). The cube has HDR headroom without clamping (`Lighting.cpp:273-279`).
- **Exposure pipeline.** Manual exposure, no physical-camera exposure (`delegate.cpp:1383-1385`), and camera exposure through Hydra's `GetLinearExposureScale`, so ISO, shutter and f-stop are covered. Dim-scene Lumen pre-exposure is re-centred (`Bridge.cpp:604-611`). None of these can explain Lumen differing from the Path Tracer.
- **PreviewSurface.** ior→F0 (`Scene.cpp:774-777`), specular workflow (`HdUEPrepareCommandlet.cpp:266-272`), `opacityThreshold` cutout, `UsdUVTexture` scale/bias/channel folding, and the `UsdTransform2d` order scale→rotate→translate (`materials.cpp:52-61`). sRGB mips are averaged in linear light (`Scene.cpp:214-237`). Emissive is in nits with pre-exposure off.
- **OpenPBR / Standard Surface mapping.** Defaults are per spec (`materials.cpp:210-221`). `emission_luminance` is treated as nits.

## Missing features vs Karma / UE USD importer

| Feature | Karma | UE USD importer | hdUE |
|---|---|---|---|
| IES absolute intensity / normalize / angleScale | yes | partial (UE IES asset) | no (LGT-01/03/08) |
| Light / shadow linking | yes | no | no (LGT-04) |
| Light filters, portals | yes | no | no |
| ShapingAPI cone on rect/disk, focus | yes | no | no (LGT-07) |
| DomeLight_1 `poleAxis` | yes | reads `DomeLight_1` (orientation handling unverified) | no (LGT-06) |
| Dome mirroredBall / angular / cross formats | yes | no | rejected, all lights fail (LGT-05) |
| Dome / light diffuse & specular multipliers | yes | no | lights yes, dome rejected |
| UsdUVTexture wrap / fallback / sourceColorSpace | yes | yes (Interchange) | no (MAT-01/02/03) |
| True clearcoat lobe, `opacityMode` | yes | partial | no (MAT-04/05) |
| Micropolygon displacement | yes | no | WPO only (MAT-08) |
| OCIO per-texture colour spaces | yes | UE OCIO plugin, per texture | scene_linear role only (COL-02/03) |
| Correct rect/sphere unit conversion | n/a | rect 2x, sphere 4x too bright | yes |
