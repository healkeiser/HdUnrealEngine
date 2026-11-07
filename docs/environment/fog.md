# Fog

Fog fills the scene with UE's Exponential Height Fog: a haze that is densest at the ground and thins with height. Turn on **Volumetric Fog** and lights scatter into it, so beams and shadows show in the air.

A spot light through the fog of `samples/docs/fog.usda`, without and with Volumetric Fog.

<div class="hdue-figure" data-figure="fog-volumetric"></div>

## Using fog in Solaris

There are three ways to add fog. Use one height fog per scene: UE draws only one.

| Workflow | Where | Use it when |
| --- | --- | --- |
| Render settings | **Fog** tab of **hdUE Render Settings** | the fog belongs to a render, not to the scene |
| Height fog prim | **hdUE Height Fog** LOP | the fog travels with the scene, as a fog actor in a UE level |
| Local fog volume | **hdUE Local Fog Volume** LOP | you need a patch of fog in one place: a valley, a doorway |

**From the render settings:**

1. On **hdUE Render Settings**, open the **Fog** tab.
2. Turn on **Exponential Height Fog**.
3. Set **Fog Density**, then turn on **Volumetric Fog** for lit fog.

**From a prim:** add **hdUE Height Fog** (TAB menu > hdUE > Visual Effects). It creates an `UnrealExponentialHeightFog` prim with the same properties as the Fog tab. Its controls start at **Do Nothing**: set each one you want to author. The prim's height in the world is the fog's height, so move it with its transform. A height fog prim replaces the render settings' fog, whose switch and values are then ignored. With several prims, the first visible one by path renders.

Outside Houdini, author the attributes on the render settings prim:

```usda
def RenderSettings "Settings"
{
    bool unreal:heightFog = 1
    float unreal:heightFog:FogDensity = 0.3
    bool unreal:heightFog:bEnableVolumetricFog = 1
}
```

Every property of UE's Exponential Height Fog component is an `unreal:heightFog:<Property>` attribute, with UE's name. Distances are in centimetres, as in UE's Details panel. The [Fog reference](../reference/render-settings/fog.md) lists them all with their defaults. This page covers the ones you change most.

All figures below use `samples/docs/fog.usda`: an 8 m closed room, a spot light at exposure 12 with a 15° cone, a sphere in the beam, Fog Density 0.3 and Volumetric Fog on. Realtime mode.

## Exponential Height Fog

**Exponential Height Fog** (`unreal:heightFog`) · Default Off

Places the fog in the scene, as an Exponential Height Fog actor does in a UE level. See [Exponential Height Fog](../reference/render-settings/fog.md#unreal-heightfog).

## Fog Height

**Fog Height** (`unreal:heightFogHeight`) · Default 0

The height of the fog actor, in scene units along the stage's up axis. The density is **Fog Density** at this height, grows below it and falls above it. A height fog prim uses its own position instead. See [Fog Height](../reference/render-settings/fog.md#unreal-heightfogheight).

## Fog Density

**Fog Density** (`unreal:heightFog:FogDensity`) · Default 0.02

How thick the fog is at the fog height. UE's slider goes to 0.05, which suits fog seen over hundreds of metres. A room needs more: the sample uses 0.3. See [Fog Density](../reference/render-settings/fog.md#unreal-heightfog-fogdensity).

<div class="hdue-figure" data-figure="fog-density"></div>

## Fog Height Falloff

**Fog Height Falloff** (`unreal:heightFog:FogHeightFalloff`) · Default 0.2 · Range 0.001 to 2

How fast the density drops with height. Higher values keep the fog near the ground; lower values spread it over a taller band. Over a room's 4 m, only values near 2 show a gradient. See [Fog Height Falloff](../reference/render-settings/fog.md#unreal-heightfog-fogheightfalloff).

<div class="hdue-figure" data-figure="fog-height-falloff"></div>

A second fog layer, with its own density, falloff and height offset, is **Second Fog Density**, **Second Fog Height Falloff** and **Second Fog Height Offset** (`unreal:heightFog:SecondFogData:*`). Density 0, the default, turns it off.

## Fog Inscattering Color

**Fog Inscattering Color** (`unreal:heightFog:FogInscatteringLuminance`) · Default 0, 0, 0 · Realtime only

The light the fog itself adds, the same in every direction, as a luminance. With the default black, the fog only darkens what is behind it, unless Volumetric Fog lights it. Use it for distant haze that matches a sky. Within Volumetric Fog's **View Distance**, Volumetric Fog replaces it, as in UE. See [Fog Inscattering Color](../reference/render-settings/fog.md#unreal-heightfog-foginscatteringluminance).

Volumetric Fog off, so the inscattering colour shows on its own:

<div class="hdue-figure" data-figure="fog-inscattering-color"></div>

## Volumetric Fog

**Volumetric Fog** (`unreal:heightFog:bEnableVolumetricFog`) · Default Off

Lights the fog with the scene's lights and shadows: beams, light shafts and halos. Each light scatters into the fog by its **Volumetric Scattering Intensity** (`unreal:light:VolumetricScatteringIntensity`, default 1, on the Light LOP's Unreal Engine tab). Volumetric Fog ignores **Start Distance**, **Fog Max Opacity** and **Fog Cutoff Distance**. See [Volumetric Fog](../reference/render-settings/fog.md#unreal-heightfog-benablevolumetricfog).

!!! note
    The Path Tracer renders the height fog only with Volumetric Fog on. Without it, a Path Tracer render shows no fog at all.

## Scattering Distribution

**Scattering Distribution** (`unreal:heightFog:VolumetricFogScatteringDistribution`) · Default 0.2 · Range -0.9 to 0.9

The phase function: where the fog sends the light it scatters. 0 scatters equally in all directions. Values toward 0.9 scatter forward, along the light's direction: a beam seen from the side fades, and a light facing the camera gets a bright halo. Keep it near 0 for beams seen from the side. See [Scattering Distribution](../reference/render-settings/fog.md#unreal-heightfog-volumetricfogscatteringdistribution).

<div class="hdue-figure" data-figure="fog-scattering-distribution"></div>

## Albedo

**Albedo** (`unreal:heightFog:VolumetricFogAlbedo`) · Default 1, 1, 1

The fog particles' reflectance. Water droplets are near white; dust is darker. A colour tints the scattered light. See [Albedo](../reference/render-settings/fog.md#unreal-heightfog-volumetricfogalbedo).

## Extinction Scale

**Extinction Scale** (`unreal:heightFog:VolumetricFogExtinctionScale`) · Default 1 · Range 0.1 to 10

Scales how much light the fog particles absorb, without changing the density. Above 1, the fog absorbs more light everywhere. See [Extinction Scale](../reference/render-settings/fog.md#unreal-heightfog-volumetricfogextinctionscale).

<div class="hdue-figure" data-figure="fog-extinction-scale"></div>

## View Distance

**View Distance** (`unreal:heightFog:VolumetricFogDistance`) · Default 6000 (60 m) · Range 1000 to 10000

How far from the camera Volumetric Fog is computed. Beyond it, the plain height fog takes over. Volumetric Fog has a fixed number of depth slices over this distance, so larger values reach further but sample each metre more coarsely. See [View Distance](../reference/render-settings/fog.md#unreal-heightfog-volumetricfogdistance).

## Shadows in the fog

**Cast Volumetric Shadow** (`unreal:light:bCastVolumetricShadow`, Light LOP > Unreal Engine tab) · Default Off (On for distant lights)

Lets the light's shadows cut through the fog. Without it, the sphere in the beam casts no shadow in the air. Turning it on costs GPU time per light. See [Cast Volumetric Shadow](../reference/light-properties.md#unreal-light-bcastvolumetricshadow).

<div class="hdue-figure" data-figure="fog-volumetric-shadow"></div>

## Local fog volumes

A Local Fog Volume is a sphere of fog that fades out toward its edge, combined with a height fog along its up axis. Use it for fog in one place: a valley, a pool of mist, smoke in a room.

1. Add **hdUE Local Fog Volume** (TAB menu > hdUE > Visual Effects). It creates an `UnrealLocalFogVolume` prim.
2. Place it with the transform section. **Uniform Scale** is the radius: at 1, the volume is UE's base sphere of 500 cm radius.
3. Set **Radial Fog Density** and **Height Fog Density** (both 1 by default).

In **Create** mode the LOP authors every property, at UE's defaults too. **Edit** mode changes existing prims. The properties are listed in the [USD prim reference](../reference/usd-prims.md#unreallocalfogvolume).

Local fog volumes follow UE's rules:

- They render in Realtime only. UE's Path Tracer ignores them.
- UE draws none within 10 m of the camera (`r.LocalFogVolume.GlobalStartDistance`, which you can set as `unreal:cvar:r:LocalFogVolume:GlobalStartDistance`). A volume's own **Start Distance** can only push it further.
- **Height Fog Density** 0 hides the whole volume: UE multiplies the radial and height coverages. For radial fog alone, use a small height density such as 0.01.
- With Volumetric Fog on the height fog, local volumes are lit and shadowed through it too.

## Breaking up the fog

Height fog and Local Fog Volumes are smooth: UE gives neither a noise or texture input. In UE, uneven fog comes from fog particles: particles with a Volume-domain material add their own density to Volumetric Fog, and a noise in that material breaks it into wisps and gaps ([Volumetric Fog in UE 5.8](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine), "Local controls"). hdUE does the same with USD points: give a Points prim the primvars below, and each point becomes a sphere of fog, as wide as the point, in Volumetric Fog.

| Primvar on the Points prim (constant) | Type | Default | What it does |
| --- | --- | --- | --- |
| `primvars:unreal:fog:extinction` | float | 0 | Density at a sphere's centre, per metre. Above 0, the points render as fog instead of spheres |
| `primvars:unreal:fog:noiseStrength` | float | 0 | How much the noise breaks up the density: 0 is smooth, 1 leaves clear gaps and denser pockets |
| `primvars:unreal:fog:noiseSize` | float | 1 m | Size of the noise's pockets, in scene units |
| `primvars:unreal:fog:noiseOffset` | float3 | (0, 0, 0) | Moves the noise, in scene units: animate it for drifting fog |
| `primvars:unreal:fog:edgeSoftness` | float | 1 | How a sphere fades from its centre to its edge: higher fades sooner, for softer edges |
| `primvars:unreal:fog:albedo` | color3f | (1, 1, 1) | Fraction of the light the fog scatters |
| `primvars:unreal:fog:emissive` | color3f | (0, 0, 0) | Light the fog gives off, per unit of density |

1. Turn on **Exponential Height Fog** and **Volumetric Fog**: fog particles add to the volumetric fog, and nothing shows without it. A low **Fog Density** (0.02 to 0.05) leaves the particles to shape the fog.
2. Scatter points where the fog should be (a **Scatter** SOP on the ground, for example) and give them a size with `pscale`. A **SOP Import** turns them into a Points prim and converts `pscale` into USD `widths`; hdUE reads `widths` as each sphere's diameter, so check it in the Scene Graph Details pane.
3. Add the primvars with an **Edit Properties** LOP (or a wrangle) on the Points prim: at least `primvars:unreal:fog:extinction`, and `primvars:unreal:fog:noiseStrength` for uneven fog.

The fog room of `samples/docs/fog.usda`: 26 fog particles along the spot light's beam and where it lands, over a thin height fog (Realtime):

<div class="hdue-figure" data-figure="fog-particles"></div>

How it maps to UE:

- Each point is a UE primitive with hdUE's `M_HdUEFogParticle` material: Volume domain, density fading from the point to its radius, times 3D gradient noise in world space, as UE's Volumetric Fog page builds its fog particles.
- UE documents Volume-domain materials for particles. hdUE gives each point its own static mesh component instead, which UE voxelizes the same way: as a camera-facing quad over the primitive's bounding sphere (`VolumetricFogVoxelization.cpp`). One instanced mesh for all the points would give one sphere around them all.
- Like the rest of Volumetric Fog, fog particles render in Realtime only, and Volumetric Fog's **View Distance** limits how far away they show.
- A point's own material and `displayColor` are ignored while it is fog.

## Recipes

**How to get light shafts in a dark room**

1. **Exponential Height Fog**: on.
2. **Fog Density**: 0.3 (an 8 m room).
3. **Volumetric Fog**: on.
4. **Scattering Distribution**: 0.2.
5. Light: a spot (SphereLight with Shaping), cone 15°, exposure 12.
6. Light > Unreal Engine > **Cast Volumetric Shadow**: on, so objects in the beam cut it.

**How to get a low ground mist**

1. **Exponential Height Fog**: on, **Fog Height** at the ground.
2. **Fog Density**: 0.3.
3. **Fog Height Falloff**: 2.
4. **Volumetric Fog**: on, so the mist takes the scene's light.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| No fog in a Path Tracer render | The Path Tracer renders the height fog only through Volumetric Fog | Turn on **Volumetric Fog** |
| Fog is invisible in a room | **Fog Density** 0.02 is made for distances of hundreds of metres | Raise it to 0.1 to 1 |
| Fog is black, or only darkens the image | Volumetric Fog off and **Fog Inscattering Color** black | Turn on **Volumetric Fog**, or set an inscattering colour |
| Objects in a beam cast no shadow in the fog | **Cast Volumetric Shadow** is off on local lights | Turn it on on the light |
| Beam invisible from the side | **Scattering Distribution** near 0.9 sends the light forward | Lower it toward 0 |
| Render settings fog changes do nothing | A height fog prim in the scene takes its place | Edit the prim, or remove it |
| A Local Fog Volume does not show | It is within 10 m of the camera, its Height Fog Density is 0, or you render with the Path Tracer | Move the camera back or lower `r.LocalFogVolume.GlobalStartDistance`; set a small Height Fog Density; use Realtime |

## Unreal Engine and Karma

hdUE places UE's own Exponential Height Fog and Local Fog Volume components, with UE's properties and defaults; the Fog tab is generated from UE's source. UE's USD importer imports no fog, so there is no USD schema to follow. Karma has no height fog: in Karma you build fog as a volume.
