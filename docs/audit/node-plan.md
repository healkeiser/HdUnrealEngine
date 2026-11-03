# Node plan: prim LOPs and Substrate builder nodes

Planned 2026-11-02. hdUE has one material node set (the Substrate Builder, surfaces only) and one prim LOP (hdUE Local
Fog Volume, create only). Volumes render with fixed UE materials, and the UE features that a level places as actors
(post-process volumes, height fog, sky, decals) are either render settings or missing. This plan turns them into
nodes. The rules in [AGENTS.md](../../AGENTS.md) apply: each node emits or places UE's own feature, with UE's names,
defaults and ranges taken from the engine source.

## How a prim LOP is built

Houdini's prim LOPs create or edit their prim: an **Action** menu (Create, Edit, Force Edit), **Primitives** in Edit
mode, **Initialize Parameters**, and a control menu per property (Set or Create, Set If Exists, Block, Do Nothing, ...).
SideFX builds them on the Edit Properties LOP (Karma Sky Atmosphere, Sphere, Light). hdUE's do the same
(`build_prim_lop` in `scripts/build_render_settings_hda.py`):

- the asset is a subnet whose parameters are the Edit Properties header, SideFX Sphere's transform section (with its
  handles) and a control/value pair per generated UE property, named after the USD attribute and tagged with its type;
- an internal Edit Properties LOP references them all, so Create, Edit, Force Edit and the control menus are Houdini's
  own code, and Initialize Parameters (which works on the asset's parameters by name) reads the prim's values;
- in Create mode only, the subnet adds the prim's viewport guide (an Edit Properties asset alone cannot make children);
- the prim type is fixed (hidden Primitive Type) and is the default of Allowed Primitive Types, so Edit mode leaves
  other prims alone (SideFX's default handling, Ignore, so a pattern that also matches the guide does not warn);
- a node is only made where Houdini has none: attributes on existing prims go on Render Geometry Settings' hdUE tab
  (`HdUnrealEngine_Geometry.ds`), and logic Houdini does not cook goes in C++ (`src/houdini/`, HDK), not Python;
- a version bump keeps the previous definition loading (`*_legacy.hda`) and hides it from the TAB menu
  (`OPcustomize`).

`tests/houdini_prim_lops.py` (hython, `Test.ps1 -Suite houdini`) checks Create, Edit with Initialize Parameters,
other prim types, the hidden legacy type, and that the new version authors the same prim as the old one.

## Order

| # | Node | UE feature | Today | Work | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | hdUE Local Fog Volume 2.0 | `ULocalFogVolumeComponent` | Create only, Python LOP inside | Rebuild on `build_prim_lop`; 1.0 kept | Done |
| 2 | hdUE Post Process Volume | `APostProcessVolume` (`IInterface_PostProcessVolume`: Priority, BlendRadius, BlendWeight, bEnabled, bUnbound) | Global `unreal:postProcess:*` only | Codeless schema `UnrealPostProcessVolume` (box of the prim's extent), Hydra Sprim, ABI bump, backend volumes blended by UE's own `UWorld::AddPostProcessingSettings` | Done |
| 3 | hdUE Height Fog | `UExponentialHeightFogComponent` | Fog tab of the render settings | Schema `UnrealExponentialHeightFog`, height from the prim's transform; render settings keep working when no prim exists | Done |
| 4 | hdUE Sky | `USkyAtmosphereComponent`, `UVolumetricCloudComponent`, the atmosphere sun | Sky tab of the render settings | Schemas for both, sun direction from a DistantLight relationship; render settings stay the fallback | Done (three LOPs: Sky Atmosphere, Volumetric Cloud; the sun stays a DistantLight) |
| 5 | Native Asset (Render Geometry Settings) | Cooked StaticMesh, Blueprint, Level, Sparse Volume Texture | Text field on Render Geometry Settings | No new node: a menu of the cooked registry's objects on the existing field; bounds proxy authoring is the open question | Planned |
| 6 | Hair Strands (Render Geometry Settings) | Groom component | `primvars:unreal:groom = 1` by hand | No new node: a toggle on Render Geometry Settings' Unreal Engine tab | Done |
| 7 | hdUE Wet Surface | hdUE's wet layer | Python Script LOP from `hdue_surface.create()` | No new node: `create()` makes a Render Geometry Settings LOP, whose hdUE tab already has the wet attributes with control menus | Done |
| 8 | hdUE Decal | `ADecalActor`, Deferred Decal domain | Missing | Needs builder item D3 first | Later |

Item 2 reuses UE's blending: the backend already owns a `UWorld` and builds each view's settings in `ApplyPostProcess`
(`Bridge.cpp`, between `StartFinalPostprocessSettings` and `EndFinalPostprocessSettings`). It spawns one
`APostProcessVolume` per prim (its `Settings` set by reflection, as `unreal:postProcess:*` are) and calls
`UWorld::AddPostProcessingSettings(ViewOrigin, &View)` after the render settings' override, so priority, blend radius
and unbound volumes are UE's code (`World.cpp`, `DoPostProcessVolume`).

Items 3 and 4 move settings from the render settings prim into scene prims. The render settings attributes stay
readable (no saved scene changes), and a prim wins over them when both exist.

## Substrate builder nodes

Each emits the UE material expression of the same name (to check against the 5.8.4 source before writing).

| # | Node | UE expression | Output |
| --- | --- | --- | --- |
| D1 | Substrate Volumetric Fog Cloud, Volume Field, Blackbody (done; ramps later) | `SubstrateVolumetricFogCloudBSDF` (Volume domain), `TextureSampleParameterVolume`, `BlackBody`, `CurveAtlasRowParameter` | Volume material bound to Volume prims, replacing the fixed `M_HdUEVolume` (`Scene.cpp`), which it must reproduce first |
| D2 | Substrate Hair, Eye, Single Layer Water, Thin Film, Add, Select; Metalness to Albedo/F0, Transmittance to MFP, Haziness to Secondary Roughness (done) | `SubstrateHairBSDF`, `SubstrateEyeBSDF`, `SubstrateSingleLayerWaterBSDF`, `SubstrateThinFilm`, `SubstrateAdd`, `SubstrateSelect`, the conversion helpers | Surface |
| D3 | Decal Output | `SubstrateConvertToDecal`, Deferred Decal domain | Decal material for item 8 |

Light function and post-process material outputs are not planned: lights and render settings take cooked paths, and a
post-process material would belong to item 2's volume.
