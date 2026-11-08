# MaterialX surfaces

hdUE renders MaterialX **Standard Surface** and **OpenPBR Surface** materials, the surfaces of Karma's and Houdini's MaterialX builders. It translates them at render time onto UE's own Standard Surface material functions, the ones UE's Interchange uses to import MaterialX, so edits show immediately, with no compile.

The figures on this page use the shader ball (`samples/docs/lookdev.usda`) in Realtime with a Standard Surface shader. Below: Preview Surface, then Standard Surface with transmission, subsurface and sheen.

<div class="hdue-figure" data-figure="materials-models"></div>

## Using it in Solaris

1. In a **Material Library** LOP, create a **Karma Material Builder** (or a MaterialX subnet) holding a **MaterialX Standard Surface** or **MaterialX OpenPBR Surface**.
2. Wire textures with **MaterialX Image** nodes, as below.
3. Assign the material. A material with both a MaterialX output and a Preview Surface output renders from its MaterialX graph.

If a graph uses a node outside the list below, UE's MaterialX importer prepares it in the background ([automatic import](../materials.md#automatic-import-of-untranslatable-graphs)). Until it is ready, that input renders at its constant value, with a warning naming the node.

## Base

| Standard Surface | OpenPBR | Default | In UE |
| --- | --- | --- | --- |
| **Base** `base` × **Base Color** `base_color` | `base_weight` × `base_color` | 1 × (0.8, 0.8, 0.8) | Base colour |
| **Metalness** `metalness` | `base_metalness` | 0 | Metalness, as Preview Surface **Metallic** |

`diffuse_roughness` is not translated.

## Specular

| Standard Surface | OpenPBR | Default | In UE |
| --- | --- | --- | --- |
| **Specular** `specular` × **Specular Color** `specular_color` | | 1 × (1, 1, 1) | Specular tint, on materials with coat, sheen or subsurface |
| **Specular Roughness** `specular_roughness` | `specular_roughness` | 0.2 | Roughness |
| **Specular IOR** `specular_IOR` | `specular_ior` | 1.5 | Dielectric reflectance, as Preview Surface **IOR** |

Anisotropy is not translated.

## Transmission

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Transmission</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`transmission` (OpenPBR `transmission_weight`)</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

Above 0, the surface refracts: hdUE uses UE's translucent Standard Surface material, as Interchange imports a transmissive MaterialX surface, in Realtime and in the Path Tracer. `transmission_color` tints the light passing through and `transmission_depth` (scene units) is the distance at which it reaches that colour. See [Opacity and glass](opacity-and-glass.md) for the settings, figures and recipes.

## Subsurface

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Subsurface</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`subsurface` (OpenPBR `subsurface_weight`)</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

Light enters the surface, scatters and exits nearby: skin, wax, marble, milk. hdUE renders it as Substrate subsurface scattering.

<div class="hdue-figure" data-figure="materialx-surfaces-subsurface"></div>

| Standard Surface | OpenPBR | Default | What it does |
| --- | --- | --- | --- |
| `subsurface_color` | `subsurface_color` | (1, 1, 1) | Colour of the scattered light |
| `subsurface_radius` | `subsurface_radius_scale` | (1, 1, 1) | Relative scattering distance per channel: red furthest for skin |
| `subsurface_scale` | `subsurface_radius` | 1 | Scattering distance, in scene units |

**Subsurface Scale** multiplies **Subsurface Radius** into a mean free path in scene units. In a stage in metres, the default 1 is a metre of scattering: the object looks like jelly. A 9 cm shader ball wants about 0.02.

<div class="hdue-figure" data-figure="materialx-surfaces-subsurface-scale"></div>

## Sheen

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Sheen</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`sheen` (OpenPBR `fuzz_weight`)</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

A soft, bright rim at grazing angles, from fibres: velvet, felt, dust. hdUE renders it as Substrate fuzz. `sheen_color` (default (1, 1, 1)) tints it and `sheen_roughness` (default 0.3) widens it.

<div class="hdue-figure" data-figure="materialx-surfaces-sheen"></div>

## Coat

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Coat</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`coat` (OpenPBR `coat_weight`)</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

A clear layer over the base: varnish, car paint, lacquer. `coat_roughness` (default 0.1) is its own roughness; `coat_color` (default (1, 1, 1)) tints what is under it.

<div class="hdue-figure" data-figure="materialx-surfaces-coat"></div>

`coat_IOR` and `coat_normal` are not translated: the coat's IOR is 1.5.

## Emission

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Emission × Emission Color</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`emission` × `emission_color` (OpenPBR `emission_luminance` × `emission_color`)</dd></div><div markdown><dt>Default</dt><dd markdown="span">0 × (1, 1, 1)</dd></div></dl>
</div>
</div>

</div>

Emitted light, as Preview Surface **Emissive Color**. OpenPBR's `emission_luminance` is used as a plain multiplier, not in nits.

## Geometry

| Standard Surface | OpenPBR | Default | In UE |
| --- | --- | --- | --- |
| **Opacity** `opacity` | `geometry_opacity` | 1 | A cutout (UE's Masked blend mode) at 0.3333, UE's default clip value, when it is a texture or a constant below 1 |
| **Thin Walled** `thin_walled` | `geometry_thin_walled` | off | A thin two-sided shell that lets its subsurface colour through: leaves, paper, curtains |
| **Normal** `normal` | `geometry_normal` | | Tangent-space normal map, through a **MaterialX Normal Map** node |

A Standard Surface with **Transmission** above 0 is see-through instead of cut out. See [Opacity and glass](opacity-and-glass.md).

## Nodes hdUE translates

Each surface input is a constant or reaches an image through these nodes. Anything else is imported by UE.

| Node | What hdUE does |
| --- | --- |
| **MaterialX Image**, **Tiled Image** (`uvtiling`, `uvoffset`), **USD UV Texture** | The texture, with its colour space and address modes |
| **Texture Coordinate**, **Geometry Property Value** (vector2), **USD Primvar Reader** | Selects the UV set (`st`, `map1`, ...) |
| **Place 2D**, **USD Transform 2D** | Transforms the UVs of the image it feeds |
| **Geometry Property Value** (color3, vector3) | Reads a vertex colour primvar |
| **Extract** | Picks one channel (alpha for cutouts: Image color4 → Extract index 3) |
| **Normal Map** | Tangent-space decode, with its `scale` |
| **Multiply** by a constant | Scales the texture |
| **Mix** with one of `fg`, `bg`, `mix` connected | Offsets and scales that input |
| **Convert**, **Dot**, **Constant** | Passed through, or the value |

Texture colour spaces follow the image's `colorspace`: `srgb_texture` decodes sRGB, `lin_rec709` and `raw` read linear.

## Nodes UE imports

Any other graph is imported in the background by UE's own MaterialX importer (Interchange), which reads MaterialX 1.39.4; Houdini 22 writes 1.39.5. Before the import, hdUE rewrites what UE's importer cannot read, so every node Houdini ships reaches UE except the ones in the last row:

| Nodes | What hdUE writes for UE |
| --- | --- |
| Houdini's MaterialX 1.38 nodes (`swizzle`, `atan2` with its old inputs, ...) | MaterialX's own upgrade (`upgradeVersion`) |
| Integer and boolean nodes (`and`, `or`, `not`, integer maths, conversions) | The same values as floats, as UE computes everything in floats |
| Matrix nodes (`creatematrix`, `transformmatrix`, `invertmatrix`, `determinant`, `transpose`, matrix maths) | Their scalar maths, with MaterialX's column-major conventions |
| `flake2d`, `flake3d` (MaterialX 1.39.5) | MaterialX's own flake code in a UE Custom node (`unreal/HdUEBackend/Shaders/MaterialXFlake.ush`), the same flakes for the same coordinates; a flake whose size, roughness or coverage comes from another node uses a graph of standard nodes instead (statistically the same flakes, much heavier to compile) |
| `geompropvalue`, `primvar` readers | The UV set or vertex colour hdUE uploads for the primvar; other primvars use their fallback value |
| `curveadjust` | Linear interpolation between the knots |
| `frame` | The frame at 24 fps |
| `ambientocclusion` | 1: UE's Lumen and Path Tracer occlude the lighting themselves |
| `disney_principled`, `thin_surface`, `generalized_schlick_edf`, a lone `thin_film` | The nearest node UE imports |
| Graphs UE's importer fails on as written | Written as it reads them: a mix of displacement shaders as one displacement of the mixed height; a Standard Surface mixed with another shader carries its `base` weight in `base_color` (UE's importer reads any input named `base` as a Substrate layer's bottom); inputs it has no slot for (thin film on BSDFs, `color82`, `thin_walled` on `surface`) left out, with a warning when set |
| Light shaders (`light`, `point_light`, `spot_light`, `directional_light`), `osl_set_ci`, arrays no node reads | Not imported: USD lights are UsdLux prims, and the others exist only in OSL or in no graph |

`scripts/Test-MaterialXCoverage.ps1` checks this: it wires each of the 898 node definitions Houdini ships into a material, exports it, and lists what UE would not import (`-Export -One -Only ND_<name>` writes one document per node for UE's importer).

## Recipes

How to get these looks on a Standard Surface. Inputs not listed stay at their default.

| Look | Inputs |
| --- | --- |
| Skin | `base_color` (0.8, 0.5, 0.4), `specular_roughness` 0.4, `subsurface` 1, `subsurface_color` (0.9, 0.45, 0.3), `subsurface_radius` (1, 0.35, 0.2), `subsurface_scale` 0.02 (a 9 cm object in metres; scale it with the object) |
| Velvet | `base_color` (0.3, 0.05, 0.1), `specular_roughness` 0.7, `sheen` 1, `sheen_color` (1, 0.85, 0.9), `sheen_roughness` 0.3 |
| Lacquered wood | wood texture into `base_color`, `specular_roughness` 0.5, `coat` 1, `coat_roughness` 0.05 |
| Leaf | leaf texture into `base_color`, `subsurface` 0.5, `subsurface_color` (0.4, 0.6, 0.1), `thin_walled` on; alpha through **Extract** into `opacity` |
| Glass | See [glass recipes](opacity-and-glass.md#recipes) |

### Layers that need no second texture

hdUE translates a MaterialX graph at render time when each input reads at most one texture. Two layers of `standard_surface` add detail within that limit:

- **A wet surface:** a water film is a clear coat. `coat` 1 (or a grunge map, for puddles), `coat_IOR` 1.33, `coat_roughness` 0.03, `coat_color` (0.7, 0.7, 0.72) to darken what is under the water, as wet stone darkens. Lower the surface's own roughness too.
- **Grime over any texture:** a coat of `coat_IOR` 1.0 reflects nothing and only tints what is under it. Feed `coat_color` from a `mix` between white and a dirt colour, driven by a grime map read in a second UV set (`geompropvalue` `uv2`): the dirt varies over the whole building instead of repeating with the texture's tile.

Both are used on the Bistro street (`scripts/convert_bistro.py`).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| An input renders flat, with a warning "MaterialX node ... is not translated" | A node outside the list, and the import is not finished or cannot run | Wait for the background import; on a machine without UE's editor, replace the node or cook the material ([cooked libraries](../materials.md#cooked-materialx-libraries)) |
| Subsurface looks like jelly or glows through the whole object | `subsurface_scale` is in scene units: the default 1 is a metre | Lower it to the object's scattering distance |
| A textured `base`, `sheen` or `subsurface` weight renders as its constant | Textured weights are not translated | Put the texture in the colour input instead |
| `subsurface_color` or `sheen_color` ignores its texture | They follow a texture only when it is `base_color`'s | Use `base_color`'s texture, or let UE import the graph |
| Leaves or foliage cut out too much or too little compared with Karma | UE cuts `opacity` at 0.3333; Karma blends fractional coverage | Adjust the alpha in the texture, or use a Preview Surface with your own **Opacity Threshold** |

## Unreal Engine and Karma

- hdUE follows UE's Interchange MaterialX import: the same `Substrate-StandardSurface` material functions, cutout opacity at UE's default clip value (0.3333), refraction on the translucent function.
- Karma renders `opacity` as fractional coverage with no threshold; the MaterialX specification does too.
- A material with coat, sheen or subsurface uses UE's layered Standard Surface function, which costs several Substrate closures per pixel. In the Sponza gallery shot (960 x 540, Lumen), layered curtains cost 1 to 2 ms per frame more than the same curtains on Preview Surface. Other Standard Surface materials cost what a Preview Surface does.
- Subsurface uses Substrate's per-material mean free path; UE subsurface profiles are not created. One UV transform per material, from the first textured input. Thin film and dispersion are not translated.
- Position-based nodes (`position`, `noise3d`, `flake3d`, ...) read UE's local position in metres, as UE's importer translates `position`. hdUE uploads meshes in UE's axes, so a 3D pattern can be turned compared with Karma's: same density and size, different cells.
