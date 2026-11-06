# Opacity and glass

Generic `displayOpacity` maps to Substrate coverage. Constant values and identical values across another domain are supported; varying values are reduced with a warning. Opaque and translucent parents are selected accordingly.

Optical glass uses a local profile that selects meshes and supplies IOR, roughness, attenuation color/distance, thickness, and coverage. Optical properties are not inferred from display opacity. Realtime refraction uses a thickness approximation; the Path Tracer follows geometry and absorption. Profiles and scene-specific generators stay local and ignored.

The beauty's alpha behind glass is the coverage of what the glass shows: opaque objects seen through a pane keep alpha 1 (an engine edit, see [building](../building.md); UE's rough refraction dropped it). [samples/materialx/glass.usda](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/materialx/glass.usda) shows an object behind clear glass and behind glass whose roughness follows a fractal grunge (`glass.mtlx`; prepare it with `Import-MaterialX.ps1 -Source samples/materialx/glass.mtlx`).

MaterialX `transmission_depth` is the distance at which light reaches `transmission_color` (UE's `Substrate-StandardSurface-Translucent` turns the pair into a mean free path, as Interchange imports it). Left at its default of 0, which the MaterialX specification reads as a tint at the surface, the mean free path is close to zero: the Path Tracer, which follows the geometry's thickness, absorbs every channel of `transmission_color` below 1, while realtime, with Substrate's thin default slab, barely tints. Author a depth for coloured glass.
