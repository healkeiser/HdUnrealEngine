# MaterialX surfaces

hdUE asks Hydra for the `mtlx` render context before the universal one, so a material with a MaterialX surface output renders from its MaterialX graph, and one with only `outputs:surface` from its UsdPreviewSurface. MaterialX graphs are read directly at render time, with no preparation, cook or shader compile; the [prepared MaterialX library](../materials.md#materialx-and-substrate) still takes precedence for materials it maps, and stays the route for graphs this mapping cannot express.

Surfaces: `ND_standard_surface_surfaceshader`, `ND_open_pbr_surface_surfaceshader` and `ND_UsdPreviewSurface_surfaceshader` (read like `UsdPreviewSurface`). Each input is a constant, or an image reached through these nodes:

| Node | Translation |
| --- | --- |
| `ND_image_*`, `ND_tiledimage_*` (`uvtiling`, `uvoffset`), `UsdUVTexture` | The texture, with its `colorspace` / `sourceColorSpace` and address modes, as for Preview Surface |
| `ND_geompropvalue_vector2`, `UsdPrimvarReader_float2` | Selects the texture's UV set (`geomprop` / `varname`, such as `map1`) |
| `ND_extract_*` | Selects a channel (alpha cutouts: `ND_image_color4` → `ND_extract_color4` index 3) |
| `ND_convert_*`, `ND_dot_*` | Passed through |
| `ND_normalmap*` | Tangent-space decode, with its `scale` on XY |
| `ND_multiply_*` by a constant | Scales the texture or constant |
| `ND_constant_*` | Its value |

A graph with any other node (`ND_mix_*`, `ND_place2d_*`, procedurals, ...), or another surface, is prepared through UE's MaterialX importer ([automatic import](../materials.md#automatic-import-of-untranslatable-graphs)) and renders translated until then: that node leaves the input at its constant, reported once as a warning. A multiply of two nodes is such a node too. Textured weights (`base`, `sheen`, `subsurface`, ...) also use their constant.

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
| `opacity` (`geometry_opacity`) | As UE's Interchange imports MaterialX: an image, or a constant below 1, is a cutout (masked) at UMaterial's default clip value 0.3333; a standard_surface with `transmission` is see-through instead (OpenPBR's `geometry_opacity` stays the cutout). The MaterialX spec, and Karma, render opacity as fractional coverage with no threshold, so moving a scene between them calls for material adjustments: Lumen reads the cutout from mipmaps, where thin foliage averages below the clip and would thin with distance. hdUE therefore scales the mips of every texture that drives a cutout (MaterialX opacity, UsdPreviewSurface with `opacityThreshold`) to keep its alpha coverage at the clip value, as UE's "scale mips for alpha coverage" texture setting does, with three fixes to UE's algorithm (`ComputeAlphaScale` in `TextureCompressorModule.cpp`): each level's scale is measured on that level rather than the previous one, the closest tested scale is kept rather than an untested midpoint, and a level is only ever scaled up. A 9x-distant thin-branch card keeps 0.42 of its near coverage (plain mips 0.05, UE's version less than plain); the Path Tracer reads the top mip and is unaffected |
| `transmission` (`transmission_weight`), `transmission_color`, `transmission_depth` | Refraction, as UE's Interchange imports a transmissive MaterialX surface: the translucent Standard Surface parents (UE's `Substrate-StandardSurface-Translucent` function, coloured transmittance, the material's Refraction from the function's IOR output, Index Of Refraction method, per-pixel lighting), in realtime and in the Path Tracer, which refracts only with that method. Ignored with an OpenPBR `geometry_opacity` cutout |
| `normal` (`geometry_normal`) | Tangent-space normal map |

Materials with `subsurface`, `sheen` or a clear coat above 0 render with the layered Standard Surface parents: UE's own `Substrate-StandardSurface-Opaque` material function (the one UE's Interchange uses for MaterialX), fed the same texture slots, in four cooked variants (opaque or masked, solid or thin-walled; `thin_walled` is a static input of the function). Everything else, including standard_surface materials without subsurface, sheen or coat, uses the single-slab Preview Surface parents. The layered function mixes metal and dielectric slabs and layers coat and sheen vertically, so it costs several Substrate closures per pixel where Preview Surface costs one; keeping it to the materials that need it bounds that cost. In the Sponza gallery shot (960 x 540, Lumen, curtains covering about half the frame), the layered curtains cost about 1 to 2 ms per frame over the same curtains on Preview Surface (husk, 1500 realtime frames, measured twice).

Limits: `subsurface_color` and `sheen_color` follow an image only when it is `base_color`'s (the usual authoring; their tint is kept), else they use their constant. Subsurface uses Substrate's per-material mean free path; UE subsurface profiles are not created at runtime. One UV transform per material, from the first textured input. `diffuse_roughness`, anisotropy, thin film, `coat_IOR`, `coat_normal`, `transmission_*` colours and dispersion are not translated. Transmissive materials, see-through materials and translucent sorting follow the Preview Surface translucent parent, which has no subsurface or sheen.
