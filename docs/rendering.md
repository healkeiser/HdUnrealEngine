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
| `hdUE:denoiser` | `off` | `oidn` denoises the converged Path Tracer image with Open Image Denoise (Houdini build) |
| `hdUE:subdivisionLevel` | 2 | Uniform refinement of catmullClark and loop meshes; see [subdivision](#subdivision-surfaces) |
| `hdUE:tonemapper` | 0 | Run UE's tonemapper and the effects inside it; see [post-processing](#unreal-post-processing) |
| `hdUE:postProcess:<Property>` | | Any `FPostProcessSettings` property, for the beauty |
| `hdUE:postProcessMaterials` | | Cooked post-process materials added to the beauty |
| `hdUE:domeResolution` | 512 | Environment cubemap face size |
| `hdUE:autoViewerLight`, `hdUE:viewerLightIntensity` | | Review light for scenes without lights |
| `hdUE:upAxis` | stage | Up axis override |

## Geometry

Generic Hydra translation supports meshes, cameras, materials, and distant, sphere, rect, disk, cylinder, and dome lights.

Meshes reach UE indexed: the triangle corners of a point share one vertex unless a face-varying normal, UV, color or wet mask differs between them, so UE holds about as many vertices as the USD mesh has points. A mesh UE cannot take (more than about 715 million vertices, or broken indices) is skipped with a warning naming it in the UE log; the rest of the scene still loads.

Material-binding GeomSubsets partition triangles by their original USD face while preserving UVs, normals, transforms, visibility, and picking paths. Unassigned faces retain the mesh material; overlapping or invalid material subsets are rejected. Subset edits and removals update the generated meshes.

Missing or zero normals are generated from area-weighted vertex adjacency before subset partitioning, so material boundaries share the same shading normals. Authored normals are retained. Author explicit face-varying normals or split vertices where hard edges are required; normal generation does not apply subdivision creases.

Hydra instancers (USD PointInstancers, including nested ones) are translated: each prototype mesh is built once as a runtime static mesh, or uses its native cooked asset, and drawn through one instanced static mesh component with per-instance transforms composed as in hdEmbree. Preview Surface and ID materials are cooked with instanced-static-mesh usage; prepared MaterialX or Substrate materials need that usage too before they render on instances. The Solaris settings subnet still expands instanceable references in its own layer for editable mesh/material translation.

Implicit Cube, Sphere, Cone, Cylinder, Capsule and Plane prims are converted to meshes by Hydra's implicit-surface scene index (`HdUE_ImplicitSurfaceSceneIndexPlugin`), as for hdEmbree.

Points render as one shared sphere instanced per point, sized by `widths` (0.1 when unauthored). BasisCurves render as six-sided tubes with flat end caps on open curves: linear curves follow their vertices, cubic Bezier, B-spline and Catmull-Rom curves are evaluated in their basis, and `widths` set the diameter (0.01 when unauthored). Points and curves under a PointInstancer are drawn once per instance.

Mesh UVs come from the primvar that the bound material's `UsdPrimvarReader_float2` reads (`varname`, such as `st`, or `map1` from 3ds Max exports), or from `st` when the material names none. A mesh carries one UV set, chosen through its own binding.

Unbound geometry uses `displayColor`/`displayOpacity`. A `displayColor` that varies (uniform, vertex or face-varying on meshes, per point on Points, per vertex or per curve on BasisCurves, per instance on a PointInstancer) is multiplied into `M_PreviewSurface` as vertex colors or per-instance custom data. Dynamic mesh vertex colors are stored in 8 bits, so very dark colors band slightly. Varying `displayOpacity`, and colors on meshes that also carry a wet-surface mask (the mask reuses the vertex colors), are not translated. The unbound default is a Preview Surface (roughness 0.5); Karma's default for unbound geometry is darker, so compare renderers with bound materials.

## Preview Surface inputs

Every `UsdPreviewSurface` input is translated, as a constant or a `UsdUVTexture` (with `scale`, `bias`, a single-channel output and the first `UsdTransform2d`):

| Input | In UE |
| --- | --- |
| `diffuseColor`, `emissiveColor` | Base colour and emission (colour textures sampled as sRGB) |
| `metallic`, `roughness` | Metalness workflow |
| `ior` | Dielectric reflectance, F0 = ((ior - 1) / (ior + 1))² |
| `useSpecularWorkflow`, `specularColor` | Specular workflow: diffuse and F0 straight from `diffuseColor` and `specularColor` |
| `clearcoat`, `clearcoatRoughness` | A second specular lobe weighted by `clearcoat` (Substrate haziness), as UE converts its own clear coat; there is no coat absorption over the base |
| `opacity`, `opacityThreshold` | See-through (translucent) or cutout (masked) |
| `normal` | Tangent-space normal map (USD's +Y up) |
| `occlusion` | Ambient occlusion |
| `displacement` | World Position Offset along the normal (see [Displacement](#displacement)) |

## MaterialX surfaces

hdUE asks Hydra for the `mtlx` render context before the universal one, so a material with a MaterialX surface output renders from its MaterialX graph, and one with only `outputs:surface` from its UsdPreviewSurface. MaterialX graphs are read directly at render time, with no preparation, cook or shader compile; the [prepared MaterialX library](materials.md#materialx-and-substrate) still takes precedence for materials it maps, and stays the route for graphs this mapping cannot express.

Surfaces: `ND_standard_surface_surfaceshader`, `ND_open_pbr_surface_surfaceshader` and `ND_UsdPreviewSurface_surfaceshader` (read like `UsdPreviewSurface`). Each input is a constant, or an image reached through these nodes:

| Node | Translation |
| --- | --- |
| `ND_image_*`, `ND_tiledimage_*` (`uvtiling`, `uvoffset`), `UsdUVTexture` | The texture; `colorspace` / `sourceColorSpace` `raw` skips the OCIO conversion of colour images, as for Preview Surface |
| `ND_geompropvalue_vector2`, `UsdPrimvarReader_float2` | Selects the mesh's UV set (`geomprop` / `varname`, such as `map1`) |
| `ND_extract_*` | Selects a channel (alpha cutouts: `ND_image_color4` → `ND_extract_color4` index 3) |
| `ND_convert_*`, `ND_dot_*` | Passed through |
| `ND_normalmap*` | Tangent-space decode, with its `scale` on XY |
| `ND_multiply_*` by a constant | Scales the texture or constant |
| `ND_constant_*` | Its value |

Any other node (`ND_mix_*`, `ND_place2d_*`, procedurals, ...) leaves the input at its constant and is reported once as a warning; the material still renders. Textured weights (`base`, `sheen`, `subsurface`, ...) also use their constant.

| standard_surface (OpenPBR) | In UE |
| --- | --- |
| `base` × `base_color` (`base_weight` × `base_color`) | Base colour |
| `metalness` (`base_metalness`), `specular_roughness`, `specular_IOR` (`specular_ior`) | As Preview Surface `metallic`, `roughness`, `ior` |
| `specular` × `specular_color` | Specular tint (layered parents) |
| `coat` (`coat_weight`), `coat_roughness`, `coat_color` | Clear coat; `coat_color` tints it on the layered parents only |
| `sheen`, `sheen_color`, `sheen_roughness` (`fuzz_*`) | Substrate fuzz |
| `subsurface`, `subsurface_color`, `subsurface_radius` × `subsurface_scale` (`subsurface_weight`, `subsurface_color`, `subsurface_radius_scale` × `subsurface_radius`) | Substrate subsurface scattering, mean free path in scene units |
| `thin_walled` (`geometry_thin_walled`) | Thin two-sided shell that transmits its subsurface colour (cloth, leaves) |
| `emission` × `emission_color` (`emission_luminance` × `emission_color`) | Emission (OpenPBR's luminance is used as a plain weight) |
| `opacity` (`geometry_opacity`) | An image cuts out at 0.5 (masked), as UE's Interchange imports MaterialX; a constant below 1 is see-through |
| `transmission` (`transmission_weight`) | Approximated as see-through coverage, opacity × (1 − transmission); no refraction |
| `normal` (`geometry_normal`) | Tangent-space normal map |

Materials with `subsurface` or `sheen` above 0 render with the layered Standard Surface parents: UE's own `Substrate-StandardSurface-Opaque` material function (the one UE's Interchange uses for MaterialX), fed the same texture slots, in four cooked variants (opaque or masked, solid or thin-walled; `thin_walled` is a static input of the function). Everything else, including standard_surface materials without subsurface or sheen, uses the single-slab Preview Surface parents. The layered function mixes metal and dielectric slabs and layers coat and sheen vertically, so it costs several Substrate closures per pixel where Preview Surface costs one; keeping it to the materials that need it bounds that cost. In the Sponza gallery shot (960 x 540, Lumen, curtains covering about half the frame), the layered curtains cost about 1 to 2 ms per frame over the same curtains on Preview Surface (husk, 1500 realtime frames, measured twice).

Limits: `subsurface_color` and `sheen_color` follow an image only when it is `base_color`'s (the usual authoring; their tint is kept), else they use their constant. Subsurface uses Substrate's per-material mean free path; UE subsurface profiles are not created at runtime. One UV transform per material, from the first textured input. `diffuse_roughness`, anisotropy, thin film, `coat_IOR`, `coat_normal`, `transmission_*` colours and dispersion are not translated. Transmissive materials, see-through materials and translucent sorting follow the Preview Surface translucent parent, which has no subsurface or sheen.

## Colour management (OCIO, ACES)

UE renders in linear Rec.709. In the Houdini build, hdUE converts at its boundaries with the OpenColorIO configuration Houdini uses (`$OCIO`):

- The beauty is published in the config's `scene_linear` role. With an ACES pipeline whose `scene_linear` is ACEScg, renders come out in ACEScg, as Karma's do. Houdini 22's default config uses linear Rec.709 for `scene_linear`, so nothing is converted there.
- Authored colours (Preview Surface diffuse and emissive colours, `displayColor`, light colours) are read as `scene_linear` values and converted to linear Rec.709 for UE.
- With UE's tonemapper on, the beauty is display-referred sRGB and is not converted.
- `HDUE_COLOR_MANAGEMENT=0` turns the conversion off.

- Linear colour textures (EXR, HDR in the base colour and emissive inputs, unless `sourceColorSpace = "raw"`) are read as `scene_linear` and converted once to linear Rec.709 with OpenImageIO, into a cache under the temp folder (`hdue_ocio`), keyed by path, file time and colour space.

8-bit colour textures are read as sRGB-encoded Rec.709, which is what sRGB textures are in any config. Per-file colour space metadata other than `raw` is not read yet.

## Alpha

The beauty carries straight alpha: 1 where geometry covers the pixel, 0 where the camera sees no geometry, fractional on anti-aliased edges and see-through surfaces. hdUE turns on UE's `r.PostProcessing.PropagateAlpha` at startup and converts UE's inverted coverage to straight alpha. A dome light hidden from camera rays (`hdue:visibleBackground = 0`) leaves alpha 0 behind it.

## Displacement

The Preview Surface `displacement` input (a constant or a texture) moves vertices along their normal by that distance in scene units, as World Position Offset in hdUE's preview materials. Only existing vertices move: detail finer than the mesh needs a denser mesh, for example `catmullClark` subdivision (`hdUE:subdivisionLevel`). Shading normals are not recomputed from the displacement. UE's adaptive Nanite tessellation is not used: it needs displacement maps at Nanite build time.

## Nanite

Meshes from the native asset catalog render with Nanite when their cooked asset has Nanite data (`hdUE:requireNanite` enforces it). In interactive hosts (Houdini's viewport), meshes hdUE builds from Hydra become Nanite meshes too: hdUE first draws them as regular static meshes, builds their Nanite data on worker threads, then swaps each one for a Nanite static mesh, one per frame. Every Nanite page stays resident in memory, since UE streams the other pages only from cooked files. Animated or edited meshes stay dynamic, and instanced meshes stay regular instanced static meshes. Batch renders (husk) don't convert, and `HDUE_NANITE=0` turns Nanite off in the viewport.

## Denoising

With `hdUE:denoiser = "oidn"` (the **Denoiser** menu on Render Settings LOPs), the Houdini build denoises the converged Path Tracer beauty with the Open Image Denoise 2.3 library Houdini ships for Karma, on the GPU through CUDA when available, else on the CPU. hdUE renders albedo and normal passes as guides (internally when they are not bound as AOVs), so edges and textures stay sharp; the frame counts as converged only once the denoised image is published, so husk writes the denoised beauty. Other AOVs are not denoised, Lumen is unaffected, and progressive viewport updates stay noisy until the image converges. The standalone OpenUSD build has no denoiser. On the Cornell box at 32 samples, denoising cut the noise on a flat wall from 0.0071 to 0.0031 (standard deviation) with the mean brightness unchanged.

UE's own path-tracer denoiser plugins were not usable here: in UE 5.8.4 the OptiX plugin only writes a 384 x 384 tile of larger frames, and its Open Image Denoise plugin left the image unchanged.

## Subdivision surfaces

Meshes whose `subdivisionScheme` is `catmullClark` (USD's default) or `loop` are refined uniformly with OpenSubdiv to `hdUE:subdivisionLevel` levels, with their creases, corners and holes. Vertex, varying, face-varying and uniform primvars (`st`, `displayColor`, `velocities`, time-sampled `points`) follow the refined surface, and GeomSubsets keep their faces. Authored normals are ignored, as USD specifies for subdivision surfaces; normals come from the refined surface. `bilinear` and `none` render the mesh as authored.

Each level multiplies the face count by four. Level 3 matches Karma's surfaces closely on a cube; the default 2 still shows facets on close silhouettes. There is no adaptive or screen-space refinement, and displacement is not supported. Wet-surface masks are not translated on subdivided meshes.

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
- `hdUE:tonemapper` runs UE's tonemapper. Bloom, colour grading (saturation, contrast, gamma, gain, offset, white balance, LUTs), the film curve, vignette, grain, chromatic aberration and lens flares only happen inside it, so they need it on. The camera exposure is then applied before the tonemapper, added to any authored `AutoExposureBias`, rather than to the result; UE's physical-camera exposure (ISO, shutter, aperture) stays off unless `AutoExposureApplyPhysicalCameraExposure` is set. UE's on-screen warnings are never drawn into hdUE frames. The beauty becomes display-referred (sRGB), so set the Solaris viewport's colour correction to raw, and don't expect it to match Karma.

Depth of field, post-process materials with a "Before Tonemapping" blendable location, and the other settings apply with the tonemapper off too. The render settings LOP's Post-Processing section shows Tonemapper, Post-Process Materials, Bloom, Vignette, Film Grain, White Balance, Saturation and Contrast; add other properties with an Edit Properties LOP or in the USD file. Post-processing applies to the beauty only; AOVs stay raw. Changing a post-process setting restarts Path Tracer accumulation.

![UE post-processing](images/gallery/post_processing.png)

## Lights and HDR environments

`DomeLight.inputs:texture:file` accepts HDR/EXR and ordinary image files. Environment maps use a latitude-longitude layout (`latlong` or `automatic`); images that are not 2:1 are stretched, as in Karma and Storm, with a warning in the UE log. Linear HDR values are retained during conversion to the SkyLight cubemap; unusually bright maps are scaled before half-float storage and rescaled for lighting and background drawing. HDR sources are interpreted in linear Rec.709/sRGB primaries; OCIO conversion and other environment layouts are not implemented.

The dome's full transform, color, intensity, exposure, color temperature, visibility, and deletion affect lighting, reflections, and the background in Lumen and Path Tracer. Mapping follows USD's local +Y pole and +Z longitude origin independently of the stage up axis. Multiple domes are summed into one environment. `domeResolution` selects the cubemap face size (power of two from 32 to 2048). Decoded files are cached by path, size, and modification time and rechecked on submitted light updates. File changes alone do not trigger a viewport refresh. The Path Tracer waits for cubemap upload and SkyLight filtering before starting a new accumulation. The HDRI background is drawn right after the base pass, so heterogeneous volumes and translucent surfaces composite over it.

Local lights support temperature, exposure, normalization, shadows, diffuse/specular scales, source dimensions, and sphere-light cone angle/softness. RectLight also accepts image emission. DiskLight uses a masked rectangular source, and CylinderLight uses a capsule source; these are approximations. Intensity is luminance in nits as UsdLux defines it: local lights use UE nits on their own emitting area, and DistantLight illuminance is intensity × π·sin²(angle∕2), or intensity in lux when normalized. With a fully rough bound Preview Surface under a normalized distant light, hdUE and Karma agree to 0.1%; glossier surfaces come out up to about 1.4x brighter in hdUE because UE's specular model differs from Karma's. RectLights are created without UE's barn doors, which UsdLux does not have.

On the Cornell box (see the [gallery](gallery.md)), the Path Tracer's brightness matches Karma's to 0.2% with the same sample count and bounce depth (Karma's diffuse limit defaults to 1 bounce; set `karma:object:diffuselimit` to compare). Lumen reaches about 60% there: directly lit surfaces match, but its realtime GI underestimates light after several bounces, which dominates a closed, diffuse room.

Arbitrary light linking, portals, and separate dome diffuse/specular scales are unsupported.

IES profiles (`shaping:ies:file` on point, sphere, spot, rect and disk lights) are experimental. hdUE parses the file with UE's runtime `IESFile` module and applies it as the light's profile, oriented along the light's -Z as in Karma. Against Karma on a synthetic narrow beam, the profile points the same way but hdUE is much darker (more than 60x at the beam centre) and its falloff is tighter: UE normalizes profiles to a peak of 1, and the angle mapping still needs checking. `shaping:ies:normalize` scales by UE's profile multiplier, which may not match UsdLux's energy-preserving definition. `shaping:ies:angleScale` has no UE equivalent and is ignored with a warning. Dome diffuse/specular must both be 1.

MegaLights is enabled by default for supported local lights in realtime rendering. Directional lights use the regular directional-light path. The Path Tracer uses its own lighting implementation. The render statistics report both project enablement and whether MegaLights actually ran for the delivered frame; supported D3D12 hardware and ray tracing remain required.

Per-light hdUE settings are `hdue:*` attributes on the light (the **Unreal Engine** tab of Light LOPs in Houdini):

| Attribute | Default | Meaning |
| --- | --- | --- |
| `hdue:visibleBackground` | 1 | DomeLight: draw the environment as the background. Off, it still lights the scene, and camera rays that escape see black (Karma's default for domes, `karma:light:renderlightgeo`) |
| `hdue:megaLights` | 1 | Realtime: shade this local light with MegaLights; off uses UE's regular deferred lighting |

A visible dome shows through any gap in the geometry. In Intel's Sponza, cracks around the far door and in a window grille let camera rays escape (Karma's alpha drops to 0.5-0.9 there): with the dome visible hdUE drew the sky in them, while Karma, whose dome is not visible by default, drew black. hdUE always writes alpha 1, so background pixels cannot be separated by alpha yet.

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
