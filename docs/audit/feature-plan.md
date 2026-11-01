# Feature plan: environment, volumes, lights, hair, camera

Planned after the Exponential Height Fog work (`worktree-volumetric-fog`). Each item names the UE feature it configures, how
a scene authors it, what changes in the delegate and the backend, how it is verified, and its risks. The UE source
references are under `Engine/` of the engine root in `.local/paths.json` (UE 5.8.4).

The rules in [AGENTS.md](../../AGENTS.md) apply to every item: configure UE's own component or renderer feature, never
reimplement it; defaults come from UE (component defaults, USD importer, Movie Render Queue), then the USD spec, then
Karma; verification is tiered (build, unit tests, the conformance scenes touched, a Cornell husk render against the
previous build; Hydra sync or ABI changes also run the in-Houdini test they touch).

## Order

| # | Item | Size | Main risk |
| --- | --- | --- | --- |
| 0 | Per-light UE properties (ABI 14) | S | None; shared by items 1 and 3 |
| 1 | Sky Atmosphere, atmosphere sun, Volumetric Clouds | M | Cloud material cooking; Path Tracer reference atmosphere cost |
| 2 | Volumes: emission, realtime shadows, cooked SVT assets | M | Sparse Volume Textures cannot be built at runtime |
| 3 | Light functions (UsdLux light filters) | M | Atlas compatibility is decided at cook time |
| 4 | Groom hair | L | No supported runtime build path: spike first |
| 5 | Realtime refraction | M | Cooked parent variants; Lumen translucency quality |
| 6 | Lens distortion | M | No USD schema; plugin and camera actor per view |
| 7 | Motion vectors | S-M | Previous-frame state for the raster pass |

Local Fog Volumes are not here: they are phase 2 of the fog work (a LOP HDA authoring
`unreal:localFogVolume:<ULocalFogVolumeComponent property>` on a box or sphere prim; realtime only, as UE's Path Tracer
ignores them). Volumetric fog is part of its height fog (`unreal:heightFog:bEnableVolumetricFog`).

Item 1 follows the height fog's pattern: render settings `unreal:heightFog` (bool) and
`unreal:heightFog:<property>`, generated into a tab of hdUE Render Settings, sent as `HeightFog` and
`HeightFog:<Property>` names through the existing `set_post_process`; the backend builds one component per session and
rebuilds it only when those change. No ABI change.

## 0. Per-light UE properties

`HdueLight` has fixed fields and a `flags` word. Items 1 (atmosphere sun), 3 (light functions) and smaller UE light
options (contact shadows, light shafts, `bCastVolumetricShadow`, volumetric scattering intensity) all need per-light
values that UsdLux does not have.

- ABI 14: `HdueLight` gets `property_count`, `property_names`, `property_values`, set on the light component by
  reflection before it is registered, with the importer the render settings use for `unreal:postProcess:*` and
  `HeightFog:*` (`ImportPropertyValue`; object properties load a cooked path and check its class).
- Delegate: Hydra's scene delegate reads light attributes by name only, so `generate_render_settings.py` keeps a curated
  list per UE class (`LIGHT_CLASSES`: what UsdLux has no attribute for and a runtime light can change) and writes both
  the Light LOP's parameters (`HdUnrealEngine_Light.ds`, UE's labels, tooltips, ranges and constructor defaults) and
  `src/hydra/light_properties.h`, the names the delegate reads as `unreal:light:<Property>`.
- UE 5.8 has no `bUseContactShadows`: `ContactShadowLength` above 0 turns contact shadows on. Distant lights already
  default to `bAtmosphereSunLight` on (`UDirectionalLightComponent`'s constructor).
- Verify: conformance `lights/*`, a Cornell render (unchanged), one new conformance scene that sets
  `unreal:light:ContactShadowLength`.

## 1. Sky Atmosphere, atmosphere sun, Volumetric Clouds

UE: `USkyAtmosphereComponent`, `UVolumetricCloudComponent` (`Source/Runtime/Engine/Classes/Components/`), a directional
light with `bAtmosphereSunLight` (index 0 sun, 1 moon), and a SkyLight with `bRealTimeCapture` when there is no dome.

- Authoring: render settings `unreal:skyAtmosphere` (bool) and `unreal:skyAtmosphere:<property>`,
  `unreal:volumetricCloud` and `unreal:volumetricCloud:<property>`, as `unreal:heightFog` (component exists while the bool is
  set; properties by reflection). The sun is a DistantLight with `unreal:light:bAtmosphereSunLight = 1` (item 0).
- Sky light: with the atmosphere on and no DomeLight, the backend adds a real-time capture SkyLight so the sky lights
  the scene, as UE's default level does. An authored DomeLight keeps its HDRI and wins (never replace a scene's own
  lighting).
- Clouds: the component's default material is the soft reference
  `/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst` (`VolumetricCloudComponent.cpp:72`). Add that
  directory to `DirectoriesToAlwaysCook` and load it explicitly; `unreal:volumetricCloud:Material` takes another cooked
  material path.
- Height fog: with the atmosphere on, UE colours height fog from the atmosphere when its inscattering colours are
  black. `r.SupportSkyAtmosphereAffectsHeightFog` is 1 by default and read-only (baked into shaders,
  `SkyAtmosphereRendering.cpp:98-102`), so this project has it; document the interaction.
- Path Tracer: UE's docs: Sky Atmosphere "requires a Sky Light in the scene which has Real Time Capture enabled", or
  the post-process setting Reference Atmosphere, with which "any Sky Light present in the scene is automatically
  ignored"; clouds are supported with the reference atmosphere since 5.6 ([Path Tracer](https://dev.epicgames.com/documentation/unreal-engine/path-tracer-in-unreal-engine)).
  `PathTracingEnableReferenceAtmosphere` (`Scene.h:2517`, `r.PathTracing.EnableReferenceAtmosphere`,
  `r.PathTracing.EnableReferenceClouds`, `PathTracing.cpp:428-501`) stays the user's choice; the real-time capture
  SkyLight above already makes the Path Tracer see the sky.
- Units: UE's atmosphere is in kilometres and its ground sits at the component's origin; the backend converts from
  `metersPerUnit` as it does for transforms.
- Verify: new conformance scenes (atmosphere with sun at three elevations, clouds), both modes; Cornell unchanged;
  `Test-RenderSettings.ps1` with the new defaults.

## 2. Volumes: emission, realtime shadows, cooked SVT assets

Today a Volume's `density` is densified into a transient `UVolumeTexture` on a `UHeterogeneousVolumeComponent` with
`M_HdUEVolume` (`Scene.cpp:150-170`). Sparse Volume Textures cannot replace it: their build path
(`UStreamableSparseVolumeTexture::BeginInitialize`, `USparseVolumeTextureFrame::Initialize`, `FResources::Build`,
`SparseVolumeTexture.cpp:1098-1434`) returns false without `WITH_EDITORONLY_DATA`, and the runtime build would need an
engine change. So:

- Keep the transient dense texture for Hydra volumes. Add a second field (`temperature`, or `emission`/`Cd` when
  authored) as a second channel, and extend `M_HdUEVolume` (and `HdueVolumeDesc`, ABI bump) with emission from it, as
  UE's heterogeneous volume materials read SVT attributes. Temperature-to-colour follows UE's own blackbody node.
- Realtime shadows onto the scene: the project's Heterogeneous Volumes "Shadow Casting" setting
  (`r.HeterogeneousVolumes.Shadows`, 0 by default, `HeterogeneousVolumes.cpp:33`). The shadow is a volumetric shadow
  map applied in the deferred light loop only (`LightRendering.cpp:1859,2364`): lights shaded by MegaLights skip it
  (no MegaLights code sets `ADAPTIVE_VOLUMETRIC_SHADOW_MAP`), and so do lights with ray-traced shadows. hdUE shades
  local lights with MegaLights by default, so a volume shadows the scene from distant lights and from local lights
  with `unreal:megaLights = 0`. Self-shadowing needs only the light's `bCastVolumetricShadow`. This explains the open
  "volumes cast no shadow" item.
- Path Tracer's blocky silhouettes: its voxel grids use `r.HeterogeneousVolumes.{Frustum,Ortho}Grid.ShadingRate` 4 and
  128 MB; UE's docs recommend shading rate 1 and 512 MB for cinematic renders in Movie Render Queue
  ([Heterogeneous Volumes](https://dev.epicgames.com/documentation/unreal-engine/heterogeneous-volumes-in-unreal-engine)):
  the offline profile sets them. `SetVolumeResolution` already gets the grid's size.
- Emission: the Volume-domain material's Emissive output, rendered by both paths
  (`HeterogeneousVolumesLiveShadingUtils.ush:36-72`, `PathTracingHeterogeneousVolumes.ush:124`); UE's BlackBody
  expression is ordinary material code (`MaterialTemplate.ush:2113`). It needs `M_HdUEVolume` re-made by the
  HdUEPrepare commandlet, an editor build and a re-cook.
- Cooked SVTs: a Volume (or Xform) with `unreal:asset` pointing at a cooked `SparseVolumeTexture` renders it with UE's
  SVT material, through the native asset catalog, for scenes that pre-cook their VDBs in the editor.
- Verify: `volumes/*` conformance, `samples/cornell/fog.vdb` shadow on the floor in realtime, an emissive fire VDB in
  both modes.

## 3. Light functions

UE: `ULightComponent::SetLightFunctionMaterial` (`LightComponent.cpp:1187`), runtime-safe on movable lights. MegaLights
reads light functions only through the Light Function Atlas (`r.MegaLights.LightFunctions`, `r.LightFunctionAtlas*`);
the Path Tracer supports them (`FLightFunctionParametersPathTracing`, `r.PathTracing.LightFunctionColor`).

- Authoring: `unreal:light:LightFunctionMaterial` (item 0) takes a cooked material path. For textures (gobos, cookies),
  `unreal:lightFunction:texture` makes a dynamic instance of one cooked hdUE light-function parent with a texture
  parameter, marked `bForceCompatibleWithLightFunctionAtlas` so MegaLights keeps it.
- UsdLux `light:filters`: still warned. Houdini's light filter prims have no portable meaning; map one only if a later
  USD schema or UE's USD importer does.
- Verify: conformance scene with a gobo on a spot light and a rect light, both modes, MegaLights on and off.

## 4. Groom hair (spike first)

UE's strand renderer (HairStrands, Marschner shading, path-traced hair) is what Houdini hair needs; BasisCurves render as
tube meshes today. There is no supported runtime path: the `UGroomAsset` build (`CacheDerivedDatas`,
`CommitHairDescription`) is editor-only (`GroomAsset.h:890-961`). `FGroomBuilder` (`GroomBuilder.h:59-129`) compiles
at runtime and `GetHairGroupsPlatformData()` is public, so a manual build may work. UE's docs describe no runtime,
Blueprint, PCG or Niagara way to make a groom: grooms come from an Alembic or USD import in the editor.

- Spike (1-2 days, throwaway): fill `FHairDescription` from one BasisCurves prim, run `FGroomBuilder::BuildData`,
  `BuildBulkData`, interpolation and cluster data, write `PlatformData`, `InitResources`, draw one
  `UGroomComponent`. Findings to report: whether bulk data accepts in-memory buffers, what `bIsInitialized` needs,
  whether any engine patch is unavoidable.
- If it works without engine patches: BasisCurves with `unreal:groom = 1` (or Houdini's hair primvars) become a groom;
  widths per point; root UVs from `st`; hair material from a cooked hair parent with base colour and melanin inputs.
  Guides = strands at first (no simulation).
- If it needs engine patches: stop and decide with the user; the fallback is cooked Groom assets through `unreal:asset`.

## 5. Realtime refraction

MaterialX `transmission` is see-through coverage today, with no refraction in realtime (the Path Tracer refracts).

- UE's Interchange MaterialX import (Substrate on) turns any connected `transmission` into
  `Substrate-StandardSurface-Translucent` (OpenPBR: `MF_Substrate_OpenPBR_Translucent`) with blend mode
  `BLEND_TranslucentColoredTransmittance`, and wires the material's Refraction pin from the function's
  `Refraction (IOR)` output (`InterchangeMaterialXPipeline.cpp:31-35`, `MaterialXStandardSurfaceShader.cpp:42-68`,
  `MaterialXSurfaceShaderAbstract.cpp:1527`). It leaves Refraction Method and Translucency Lighting Mode at the
  engine defaults; `thin_walled` only sets two-sided. Add cooked parent variants that do the same.
- UE's docs: the Path Tracer refracts only with Refraction Method Index Of Refraction ("otherwise ... transparency
  instead of refraction"); check what the engine default gives the imported material.
- Realtime options to expose in Engine Quality, not choose silently: `r.RayTracing.Translucency` (-1: the post-process
  setting decides; `.Refraction` 1), Lumen front-layer reflections (already on), `r.Refraction.Blur` (Substrate rough
  refraction, 0).
- Verify: `materials/*` glass conformance, the glass profile scene, Cornell unchanged.

## 6. Lens distortion

UE: `CameraCalibrationCore` (runtime module): `ULensDistortionModelHandlerBase::SetDistortionState` with
`USphericalLensModel`, applied per view through `UCameraCalibrationSubsystem::SetLensDistortionSVEState`, which needs an
`ACameraActor` per view. No LensFile asset is required.

- UE's docs: "You can create a lens distortion handler object ... and drive its distortion state without the need to
  specify a Lens File"; the spherical model is Brown-Conrady with K1, K2, P1, P2, K3, and the handler computes the
  overscan ([Camera Lens Calibration](https://dev.epicgames.com/documentation/unreal-engine/camera-lens-calibration-overview)).
- Authoring: USD has no lens-distortion schema, but Hydra's camera carries `HdLensDistortionSchema` (`type`, `k1`,
  `k2`, `center`, `anaSq`, `asym`, `scale`, `ior`; [Hydra prim schemas](https://openusd.org/release/api/_page__hydra__prim__schemas.html)):
  read it first and map `standard` distortion to UE's K1, K2 (and `center`). Camera attributes
  `unreal:lensDistortion:k3, p1, p2` add what Hydra's model lacks. Karma's lens attributes are only a comparison.
- Enable the plugin in the runtime project; overscan renders a larger data window and crops, so AOVs stay aligned.
- Verify: a grid scene against a distortion computed from the same coefficients; viewport and husk match.

## 7. Motion vectors

The `velocity` AOV comes from the raster pass, which renders one time, so it is 0.5 everywhere.

- Render the buffer pass after a previous frame at the shutter's open time (camera and object transforms, deformed
  points), so UE's velocity buffer holds this frame's motion, as MRQ's motion-vector output does after its warm-up.
- Output convention: MRQ's `/MovieRenderPipeline/Materials/MovieRenderQueue_MotionVectors` reads `PPI_Velocity`, which
  decodes to current minus previous position in NDC (+Y up, 2 = the full width, `MaterialTemplate.ush:3213-3232`),
  and re-encodes it as `v * 0.2495 + 0.5` in R and G (UE's docs: "stored in the 0,1 coordinate space for X and Y,
  where 0.5, 0.5 is no motion", normalized to the screen;
  [Cinematic Render Passes](https://dev.epicgames.com/documentation/unreal-engine/cinematic-render-passes-in-unreal-engine)). Pixels with no velocity get the camera's motion. Document it next
  to Karma's `motionvector` for comparison.
- First frame: MRQ sets a camera cut at a shot's first sample, which zeroes motion unless warm-up frames run first
  (`MoviePipelineRendering.cpp:407,451`); hdUE's sub-frame at shutter open plays that part.
- Verify: a moving cube and a moving camera, both modes (the raster pass serves both), checked against the expected
  pixel displacement.
