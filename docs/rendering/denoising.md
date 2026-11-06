# Denoising

The denoiser removes the noise left in a Path Tracer image after its last sample. hdUE runs UE's own denoiser plugins, as UE's Path Tracer does: NNE Denoiser by default, or NVIDIA OptiX.

The Cornell box at 16 samples per pixel, without a denoiser and with each plugin.

<div class="hdue-figure" data-figure="denoiser"></div>

## Using it in Solaris

The denoiser settings are on the **Unreal Engine** tab of your Render Settings LOP, under **Rendering > Path Tracing**. They show when **Render Mode** is Path Tracer.

1. Leave **Denoiser** on (the default).
2. Pick a **Denoiser Plugin**: NNE Denoiser (the default) or NVIDIA OptiX.
3. Set **Path Tracer Samples** high enough that the denoiser keeps your detail (see [Samples and the denoiser](#samples-and-the-denoiser)).

The denoiser only touches the Path Tracer's beauty. Realtime renders have no Path Tracer noise and are unchanged.

## Denoiser

**Denoiser** (`unreal:postProcess:PathTracingEnableDenoiser`), default on. Path Tracer only.

Runs the selected denoiser plugin on the last sample. It reads the Path Tracer's albedo and normal as well as its colour, so edges and textures stay sharp.

On the glossy Cornell box at 1920 x 1080 and 32 samples, NNE Denoiser cut the noise 20 times, with the mean brightness unchanged (0.0291 before, 0.0290 after).

## Denoiser Plugin

**Denoiser Plugin** (`unreal:cvar:r:PathTracing:Denoiser:Name`), default NNE Denoiser. Path Tracer only.

| Value | Plugin | Runs on | Notes |
| --- | --- | --- | --- |
| `NNEDenoiser` | UE's NNE Denoiser: Intel Open Image Denoise 2.3 networks | any D3D12 GPU, through DirectML | UE's default |
| `OptiX` | UE's OptiX Denoise plugin (experimental in UE) | NVIDIA GPUs, through CUDA | |

On the same Cornell frame, OptiX left a little less noise than NNE Denoiser (0.00009 against 0.00015, mean absolute Laplacian), with the same brightness.

## NNE Denoiser Model

**NNE Denoiser Model** (`unreal:cvar:NNEDenoiser:Asset`), default Project Default (Balanced, Alpha). Path Tracer only, Advanced.

The Open Image Denoise network NNE Denoiser runs: Fast, Balanced or High Quality, each with or without the alpha channel. In Open Image Denoise, High Quality keeps the most detail and Fast takes the least time. The Alpha models also denoise the alpha channel.

The Cornell box at 4 samples per pixel, where the models differ most.

<div class="hdue-figure" data-figure="denoising-model"></div>

## NNE Denoiser Runtime

**NNE Denoiser Runtime** (`unreal:cvar:NNEDenoiser:Runtime:Type`), default RDG. Path Tracer only, Advanced.

Where NNE Denoiser runs its network: RDG (ONNX Runtime on DirectML inside UE's render graph), GPU or CPU.

## OptiX Tile Width and Tile Height

**OptiX Tile Width** (`unreal:cvar:r:OptixDenoise:TileWidth`) and **OptiX Tile Height** (`unreal:cvar:r:OptixDenoise:TileHeight`), default 0. Path Tracer only, Advanced.

The tile size OptiX denoises in, in pixels; 0 denoises the whole image at once. Leave them at 0: UE's default of 256 fails in UE 5.8.4's OptiX library and leaves a garbled strip in the image.

## Samples and the denoiser

The denoiser cannot recover detail the samples never reached. Check the denoised image at the sample count you render with.

<div class="hdue-figure" data-figure="denoising-samples"></div>

## When to turn it off

- **You denoise in compositing.** AOVs are never denoised, so a denoised beauty no longer matches its AOVs. Turn **Denoiser** off and denoise all layers together.
- **You are judging the sample count.** Compare noise with the denoiser off.
- **You render many converged samples.** At thousands of samples there is little noise to remove.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The viewport stays noisy while the Path Tracer accumulates | the denoiser runs only on the last sample | Wait for the last sample, or lower **Path Tracer Samples** while you work. |
| Fine texture or contact shadows look smeared | too few samples for the denoiser | Raise **Path Tracer Samples**, or pick the High Quality model. |
| AOVs are noisy but the beauty is clean | AOVs are not denoised | Expected. Turn the denoiser off and denoise in compositing to keep them matched. |
| A garbled strip across an OptiX image | OptiX Tile Width or Height set to a non-zero value | Set both to 0. |
| A Realtime image is noisy | the denoiser is for the Path Tracer only | Raise **Warmup Frames**; see [Render settings](render-settings.md#warmup-frames). |

## Unreal Engine and Karma

hdUE follows UE: the denoiser is on by default, and NNE Denoiser is UE's default plugin. Karma's denoiser settings do not affect hdUE.
