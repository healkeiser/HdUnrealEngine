# Debug views

A debug view replaces the lit image with one of the view modes of UE's viewport **View Mode** menu: lighting only, a GBuffer channel, Lumen's scene, shadow map pages. Use them to find out why an image looks the way it does.

The Bistro in UE's lighting view modes.

<div class="hdue-figure" data-figure="debug-views-modes"></div>

## Using it in Solaris

1. Set **Render Mode** to Realtime: debug views apply to the Realtime beauty only.
2. In the viewport's **Display Options > Unreal Engine** tab, or on your Render Settings LOP (**Rendering > View Mode**, Advanced), set **Debug View**.
3. Set it back to **Lit** when you are done. A debug view set on a Render Settings LOP renders in husk too.

**Debug View** (`unreal:debugView`), default Lit (`off`). Realtime only, Advanced.

AOVs and Path Tracer renders are unchanged by a debug view.

## Lighting views

| Value | Label | Shows | Use it to |
| --- | --- | --- | --- |
| `off` | Lit | the final lit image | |
| `unlit` | Unlit | base colour and emission, no lighting | check textures and material assignments |
| `lightingOnly` | Lighting Only | lighting on a neutral grey surface | judge light placement and bounce light without textures |
| `detailLighting` | Detail Lighting | lighting with the materials' normal maps | check normal maps |
| `reflections` | Reflections | reflections on smooth chrome-like surfaces | find what each surface reflects, and missing reflections |
| `lightComplexity` | Light Complexity | how many lights reach each pixel, cool to hot | find areas where many overlapping lights cost time |

These views, and the Lumen, shadow and Nanite views below, are display images drawn through UE's tonemapper, as in UE's viewport; the lighting views take the camera exposure first. To see them as UE shows them, set the Houdini viewer's colour correction to Raw.

## Buffer views

A buffer view shows one channel of UE's GBuffer, the per-pixel material data the lighting reads.

| Value | Shows |
| --- | --- |
| `buffer:BaseColor` | base colour |
| `buffer:Metallic` | metallic |
| `buffer:Roughness` | roughness |
| `buffer:Specular` | specular |
| `buffer:WorldNormal` | world-space normal |
| `buffer:SceneDepth` | distance from the camera |
| `buffer:ShadingModel` | UE's shading model, one colour per model |

Buffer views are linear data, like AOVs. Use them to check that a USD or MaterialX input reaches UE with the value you authored, for example a roughness texture.

<div class="hdue-figure" data-figure="debug-views-buffers"></div>

## Lumen, shadow and Nanite views

| Value | Shows | Use it to |
| --- | --- | --- |
| `lumen:Overview` | Lumen's scene, surface cache and geometry side by side | a first look at what Lumen sees |
| `lumen:LumenScene` | the simplified scene Lumen traces against | find objects missing from Lumen's bounce light |
| `lumen:SurfaceCache` | Lumen's surface cache; pink areas have no coverage | find surfaces that bounce no light |
| `lumen:GeometryNormals` | the normals of Lumen's scene | check Lumen's geometry against the real one |
| `lumen:ReflectionView` | the scene Lumen reflections trace against | debug wrong or missing reflections |
| `vsm:mip`, `vsm:vpage`, `vsm:cache` | Virtual Shadow Map mip levels, virtual pages and cached pages | find blurry or flickering shadows; needs Virtual Shadow Maps on |
| `nanite:Mask`, `nanite:Triangles`, `nanite:Clusters`, `nanite:Primitives`, `nanite:Instances`, `nanite:Overdraw` | Nanite's rendering data | check Nanite triangle density and coverage |

Nanite views show only Nanite meshes: the meshes Houdini's viewport converts to Nanite, and native Nanite assets. In husk, meshes from the stage are not Nanite, so these views show nothing for them. See [Nanite](../geometry/nanite.md).

<div class="hdue-figure" data-figure="debug-views-scene"></div>

## Not available

hdUE runs a cooked UE runtime, not the editor, so some editor view modes are missing:

- **Wireframe**: UE draws it in an editor-only pass.
- **Shader Complexity**, **Quad Overdraw**, **LOD Coloration** and the other material debug modes: their shaders are not cooked.
- **Lightmap** modes: hdUE has no static lighting.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The debug view does not change the image | **Render Mode** is Path Tracer | Switch to Realtime. |
| A Nanite view is black or empty | no Nanite meshes in view (husk, or meshes not converted) | Use it in Houdini's viewport, or on native Nanite assets. |
| A virtual shadow map view is empty | Virtual Shadow Maps are off | Turn **Virtual Shadow Maps** on ([Lighting > Shadows](../reference/render-settings/lighting.md#unreal-cvar-r-shadow-virtual-enable)). |
| A final husk render shows a debug view | **Debug View** left set on the render settings | Set it to **Lit**. |

## Unreal Engine and Karma

The values are UE's view modes and visualization names, as UE's `r.*.Visualize` console variables take them. To write material data to files instead, render [AOVs](../output/aovs.md), as you would with Karma.
