# Rendering reference

## Render settings

Renderer settings are `hdUE:*` attributes on the UsdRenderSettings prim (the **Unreal Engine** tab of Render Settings LOPs in Houdini). String settings accept `string` or `token`, numbers accept any numeric type.

| Setting | Default | Meaning |
| --- | --- | --- |
| `hdUE:renderMode` | `realtime` | `realtime` (Lumen) or `pathtraced` |
| `hdUE:samplesPerPixel` | 64 | Path Tracer samples |
| `hdUE:lumenEnabled` | 1 | Lumen GI and reflections in realtime mode |
| `hdUE:warmupFrames` | 16 | Realtime frames before a frame counts as converged |
| `hdUE:motionBlur` | 1 | Motion blur when the camera shutter is open and something moves |
| `hdUE:motionSamples` | 16 | Motion-blur sub-frames; Path Tracer samples are divided between them |
| `hdUE:tonemapper` | 0 | Run UE's tonemapper and the effects inside it; see [post-processing](#unreal-post-processing) |
| `hdUE:postProcess:<Property>` | | Any `FPostProcessSettings` property, for the beauty |
| `hdUE:postProcessMaterials` | | Cooked post-process materials added to the beauty |
| `hdUE:domeResolution` | 512 | Environment cubemap face size |
| `hdUE:autoViewerLight`, `hdUE:viewerLightIntensity` | | Review light for scenes without lights |
| `hdUE:upAxis` | stage | Up axis override |

## Geometry

Generic Hydra translation supports meshes, cameras, materials, and distant, sphere, rect, disk, cylinder, and dome lights.

Material-binding GeomSubsets partition triangles by their original USD face while preserving UVs, normals, transforms, visibility, and picking paths. Unassigned faces retain the mesh material; overlapping or invalid material subsets are rejected. Subset edits and removals update the generated meshes.

Missing or zero normals are generated from area-weighted vertex adjacency before subset partitioning, so material boundaries share the same shading normals. Authored normals are retained. Author explicit face-varying normals or split vertices where hard edges are required; normal generation does not subdivide geometry or apply subdivision creases.

Hydra instancers (USD PointInstancers, including nested ones) are translated: each prototype mesh is built once as a runtime static mesh, or uses its native cooked asset, and drawn through one instanced static mesh component with per-instance transforms composed as in hdEmbree. Preview Surface and ID materials are cooked with instanced-static-mesh usage; prepared MaterialX or Substrate materials need that usage too before they render on instances. The Solaris settings subnet still expands instanceable references in its own layer for editable mesh/material translation.

Implicit Cube, Sphere, Cone, Cylinder, Capsule and Plane prims are converted to meshes by Hydra's implicit-surface scene index (`HdUE_ImplicitSurfaceSceneIndexPlugin`), as for hdEmbree.

Points render as one shared sphere instanced per point, sized by `widths` (0.1 when unauthored). BasisCurves render as six-sided tubes with flat end caps on open curves: linear curves follow their vertices, cubic Bezier, B-spline and Catmull-Rom curves are evaluated in their basis, and `widths` set the diameter (0.01 when unauthored). Points and curves under a PointInstancer are drawn once per instance.

Unbound geometry uses `displayColor`/`displayOpacity`. A `displayColor` that varies (uniform, vertex or face-varying on meshes, per point on Points, per vertex or per curve on BasisCurves, per instance on a PointInstancer) is multiplied into `M_PreviewSurface` as vertex colors or per-instance custom data. Dynamic mesh vertex colors are stored in 8 bits, so very dark colors band slightly. Varying `displayOpacity`, and colors on meshes that also carry a wet-surface mask (the mask reuses the vertex colors), are not translated. The unbound default is a Preview Surface (roughness 0.5); Karma's default for unbound geometry is darker, so compare renderers with bound materials.

## Cameras

Cameras use Hydra's projection as is, so the conformed window, aperture offsets (lens shift) and orthographic projection match Karma; orthographic depth spans the camera's clipping range.

`fStop` and `focusDistance` drive UE's physical depth of field. Lens attributes follow USD's convention of tenths of a scene unit, as Hydra and Karma read them (a `focalLength` of 35 in a metres stage is a 3.5 m lens). The Path Tracer ray-traces the thin lens (reference DOF); realtime uses UE's post-process DOF, which approximates it and clamps very large blur.

The camera's `exposure` (Hydra's linear exposure scale) scales the beauty, as Karma does, and leaves AOVs untouched. Split diopters and lens distortion are not translated.

## Motion blur

With the render camera's `shutter:open` before `shutter:close` and something moving, hdUE averages `hdUE:motionSamples` sub-frames spread over the shutter. Moving means:

- transform samples on meshes, curves, points and their ancestors;
- PointInstancer transform samples, and per-instance `velocities` on top-level instancers;
- per-point `velocities` on Points;
- deforming meshes: time-sampled `points` with a constant point count, else per-point `velocities` (Houdini's `v`);
- camera transform samples.

`velocities` are in units per second, converted with `houdini:fps` when husk provides it, else `hdUE:framesPerSecond` (24). Matrices are interpolated linearly between samples, so fast rotations shorten slightly within a frame. Deforming meshes are uploaded again for each sub-frame, which costs time on dense meshes. `angularVelocities`, `accelerations` and velocities on nested instancers are not blurred. Lumen keeps its history between sub-frames, so the realtime warmup and the Path Tracer samples are divided between sub-frames and the total stays about the same. Too few sub-frames show discrete copies of fast objects.

## Unreal post-processing

By default the beauty is linear scene-referred HDR, like Karma's, so colour management and compositing work as usual. UE's post-processing can be applied on top:

- `hdUE:postProcess:<Property>` sets any [`FPostProcessSettings`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/FPostProcessSettings) property by name, and its override flag: `float hdUE:postProcess:BloomIntensity = 2`, `float4 hdUE:postProcess:ColorSaturation = (0, 0, 0, 1)`. Numbers, booleans, enum names, object paths and tuples (filling a struct's fields in order) are accepted. Unknown names or values are skipped with a warning in the UE log (`HdUEBackend\Saved\Logs`).
- `hdUE:postProcessMaterials` lists cooked post-process material paths (a string array, or one string separated by spaces), added as blendables.
- `hdUE:tonemapper` runs UE's tonemapper. Bloom, colour grading (saturation, contrast, gamma, gain, offset, white balance, LUTs), the film curve, vignette, grain, chromatic aberration and lens flares only happen inside it, so they need it on. The camera exposure is then applied before the tonemapper (`AutoExposureBias`) rather than to the result. The beauty becomes display-referred (sRGB), so set the Solaris viewport's colour correction to raw, and don't expect it to match Karma.

Depth of field, post-process materials with a "Before Tonemapping" blendable location, and the other settings apply with the tonemapper off too. The render settings LOP shows Tonemapper, Post-Process Materials, Bloom, Vignette, Film Grain, White Balance, Saturation and Contrast; add other properties with an Edit Properties LOP or in the USD file. Post-processing applies to the beauty only; AOVs stay raw. Changing a post-process setting restarts Path Tracer accumulation.

![UE post-processing](images/postprocess.png)

## Lights and HDR environments

`DomeLight.inputs:texture:file` accepts HDR/EXR and ordinary image files. Environment maps use a latitude-longitude layout (`latlong` or `automatic`); images that are not 2:1 are stretched, as in Karma and Storm, with a warning in the UE log. Linear HDR values are retained during conversion to the SkyLight cubemap; unusually bright maps are scaled before half-float storage and rescaled for lighting and background drawing. HDR sources are interpreted in linear Rec.709/sRGB primaries; OCIO conversion and other environment layouts are not implemented.

The dome's full transform, color, intensity, exposure, color temperature, visibility, and deletion affect lighting, reflections, and the background in Lumen and Path Tracer. Mapping follows USD's local +Y pole and +Z longitude origin independently of the stage up axis. Multiple domes are summed into one environment. `domeResolution` selects the cubemap face size (power of two from 32 to 2048). Decoded files are cached by path, size, and modification time and rechecked on submitted light updates. File changes alone do not trigger a viewport refresh. The Path Tracer waits for cubemap upload and SkyLight filtering before starting a new accumulation. The HDRI background is drawn right after the base pass, so heterogeneous volumes and translucent surfaces composite over it.

Local lights support temperature, exposure, normalization, shadows, diffuse/specular scales, source dimensions, and sphere-light cone angle/softness. RectLight also accepts image emission. DiskLight uses a masked rectangular source, and CylinderLight uses a capsule source; these are approximations. Intensity is luminance in nits as UsdLux defines it: local lights use UE nits on their own emitting area, and DistantLight illuminance is intensity × π·sin²(angle∕2), or intensity in lux when normalized. With a fully rough bound Preview Surface under a normalized distant light, hdUE and Karma agree to 0.1%; glossier surfaces come out up to about 1.4x brighter in hdUE because UE's specular model differs from Karma's. Arbitrary light linking, IES profiles, portals, and separate dome diffuse/specular scales are unsupported. Dome diffuse/specular must both be 1.

MegaLights is enabled by default for supported local lights in realtime rendering. Directional lights use the regular directional-light path. The Path Tracer uses its own lighting implementation. The render statistics report both project enablement and whether MegaLights actually ran for the delivered frame; supported D3D12 hardware and ray tracing remain required.

Optional camera-following review lights are used when there are no active supported scene lights. Authored lights, including DomeLight, take precedence.

The repository fixture contains a procedural HDRI, a textured rect light, disk, cylinder, spot, directional, and 24 sphere lights:

```powershell
.\scripts\Test-Hydra.ps1 -InputUsd samples/lighting/lighting.usda -LightingTest -Mode both
.\scripts\Launch-Houdini.ps1 -InputUsd samples/lighting/lighting.usda
```

## Volumes

Volumes (Houdini build) render the `density` field, or the first field, of a Volume prim as a UE heterogeneous volume with `M_HdUEVolume` (albedo 0.8; density is extinction per scene unit). The field can be:

- an `OpenVDBAsset` pointing to a `.vdb` file, read with OpenVDB;
- an unsaved SOP volume in the Solaris viewport: `OpenVDBAsset` or `HoudiniFieldAsset` prims whose `filePath` is an `op:` SOP layer. hdUE takes the cooked geometry from Houdini's locked-geometry registry, or cooks the SOP;
- a `.bgeo` / `.bgeo.sc` file, which is where the USD Render ROP writes native Houdini volumes for husk.

From Houdini geometry, the VDB or Houdini volume primitive named after `fieldName` is used, else the `fieldIndex`-th one. Grids are densified over their active bounding box and point-sampled with a stride above 512 voxels per axis. Volume materials, other fields, temperature/emission, tapered Houdini volumes and frustum VDBs are not translated.

Measured against Karma: in realtime, a fog's brightness is within about 15%, but volumes cast no shadow onto surfaces (enabling `r.HeterogeneousVolumes.Shadows` did not produce one in testing; the cause is not identified yet). The Path Tracer casts it (optical depth 1.4 against Karma's 1.9) and includes multiple scattering, but resamples volumes into a coarse grid, so their silhouettes are blocky.

## Opacity and glass

Generic `displayOpacity` maps to Substrate coverage. Constant values and identical values across another domain are supported; varying values are reduced with a warning. Opaque and translucent parents are selected accordingly.

Optical glass uses a local profile that selects meshes and supplies IOR, roughness, attenuation color/distance, thickness, and coverage. Optical properties are not inferred from display opacity. Realtime refraction uses a thickness approximation; the Path Tracer follows geometry and absorption. Profiles and scene-specific generators stay local and ignored.
