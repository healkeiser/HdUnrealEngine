# Sky and clouds

The sky is UE's Sky Atmosphere: a physical sky lit by your sun. Move the sun and the sky changes with it, from a blue midday sky to an orange sunset. UE's Volumetric Cloud adds a cloud layer on top.

The sun's elevation above the horizon, with **Sky Atmosphere** on and no dome light.

<div class="hdue-figure" data-figure="sky-sun-elevation"></div>

## Using the sky in Solaris

1. Remove or deactivate any DomeLight. A dome's environment takes the place of the sky's lighting.
2. Add a **Distant Light** for the sun. Its rotation sets the sun's direction; its **Atmosphere Sun Light** (Unreal Engine tab) is on by default.
3. On **hdUE Render Settings**, open the **Sky** tab and turn on **Sky Atmosphere**.
4. For clouds, also turn on **Volumetric Cloud** on the same tab.

Without a DomeLight, hdUE adds a Sky Light that captures the sky in real time, as UE's default level has. The sky then lights the scene: Lumen needs it, and the Path Tracer renders the atmosphere through it.

Outside Houdini:

```usda
def RenderSettings "Settings"
{
    bool unreal:skyAtmosphere = 1
    bool unreal:volumetricCloud = 1
}
```

Every property of UE's Sky Atmosphere component is an `unreal:skyAtmosphere:<Property>` attribute, and every Volumetric Cloud property is `unreal:volumetricCloud:<Property>`, with UE's names. UE's atmosphere is measured in kilometres. The [Sky reference](../reference/render-settings/sky.md) lists them all with their defaults.

**As scene prims.** **hdUE Sky Atmosphere** and **hdUE Volumetric Cloud** (TAB menu > hdUE > Visual Effects) create `UnrealSkyAtmosphere` and `UnrealVolumetricCloud` prims with the same properties, as the actors are placed in a UE level. Their controls start at **Do Nothing**. A prim replaces its render settings counterpart; with several prims of a kind, the first visible one by path renders. See the [USD prim reference](../reference/usd-prims.md#unrealskyatmosphere).

The figures use `samples/docs/lights.usda` outdoors: its dome and key light off, the normalized sun (exposure 2) on, the camera level with the horizon. Realtime mode.

## Sky Atmosphere

**Sky Atmosphere** (`unreal:skyAtmosphere`) · Default Off

Places UE's Sky Atmosphere in the scene. Distant lights with **Atmosphere Sun Light** on light it and draw their sun disk in it. See [Sky Atmosphere](../reference/render-settings/sky.md#unreal-skyatmosphere).

## The sun

The sky's colour comes from the sun's elevation: the lower the sun, the longer its light travels through the air and the redder it gets. Rotate the Distant Light to set the time of day.

| Light property (Light LOP > Unreal Engine) | Default | What it does |
| --- | --- | --- |
| **Atmosphere Sun Light** (`unreal:light:bAtmosphereSunLight`) | On | The light lights the atmosphere and clouds and draws a sun disk |
| **Atmosphere Sun Light Index** (`unreal:light:AtmosphereSunLightIndex`) | 0 | Two atmosphere lights: 0 for the sun, 1 for a moon or second sun |
| **Atmosphere Sun Disk Color Scale** (`unreal:light:AtmosphereSunDiskColorScale`) | 1, 1, 1 | Multiplies the sun disk's colour |
| **Cast Cloud Shadows** (`unreal:light:bCastCloudShadows`) | Off | Clouds shadow the atmosphere and the ground |

The sun disk's size is the Distant Light's **Angle** (`inputs:angle`; 0.53° is the real sun). See the [light properties reference](../reference/light-properties.md#unreal-light-batmospheresunlight).

## Mie Scattering Scale

**Mie Scattering Scale** (`unreal:skyAtmosphere:MieScatteringScale`) · Default 0.003996 · Range 0 to 5

Mie scattering comes from larger particles: dust, pollen, pollution. More of it makes the sky hazy and whiter and the halo around the sun larger. See [Mie Scattering Scale](../reference/render-settings/sky.md#unreal-skyatmosphere-miescatteringscale).

<div class="hdue-figure" data-figure="sky-mie-scattering"></div>

**Mie Anisotropy** (`unreal:skyAtmosphere:MieAnisotropy`, default 0.8) sets how forward that light scatters: higher values concentrate the halo around the sun.

## Rayleigh Scattering Scale

**Rayleigh Scattering Scale** (`unreal:skyAtmosphere:RayleighScatteringScale`) · Default 0.0331 · Range 0 to 2

Rayleigh scattering comes from air molecules. It scatters blue more than red: it makes the sky blue by day and sunsets red. Raise it for a deeper blue sky; lower it for a paler one. See [Rayleigh Scattering Scale](../reference/render-settings/sky.md#unreal-skyatmosphere-rayleighscatteringscale).

## Volumetric Cloud

**Volumetric Cloud** (`unreal:volumetricCloud`) · Default Off

A layer of ray-marched clouds, lit by the sun and the sky. It needs **Sky Atmosphere** on. See [Volumetric Cloud](../reference/render-settings/sky.md#unreal-volumetriccloud).

<div class="hdue-figure" data-figure="sky-clouds"></div>

| Parameter | Default | What it does |
| --- | --- | --- |
| **Layer Bottom Altitude** (`unreal:volumetricCloud:LayerBottomAltitude`) | 5 km | Height of the cloud base above the ground |
| **Layer Height** (`unreal:volumetricCloud:LayerHeight`) | 10 km | Thickness of the cloud layer |
| **Material** (`unreal:volumetricCloud:Material`) | UE's `m_SimpleVolumetricCloud_Inst` | The cloud material, as the path of a cooked material |
| **View Sample Count Scale** (`unreal:volumetricCloud:ViewSampleCountScale`) | 1 | Ray-marching samples: higher is smoother and slower (Realtime only) |

The cloud shapes come from the material. For other shapes or coverage, cook your own cloud material in UE and set **Material** to its path.

## Path Tracer

The Path Tracer renders the atmosphere through the Sky Light hdUE adds. For clouds, and for the atmosphere traced directly, turn on **Reference Atmosphere** (`unreal:postProcess:PathTracingEnableReferenceAtmosphere`, Rendering tab, default Off). With it on, UE ignores any Sky Light. See [Reference Atmosphere](../reference/render-settings/rendering.md#unreal-postprocess-pathtracingenablereferenceatmosphere).

Several art-direction properties render in Realtime only: **Sky Luminance Factor**, **MultiScattering** and the aerial perspective settings. The reference marks each one.

## Recipes

**How to get a sunset**

1. **Sky Atmosphere**: on. No DomeLight.
2. Distant Light: rotate X to -3 (the sun 3° above the horizon) and Y so the sun sits in front of the camera.
3. Distant Light **Angle**: 0.53, **Normalize** on.
4. **Mie Scattering Scale**: 0.01, for a warmer haze around the sun.
5. Optional: **Volumetric Cloud** on, and **Cast Cloud Shadows** on the sun.

**How to get an overcast day**

1. **Sky Atmosphere** and **Volumetric Cloud**: on. No DomeLight.
2. Distant Light: rotate X to -50 (sun high), **Cast Cloud Shadows** on.
3. **Mie Scattering Scale**: 0.05, for a white, hazy sky.
4. **Layer Bottom Altitude**: 1 (km), for a low cloud base.
5. The coverage is the cloud material's. For a closed cloud deck, set **Material** to a cooked cloud material with full coverage.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The sky is black | Sky Atmosphere is on but no Distant Light has **Atmosphere Sun Light** on | Add a Distant Light, or turn its Atmosphere Sun Light on |
| The sky shows but the scene keeps its old lighting | A DomeLight is present: its environment lights the scene | Deactivate the DomeLight |
| No clouds | Sky Atmosphere is off, or you render with the Path Tracer without Reference Atmosphere | Turn on Sky Atmosphere; in the Path Tracer, turn on **Reference Atmosphere** |
| Render settings sky changes do nothing | An `UnrealSkyAtmosphere` or `UnrealVolumetricCloud` prim takes their place | Edit the prim, or remove it |

## Unreal Engine and Karma

hdUE places UE's Sky Atmosphere and Volumetric Cloud components with UE's properties and defaults, and lights them with distant lights as UE's directional lights do. Karma's sky is a separate Karma Sky Atmosphere LOP with its own parameters: values do not carry over.
