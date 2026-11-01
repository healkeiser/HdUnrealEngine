# Ambient Cubemap in UE 5.8: repro

UE 5.8's Ambient Cubemap adds no light with Substrate's adaptive GBuffer, which is UE's default on SM6 (`r.Substrate.ProjectGBufferFormat=1`) and the format hdUE uses. This repro shows it in a stock UE 5.8 editor, without hdUE.

1. Create a blank project (`AC.uproject`) with the Python Editor Script Plugin enabled, and set its `Config/DefaultEngine.ini`:

   ```ini
   [/Script/Engine.RendererSettings]
   r.Substrate=True
   r.Substrate.ProjectGBufferFormat=1

   [/Script/WindowsTargetPlatform.WindowsTargetSettings]
   DefaultGraphicsRHI=DefaultGraphicsRHI_DX12
   +D3D12TargetedShaderFormats=PCD3D_SM6
   -D3D12TargetedShaderFormats=PCD3D_SM5
   ```

2. Run [capture.py](capture.py) twice (the first run may capture before the materials have compiled):

   ```powershell
   & "<UE 5.8>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" AC.uproject -run=pythonscript -script=<repo>\samples\repro\ambient_cubemap\capture.py -AllowCommandletRendering -unattended -nosplash -nop4 -stdout ACINT=10
   ```

3. Read the mean of `Saved/ac.hdr`.

Measured on 2026-11-01 (UE 5.8 editor, RTX GPU, cubemap `/Engine/MapTemplates/Sky/SunsetAmbientCubemap` at intensity 10, no other light):

| Shader format | GBuffer | Other settings | Mean |
| --- | --- | --- | --- |
| SM5 | legacy (Substrate off) | | 0.236 |
| SM5 | blendable (SM5 has no adaptive GBuffer) | Substrate on; also with hdUE's Substrate format, Lumen, MegaLights + VSM + virtual textures | 0.236 |
| SM6 | adaptive (`ProjectGBufferFormat=1`) | Substrate on, Lumen; also with ray tracing and the Path Tracer | 0.000 |
| SM6 | adaptive | `CONTROL=1`, `ACINT=0` (point light only) | 39.99 |
| SM6 | blendable (`ProjectGBufferFormat=0`) | Substrate on, Lumen | 0.236 |

Cause: `Engine/Shaders/Private/AmbientCubemapComposite.usf` never defines `SUBSTRATE_LOAD_FROM_MATERIALCONTAINER`, so it reads as 0 and the pass reads the legacy GBuffer, which the adaptive format does not write. Every pixel's shading model reads as unlit and the pass adds nothing. The other Substrate composites set it from `SUBSTRATE_GBUFFER_FORMAT` (`ReflectionEnvironmentPixelShader.usf`, lines 25-32):

hdUE's `scripts/Apply-ResidencyPatch.ps1` adds this to the source engine and the editor installation. With it, the SM6 adaptive capture above renders 0.225 (the blendable GBuffer's 0.236 differs by its own shading), and hdUE's Cornell box with a cubemap from `samples/cornell/ramp.png` at intensity 4 goes from 0.050 to 1.033:

```hlsl
#if SUBSTRATE_GBUFFER_FORMAT==0
#define SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 0
#else
#define SUBSTRATE_LOAD_FROM_MATERIALCONTAINER 1
#endif
```
