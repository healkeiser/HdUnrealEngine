# Volumes

A Volume prim, such as a VDB of smoke or a cloud, renders as a UE Heterogeneous Volume: fog with its own density field, lit and shadowed by the scene's lights. It renders in both Realtime and the Path Tracer.

The fog sphere of `samples/cornell/variants/volume.usda` in both render modes.

<div class="hdue-figure" data-figure="volumes-render-mode"></div>

## Using volumes in Solaris

1. Bring the volume in with a **Volume** LOP (a `.vdb` file) or a **SOP Import** of a SOP volume.
2. Make sure the Volume prim has a `density` field. hdUE renders that field, or the first field when none is named `density`.
3. Render. The volume needs no material: hdUE's default volume material has an albedo of 0.8, and the density is the extinction per scene unit.

hdUE reads the field from:

- an `OpenVDBAsset` that points to a `.vdb` file;
- a SOP volume in the Solaris viewport, not yet saved to disk (an `op:` path on an `OpenVDBAsset` or `HoudiniFieldAsset`);
- a `.bgeo` or `.bgeo.sc` file, which is where the USD Render ROP writes Houdini volumes for husk.

In a Houdini file, the VDB or Houdini volume primitive named by the field's `fieldName` is used, else the one at `fieldIndex`.

Outside Houdini:

```usda
def Volume "fog"
{
    rel field:density = </fog/density>
    def OpenVDBAsset "density"
    {
        asset filePath = @fog.vdb@
        token fieldName = "density"
    }
}
```

## Breaking up a volume with noise

Real mist, smoke and haze are never uniform. A box or sphere of constant density looks like a solid object: it has hard edges and the same brightness everywhere. Shape the density field itself with noise, in SOPs, before it reaches Solaris. The noise then renders the same in Realtime, the Path Tracer and Karma, because hdUE renders whatever field it is given.

1. **Volume** SOP: name `density`, the size of the region, an initial value of the density you want at its thickest (3 here: extinction per metre).
2. **Volume Noise Fog** SOP: **Operation** Multiply, **Noise Type** Sparse Convolution, **Element Size** about the size of the pockets you want (0.6 m here). Set the range so part of it falls below 0 (Min -1.2, Max 1.8 here) and clamp at 0: those parts become clear air, which breaks up the volume's outline.
3. **Convert VDB** (VDB, fog class), then a **SOP Import** LOP, or write the VDB and load it with a **Volume** LOP.

The 4 x 3 m slab of `samples/docs/mist.usda`, lit by a low sun (Realtime). `scripts/build_docs_volumes.py` builds both volumes with these nodes:

<div class="hdue-figure" data-figure="volumes-noise"></div>

For cloud shapes, the **Cloud Noise** SOP and its billowy and wispy variants do the same job on a cloud's density.

!!! note "Unreal Engine's own fog has no noise input"
    UE's Exponential Height Fog and Local Fog Volumes are smooth by design: neither has a noise or texture input. In UE you break up fog with a heterogeneous volume (a VDB, as here) or with particles that use a Volume-domain material, which add density to Volumetric Fog. See [Breaking up the fog](fog.md#breaking-up-the-fog).

## Render modes

| | Realtime | Path Tracer |
| --- | --- | --- |
| Volume renders | yes | yes |
| Shadows on the scene | from distant lights, and from local lights with MegaLights off; the light needs **Cast Volumetric Shadow** | yes, from every light, with multiple scattering |
| Emission | yes | yes |

In Realtime, shadows are UE's heterogeneous volume shadow casting. UE's MegaLights does not support heterogeneous volumes, so a local light shaded by MegaLights casts no volume shadow: turn off **MegaLights** (`unreal:megaLights`, Light LOP > Unreal Engine tab) on that light. **Cast Volumetric Shadow** (`unreal:light:bCastVolumetricShadow`, Light LOP > Unreal Engine tab) is on by default for distant lights and off for the others. See [Cast Volumetric Shadow](../reference/light-properties.md#unreal-light-bcastvolumetricshadow).

The Path Tracer re-voxelizes the volumes for each frame. Batch renders (husk) use the shading rate and memory UE's documentation recommends for cinematic renders, so silhouettes stay sharp.

## Emission

A volume emits light from a second field, such as `temperature` or `flame`, when you name it. The emitted colour is UE's BlackBody of the field's value, in kelvin.

| Primvar on the Volume prim | Default | What it does |
| --- | --- | --- |
| `primvars:unreal:emissionField` | none | The field that emits. Without it nothing emits |
| `primvars:unreal:temperatureScale` | 1 | Kelvin per unit of the field's value |
| `primvars:unreal:emissionIntensity` | 1 | Multiplies the emitted light |

These are constant primvars: in Houdini, detail attributes on the volume become them. The emission field is sampled at the density field's voxels; outside its own grid it reads 0.

The Cornell fog sphere emitting from its own density field, at a Temperature Scale of 2500 (Path Tracer):

<div class="hdue-figure" data-figure="volumes-emission"></div>

As in UE, nothing emits unless you ask: UE's heterogeneous volumes emit what their material makes of a grid, and prescribe no field.

## Materials

Without a material, volumes render with hdUE's default volume material. In Houdini's viewport, a bound Substrate Builder volume material replaces it; see [Substrate Builder](../substrate-builder.md). Other volume materials are not translated.

## Sparse Volume Textures

A VDB imported into a UE project as a Sparse Volume Texture renders through the **Native Asset** field (Render Geometry Settings LOP > Unreal Engine tab), as a UE Heterogeneous Volume, in both modes. UE reads the VDB's units as centimetres, so a VDB written in metres needs a scale of 100 on the prim. For an animated sequence, **Volume Frame** (`primvars:unreal:volume:frame`) picks the frame, from 0 for the first file: set it to `$F - 1` for a sequence starting at frame 1. See [cooked native assets](../materials.md).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The volume does not render | The Volume prim has no field hdUE reads, or its file is missing | Name the density field `density`; check the file path |
| No volume shadow in Realtime | The light is a local light shaded by MegaLights, or its Cast Volumetric Shadow is off | Turn off MegaLights on the light, turn on Cast Volumetric Shadow, or use a distant light |
| Volume looks blocky | Grids larger than 512 voxels on an axis are point-sampled with a stride | Render with the Path Tracer for detail, or resample the VDB to 512 voxels per axis |
| A tapered (frustum) VDB renders wrong | Frustum VDBs and tapered Houdini volumes are not translated | Resample to a regular grid |
| The volume does not glow | No `primvars:unreal:emissionField` | Name the emitting field, then tune Temperature Scale |

## Unreal Engine and Karma

hdUE renders volumes with UE's Heterogeneous Volumes, as UE does for a VDB placed in a level. Compared with Karma on the Cornell fog sphere, a Realtime fog's brightness is within about 15%. The Path Tracer's shadow is lighter than Karma's: an optical depth of 1.4 against Karma's 1.9.
