# Debug views

`unreal:debugView` (**Debug View** under Rendering, in the viewport's display options and on Render Settings LOPs) replaces the realtime beauty with one of the view modes of UE's viewport **View Mode** menu, set the way UE's game viewport sets them (view mode show flags, the view's visualization name and material overrides). Values are `<mode>` or `<mode>:<visualization>`, with the names UE's `r.*.Visualize` console variables take:

| Value | Shows |
| --- | --- |
| `off` | the lit image (default) |
| `unlit`, `lightingOnly`, `detailLighting`, `reflections` | UE's Unlit, Lighting Only, Detail Lighting and Reflections modes |
| `lightComplexity` | lights affecting each pixel |
| `buffer:BaseColor`, `Metallic`, `Roughness`, `Specular`, `WorldNormal`, `SceneDepth`, `ShadingModel` (any `[Engine.BufferVisualizationMaterials]` entry) | a GBuffer channel |
| `nanite:Mask`, `Triangles`, `Clusters`, `Primitives`, `Instances`, `Overdraw` (any Nanite visualization) | Nanite data; only Nanite meshes show (the viewport's converted meshes and native Nanite assets, not husk's) |
| `lumen:Overview`, `LumenScene`, `SurfaceCache`, `GeometryNormals`, `ReflectionView` | Lumen's scene and surface cache |
| `vsm:mip`, `vpage`, `cache` (any Virtual Shadow Map visualization) | Virtual Shadow Map pages |

Buffer views are linear data, like AOVs. The other views are drawn for display through UE's tonemapper, as with `unreal:tonemapper` (set the viewport colour correction to raw to see them as UE does); lighting views take the camera exposure before it. Debug views apply to the realtime beauty only: AOVs and Path Tracer renders are unchanged.

Not offered, because this runtime is a cooked non-editor build: Wireframe (UE composites it in the editor-primitives pass, compiled into editor builds only), Shader Complexity, Quad Overdraw, LOD coloration and the other material debug modes (their shaders are cooked only with `r.ForceDebugViewModes=1`, for every material), and the Lightmap modes (no static lighting). Cooked runtimes also leave UE's buffer visualization data uninitialized, so hdUE blends the buffer materials (`/Engine/BufferVisualization`, cooked through `DirectoriesToAlwaysCook`) as post-process materials itself.
