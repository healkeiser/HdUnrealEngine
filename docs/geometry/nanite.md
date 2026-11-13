# Nanite

Nanite is UE's virtualized geometry: it draws each mesh with about as many triangles as the image has pixels, whatever the mesh's own count. hdUE turns the static meshes of your stage into Nanite meshes, so scenes of hundreds of millions of triangles render in Realtime.

SideFX's shader ball lit, then in UE's Nanite views: each colour is one triangle, then one cluster of triangles.

<div class="hdue-figure" data-figure="nanite-views"></div>

## When hdUE uses Nanite

hdUE decides as UE's USD Stage Actor does: a mesh with at least **Nanite Triangle Threshold** triangles (`unreal:naniteTriangleThreshold`, default 1,000,000, UE's default) builds Nanite data; a smaller one stays a regular static mesh. Instancer prototypes always build Nanite: without it, every instance of a scattered prototype draws all its triangles.

| Mesh | Nanite |
| --- | --- |
| Static meshes from your stage, at or above the threshold | yes |
| Static meshes below the threshold | no, as in UE's USD import |
| Instancer prototypes (PointInstancer, native instancing) | yes |
| Cooked UE meshes (**Native Asset**) with Nanite data | yes |
| Meshes with a translucent material | no, as in UE |
| Animated or edited meshes (deforming points, in the viewport while you edit) | no: they stay regular meshes |
| Meshes with a wet surface | no |

Identical meshes share one Nanite mesh, as UE's USD importer shares one asset between identical prims.

**Batch renders** (husk, the USD Render ROP) build all Nanite data before the first frame.

**Houdini's viewport** draws regular meshes first, builds Nanite data in the background, then swaps each mesh, one per frame. The viewport keeps drawing meanwhile and settles once the last mesh is in. On large scenes some meshes appear only when their build is swapped in: hdUE draws the regular meshes first only while they fit in 2 GB of GPU memory.

## Nanite settings

These are UE console variables, on **hdUE Render Settings** > Engine Quality tab (**Show: Advanced**).

| Parameter | Default | What it does |
| --- | --- | --- |
| **Nanite** ([`unreal:cvar:r:Nanite`](../reference/render-settings/engine-quality.md#unreal-cvar-r-nanite)) | On | Draws Nanite meshes with Nanite. Off, UE draws their fallback meshes |
| **Max Pixels per Edge** ([`unreal:cvar:r:Nanite:MaxPixelsPerEdge`](../reference/render-settings/engine-quality.md#unreal-cvar-r-nanite-maxpixelsperedge)) | 1 | The triangle edge length Nanite aims for, in pixels (Realtime only) |
| **Nanite Ray Tracing** ([`unreal:cvar:r:RayTracing:Nanite:Mode`](../reference/render-settings/engine-quality.md#unreal-cvar-r-raytracing-nanite-mode)) | Fallback Mesh (0) | What ray tracing sees: the fallback mesh (0) or the full Nanite geometry streamed for the view (1) |
| **Require Nanite** ([`unreal:requireNanite`](../reference/render-settings/rendering.md#unreal-requirenanite)) | Off | Cooked native meshes must render with Nanite |

## Max Pixels per Edge

**Max Pixels per Edge** (`unreal:cvar:r:Nanite:MaxPixelsPerEdge`) · Default 1 · Range 0.25 to 8

Higher values draw fewer, larger triangles: faster, with coarser silhouettes. The Path Tracer does not use it.

<div class="hdue-figure" data-figure="nanite-pixels-per-edge"></div>

## Ray tracing and fallback meshes

Nanite draws the rasterized image. Ray tracing (Lumen's reflections and the Path Tracer) traces each Nanite mesh's **fallback mesh** instead: a simplified copy UE's Nanite builder makes, at UE's default error (Fallback Target Auto). On most meshes the difference does not show. On thin geometry, such as foliage, the fallback is thinner and sparser in the Path Tracer and in Lumen's reflections.

Set **Nanite Ray Tracing** to 1 to trace the full Nanite geometry streamed for the view. It costs time: Intel Sponza's tree took 65 s instead of 25 s for a 256-sample husk frame, load included.

## Cost and memory

Building Nanite data is most of a scene's load time. hdUE caches each build on disk, as UE's derived data cache does for its static meshes:

- The cache is `%LOCALAPPDATA%\hdUE\Saved\NaniteCache`, or `Saved/NaniteCache` under `HDUE_USER_DIR` when it is set.
- A cache entry not used for a week is removed.
- 450 meshes of 9,800 triangles: 10.5 s to build, 0.7 s from the cache (75 MB on disk).
- NVIDIA's Da Vinci Workshop (617 million triangles, 6,750 meshes): 11 minutes in husk with an empty cache (3.6 GB written), 2 minutes from it, with a peak of 22 GB of memory.

On the GPU, each Nanite mesh keeps one root page resident; the rest stream in from system memory when the view needs them, into UE's 512 MB streaming pool (`r.Nanite.Streaming.StreamingPoolSize`). When a view needs more than the pool holds, Nanite draws coarser clusters, as UE does. The root pages share one GPU buffer of at most 2 GB on NVIDIA (4 GB on AMD), less the streaming pool: tens of thousands of meshes. Near that limit, hdUE renders the remaining meshes as regular static meshes and writes a warning to the UE log.

In Houdini's viewport, background builds start only while their memory fits. The console variable `r.HdUE.MeshBuildMaxMemoryUsage` (in GB, default 0: all available memory) caps them lower; author it as `int unreal:cvar:r:HdUE:MeshBuildMaxMemoryUsage` on the render settings. In a Windows job with a memory limit, such as a farm container, the job's limit counts as the available memory.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| First husk frame takes minutes | Nanite data is being built; the cache is empty | Render again: later renders read the cache. Keep `Saved/NaniteCache` on the farm, or point `HDUE_USER_DIR` at a shared disk |
| Meshes pop in one by one in the viewport | Nanite builds are swapped in as they finish | Wait for the viewport to settle |
| A mesh looks faceted in the Path Tracer but smooth in Realtime | The mesh is Nanite (at or above **Nanite Triangle Threshold**, or an instancer prototype), and ray tracing sees its fallback mesh | Raise **Nanite Triangle Threshold** above the mesh's triangle count |
| Foliage looks thinner in the Path Tracer than in Realtime | Ray tracing sees the fallback mesh of an instanced or very dense mesh | Set **Nanite Ray Tracing** to 1 (its normals can show faint streaks in glossy reflections) |
| Some meshes render without Nanite on a huge scene; the UE log warns | The root page buffer is nearly full | Merge small distinct meshes: each distinct mesh keeps a root page, while identical meshes share one |
| Distant detail looks soft on a huge scene | The view's geometry overflows the streaming pool, so Nanite draws coarser clusters | Render at a lower resolution, or reduce unique geometry |
| A translucent mesh is not in the Nanite views | Translucent materials do not use Nanite, as in UE | Expected |
| Viewport runs out of memory while loading | Too many background builds at once | Set `unreal:cvar:r:HdUE:MeshBuildMaxMemoryUsage` to a few GB |

To check which meshes are Nanite, set **Debug View** (`unreal:debugView`) to **Nanite: Mask**: see [Debug views](../rendering/debug-views.md).

To render without Nanite, for comparison, set the environment variable `HDUE_NANITE=0` before starting Houdini or husk.

## Unreal Engine and Karma

hdUE builds Nanite meshes with UE's own Nanite builder and settings, as UE's static mesh build does, for the meshes UE's USD Stage Actor would make Nanite, and ray traces their fallback meshes as UE does by default. One departure: instancer prototypes are Nanite at any size. Karma has no Nanite: it renders every triangle of the mesh.
