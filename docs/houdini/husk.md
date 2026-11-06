# Rendering with husk

husk is Houdini's command-line USD renderer. With hdUE installed, `husk --renderer "Unreal Engine"` renders a USD file with Unreal Engine, using the render settings saved in the file. The USD Render ROP runs husk the same way, so the viewport, the ROP and a farm render the same image.

## Command lines

Render frames 1 to 24 of a USD file:

```powershell
husk --renderer "Unreal Engine" -f 1 -n 24 -o render.$F4.exr scene.usd
```

| Option | What it does |
| --- | --- |
| `--renderer "Unreal Engine"` | Renders with hdUE. |
| `-f 1` | First frame. |
| `-n 24` | Number of frames. |
| `-o render.$F4.exr` | Output image, overriding the render products' names. |
| `--settings /Render/rendersettings` | The render settings prim, when the file holds several. |
| `-V 3` | Prints progress as it renders (see [Progress](#progress-and-logs)). |
| `--tile-count 2 2 --tile-index 0` | Renders one tile of a 2 x 2 grid. |

Relative output paths resolve from the folder you run husk in, as with other renderers.

### From Houdini

1. Add a **USD Render ROP** LOP after your render settings, or a USD Render node in `/out`.
2. Set **Render Delegate** to **Unreal Engine**.
3. Turn on **Render All Frames with a Single Process**.
4. Click **Render to Disk**.

Step 3 matters: each husk process starts Unreal Engine once. See [Frame ranges](#frame-ranges-and-farms).

## Frame ranges and farms

Unreal Engine starts once per husk process, and stays until the process exits. Between frames, husk sends only what changed. Render a frame range in one husk call, and give farm tasks chunks of frames, not one frame each.

On an RTX 4090, measured on scenes of this repository:

| Scene | Startup per process | Per frame after the first |
| --- | --- | --- |
| Cornell box, 512 x 512, Realtime | about 3 s | about 50 ms |
| Intel's Sponza, 2560 x 1440, Realtime, animated camera and sun | about 30 s (UE and the scene's meshes) | about 0.3 s, of which 0.2 s is husk writing the float EXR |

[Nanite](../geometry/nanite.md) builds are cached on disk across processes, so the second husk run of a scene skips most of its mesh building.

### Realtime sequences

In Realtime, husk renders a frame range the way Movie Render Queue renders a shot:

1. The first frame renders **Warmup Frames** (`unreal:warmupFrames`, default 64) frames, so Lumen, anti-aliasing and eye adaptation settle.
2. Each later frame draws once and keeps that temporal history.

A sudden change between frames, such as a light switched on, therefore catches up over the next few frames, as in Unreal Engine. Render each cut as its own husk call, or pass husk's `--restart-delegate <n>`, which restarts the renderer every n frames; each restart warms up again.

### Farm setup

Each render node needs the hdUE package installed ([Installation](../getting-started/installation.md)), a Houdini 22.0 build the package supports, and a DirectX 12 GPU with hardware ray tracing.

## Render products and AOVs

husk writes every render product of the render settings. Use hdUE's LOPs to author them:

- **hdUE Render Settings**: **Output Picture** and one toggle per AOV on its Image Output tab.
- **hdUE Standard Render Vars**: the beauty, depth, normals, lighting components, IDs and Cryptomatte, each with its precision.
- **hdUE Render Products**: several output images, each with its render vars.

See [AOVs](../output/aovs.md#aovs-in-houdini) for what each AOV holds and costs, and [Alpha](../output/alpha.md) for the alpha channel.

husk also honours the render settings' data window (crops and overscan), the pixel aspect ratio and husk's tiling (`--tile-count`, `--tile-index`, `--autotile`): each render covers only its window ([Cameras](../cameras.md)). A single render is at most 16384 pixels a side; render larger images in tiles.

### Output format

The EXR is husk's. Writing it costs more than the compression choice. Measured on the Cornell box at 2560 x 1440, per frame of a sequence:

| Beauty format | Compression | Time per frame | File size |
| --- | --- | ---: | ---: |
| Float (`color4f`) | ZIP | 269 ms | 9.4 MB |
| Float | none | 257 ms | |
| Half (`color4h`) | ZIP | 195 ms | 5.2 MB |
| Half | none | 194 ms | |
| Half | PIZ | 249 ms | |

hdUE's part is about 70 ms of each; the rest is husk. For a cheaper beauty, set the beauty render var's `dataType` and `driver:parameters:aov:format` to `color4h`.

## Progress and logs

| husk verbosity | Prints |
| --- | --- |
| `-V 2` | Each frame's start, and its render time at the end. |
| `-V 3` | Progress while rendering: Path Tracer samples or Realtime warmup, Unreal Engine startup, scene upload, textures and mesh builds with the time left. |
| `-V 5` | Also the scene statistics after each frame. |

husk's output also shows every Unreal Engine warning and error, each warning once. For every line of Unreal Engine's log, set `HDUE_UE_ARGS=-stdout`. The full log is in `Saved\Logs` under `HDUE_USER_DIR` ([Environment variables](../getting-started/environment-variables.md)).

## Exit codes and failures

husk exits with a non-zero code when a frame fails, so a farm manager marks the task as failed. That happens when:

- Unreal Engine fails to render the frame;
- none of the requested render vars is one hdUE renders;
- a material the frame needs could not be prepared ([Substrate materials](substrate-materials.md)).

| Symptom | Cause | Fix |
| --- | --- | --- |
| An AOV is empty (its clear value), with a warning in husk's output. | hdUE does not render that render var: a light path expression other than the beauty's, or a primvar. | Use an AOV from the [AOV table](../output/aovs.md). |
| `Resolution ... is outside 1..16384, the largest texture this GPU's RHI renders` | The image is larger than Unreal Engine's largest render target. | Render in tiles: `--autotile --tile-count 2 2`. |
| `hdUE warning: hdUE was built against Houdini ...` | The render node runs another Houdini 22.0 build than the package's. | See [Installation](../getting-started/installation.md#troubleshooting). |
| Each frame takes several seconds longer than expected. | One husk process per frame: Unreal Engine starts each time. | Render frame ranges per process (see [Frame ranges](#frame-ranges-and-farms)). |

## Unreal Engine and Karma

- Command lines are Karma's with `--renderer "Unreal Engine"`; the same USD file can hold both renderers' settings.
- In Realtime, sequence frames reuse their history as Movie Render Queue does; Karma renders each frame independently.
