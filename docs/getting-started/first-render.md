# Your first render

This page renders the Cornell box that ships in `samples/cornell`, first in the Solaris viewport, then to disk with husk.

## In the Solaris viewport

1. In a LOP network, add a **Reference** LOP and point it at `samples/cornell/cornell.usda` from the hdUE folder.
2. In the Scene Viewer's renderer menu, pick **Unreal Engine**. The first selection starts Unreal Engine in Houdini's process, which takes a few seconds; later selections reuse it.
3. Add a **Render Settings** LOP. Its **Unreal Engine** tab holds every hdUE setting, laid out as Unreal Engine's editor lays them out. Above the tabs: the render mode, the Path Tracer samples or the Realtime warmup frames, the Unreal Tonemapper and the exposure compensation.

**Render Mode** picks between Unreal's two renderers:

- **Realtime (Lumen)**: UE's deferred renderer with Lumen global illumination and reflections. It is meant for final frames too.
- **Path Tracer**: UE's reference path tracer, accumulated to the sample count.

See [Solaris viewport](../houdini/viewport.md) for the Unreal menu and viewport performance, and [Render settings](../rendering/render-settings.md) for what each mode does.

## To disk with husk

```powershell
husk --renderer "Unreal Engine" -f 1 -n 24 -o render.$F4.exr scene.usd
```

The render settings travel with the USD file, so a farm renders what the viewport showed. Render one frame range per husk call and give farm tasks chunks of frames: each process starts Unreal Engine once (about 3 s). See [Rendering to disk with husk](../houdini/husk.md).

## Outputs

The beauty is scene-linear unless the Unreal Tonemapper is on. Render vars add AOVs (albedo, normals, lighting components, IDs, Cryptomatte): see [AOVs](../output/aovs.md).
