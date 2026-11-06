# AOVs

A render var selects an AOV by its `sourceName` (`raw`), or by its light path expression (`lpe`): Karma's names and expressions for the same output select the same AOV. The table is data in the delegate (`src/hydra/aov_table.h`), which the hdUE Standard Render Vars and hdUE Render Settings assets read. Every output comes from something UE 5.8 already provides:

- **Buffer visualizations** (`[Engine.BufferVisualizationMaterials]`, UE's Buffer Visualization view mode): all the frame's buffer AOVs render in **one** raster pass after the beauty converges, read back through Movie Render Queue's buffer visualization pipes (`FinalPostProcessSettings.BufferVisualizationPipes`, as `MoviePipelineDeferredPasses` does), from the beauty's camera without Lumen, anti-aliasing or post effects.
- **The Path Tracer's own albedo and normal**, which it accumulates for denoisers and hands post-process materials (`PathTracingBufferTexture`): read back from the converged beauty, at no extra cost.
- **Lighting components**: the Path Tracer's Lighting Components post-process settings (`PathTracingIncludeDiffuse`, `IndirectDiffuse`, `Specular`, `IndirectSpecular`, `Emissive`, `Volume`, `IndirectVolume`), which MRQ's render layers use; in realtime, the Diffuse, Specular, Direct Lighting and Global Illumination show flags of UE's Lighting Components menu. Each is a full render of the beauty (its mode, samples, warmup, Lumen and exposure): UE has no light path expressions, so components do not come out of one render.
- **Cryptomatte**: MRQ's Object ID pass recipe (MurmurHash3 ids, sub-pixel spatial samples, `cryptomatte/<key>/*` metadata); see [Houdini, Cryptomatte](#cryptomatte).

| Render var (`sourceName`) | Karma name / LPE | Data type | UE source | Realtime | Path traced |
| --- | --- | --- | --- | --- | --- |
| `albedo` (`baseColor`) | `CDA` | color3f | `BaseColor` buffer; path traced, the Path Tracer's albedo (diffuse and specular reflectance, followed through mirrors and glass) | yes | yes |
| `normal` | `N` (`ray:hitN`) | normal3f | `WorldNormal` buffer; path traced, the Path Tracer's normal | yes | yes |
| `roughness`, `metallic`, `specular`, `occlusion` | | float | `Roughness`, `Metallic`, `Specular`, `MaterialAO` buffers | yes | yes |
| `ambientOcclusion` | `AO` | float | `AmbientOcclusion` buffer (screen-space AO of the raster pass) | yes | yes |
| `opacity` | | float | `Opacity` buffer (UE's GBuffer custom data: subsurface and foliage opacity, 0 for other surfaces) | yes | yes |
| `subsurfaceColor` | | color3f | `SubsurfaceColor` buffer | yes | yes |
| `anisotropy` | | float | `Anisotropy` buffer | yes | yes |
| `tangent` | | normal3f | `WorldTangent` buffer, USD axes | yes | yes |
| `shadingModel` | | color3f | `ShadingModel` buffer (the colour code of UE's Shading Model view) | yes | yes |
| `customDepth`, `customStencil` | | float | `CustomDepthWorldUnits`, `CustomStencil` buffers; hdUE meshes do not render custom depth, so far and 0 | yes | yes |
| `velocity` | | color3f | Motion vectors as Movie Render Queue writes them: UE's `Velocity` buffer, the motion since the frame before in R (x) and G (y), 0.5 for none, v × 0.2495 + 0.5 for a motion of v in NDC (2 is the whole width). The buffers pass first renders the frame before (camera and meshes one frame back, not published); deforming meshes and volumes show no motion of their own | yes | yes |
| `combineddiffuse` (`diffuse`) | `C<RD>.*L` | color3f | Diffuse + Indirect Diffuse components | yes | yes |
| `directdiffuse` | `C<RD>L` | color3f | Diffuse component | yes | yes |
| `indirectdiffuse` | `C<RD>.+L` | color3f | Indirect Diffuse component (realtime: Lumen GI) | yes | yes |
| `combinedglossyreflection` (`glossy`) | `C<RG>.*L` | color3f | Specular + Indirect Specular components (UE's specular includes transmission) | yes | yes |
| `directglossyreflection` | `C<RG>L` | color3f | Specular component | yes | yes |
| `indirectglossyreflection` | `C<RG>.+L` | color3f | Indirect Specular component (realtime: Lumen reflections) | yes | yes |
| `directemission` (`emission`) | `CO` | color3f | Emissive component | yes | yes |
| `combinedvolume`, `directvolume`, `indirectvolume` (`volume`) | `CV.*L`, `CVL`, `CV.+L` | color3f | Volume and Indirect Volume components | warned, cleared | yes |
| `primId`, `instanceId` | | int | hdUE's unlit ID passes | yes | yes |
| `CryptoObject<NN>`, `CryptoMaterial<NN>` | | float4 | 16 sub-pixel ID passes (Cryptomatte) | yes | yes |
| `depth`, `cameraDepth` | | float | the beauty's scene depth | yes | yes |

Costs per frame: the buffer AOVs together, one short raster render; each lighting component, one more full render of the beauty; each ID AOV one short raster render, Cryptomatte 16; path-traced albedo and normal nothing. Realtime components are approximations: emission stays in every component (alone it is the image with diffuse and specular off) and volumes are not split. A motion-blurred Path Tracer beauty gives raster albedo and normal (its sub-frames each show another pose); lighting components render without motion blur. Other render vars (other light path expressions, primvars, unknown names) and AOVs a mode cannot produce receive their clear value, with one warning each.

UE buffer visualizations hdUE does not offer: `SceneColor`, `PreTonemapHDRColor`, `PostTonemapHDRColor`, `FinalImage`, `SeparateTranslucencyRGB`/`A` (states of the beauty, which is `C`; in the raster pass they would show its unlit image) and `SceneDepth`/`SceneDepthWorldUnits` (`depth` is the beauty's, in scene units).

## AOVs in Houdini

The beauty is the render var whose `sourceName` is `color`, `C`, `Ci` or `beauty` (`raw`), or the `lpe` source `C.*[LO]` (Karma's) or `C.*`. Besides the beauty and `depth`, render vars select the AOVs in the [AOV table]() by `sourceName` or light path expression: surface buffers (`albedo`, `normal`, `roughness`, ... from UE's buffer visualizations), lighting components (`combineddiffuse`, `directglossyreflection`, ... Karma's names and expressions), `primId`, `instanceId` and [Cryptomatte](#cryptomatte) layers. The delegate matches a binding by its `sourceName`, so the render var and channel may be named anything. hdUE Standard Render Vars creates them by section (Surface, Lighting, Utility, Cryptomatte); hdUE Render Settings offers them under Image Output.

After the beauty converges, the bound AOVs render from the same camera and time, each kind as cheaply as UE allows: every surface buffer together in one raster render with Lumen, anti-aliasing and post effects off (so the channels stay exact and pixel-aligned with a Path Tracer beauty), each lighting component as a render of the beauty, each ID AOV as a raster render. Surface buffers describe the nearest opaque surface at the current time (without motion blur). AOV renders use their own UE view state and frame slot (`include/hdue/bridge.h`), so the beauty's Lumen or Path Tracer history is unaffected.

In path-traced mode, `albedo` and `normal` are instead the Path Tracer's own buffers, which it accumulates for denoisers and hands to post-process materials (`PathTracingBufferTexture`): averages over its samples, so they are anti-aliased and carry the beauty's depth of field, and they cost nothing extra. The albedo is the denoiser albedo (diffuse and specular reflectance, followed through mirror and glass hits), not the base colour. hdUE reads them back from the converged beauty and answers the AOV request with them when it shows the same view; a motion-blurred beauty (whose sub-frames each show another pose) falls back to the raster GBuffer channels. UE's denoisers use the same buffers as their guides.

`primId` and `instanceId` come from raster passes in which every mesh emits its prim ID from custom primitive data, and instanced meshes their Hydra instance index from per-instance custom data. The materials hdUE generates switch to that emission themselves (custom primitive data 5), keeping their cutout opacity, so the pass costs no render-state rebuild: the Preview Surface and Standard Surface parents, `M_Glass`, and prepared MaterialX and Substrate Builder materials (`IdSwitch.cpp` zeroes their lobes, sets their coverage to 1 and puts the ID in their emission, or for UE's OpenPBR function, which scales its emission, mixes in an ID slab; a prepared graph with a node it does not know, such as a Substrate Add, keeps the swap). Meshes drawn with a cooked native material, which hdUE cannot change, draw `M_HdUEId` for the pass, which ignores cutout opacity (masked foliage selects as its whole card); UE's own Object ID pass draws hit proxies, which only editor builds compile (`SceneHitProxyRendering.cpp`). `unreal:cvar:r:HdUE:IdSwap = 1` swaps every mesh, to compare the two. Meshes placed from the native asset catalog read as background. Because AOVs follow the converged beauty, a viewer that tints its selection from `primId` (usdview, Solaris) can show the previous frame's IDs for a moment after an edit.

### Cryptomatte

Render vars named `CryptoObject<NN>` and `CryptoMaterial<NN>` (`float4`, 32-bit: the ids are float bit patterns, a half format destroys them) hold layer NN of a [Cryptomatte](https://github.com/Psyop/Cryptomatte) (specification 1.2): ranks 2NN and 2NN + 1 as (id, coverage) pairs in RGBA, most coverage first. Three layers (`00` to `02`, six ranks) are the usual set; hdUE Standard Render Vars adds them with **Crypto Object** and **Crypto Material**. An id is the MurmurHash3_32 of the prim's scene path (`CryptoObject`) or of its bound material's path (`CryptoMaterial`, none for unbound `displayColor` geometry), as UE's Movie Render Queue Object ID pass hashes names (`MoviePipelineMaskRenderPass`).

Coverage comes from 16 ID passes after the beauty converges, each shifted by a sub-pixel offset of a 4 x 4 grid, as MRQ accumulates its spatial samples: an id's coverage is its share of the 16 samples (a box filter). The ID passes are rasterized like `primId`, so they ignore depth of field, motion blur, transparency and (on cooked native materials) cutout opacity, and a GeomSubset's material reads as its mesh's. The rest of the image is unaffected; the cost is 16 short raster renders per frame.

husk writes the Cryptomatte metadata of each layer (`cryptomatte/3ae39a5/...` for `CryptoObject`, `cryptomatte/be359d6/...` for `CryptoMaterial`: `name`, `hash`, `conversion` and a `manifest` of the names in the frame) into every image of the frame, through `husk.metadata` in `UsdRenderers.json` from the delegate's render stats. Karma's Cryptomatte LOP does not apply: it authors one Karma-specific render var per layer (`CryptoPrimitives`, `primvar` source `__name`, `driver:parameters:aov:karma:cryptomatte`), which hdUE does not read; Houdini's documentation leaves Cryptomatte to each delegate.
