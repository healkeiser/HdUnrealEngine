# Lights

hdUE turns each UsdLux light into a UE light: a rect light, a point or spot light, a directional light, or the sky light for a dome. Intensity, exposure, colour and size keep UsdLux's units.

The light types on the chrome ball and grey box: the RectLight key, a sphere light, a spot and a normalized sun.

<div class="hdue-figure" data-figure="lights-types"></div>

## Using it in Solaris

1. Create a **Light** LOP (or **Distant Light**, **Dome Light**) and pick its **Type**.
2. Set **Intensity** to 1 and tune brightness with **Exposure**.
3. UE-only settings are on the Light LOP's **Unreal Engine** tab (see [Unreal Engine properties](#unreal-engine-properties)).

## Light types

| UsdLux | Houdini Light type | UE light | Notes |
| --- | --- | --- | --- |
| `RectLight` | Rectangle Light | Rect Light | **Texture** (`inputs:texture:file`) emits an image. |
| `DiskLight` | Disk Light | Rect Light with a round mask | An approximation of a disk. |
| `SphereLight` | Sphere Light, Point Light | Point Light | **Radius** is UE's Source Radius; `treatAsPoint` sets it to 0. |
| `SphereLight` + `ShapingAPI` | Sphere Light with **Spotlight** on | Spot Light | Cones up to 90°; see [Spot lights](#spot-lights). |
| `CylinderLight` | Cylinder Light | Point Light with a Source Length | A capsule: an approximation of a cylinder. |
| `DistantLight` | Distant Light | Directional Light | **Angle** is UE's Source Angle. |
| `DomeLight`, `DomeLight_1` | Dome Light | Sky Light | See [Dome lights and HDRIs](#dome-lights-and-hdris). |
| `MeshLightAPI` | Geometry Light | an emissive surface | See [Mesh lights](#mesh-lights). |

Not supported: `GeometryLight`, `PortalLight` and `PluginLight` prims render nothing, with a warning. `VolumeLightAPI` is ignored with a warning; the volume renders without emitting, as in UE's USD importer.

## Intensity and exposure

**Intensity** · `inputs:intensity` · default 1 (DistantLight: 50000)
**Exposure** · `inputs:exposure` · default 0, stops

UsdLux intensity is luminance, in nits, of the light's surface. The light's brightness is **Intensity** × 2^**Exposure**: each stop of exposure doubles it. Keep **Intensity** at 1 and set brightness with **Exposure**.

<div class="hdue-figure" data-figure="lights-exposure"></div>

A DistantLight's intensity is the luminance of its sun disk. Its illuminance is intensity × π·sin²(angle/2): a 0.53° sun at intensity 1 gives 0.000067 lux. Turn on **Normalize Power** to set the sun in lux instead.

## Size and normalize

**Width**, **Height** · `inputs:width`, `inputs:height` · default 1 (RectLight)
**Radius** · `inputs:radius` · default 0.5 (SphereLight, DiskLight, CylinderLight)
**Length** · `inputs:length` · default 1 (CylinderLight)
**Normalize Power** · `inputs:normalize` · default off

A bigger light gives softer shadows and wider highlights. Without **Normalize Power**, a bigger light also emits more: twice the width and height is 4 times the light. With it on, hdUE divides the intensity by the light's area, so size changes softness only.

A RectLight from 0.25 m to 2 m, normalized.

<div class="hdue-figure" data-figure="lights-size"></div>

A 2 × 2 m RectLight with **Normalize Power** off and on. Off, it is 4 times (2 stops) brighter than the 1 m light it replaced.

<div class="hdue-figure" data-figure="lights-normalize"></div>

A light's size also decides how sharp its reflection is on glossy surfaces. On SideFX's Veach planks, four sphere lights of increasing size shine on planks of increasing roughness: small lights give sharp highlights on the smooth planks, large lights wide ones on the rough planks. Realtime and the Path Tracer:

<div class="hdue-figure" data-figure="lights-veach-render-mode"></div>

## Colour and temperature

**Color** · `inputs:color` · default (1, 1, 1)
**Enable Color Temperature** · `inputs:enableColorTemperature` · default off
**Color Temperature** · `inputs:colorTemperature` · default 6500, kelvin

With temperature on, the light's colour is multiplied by the colour of a black body at that temperature. 6500 K is about white; lower is warmer, higher is bluer.

<div class="hdue-figure" data-figure="lights-color-temperature"></div>

## Diffuse and specular

**Diffuse Multiplier** · `inputs:diffuse` · default 1
**Specular Multiplier** · `inputs:specular` · default 1

UE's Diffuse Scale and Specular Scale on the light. Set **Specular Multiplier** to 0 to remove a light's highlights and reflections while keeping its diffuse light. Dome lights ignore both, with a warning: UE's sky light has one environment.

## Shadows

**Enable** (Shadows) · `inputs:shadow:enable` · default on

Off, the light casts no shadows (UE's Cast Shadows). Shadow softness comes from the light's size. The realtime shadow method (Virtual Shadow Maps or ray-traced shadows) is a render setting: see [Shadows](reference/render-settings/lighting.md#unreal-cvar-r-shadow-virtual-enable). Per light, **Cast Ray Traced Shadows**, **Contact Shadow Length** and the shadow biases are on the Unreal Engine tab.

Not supported, each with a warning: **Color** (`shadow:color`: UE shadows are black), **Distance** and **Falloff** (`shadow:distance`, `shadow:falloff`: UE lights have no shadow length limit).

## Spot lights

**Spotlight** · applies `ShapingAPI` to a Sphere Light
**Angle** · `inputs:shaping:cone:angle` · default 90, degrees
**Softness** · `inputs:shaping:cone:softness` · default 0

A Sphere Light with a cone of 90° or less becomes a UE Spot Light, as UE's USD importer makes it. **Angle** is the half-angle of the cone (UE's Outer Cone Angle). UE's spot lights stop at 88°: wider cones up to 90° render at 88°. A sphere light with a wider cone stays a point light.

<div class="hdue-figure" data-figure="lights-spot-cone"></div>

**Softness** sets UE's Inner Cone Angle to Angle × (1 − Softness): 0 is a hard edge, 1 fades from the centre.

<div class="hdue-figure" data-figure="lights-spot-softness"></div>

Cones on rect, disk and cylinder lights are ignored, with a warning: UE spots are point lights. **Focus** and **Focus Tint** are not read.

## IES profiles

**IES** · `inputs:shaping:ies:file`
**Normalize** · `inputs:shaping:ies:normalize` · default off
**Angle Scale** · `inputs:shaping:ies:angleScale` · default 0

An IES file shapes a sphere, spot, rect or disk light, read with UE's IES loader and pointed along the light's −Z, as in Karma. The light is scaled by the profile's candela values, as UsdLux defines it; **Normalize** instead keeps the unshaped light's total power. **Angle Scale** stretches the profile's vertical angles with the UsdLux formula. IES profiles on distant and dome lights are ignored, with a warning.

## Distant lights and the sun

**Angle** · `inputs:angle` · default 0.53, degrees

The angular diameter of the sun disk (UE's Source Angle). Larger angles give softer shadows. With **Normalize Power** on, the illuminance stays the same.

<div class="hdue-figure" data-figure="lights-sun-angle"></div>

A distant light is UE's Atmosphere Sun Light by default, so it lights UE's [Sky Atmosphere](environment/sky.md) when that is on. Distant lights never use MegaLights.

## Dome lights and HDRIs

**Texture** · `inputs:texture:file`
**Format** · `inputs:texture:format` · default `automatic`
**Pole Axis** · `poleAxis` (DomeLight_1) · default `scene`
**Visible to Camera** · `unreal:visibleBackground` · default on (Unreal Engine tab)

The dome becomes UE's sky light: it lights the scene, appears in reflections and draws the background. The texture is an HDR, EXR or ordinary image in latitude-longitude layout (`latlong` or `automatic`). HDR values are read as linear Rec.709; no OCIO conversion is applied. An image that is not 2:1 is stretched over the sphere, with a warning. Several domes add up into one environment.

The dome's transform, colour, intensity, exposure and colour temperature all apply. **Pole Axis** turns the dome alone: `Z` (or `scene` on a Z-up stage) puts the pole on +Z.

**Visible to Camera** off keeps the dome's light and reflections and renders black where camera rays escape, as Karma's dome does by default.

<div class="hdue-figure" data-figure="lights-dome-visible"></div>

The dome is baked into a cube map of [Environment Cube Resolution](reference/render-settings/lighting.md#unreal-domeresolution) (default 128) pixels per face. Higher values keep more of the HDRI's detail and cost more memory and bake time.

Not supported on domes: light linking, light functions, IES, the diffuse and specular multipliers, and portals.

## Mesh lights

A mesh with `MeshLightAPI` (Houdini's **Geometry Light** type) emits **Color** × **Intensity** × 2^**Exposure**, colour temperature included, from its surface, as UE lights a scene from emissive surfaces. **Normalize Power** and `light:materialSyncMode` are ignored. The Path Tracer lights the surroundings from emissive meshes; Lumen lights them much less from small emitters (about 1% of the Path Tracer on a test wall). Use a rect or sphere light for a small emitter that must light its surroundings in realtime.

## Light linking

**Light Linker** LOP · `collection:lightLink`, `collection:shadowLink`

UE has three lighting channels, and hdUE gives each light-link collection one of them. That limits how many links a scene can have:

| Scene | Linked lights that work |
| --- | --- |
| Every light is linked | 3 distinct collections |
| Some lights stay unlinked | 2 distinct collections |

Further linked lights light everything, with a warning. Lights whose collection includes everything keep every channel.

- Links restrict direct light only. In Realtime, Lumen still bounces a linked light's indirect light onto every surface. The Path Tracer honours links exactly.
- Dome lights have no channels: their light link is ignored, with a warning.
- Shadow links: UE has no per-light shadow exclusion. An object stops casting shadows only when it is outside the shadow link of every light that casts shadows. With one light this is exact; with several, an object excluded from only some of them still shadows all of them (warned once).

## Light filters and light functions

UsdLux light filters (`light:filters`) are not supported, with a warning: UsdLux defines no standard filter types. UE's light functions are, as a gobo or cookie:

**Light Function Texture** · `unreal:lightFunction:texture` (Unreal Engine tab)

Filters a local or distant light with an image, in Realtime and in the Path Tracer. [Light Function Scale](reference/light-properties.md#unreal-light-lightfunctionscale) sizes the projection (1024 cm by default). [Light Function Material](reference/light-properties.md#unreal-light-lightfunctionmaterial) takes a cooked UE light function material instead. In Volumetric Fog, UE applies light functions from directional, point and spot lights only.

## MegaLights

**MegaLights** · `unreal:megaLights` · default on (Unreal Engine tab)

In Realtime, local lights are shaded by UE's MegaLights when the render setting [MegaLights](reference/render-settings/rendering-features.md#unreal-postprocess-bmegalights) is on. Off on a light, that light uses UE's regular deferred lighting. Distant lights never use MegaLights; the Path Tracer has its own lighting.

## Unreal Engine properties

The Light LOP's **Unreal Engine** tab sets properties of the UE light component, by UE's names: `unreal:light:<Property>`. They apply after the UsdLux translation, so they can override it. The full list, with UE's defaults and ranges, is in [Light properties](reference/light-properties.md). The ones that change the picture most:

**Volumetric Scattering Intensity** · [`unreal:light:VolumetricScatteringIntensity`](reference/light-properties.md#unreal-light-volumetricscatteringintensity) · default 1

How much the light scatters in Volumetric Fog, as a multiplier of its intensity. It changes the beam, not the lit surfaces.

<div class="hdue-figure" data-figure="lights-volumetric-scattering"></div>

**Cast Volumetric Shadow** · [`unreal:light:bCastVolumetricShadow`](reference/light-properties.md#unreal-light-bcastvolumetricshadow) · default off (distant lights: on)

Lets objects shadow the fog, so the ball cuts a dark shaft out of the beam. It costs GPU time per light.

<div class="hdue-figure" data-figure="lights-volumetric-shadow"></div>

**Indirect Lighting Intensity** · [`unreal:light:IndirectLightingIntensity`](reference/light-properties.md#unreal-light-indirectlightingintensity) · default 1

Scales the light's bounced light in Realtime (Lumen); 0 removes it. Direct light is unchanged. In the Bistro, the lamps' bounce lights the walls and ceiling.

<div class="hdue-figure" data-figure="lights-indirect-intensity"></div>

Rect lights also take UE's barn doors: [Barn Door Angle](reference/light-properties.md#unreal-light-barndoorangle) and [Barn Door Length](reference/light-properties.md#unreal-light-barndoorlength). hdUE shortens them to 0.1 cm by default, since UsdLux rect lights have none. Houdini's own barndoor parameters are not read.

## Recipes

**Sun and sky**:

1. Distant Light: **Angle** 0.53, **Normalize Power** on, **Intensity** 1, **Exposure** 4.
2. Dome Light: an HDRI sky in **Texture**, **Intensity** 1, **Exposure** 0.
3. For UE's physical sky instead of the HDRI, deactivate the dome and turn on **Sky Atmosphere** ([Sky and clouds](environment/sky.md)): the distant light lights it.

**Soft studio key:**

1. Rectangle Light, **Width** and **Height** 2, **Normalize Power** on.
2. **Intensity** 1, **Exposure** 4.
3. **Enable Color Temperature** on, **Color Temperature** 5600 for a daylight key.

**Light shafts in fog** (as in `samples/docs/fog.usda`):

1. Render settings, Fog tab: **Exponential Height Fog** on, **Fog Density** 0.3, **Volumetric Fog** on.
2. Sphere Light, **Radius** 0.05, **Spotlight** on, **Angle** 15, **Softness** 0.1.
3. **Intensity** 1, **Exposure** 12.
4. Unreal Engine tab: **Cast Volumetric Shadow** on, so objects in the beam cast shafts.
5. Raise **Volumetric Scattering Intensity** for a brighter beam without brighter surfaces.

## Troubleshooting

**A DistantLight is almost black.** Its intensity is the luminance of a 0.53° sun disk, not illuminance: intensity 1 gives 0.000067 lux. Turn on **Normalize Power**, so intensity is in lux.

<div class="hdue-figure" data-figure="lights-distant-normalize"></div>

| Symptom | Cause | Fix |
| --- | --- | --- |
| A light gets brighter when you scale it up. | **Normalize Power** is off: emission is per area. | Turn on **Normalize Power**. |
| A spot light lights a whole hemisphere. | The cone is wider than 90°, so it is a point light. | Set **Angle** to 90 or less. |
| A linked light still brightens other objects in Realtime. | Lumen's indirect light ignores links. | Set the light's **Indirect Lighting Intensity** to 0, or render with the Path Tracer. |
| A linked light lights everything, with a warning. | More link collections than UE's three lighting channels. | Merge collections, or link at most 3 (2 with unlinked lights). |
| A small emissive mesh lights its surroundings only in the Path Tracer. | Lumen lights little from small emitters. | Add a rect or sphere light. |
| An edited HDRI file does not update the viewport. | A changed file alone sends no light update. | Touch any dome parameter, or restart the render. |
| The sky shows through gaps in a closed room. | The dome is visible to camera rays that escape. | Turn off **Visible to Camera**. |
| The HDRI looks blurred in the background and reflections. | **Environment Cube Resolution** is 128. | Raise it to 512 or 1024. |
| An unlit scene renders lit. | **Light Unlit Scenes** adds a camera key and fill when the scene has no lights. | Add a light, or turn off [Light Unlit Scenes](reference/render-settings/lighting.md#unreal-autoviewerlight). |

## Unreal Engine and Karma

- Intensity, exposure, normalize and size follow UsdLux. With a fully rough material under a normalized distant light, hdUE and Karma agree to 0.1%; glossy surfaces come out up to 1.4 times brighter in hdUE, from UE's specular model.
- UE's Substrate shades diffuse with energy-preserving Oren-Nayar, where Karma's Preview Surface is Lambertian: surfaces seen at grazing angles render about 0.8 to 0.85 times Karma's.
- On the Cornell box, mean brightness is 1.01 times Karma CPU's with the Path Tracer and 0.98 times with Lumen ([Gallery](gallery.md)).
- The dome is visible to the camera by default in hdUE, not in Karma.
- Disk and cylinder lights are UE approximations (a masked rect, a capsule).
- Compare renderers with materials bound: unbound prims use each renderer's fallback (white in hdUE, as in UE's USD importer; grey in Karma).
