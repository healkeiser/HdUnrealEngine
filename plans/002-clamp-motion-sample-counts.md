# Plan 002: Motion-blur primvar sampling never reads past its 16-slot arrays

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/prims.cpp samples/conformance/geom`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE reads motion-blur samples from Hydra into fixed arrays of 16 entries. Hydra's
`SamplePrimvar` returns how many samples the primvar *has*, which can be more than the 16
it copied. Two loops then read up to that larger count, past the end of the arrays. A mesh
whose points are cached every 0.05 frame has 41 samples in hdUE's ±1 frame window, so
rendering it with motion blur reads garbage `VtValue`s from the stack and can crash Houdini
or husk. After this plan, dense caches render with correct blur and never overrun.

## Current state

Files:

- `src/hydra/prims.cpp` — Hydra prims. Motion constants at lines 281-283; the instancer's
  primvar sampling at lines 404-432; the mesh's deformation sampling at lines 763-786.

The window and capacity (`src/hydra/prims.cpp:281-283`):

```cpp
// Motion samples cover one frame either side, enough for any shutter; Pass evaluates them at sub-frame times.
constexpr float HdUEMotionWindow = 1.f;
constexpr size_t HdUEMaxMotionSamples = 16;
```

The transform paths already clamp the returned count (`src/hydra/prims.cpp:289`, and the
camera at `:1856`):

```cpp
    for (size_t i = 0; i < std::min(count, HdUEMaxMotionSamples); ++i) {
```

Site 1, instancer primvars, unclamped (`src/hydra/prims.cpp:404-425`):

```cpp
    void SampledMotion(const SdfPath& prototype, size_t count, HdUEMeshData& mesh) const {
        struct Samples {
            size_t n = 0;
            float times[HdUEMaxMotionSamples];
            VtValue values[HdUEMaxMotionSamples];
        };
        std::map<TfToken, Samples> sampled;
        std::set<float> times{0.f};
        for (const auto& name : {HdInstancerTokens->instanceTranslations, HdInstancerTokens->instanceRotations,
                                 HdInstancerTokens->instanceScales, HdInstancerTokens->instanceTransforms}) {
            Samples s;
            s.n = GetDelegate()->SamplePrimvar(GetId(), name, -HdUEMotionWindow, HdUEMotionWindow, HdUEMaxMotionSamples,
                                               s.times, s.values);
            if (std::all_of(s.values, s.values + s.n, [&](const VtValue& v) { return v == s.values[0]; })) continue;
            times.insert(s.times, s.times + s.n);
            sampled[name] = std::move(s);
        }
        ...
            for (const auto& [name, s] : sampled) at[name] = HdResampleRawTimeSamples(t, s.n, s.times, s.values);
```

Site 2, mesh points, unclamped (`src/hydra/prims.cpp:763-786`):

```cpp
            // Deformation blur from time-sampled points of the same count, else from per-point velocities.
            float times[HdUEMaxMotionSamples];
            VtValue samples[HdUEMaxMotionSamples];
            const size_t count = delegate->SamplePrimvar(GetId(), HdTokens->points, -HdUEMotionWindow, HdUEMotionWindow,
                                                         HdUEMaxMotionSamples, times, samples);
            bool moves = false;
            for (size_t k = 0; k < count && count > 1; ++k) {
                auto sample = value(samples[k], VtVec3fArray());
                ...
                mesh.deformTimes.push_back(times[k]);
            }
```

The Hydra contract, from Houdini 22's `toolkit/include/pxr/imaging/hd/sceneDelegate.h:650-653`:
"If the number of contributing sample times is greater than maxSampleCount, you might want to
call this function again to get all the authored data." Hydra's own convenience overload
(`sceneDelegate.h:1129-1145`) resizes and calls again when the return value exceeds its
capacity. Both SDKs hdUE builds against (Houdini 22's USD and standalone OpenUSD 26.03,
`build/OpenUSD-src/include/pxr/imaging/hd/sceneDelegate.h:695,707`) have the overload that takes
an explicit interval and an `HdTimeSampleArray<VtValue, CAPACITY>`:

```cpp
    template <unsigned int CAPACITY>
    void SamplePrimvar(SdfPath const &id, TfToken const& key, float startTime, float endTime,
                       HdTimeSampleArray<VtValue, CAPACITY> *sa);
```

`HdTimeSampleArray` (`pxr/imaging/hd/timeSampleArray.h`) has public members `count`,
`times` and `values` (`TfSmallVector`s), `Resize(n)` and `Resample(float u)`, which
interpolates linearly with `HdResampleRawTimeSamples` — the same function site 1 already uses.

**Why not just clamp with `std::min`.** Clamping stops the overrun but keeps the *first* 16
samples. Samples come in time order, so a cache with 0.05-frame steps would keep only times
-1.0 to -0.25 and blur over the wrong part of the window. Fetching every sample and then
resampling down to 16 evenly spaced times keeps both the memory bound (16 per primvar, which
`deformOffsets` and `instanceSamples` multiply by vertices or instances) and the correct
coverage.

Conventions: C++ is Google style, 4 spaces, 120 columns (`.clang-format`). Comments explain
*why*, one line, at the density of the surrounding code. Helpers that the file defines at
namespace scope are prefixed `HdUE` (see `HdUERelativeMotion` at `prims.cpp:285`).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build the Houdini delegate (Houdini must be closed; also runs its ctest) | `.\scripts\Build-Houdini.ps1` | exit 0, ctest "100% tests passed" |
| Build the standalone delegate (OpenUSD 26.03; runs its ctest) | `.\scripts\Build-Host.ps1` | exit 0 |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/prims.cpp` | no output, exit 0 |
| Unit suite (no GPU) | `.\scripts\Test.ps1 -Suite unit` | exit 0, "Suite unit passed." |
| Targeted conformance | `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only geom/* instancer/* camera/motion_blur render_settings/*Motion* render_settings/instantaneousShutter` | "0 failing runs" |

`hython` is `<houdini_root>\bin\hython.exe`; `houdini_root` is in `.local/paths.json`.

## Suggested executor toolkit

- If a `vfx:openusd-26-11` skill is available, read `api/hd_page_front` for the Hydra
  sampling API. The source of truth is the two `sceneDelegate.h` headers named above.

## Scope

**In scope** (the only files you should modify):

- `src/hydra/prims.cpp`
- `samples/conformance/geom/dense_motion_samples.usda` (create)

**Out of scope** (do NOT touch, even though they look related):

- `HdUERelativeMotion`, `HdUETransformMotion`, the camera's `SampleTransform` loop and
  `ComputeInstanceMotion`'s `SampleInstancerTransform` call: they already clamp, and transform
  matrices are not resampled the same way (a `GfMatrix4d` lerp is not a rotation lerp).
- `HdUEMaxMotionSamples` and `HdUEMotionWindow`: do not change their values.
- Any backend file under `unreal/`.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, on `main` (AGENTS.md:
  `main` is the only branch). Never push unless told.
- One commit for the fix and the scene. Message style: one plain English sentence describing
  the result, then a body saying why and how it was verified, ending with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Example subject from `git log`:
  "Logging in the Test backend; the viewport lighting that restarts hdUE".

## Steps

### Step 1: Add one helper that fetches every sample and bounds the count

In `src/hydra/prims.cpp`, right after `HdUETransformMotion` (ends at line 303), add a helper
with this shape:

```cpp
// SamplePrimvar returns how many samples a primvar has, which can exceed the capacity it fills (sceneDelegate.h); the
// array overload fetches them all. Past HdUEMaxMotionSamples they are resampled at even times over the same span.
HdTimeSampleArray<VtValue, HdUEMaxMotionSamples> HdUESamplePrimvar(HdSceneDelegate* delegate, const SdfPath& id,
                                                                   const TfToken& name) {
    HdTimeSampleArray<VtValue, HdUEMaxMotionSamples> all;
    delegate->SamplePrimvar(id, name, -HdUEMotionWindow, HdUEMotionWindow, &all);
    if (all.count <= HdUEMaxMotionSamples) return all;
    HdTimeSampleArray<VtValue, HdUEMaxMotionSamples> even;
    even.Resize(HdUEMaxMotionSamples);
    const float first = all.times[0], last = all.times[all.count - 1];
    for (size_t i = 0; i < HdUEMaxMotionSamples; ++i) {
        even.times[i] = first + (last - first) * float(i) / float(HdUEMaxMotionSamples - 1);
        even.values[i] = all.Resample(even.times[i]);
    }
    return even;
}
```

Add `#include "pxr/imaging/hd/timeSampleArray.h"` near the other `pxr/imaging/hd` includes if
it is not already included (check with `grep -n timeSampleArray src/hydra/prims.cpp`).

**Verify**: `grep -n "HdUESamplePrimvar" src/hydra/prims.cpp` → exactly one definition line.

### Step 2: Use it at the mesh's deformation site

Replace lines 765-769 and the loop that reads them so the loop runs over the helper's result:

```cpp
            const auto sampled = HdUESamplePrimvar(delegate, GetId(), HdTokens->points);
            bool moves = false;
            for (size_t k = 0; k < sampled.count && sampled.count > 1; ++k) {
                auto sample = value(sampled.values[k], VtVec3fArray());
                ...
                mesh.deformTimes.push_back(sampled.times[k]);
            }
```

Keep everything else in that block unchanged (the subdivision refine, the size check, the
velocity fallback).

**Verify**: `sed -n 760,790p src/hydra/prims.cpp | grep -c "samples\[k\]\|times\[k\]"` → `0`.

### Step 3: Use it at the instancer's site

In `SampledMotion` (lines 404-432), replace the local `Samples` struct with the helper's type:

```cpp
        std::map<TfToken, HdTimeSampleArray<VtValue, HdUEMaxMotionSamples>> sampled;
        std::set<float> times{0.f};
        for (const auto& name : {...same four tokens...}) {
            auto s = HdUESamplePrimvar(GetDelegate(), GetId(), name);
            const auto first = s.values.begin(), end = s.values.begin() + s.count;
            if (std::all_of(first, end, [&](const VtValue& v) { return v == s.values[0]; })) continue;
            times.insert(s.times.begin(), s.times.begin() + s.count);
            sampled[name] = std::move(s);
        }
```

and the resample line becomes `at[name] = s.Resample(t);`. Note: when `s.count` is 0,
`std::all_of` over an empty range returns true and the primvar is skipped, as today.

**Verify**: `grep -n "s\.n\b\|struct Samples" src/hydra/prims.cpp` → no matches.

### Step 4: Format and build

**Verify**:
- `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/prims.cpp` → no output.
- `.\scripts\Build-Houdini.ps1` → exit 0, ctest passes.
- `.\scripts\Build-Host.ps1` → exit 0 (the standalone OpenUSD 26.03 build must compile the
  same overload).

### Step 5: Add a conformance scene with dense samples

Create `samples/conformance/geom/dense_motion_samples.usda`. Model it on
`samples/conformance/geom/motion_blur_scale.usda` (same header: `metersPerUnit = 1`,
`upAxis = "Y"`, `startTimeCode = 1`, `endTimeCode = 1`, `timeCodesPerSecond = 24`,
`renderSettingsPrimPath = "/Render/Settings"`, `subLayers = [@../common/stage.usda@]`, and the
`over "Camera"` with `shutter:open = -0.5`, `shutter:close = 0.5`). It holds:

1. A `Mesh "Deforming"`: a unit quad (x -0.5..0.5, y -2.5..-1.5, z 0), `subdivisionScheme =
   "none"`, whose `points.timeSamples` are authored every 0.05 frame from time 0 to time 2
   (41 samples), each sample the quad shifted along +X by `2 * (t - 1)` units.
2. A `PointInstancer "Instancer"` with one cube prototype (`size 1`) and one instance whose
   `positions.timeSamples` are authored every 0.05 frame from 0 to 2, at `(2 * (t - 1), 2, 0)`,
   with `protoIndices = [0]` and no `velocities`. Copy the instancer layout from
   `samples/conformance/instancer/sampled_positions.usda`.

Generate the 41-sample lists with a throwaway Python one-liner and paste the output; do not
commit a generator. The `customLayerData` conformance JSON has `"schema": "UsdGeomPrimvarsAPI"`,
`"feature": "more than 16 motion samples in the shutter window"`, `"frames": 1` and three checks
(regions use the shared camera, which frames x and y in [-5, 5] on z = 0):

- `"name": "dense deforming mesh smears past its edge"`, `"value": {"region": [0.7, -2.2, 1.3, -1.8], "stat": "alpha"}`, `"min": 0.1`
- `"name": "dense instancer smears past its edge"`, `"value": {"region": [0.7, 1.8, 1.3, 2.2], "stat": "alpha"}`, `"min": 0.1`
- `"name": "the mesh centre stays covered"`, `"value": {"region": [-0.2, -2.2, 0.2, -1.8], "stat": "alpha"}`, `"min": 0.5`

The doc string explains in plain sentences: 41 samples inside the ±1 frame window, more than the
16 hdUE keeps; before this fix the extra samples were read past the end of a 16-entry array.

**Verify**: `hython -c "from pxr import Usd; s=Usd.Stage.Open(r'samples/conformance/geom/dense_motion_samples.usda'); a=s.GetPrimAtPath('/Deforming').GetAttribute('points'); print(len(a.GetTimeSamples()))"` → `41`.

### Step 6: Run the targeted conformance set

**Verify**: `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only geom/* instancer/* camera/motion_blur` → "0 failing runs", and `geom/dense_motion_samples` passes in both realtime and pathtraced.

### Step 7: Cornell check

Render the Cornell box with husk as AGENTS.md requires for every change and compare it with
the previous build's render (the render should be identical; Cornell has no motion).

**Verify**: the comparison you normally run shows no change beyond render noise.

## Test plan

- New conformance scene `samples/conformance/geom/dense_motion_samples.usda` (Step 5), covering
  the mesh deformation site and the instancer primvar site. It is the regression test for the
  overrun: 41 samples in the window, more than 16.
- A C++ unit test would need a mock `HdSceneDelegate`, which needs an `HdRenderIndex`; that is
  heavier than the fix, so it is not part of this plan.
- Existing motion scenes (`geom/motion_blur_scale`, `instancer/sampled_positions`,
  `instancer/sampled_scales`, `camera/motion_blur`) must still pass: they have 3 samples, which
  take the unchanged `count <= 16` path.

## Done criteria

- [ ] `grep -n "HdUESamplePrimvar" src/hydra/prims.cpp` shows the definition and two call sites.
- [ ] `grep -n "struct Samples\|samples\[k\]" src/hydra/prims.cpp` returns nothing.
- [ ] `.\scripts\Build-Houdini.ps1` and `.\scripts\Build-Host.ps1` exit 0.
- [ ] clang-format check on `src/hydra/prims.cpp` passes.
- [ ] `run_conformance.py --only geom/* instancer/* camera/motion_blur` reports "0 failing runs".
- [ ] `git status --short` lists only `src/hydra/prims.cpp`, `samples/conformance/geom/dense_motion_samples.usda` and `plans/README.md`.
- [ ] `plans/README.md` status row updated.

## STOP conditions

- The excerpts above do not match `src/hydra/prims.cpp` (someone changed the sampling code).
- The interval overload `SamplePrimvar(id, key, start, end, HdTimeSampleArray*)` does not exist
  in either SDK's `sceneDelegate.h`.
- The new scene fails *before* your change in a way other than a crash or garbage (for example
  it already passes on the old build): then the delegate may not receive more than 16 samples
  from Houdini's scene delegate; report what `HDUE_TRACE_SCENE=1` shows and stop.
- An existing motion conformance scene starts failing after the change.

## Maintenance notes

- If `HdUEMaxMotionSamples` is ever raised, nothing else changes: the helper bounds to it.
- Any new `SamplePrimvar` call in the delegate should go through `HdUESamplePrimvar`. Reviewers:
  grep for `SamplePrimvar(` and check each one.
- Resampling to even times can soften a sharp change that happens between two of the 16 times.
  That only matters for caches with more than 16 samples in two frames, which had undefined
  behaviour before.
