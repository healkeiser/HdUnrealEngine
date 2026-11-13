# Plan 022: The HDRI background and the CPU depth AOV use the render-resolution view rectangle

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- unreal/HdUEBackend/Source/HdUEBackend/Private/EnvironmentBackground.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/EnvironmentBackground.h unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp unreal/HdUEBackend/Shaders/Environment.usf samples/conformance/render_settings`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE (a Hydra render delegate running Unreal Engine 5.8 inside Houdini and husk) draws a dome light's HDRI as the visible background with its own compute pass, right after UE's base pass. At that point UE's scene textures are at **render resolution**: the output size times the screen percentage. But the pass sizes itself with the **output** rectangle. Whenever the screen percentage is below 100 % — every camera move in Houdini's viewport (rendered at 50 %), or any frame where a scene sets `r.ScreenPercentage` — the background samples the wrong directions: at 50 % the frame shows only the top-left quarter of the sky, stretched.

The CPU depth AOV (the `depth` render var husk writes) has the same mistake: it copies an output-sized block from the render-sized depth texture, so the depth image is squeezed into one corner with garbage elsewhere. The shared-GPU depth path already does this right. This plan makes the other two paths match it.

The problem was found by reading the code. Step 1 reproduces it first; if the image is already correct, the plan stops.

## Current state

Files (under `unreal/HdUEBackend/`):

- `Source/HdUEBackend/Private/EnvironmentBackground.cpp` — the background pass (`HdUEDrawEnvironment`, lines 31-57).
- `Source/HdUEBackend/Private/EnvironmentBackground.h` — its declaration (8 lines).
- `Shaders/Environment.usf` — the compute shader (23 lines).
- `Source/HdUEBackend/Private/Bridge.cpp` — the scene view extension that calls it (lines 271-280), the CPU depth copy (lines 293-308), and where the screen fraction is chosen (lines 1282-1290, stored in `Job->ScreenFraction` at line 1171).
- `Source/HdUEBackend/Private/SharedFrames.cpp` — the shared-GPU depth path that already scales correctly (lines 258-276).

`Bridge.cpp:271-279`, the background runs after the base pass:

```cpp
    // The HDRI background goes in right after the base pass, so heterogeneous volumes and
    // translucency composite over it; deferred lighting leaves far-depth pixels alone.
    void PostRenderBasePassDeferred_RenderThread(
        FRDGBuilder& GraphBuilder, FSceneView& View, const FRenderTargetBindingSlots& RenderTargets,
        TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override {
        if (!Job->Environment.IsValid() || !SceneTextures) return;
        HdUEDrawEnvironment(GraphBuilder, View, RenderTargets.Output[0].GetTexture(),
                            SceneTextures->GetParameters()->SceneDepthTexture, Job->Environment.GetReference(),
                            Job->EnvironmentScale);
    }
```

`EnvironmentBackground.cpp:45-47` and `:54-56`, sized by the output rectangle:

```cpp
    Parameters->ViewMin = View.UnscaledViewRect.Min;
    Parameters->ViewSize = View.UnscaledViewRect.Size();
    Parameters->InverseViewSize = FVector2f(1.f / Parameters->ViewSize.X, 1.f / Parameters->ViewSize.Y);
    ...
    FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("hdUE HDR environment background"), Shader, Parameters,
                                 FIntVector(FMath::DivideAndRoundUp(Parameters->ViewSize.X, 8),
                                            FMath::DivideAndRoundUp(Parameters->ViewSize.Y, 8), 1));
```

`Shaders/Environment.usf:15-22`, the UV runs over that size, and the depth test and write use the same pixel:

```hlsl
void MainCS(uint3 DispatchId:SV_DispatchThreadID) {
    if(any(DispatchId.xy>=uint2(ViewSize))) return;
    int2 Pixel=int2(DispatchId.xy)+ViewMin;
    if(Depth.Load(int3(Pixel,0))>0) return;
    float2 UV=(float2(DispatchId.xy)+.5)*InverseViewSize;
    ...
    Color[Pixel]=float4(Environment.SampleLevel(EnvironmentSampler,Direction,0).rgb*RadianceScale,1);
}
```

`Bridge.cpp:299-304`, the CPU depth copy, also output-sized:

```cpp
        const FIntPoint Size(Session->Width, Session->Height);
        auto Desc = FRDGTextureDesc::Create2D(Size, PF_R32_FLOAT, FClearValueBinding::None,
                                              TexCreate_ShaderResource | TexCreate_RenderTargetable);
        FRDGTextureRef Target = GraphBuilder.CreateTexture(Desc, TEXT("Hdue.DeviceDepth"));
        AddDrawTexturePass(GraphBuilder, View, Source, Target, View.UnscaledViewRect.Min, FIntPoint::ZeroValue, Size);
```

`SharedFrames.cpp:264-266`, the path that is already right — it scales by the draw's screen fraction:

```cpp
    // The scene depth covers the view rect at the draw's screen percentage (reduced while navigating).
    Parameters->ViewMin = View.UnscaledViewRect.Min;
    Parameters->ViewScale = FVector2f(ScreenFraction, ScreenFraction);
```

`Bridge.cpp:1287-1290`, the fraction itself (it already includes `r.ScreenPercentage` for still realtime frames):

```cpp
    const float ScreenFraction =
        PathTracing || Aov ? 1.f
        : Moving           ? FMath::Clamp(CVarInteractiveScreenPercentage.GetValueOnGameThread() / 100.f, .1f, 1.f)
                           : FLegacyScreenPercentageDriver::GetCVarResolutionFraction();
```

UE's definitions (engine root from `.local/paths.json`, `engine_root`):

- `Engine/Source/Runtime/Engine/Public/SceneView.h:1512-1513`: `/* Final position of the view in the final render target (in pixels), potentially constrained by an aspect ratio requirement (black bars) */ const FIntRect UnscaledViewRect;` — the output rectangle.
- `Engine/Source/Runtime/Renderer/Private/SceneRendering.h:1259-1260`: `FViewInfo::ViewRect`, "potentially scaled by ScreenPercentage" — the render rectangle. `FViewInfo` is a private Renderer type; `FSceneView` (what the extension receives) has no `ViewRect`.
- `Engine/Source/Runtime/Renderer/Private/SceneRendering.cpp:3345-3355`: UE sizes the render rectangle as `CeilToInt(UnscaledSize * Fraction)` per axis (`ApplyResolutionFraction`).

Approach chosen: compute the render size the way `SharedFrames.cpp` and UE do, from `UnscaledViewRect` and `Job->ScreenFraction`. This needs no private header. Known limit: when lens distortion overscan is on (`Bridge.cpp` `UpdateLensDistortion`, around line 647), UE also multiplies by an overscan fraction that `ScreenFraction` does not include. `SharedFrames.cpp` has the same limit today; mark it with a `shortcut:` comment.

Conventions: comments explain why; C++ follows `.clang-format` (Google, 4 spaces, 120 columns). Shaders in `Shaders/` use the compact style of `Environment.usf` and `SharedFrame.usf`.

## Commands you will need

| Purpose | Command | Expected on success |
|---------|---------|---------------------|
| Build the backend | `.\scripts\Build-Backend.ps1` | exit 0 |
| Stage to a side profile | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\.local\runtime-plan022.json` | exit 0 |
| One conformance scene | `hython scripts\run_conformance.py --runtime-config <absolute profile path> --only render_settings/screen_percentage` | per-check results, `0 failing runs` when fixed |
| Area conformance | `hython scripts\run_conformance.py --runtime-config <absolute profile path> --only lux/*` and `--only render_settings/*` | `0 failing runs` |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror unreal/HdUEBackend/Source/HdUEBackend/Private/EnvironmentBackground.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` | exit 0 |

`run_conformance.py` renders each scene with husk in both modes and checks probes written in the scene's `customLayerData`. Regions are in "view units": x and y in [-5, 5] of the shared camera's frame, y up (`samples/conformance/common/stage.usda`).

## Scope

**In scope** (the only files you should modify or create):
- `unreal/HdUEBackend/Source/HdUEBackend/Private/EnvironmentBackground.cpp`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/EnvironmentBackground.h`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` (the two call sites only)
- `samples/conformance/render_settings/screen_percentage.usda` (create)

**Out of scope** (do NOT touch, even though they look related):
- `Shaders/Environment.usf` — it does not need to change if the C++ passes render-sized values. Change it only if Step 3 shows it must, and say why in the commit.
- `SharedFrames.cpp` / `SharedFrame.usf` — already correct for the ring's needs.
- The lens-distortion overscan fraction — documented as a shortcut, not fixed here.
- `docs/conformance.md`'s matrix — regenerated by the full conformance run, which is not part of this plan.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, `main` (per `AGENTS.md`).
- One commit for the scene (Step 1), one for the fix (Steps 2-3). Plain English subjects, e.g. `Logging in the Test backend; the viewport lighting that restarts hdUE`; body with why and verification; end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless told.

## Steps

### Step 1: Write a conformance scene that shows the problem, and run it on the current build

Create `samples/conformance/render_settings/screen_percentage.usda`, modelled on `samples/conformance/lux/dome_pole_axis.usda` (dome) and `samples/conformance/render_settings/depth_var.usda` (depth var). It:

- sublayers `../common/stage.usda` (128 × 128, camera at z = 10 looking down -Z, frame x, y in [-5, 5]);
- deactivates the shared key light (`over "Lights" { over "key" (active = false) {} }`, as `dome_pole_axis.usda` does);
- adds `def DomeLight "L"` with `inputs:intensity = 1`, `inputs:texture:file = @../textures/dome_top_blue_bottom_red.exr@`, `inputs:texture:format = "latlong"` (blue upper hemisphere, red lower; no pole axis override);
- adds the two cubes and the depth product/var of `depth_var.usda` (`Near` at (-1, 0, 5) size 0.5, `Far` at (2, 0, 0) size 1, the `Depth` product and `depth` var, and `rel products = [</Render/Beauty>, </Render/Depth>]`);
- sets the screen percentage on the render settings: `over "Render" { over "Settings" { float unreal:cvar:r:ScreenPercentage = 50 } }` (attribute names use `:` for `.`; see `samples/conformance/render_settings/local_fog_volume.usda:85`).

Its `customLayerData` `conformance` JSON: `"schema": "UE screen percentage"`, `"feature": "HDRI background and depth AOV at a reduced screen percentage"`, `"frames": 1`, and checks, each with `"modes": ["realtime"]` (the Path Tracer always renders at 100 %):

1. `"top of the frame is the blue sky"`: `"color": {"region": [-4.5, 3.5, 4.5, 4.8]}`, `"rgb": [0.05, 0.05, 0.9]`, `"tol": 0.2`.
2. `"bottom of the frame is the red ground"`: `"color": {"region": [-4.5, -4.8, 4.5, -3.5]}`, `"rgb": [0.9, 0.05, 0.05]`, `"tol": 0.2`.
3. and 4.: the two depth ratio checks of `depth_var.usda`, unchanged (`"near < far"` and `"far < background"`, each with `"max": 0.9999`).

Add a short `doc` string saying what the scene shows (both hemispheres where they belong, and a depth AOV in place, at 50 %).

Run it on the current build (default profile):

**Verify**: `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only render_settings/screen_percentage` → in realtime, check 2 (`bottom ... red`) **fails** and at least one depth check fails. Also open the beauty EXR it wrote (the script prints the output folder) and confirm by eye that the bottom of the frame is not red.

### Step 2: Size the background pass by the render rectangle

- Add a `float ScreenFraction` parameter to `HdUEDrawEnvironment` (header and definition) and pass `Job->ScreenFraction` from `Bridge.cpp:277`.
- In `EnvironmentBackground.cpp`, compute the render size as UE does: `FIntPoint(FMath::CeilToInt(View.UnscaledViewRect.Width() * ScreenFraction), FMath::CeilToInt(View.UnscaledViewRect.Height() * ScreenFraction))`, and use it for `ViewSize`, `InverseViewSize` and the dispatch group count. Keep `ViewMin = View.UnscaledViewRect.Min` (hdUE's single view starts at 0, 0, as `SharedFrames.cpp` assumes).
- Comment why: after the base pass the scene textures are at the render resolution (`FViewInfo::ViewRect`), not the output's. Add `// shortcut: ignores lens-distortion overscan (as SharedFrames.cpp does); use FViewInfo::ViewRect if overscanned frames show a shifted sky.`

The shader stays as it is: the UV now runs over the render rectangle, which covers the same field of view.

**Verify**: `.\scripts\Build-Backend.ps1` → exit 0.

### Step 3: Copy the CPU depth from the render rectangle, resampled to the output size

In `Bridge.cpp` `PrePostProcessPass_RenderThread`, replace the `AddDrawTexturePass` call at line 304 with the overload that takes an input size and an output size (`Engine/Source/Runtime/Renderer/Public/ScreenPass.h:659-668`):

```cpp
AddDrawTexturePass(GraphBuilder, View, Source, Target, View.UnscaledViewRect.Min, RenderSize, FIntPoint::ZeroValue, Size,
                   TStaticSamplerState<SF_Point>::GetRHI());
```

where `RenderSize` is computed from `Job->ScreenFraction` exactly as in Step 2. Point sampling, because averaging depths across an edge invents surfaces that do not exist. Comment why, in one line.

**Verify**: `.\scripts\Build-Backend.ps1` → exit 0, then `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\.local\runtime-plan022.json` → exit 0.

### Step 4: The scene now passes, and nothing else changed

**Verify**:
- `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-plan022.json --only render_settings/screen_percentage` → `0 failing runs`.
- `--only lux/*` → `0 failing runs` (the dome scenes, at 100 %, must be unchanged).
- `--only render_settings/*` → `0 failing runs` (includes `depth_var.usda`).
- A Cornell husk render with the side profile matches the one with the default profile (Cornell has no dome background).
- The format check exits 0.

## Test plan

- New conformance scene `samples/conformance/render_settings/screen_percentage.usda` (Step 1): top band blue, bottom band red, near < far < background depth, at `r.ScreenPercentage = 50`, realtime only. Pattern: `lux/dome_pole_axis.usda` and `render_settings/depth_var.usda`.
- It must fail on the build before the fix (Step 1) and pass after (Step 4). Keep a note of both outputs for the commit message.
- Existing scenes that must stay green: `lux/*`, `render_settings/*`.

## Done criteria

- [ ] `samples/conformance/render_settings/screen_percentage.usda` exists and its checks failed before the fix (recorded in the commit message)
- [ ] `run_conformance.py --only render_settings/screen_percentage` → `0 failing runs` with the fix
- [ ] `run_conformance.py --only lux/*` and `--only render_settings/*` → `0 failing runs`
- [ ] `grep -n "UnscaledViewRect.Size()" unreal/HdUEBackend/Source/HdUEBackend/Private/EnvironmentBackground.cpp` returns nothing
- [ ] `.\scripts\Build-Backend.ps1` exits 0; clang-format check exits 0
- [ ] No files outside the in-scope list are modified (`git status`)
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- In Step 1, the new scene **passes** on the current build (the background and depth are already right at 50 %). Then the finding is wrong: report what you saw, and do not change any code.
- The cvar `unreal:cvar:r:ScreenPercentage` does not reach the frame (the beauty is still rendered at full resolution; for example `HDUE_TRACE_TIMING=1` or the UE log shows no change). The test cannot show anything then; report it.
- After Step 2, the background is right in the middle of the frame but shifted or cut at the edges: the render rectangle may not start at (0, 0) or may include overscan. Report it rather than guessing an offset.
- `lux/*` or `render_settings/*` scenes that passed before now fail.

## Maintenance notes

- Any new pass hooked after the base pass and before UE's upscale (`PostRenderBasePassDeferred_RenderThread`, `PrePostProcessPass_RenderThread`) works at render resolution. It must size itself from the render rectangle, not `UnscaledViewRect`.
- The three paths (background, CPU depth, shared depth) now share one assumption: render size = `ceil(output × ScreenFraction)` from the origin. If hdUE ever enables lens-distortion overscan with a visible dome or a depth AOV, switch all three to `FViewInfo::ViewRect` (a private Renderer header, which the runtime target can include: see `HdUEBackend.Build.cs`).
- A reviewer should check that the dispatch, the UV and the depth test all use the same size.
