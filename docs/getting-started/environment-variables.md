# Environment variables

Most installs need no variable set by hand: `Install.cmd` writes a Houdini package file that sets the ones hdUE needs. The others are switches for troubleshooting and performance tests. Set them before you start Houdini or husk; they are read once per process.

## What each one is for

| You want to | Use |
| --- | --- |
| Run hdUE at all | `HDUE_RUNTIME_CONFIG`, `HDUE_HOUDINI_ENABLE` (set by the package) |
| Keep logs and the shader cache out of a shared install folder | `HDUE_USER_DIR` (set by the package) |
| See where a frame's time goes | `HDUE_TRACE_TIMING`, `HDUE_TRACE_FILE` |
| Check what reaches Unreal Engine | `HDUE_TRACE_SETTINGS`, `HDUE_TRACE_SCENE`, `HDUE_TRACE_PASS` |
| Rule out a viewport or Nanite problem | `HDUE_GPU_FRAMES=0`, `HDUE_NANITE=0` |
| Pass arguments to Unreal Engine | `HDUE_UE_ARGS` |

## Variables

--8<-- "README.md:environment-variables"

The package file also sets:

| Variable | Description |
|----------|-------------|
| `HDUE` | The package folder. Use it in paths: `$HDUE/samples/cornell/cornell.usda` |
| `HDUE_VERSION` | The package's hdUE version |

These are read too, and not set by the package:

| Variable | Description |
|----------|-------------|
| `HDUE_TRACE_TIMING` | `1` prints each frame's timeline: request to frame, readback, setting changes, scene uploads |
| `HDUE_TRACE_FILE` | File that receives the `HDUE_TRACE_TIMING` output instead of the console. Houdini's UI shows no console output, so set it there |
| `HDUE_UE_ARGS` | Arguments appended to Unreal Engine's command line. `-stdout` echoes every UE log line to the console |
| `HDUE_SHOW_WINDOW` | `1` opens Unreal Engine's window; by default it renders offscreen |
| `HDUE_VIRTUAL_TEXTURE_CACHE` | Folder for the UDIM virtual texture cache, instead of `%LOCALAPPDATA%\hdUE\VirtualTextureCache` |

## Examples

Trace the timeline of a husk render:

```powershell
$env:HDUE_TRACE_TIMING = '1'
husk --renderer "Unreal Engine" -f 1 -n 4 -o render.$F4.exr scene.usd
```

Trace the viewport in Houdini, to a file:

```powershell
$env:HDUE_TRACE_TIMING = '1'
$env:HDUE_TRACE_FILE = "$env:TEMP\hdue-trace.txt"
houdini
```

Record a GPU profile of 600 frames with Unreal Engine's CSV profiler (written to `Saved\Profiling` under `HDUE_USER_DIR`):

```powershell
$env:HDUE_UE_ARGS = '-csvGpuStats -csvCaptureFrames=600'
```

Unreal Engine's own log is in `Saved\Logs` under `HDUE_USER_DIR` (**Unreal > Open Unreal Engine Logs** in Houdini). Its timestamps are in UTC.
