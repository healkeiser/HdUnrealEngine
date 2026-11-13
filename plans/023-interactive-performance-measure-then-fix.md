# Plan 023: Measured interactive speed-ups, each kept only if it pays

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/delegate.cpp src/hydra/prims.cpp src/hydra/color_management.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/NativeAssets.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp integrations/houdini/python3.13libs/hdue_batch.py`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: M
- **Risk**: MED
- **Depends on**: plans/010-test-tiers-run-everything-they-claim.md (verification tiers) (a working `Test.ps1` tier and conformance wrapper to check each step)
- **Category**: perf
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

Several places in hdUE do work proportional to the whole scene for one small edit, or block a thread while they wait. On small scenes nobody notices. On large ones they make look-dev and tumbling stutter. Each cost below was found by reading the code, and none has been measured. So this plan measures first, changes one thing at a time, and keeps a change only when the measurement shows it helps and the image does not change.

## Current state

All excerpts were read at `6fe3e12`. Line numbers are from that commit.

**Terms.** *Game thread*: the thread that calls UE (in Houdini and husk it is hdUE's own thread, `HdUESelfHost` in `src/hydra/delegate.cpp`). *Render flush*: `FlushRenderingCommands()`, which makes the game thread wait until UE's render thread has finished all queued work. *Execute*: Hydra's per-frame render call on the render pass.

### A. A render flush on every material update

`unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp:2308-2324`:

```cpp
HdueResult HDUE_CALL SetMaterialParameters(HdueSession Handle, const HdueMaterialUpdate* Update) {
    ...
    if (Code != HDUE_OK) return Fail(Code, *Error);
    Runtime.Session->HasRequest = false;
    ReleaseResidency(Runtime.Session);
    FlushRenderingCommands();
    Runtime.Error.Empty();
    return HDUE_OK;
}
```

The delegate calls it once per material, in a loop (`src/hydra/delegate.cpp:1711-1712`):

```cpp
    for (const auto& update : packet.materialUpdates)
        backend.require(api.set_material_parameters(session, &update), "set_material_parameters");
```

Replaced material instances are already kept alive until the next Apply, because frames still in flight may reference them (`Scene.cpp:2219-2222`):

```cpp
        // A replaced material can still be referenced by frames in flight on the render thread (ray-tracing instance
        // gathering crashed on a material edit); keep it until the next Apply.
        if (auto* Previous = Materials.Find(UTF8_TO_TCHAR(Desc.id)); Previous && Previous->Get() != Material)
            RetiredMaterials.Add(MoveTemp(*Previous));
```

`SetMaterialParameters` only changes parameters on existing instances, so it creates nothing that needs retiring. Commits that touched `FlushRenderingCommands` in `Bridge.cpp` (from `git log -S`): `ef03adf`, `fe1d95e`, `82226b3`, `b5bdcf8`, `35b8278`. None of their subjects explains this flush. This exact item was reported before, as EFF-13 in `docs/audit/07-simplify.md:57`, and never fixed.

### B. One wet-surface edit marks every mesh dirty

`src/hydra/delegate.cpp:4022-4035` (inside `SetRenderSetting`, under `state_->mutex`):

```cpp
    if (key == TfToken("surfaceEffects")) {
        auto bindings = std::make_shared<const VtDictionary>(state_->setting<VtDictionary>("surfaceEffects", {}));
        // Wet meshes upload editable (HDUE_MESH_EDITABLE): one that becomes wet, or stops being wet, uploads again.
        for (const auto& [id, mesh] : state_->meshes)
            if ((state_->wetBindings && !HdUEWetSettings(*state_->wetBindings, mesh).empty()) !=
                !HdUEWetSettings(*bindings, mesh).empty())
                state_->dirtyMeshes.insert(id);
        state_->wetBindings = std::move(bindings);
    }
    if (key == TfToken("surfaceEffects") || key == TfToken("wetMaskPreview") || key == TfToken("wetValidationError")) {
        ++state_->revision;
        if (key == TfToken("surfaceEffects") && state_->renderIndex)
            for (const auto& id : state_->renderIndex->GetRprimIds())
                state_->renderIndex->GetChangeTracker().MarkRprimDirty(id, HdChangeTracker::DirtyPrimvar);
```

`DirtyPrimvar` takes Mesh::Sync's full path (re-triangulate, weld, normals, subsets, hash), because the quick path only accepts transform, visibility and render-tag bits (`src/hydra/prims.cpp:582-585`):

```cpp
        if (!emitted_.empty() && GetInstancerId().IsEmpty() &&
            !(*bits & ~(HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
                        HdChangeTracker::DirtyRenderTag))) {
```

Mask primvars for meshes that need one are already fetched without a Sync, by `HdUERefreshWetMasks` (`src/hydra/prims.cpp:1881-1894`). It reads `maskPrimvar` from each mesh's wet settings and refreshes only meshes missing that mask.

The `subdivisionLevel` handler (`delegate.cpp:4055-4059`) also marks every Rprim `DirtyTopology`. That one is correct: a global level change affects every mesh that has no `primvars:unreal:subdivisionLevel` of its own (`prims.cpp:549-553`). Leave it.

### C. The Path Tracer rebuilds its texture list and warms up on every request

`Bridge.cpp:832-836`, in `RequestRender`:

```cpp
    if (Request->mode == HDUE_PATH_TRACED) {
        Session->Residency = MakeShared<FHdueVirtualTextureResidency, ESPMode::ThreadSafe>(
            Session->Scene->GatherVirtualTextures(), Request->generation, Session->ResidencyOptions);
        Session->Preparing = Session->Residency->Status().phase != HDUE_RESIDENCY_READY;
    }
```

`GatherVirtualTextures` (`Scene.cpp:85-114`) walks every actor, mesh component and material and calls `GetUsedTextures`. The warm-up (`VirtualTextureResidency.cpp:39-44`, `:79-80`) calls `Commands.SubmitAndBlockUntilGPUIdle()` per draw and needs `r.vt.FeedbackLatency + 2` stable draws. With no virtual-texture textures the phase is READY at once (`VirtualTextureResidency.cpp:19`), so only scenes with UDIM/virtual textures pay it.

UE's own viewport Path Tracer drops its samples and starts again on a camera move, with no blocking warm-up (UE 5.8 docs: UE 5.8 docs page `path-tracer-in-unreal-engine.md` (dev.epicgames.com, Unreal Engine 5.8):64-68`). Offline renders must keep the warm-up (Movie Render Queue warms up too).

### D. Native-asset scene status rebuilt on every Execute

`src/hydra/delegate.cpp:2922-2925`:

```cpp
                HdueAssetSceneStatus assets{};
                assets.struct_size = sizeof(assets);
                s.backend->require(s.backend->api().get_asset_scene_status(s.session, &assets), "asset_scene_status");
                const bool lightsChanged = assets.lights != s.submittedNativeLights;
```

`FHdueNativeAssets::GetSceneStatus` (`NativeAssets.cpp:381-394`) calls `Stats()` per entry. `Stats()` (`:334-362`) collects every actor, their mesh and light components, and builds bounds. The delegate reads only `loading`, `failed`, `lights` and `message`.

### E. The UE thread ticks every 2 ms while any viewport is unpaused

`src/hydra/delegate.cpp:345-349`:

```cpp
                const bool ticking =
                    host.continuous && host.delegates > host.paused;  // UE idles while every viewport is paused
                if (ticking)
                    host.queueReady.wait_for(lock, std::chrono::milliseconds(2), due);
```

When the image has converged and nothing is pending, the loop still ticks UE and runs every pump.

### F. One finished mesh build swapped in per tick

`Scene.cpp:1842-1846`:

```cpp
        for (UStaticMeshComponent* Component : Components) Component->SetStaticMesh(Built);
        ++SwappedBuilds;
        SwappedMeshes += Components.Num();
        It.RemoveCurrent();
        return;  // one per tick keeps frames smooth
```

The texture swaps right above use a time budget instead (`Scene.cpp:1709`: `for (...; FPlatformTime::Seconds() - Start < 0.008; ...)`).

### G. A Niagara seek may block the UE thread for up to 10 minutes

`NativeAssets.cpp:276`:

```cpp
            C->SetMaxSimTime(600);  // a seek reaches its age within one tick, however far
```

UE's default is 33 ms per tick (`<engine_root>/Engine/Plugins/FX/Niagara/Source/Niagara/Private/NiagaraComponent.cpp:650`, `MaxSimTime(33.0f / 1000.0f)`). Sequencer does not override it (no `SetMaxSimTime` under `Niagara/Private/MovieScene`). The wall-clock limit is applied in `NiagaraSystemSimulation.cpp:1109-1129`. But an effect's entry becomes READY two frames after it is placed (`NativeAssets.cpp:187-192`, the `Settle` lambda), not when the seek is done. So with UE's default, a husk frame could render before the effect reaches its age. That must be fixed together with the time limit.

### H. The dome cubemap is built on one thread

`Lighting.cpp:613-633`: a triple loop over 6 faces × R × R texels, with R up to 2048, inside `FHdueLighting::Tick` (`:679-684`). The post-process cube code already uses `ParallelFor(6, ...)` (`Bridge.cpp:1682`).

### I. One mutex serialises every OCIO texture conversion

`src/hydra/color_management.cpp:172-174`:

```cpp
    const auto folder = std::filesystem::temp_directory_path() / "hdue_ocio";
    static std::mutex convert;
    std::lock_guard lock(convert);
```

The lock covers the existence check, read, convert and write (`:188-254`). Writes already go to a pending file and are renamed, so concurrent writes of the same key are safe. The `std::call_once(evict, ...)` (`:178-185`) must keep running once.

### J. A displacement image is decoded once per mesh, under the state lock

`displaced()` (`delegate.cpp:843-872`) calls `displacementImage(textureFile(state, texture.file))` (decoder at `:694-729`) for each mesh. It is called from `prepareMeshes` under the state lock (`delegate.cpp:1153-1157`). N meshes sharing one height map decode it N times.

### K. Smaller items

- **Python walk per husk frame.** `hdue_batch.py:219-221` returns `static = not any(result.values())`, so any prepared material makes husk re-walk the stage in Python every frame.
- **Runtime profile re-read per asset prim.** `HdUEAssetBoundsCm` (`delegate.cpp:388-400`) calls `runtimeConfig("asset_catalog")`, which opens and parses the `HDUE_RUNTIME_CONFIG` JSON (`delegate.cpp:268-284`) every call.
- **Render stats scan.** `renderStats` (`delegate.cpp:4186-4199`) walks every mesh and deep-copies `nativeMaterialAssets` under `state.mutex`, up to ten times a second (`:4130-4139`).
- **OpenSubdiv refiner.** It is rebuilt on every Sync of a subdivided mesh (`prims.cpp:549-565`, `subdivision.h`), even when the topology has not changed.

### Not a finding: the per-draw streaming flush in offline realtime

`Bridge.cpp:1332-1344` runs `UpdateResourceStreaming(..., true)` and `BlockTillAllRequestsFinished` before every offline realtime draw, warm-up included. Movie Render Queue does the same: `UMoviePipeline::RenderFrame` calls `FlushAsyncEngineSystems()` on every engine tick (`<engine_root>/Engine/Plugins/MovieScene/MovieRenderPipeline/Source/MovieRenderPipelineCore/Private/MoviePipelineRendering.cpp:174`), and that flushes the streaming managers (`:658-664`). Unreal is the benchmark: **do not change this**.

### Conventions

- C++ is clang-format (Google style, 4 spaces, 120 columns). Comments explain why, at the density of the surrounding code (see the comments in the excerpts above).
- Project rule (`AGENTS.md`): do what Unreal does. Cite the UE source or doc a change follows in its comment.
- Never silence a warning to make a log quieter.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build backend | `.\scripts\Build-Backend.ps1` | exit 0 (~40 s incremental) |
| Stage a side runtime | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig $PWD\.local\perf-runtime.json` | exit 0; prints the hashed DLL path |
| Build delegate | `.\scripts\Build-Houdini.ps1` (Houdini closed) | exit 0; its ctest step passes |
| Unit tests | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| Conformance (targeted) | `hython scripts\run_conformance.py --runtime-config <repo>\.local\perf-runtime.json --only <area>/*` (areas: the folders of `samples/conformance`, e.g. `preview_surface`, `materialx`, `texture`, `lux`, `volume`, `render_settings`) | `0 failing runs` |
| Timeline | set `$env:HDUE_TRACE_TIMING = 1` before husk or Houdini | per-request lines on stderr |
| In-Houdini test | `.\scripts\Launch-Houdini.ps1 -TestKind <kind>` (kinds in `tests/houdini/python3.13libs/hdue_tests.py`) | exit 0 |
| Format | `uvx --from clang-format==19.1.5 clang-format -i <file>`; `uvx black@24.10.0 <file>`; `uvx ruff@0.7.4 check <file>` | no diff / no findings |

Cornell husk render (the "image check" used below), from the repo root:

```powershell
$env:HDUE_RUNTIME_CONFIG = "$PWD\.local\perf-runtime.json"
husk --renderer "Unreal Engine" --frame 1 --output out\perf\cornell_<step>.exr samples\cornell\cornell.usda
```

If `samples\cornell\cornell.usda` is not the Cornell entry point, run `Get-ChildItem samples\cornell -Filter *.usda` and use the scene `scripts/render_gallery.py` renders (search it for `cornell`).

Compare two EXRs with `uv run scripts\compare_pfm.py` if it accepts EXR, or with this snippet (OpenImageIO ships with Houdini's Python):

```powershell
hython -c "import OpenImageIO as o,sys,numpy as n;a,b=[o.ImageBuf(p).get_pixels(o.FLOAT) for p in sys.argv[1:3]];print(float(n.abs(a-b).mean()), float(n.abs(a).mean()))" out\perf\cornell_base.exr out\perf\cornell_<step>.exr
```

The image check passes when the mean absolute difference is below 1% of the mean pixel value. Path Tracer runs differ by sample noise, so render both images in the same mode and sample count.

## Suggested executor toolkit

- Read UE's own code before changing behaviour that UE also has. The engine root is `engine_root` in `.local/paths.json`.
- The UE 5.8 docs mirror: the UE 5.8 documentation (dev.epicgames.com; a local mirror, if you have one) (grep `index.tsv` first).

## Scope

**In scope** (the only files you should modify):
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` (items A, C)
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp`, `Scene.h` (items C, F)
- `unreal/HdUEBackend/Source/HdUEBackend/Private/NativeAssets.cpp`, `NativeAssets.h` (items D, G)
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp` (item H)
- `src/hydra/delegate.cpp`, `src/hydra/delegate.h` (items B, D, E, J, K)
- `src/hydra/prims.cpp`, `src/hydra/subdivision.h` (items B, K)
- `src/hydra/color_management.cpp` (item I)
- `integrations/houdini/python3.13libs/hdue_batch.py` (item K)
- A generated test scene under `samples/repro/perf/` (create; small, see Step 0)

**Out of scope** (do NOT touch, even though they look related):
- `Bridge.cpp:1332-1344` (the offline streaming flush). It matches Movie Render Queue; see "Not a finding".
- The `subdivisionLevel` all-Rprims dirty marking (`delegate.cpp:4055-4059`). It is correct.
- `include/hdue/bridge.h` and the ABI version. No step here needs an ABI change. If one seems to, STOP.
- Transform-only paths for instancers, volumes and grooms. That is plan 024.
- Sponza, Jungle Ruins and the Da Vinci Workshop. Never use them to verify (AGENTS.md).

## Git workflow

- Work on the branch or worktree the operator gave you. If none, work on `main` (AGENTS.md: `main` is the only branch).
- Commit once per kept step, and leave reverted steps uncommitted. Subjects are plain English sentences, as in `git log`, for example: "Logging in the Test backend; the viewport lighting that restarts hdUE". The body says why, and gives the measured before and after from the step's table. It ends with:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Do NOT push unless the operator says so.

## Steps

Every step from 1 on has the same keep rule:

- **Keep** the change when the step's metric improves by at least the stated threshold, the image check passes, and the step's verification passes.
- **Otherwise revert it** (`git checkout -- <files>`), and write a one-line "reverted: <metric before → after>" in this plan's status row in `plans/README.md`.

Record every measurement in `out/perf/measurements.md` (untracked), one table row per step: step, metric, before, after, kept or reverted.

### Step 0: Build the baseline and a measuring scene

1. Build and stage the current code: `.\scripts\Build-Backend.ps1`, `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig $PWD\.local\perf-runtime.json`, `.\scripts\Build-Houdini.ps1`.
2. Write `samples/repro/perf/build_scene.py`, a stdlib + `pxr` script run with hython. It writes `samples/repro/perf/many.usda` containing:
   - 400 meshes (a 20×20 grid of small subdivided grids, about 2,000 triangles each);
   - 200 `UsdPreviewSurface` materials, two meshes each;
   - one RectLight with `inputs:intensity = 1` and brightness through `inputs:exposure` (repo rule);
   - a camera.
   Keep it under 2 MB. Follow `scripts/build_docs_box.py` for how the repo writes small sample stages.
3. Render the Cornell box (command above) to `out/perf/cornell_base.exr`, in realtime and in Path Tracer mode (`--renderer-settings` or the scene's `unreal:renderMode`; check how `scripts/render_gallery.py` switches mode).
4. With `$env:HDUE_TRACE_TIMING = 1`, render `samples/repro/perf/many.usda` with husk, 3 frames. Save stderr to `out/perf/many_base.log`.

**Verify**: `Test-Path out\perf\cornell_base.exr, out\perf\many_base.log` → `True True`; `Select-String -Path out\perf\many_base.log -Pattern 'hdUE' | Measure-Object` → Count > 0.

### Step 1: Item A, no render flush per material update

1. Read `git show ef03adf -- unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` and `git show 82226b3 -- ...Bridge.cpp`, and search the commit bodies for the reason. If a commit says the flush prevents a crash in `SetMaterialParameters`, STOP.
2. Remove the `FlushRenderingCommands();` line in `SetMaterialParameters`, and add one comment line saying why it is safe: parameters change on live instances, and replaced instances are retired by `Apply` (`Scene.cpp:2219-2222`).
3. Measure:
   - Write a hython script `samples/repro/perf/material_edit.py`. It opens `many.usda` in a LOP network, renders with hdUE in the viewport, then changes `inputs:diffuseColor` on 50 materials in one `Sdf.ChangeBlock` and times until the next converged frame (`HDUE_TRACE_TIMING` lines).
   - If driving the viewport from a script is not practical, measure in husk instead: a 2-frame render where frame 2 changes 50 material colours (time samples on `inputs:diffuseColor`). Compare frame 2's upload time.

**Keep threshold**: the frame-2 upload time drops by at least 20%.
**Verify**: `.\scripts\Launch-Houdini.ps1 -TestKind materials` → exit 0, and `-TestKind integration` → exit 0. Run each 3 times: a crash on any run is a STOP condition.

### Step 2: Item B, mark only the meshes whose mask changed

1. In `delegate.cpp`'s `surfaceEffects` branch, replace the loop that marks every Rprim `DirtyPrimvar`. Mark only the source Rprims (`mesh.sourceId`) whose `maskPrimvar` differs between the old and new bindings. Use `HdUEWetValue<std::string>(HdUEWetSettings(<bindings>, mesh), "maskPrimvar", "")`, as `HdUERefreshWetMasks` does (`prims.cpp:1886`).
2. Keep the first loop, which marks meshes that become wet or stop being wet.
3. Measure: in usdview or hython, apply a `surfaceEffects` change that edits only `amount` on `many.usda` with one wet binding. Count Mesh::Sync calls (`MeshSyncTimer` output, or add a temporary counter you remove before committing).

**Keep threshold**: Mesh::Sync calls for an amount-only edit drop from about 400 to 0 or 1.
**Verify**: `.\scripts\Launch-Houdini.ps1 -TestKind surfaces` → exit 0, and `uv run --python 3.13 -m pytest tests/surface_effects_test.py` passes, or `Test.ps1 -Suite unit` passes if that is how it runs.

### Step 3: Item C, cache the virtual-texture list; no blocking warm-up while the camera moves

1. In `FHdueScene`, cache the result of `GatherVirtualTextures()`. Clear the cache wherever the scene's actors or materials change: `Apply`, `UpdateMeshes`, `SetMaterialParameters`, and `UpsertAsset`/`RemoveAsset` (grep `Bridge.cpp` for the asset entry points).
2. In `RequestRender` (`Bridge.cpp:832`), skip creating a new `FHdueVirtualTextureResidency` for an interactive request (`IsInteractive()`, already used at `Bridge.cpp:1332`) whose texture list is unchanged since the last one that reached READY. Offline requests keep today's behaviour exactly.
3. Measure: interactive Path Tracer camera orbit on a scene with a UDIM texture. UDIM scenes: `samples/conformance/texture/udim.usda` and `samples/conformance/materialx/udim.usda`. Measure the time from request to first sample (`HDUE_TRACE_TIMING`).

**Keep threshold**: time to first sample during camera moves drops by at least 30%.
**Verify**: `hython scripts\run_conformance.py --runtime-config <repo>\.local\perf-runtime.json --only texture/* materialx/* --mode pathtraced` → `0 failing runs`. Cornell image check in Path Tracer mode passes.

### Step 4: Item D, cached asset scene counts

1. Keep `loading`, `failed`, `lights` and `message` up to date in `FHdueNativeAssets`: on `Upsert`, `Remove`, every state change in `Tick`, and on visibility changes.
2. Make `GetSceneStatus` return the cached counts. `actors` and `meshes` may be computed lazily, since the delegate does not read them (confirm with `git grep -n "assets\.\(actors\|meshes\)" src`). Compute bounds only in `GetStatus`.

**Keep threshold**: none needed (it removes work). Keep it when the verification passes.
**Verify**: `hython scripts\run_conformance.py ... --only volume/sparse_volume_*` → `0 failing runs` (the conformance scenes that author `unreal:asset`). `uv run --python 3.13 tests\asset_controls_test.py` passes, or `Test.ps1 -Suite unit` passes.

### Step 5: Item G, UE's Niagara time limit, and READY only when the seek is done

1. Remove `C->SetMaxSimTime(600);` (`NativeAssets.cpp:276`) so UE's 33 ms default applies, and update the comment to cite `NiagaraComponent.cpp:650`.
2. In `FHdueNativeAssets::Tick`, keep an effect entry in LOADING until its component has reached its desired age. Find the public getter in `<engine_root>/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraComponent.h` (look for `GetAge`, `IsSeeking` or similar). If no public getter exists, STOP and report. The 120 s timeout (`NativeAssets.cpp:182-186`) still applies.
3. Measure: `samples/docs/effects.usda`, a husk render of frames 1 and 100 in one process. Time the frame-100 request, and check the UE thread never blocks more than 100 ms per tick (`hdUE slow task`/`slow tick` lines from `HDUE_TRACE_TIMING`).

**Keep threshold**: no tick over 100 ms during the seek, and the frame-100 image matches the frame-100 image from the base build (image check).
**Verify**: the same two husk renders before and after; the image check passes on both frames.

### Step 6: Item H, the dome cubemap in parallel

1. Wrap the face/row loops in `Lighting.cpp:620-633` in `ParallelFor(6 * Resolution, ...)` over (face, row). Each texel is written by one iteration (`Pixels[...] += ...` per dome), so there is no race as long as the dome loop stays outside the `ParallelFor`.
2. Measure: change a dome light's `inputs:exposure` in a 2-frame husk render with the dome at `unreal:domeResolution` 1024. Time the second frame's environment rebuild.

**Keep threshold**: at least 3× faster on the rebuild.
**Verify**: the image check passes on a dome-lit scene (`hython scripts\run_conformance.py ... --only lux/*` → `0 failing runs`).

### Step 7: Item F, swap finished builds within a time budget

1. Replace the `return;  // one per tick ...` at `Scene.cpp:1846` with an 8 ms budget, as `TickTextures` uses (`Scene.cpp:1709`). Keep swapping finished builds until the budget runs out, then return.
2. Measure: the interactive load of `many.usda` in Houdini. Time from the last build finished to converged (`HDUE_TRACE_TIMING`).

**Keep threshold**: at least 30% shorter. No interactive frame over 50 ms during the swaps (trace).
**Verify**: `.\scripts\Launch-Houdini.ps1 -TestKind shared-builds` → exit 0.

### Step 8: Item E, slower ticking when converged and idle

1. In the worker loop (`delegate.cpp:345-349`), wait with a longer timeout, 50 ms, when every pump reports converged and the backend reports no pending conversions, textures or assets.
   - Do not add a new ABI call. Use the state the pumps already have: `IsConverged()`, plus the status the delegate already polls.
   - A queued task must still wake the loop at once (`queueReady.notify_one` already does).
2. Measure: Houdini idle with a converged Cornell viewport. Measure the process's CPU use over 30 s (`Get-Process houdini | Select CPU`, sampled twice).

**Keep threshold**: idle CPU drops by at least 30%. Niagara effects and the Lumen viewport still update when the camera moves (manual check: tumble, the image updates within one second).
**Verify**: `.\scripts\Launch-Houdini.ps1 -TestKind integration` and `-TestKind selection` → exit 0.

### Step 9: Items I, J and K (small; one commit each if kept)

1. **I.**
   - Replace the global `convert` mutex with a lock per cache key: a `std::map<std::string, std::shared_ptr<std::mutex>>` guarded by a small map mutex.
   - Keep the `call_once` eviction.
   - Measure a cold load of a scene with 20 colour textures under a non-identity OCIO config (`OCIO` env var to an ACES config). Keep it when the load is at least 30% faster.
2. **J.**
   - Cache decoded displacement images per file for one `prepareMeshes` call: a `std::map<std::string, HdUEImage>` local to the packet.
   - Measure 50 meshes sharing one displacement map. Keep it when `prepareMeshes` is at least 2× faster.
   - Check: `hython scripts\run_conformance.py ... --only preview_surface/displacement` → `0 failing runs`.
3. **K, runtime profile.** Read the profile once per process for `HdUEAssetBoundsCm`: a function-local static keyed by the `HDUE_RUNTIME_CONFIG` value. No threshold; keep it when conformance `volume/sparse_volume_*` passes.
4. **K, husk Python walk.** In `hdue_batch.py`, return `static = True` when nothing in the result is time-sampled. Reuse the animation check the viewport host already has (search `hdue_houdini.py` for `_materials_animated_steps`).
   - Keep the `shortcut:` comment about wet switches.
   - Measure a 10-frame husk render of a scene with prepared materials. Keep it when per-frame time drops by at least 10%.
   - Check: `hython tests\husk_prepared_materials.py` passes (see its docstring for arguments).
5. **K, render stats, refiner.**
   - Do the render-stats scan and the OpenSubdiv refiner cache only if Step 0's trace shows `renderStats` or subdivision above 5% of frame time.
   - Otherwise skip them, and write "skipped: below 5%" in the status row.

**Verify** (after the whole step): `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`; Cornell image check passes.

### Step 10: Final regression run

Run the full verification of the kept steps together.

**Verify**:
- `.\scripts\Build-Backend.ps1`, `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig $PWD\.local\perf-runtime.json` and `.\scripts\Build-Houdini.ps1` → exit 0.
- `.\scripts\Test.ps1 -Suite unit` → passed.
- `hython scripts\run_conformance.py --runtime-config <repo>\.local\perf-runtime.json --only preview_surface/* materialx/* texture/* lux/* render_settings/* volume/*` → `0 failing runs`.
- The Cornell image check passes in both modes.

## Test plan

- No new unit tests are required for the measured items; their guard is the conformance and in-Houdini runs listed per step.
- Step 2 (wet marking): add a case to `tests/surface_effects_test.py` if it can run without UE. Model it after the existing cases in that file. Otherwise rely on `-TestKind surfaces`.
- Step 5 (Niagara): the frame-100 image check is the regression test. If plan 010 added a conformance wrapper with effects scenes, add `samples/docs/effects.usda` at frames 1 and 100 to it.
- The measuring scene and its scripts under `samples/repro/perf/` stay in the repo so the numbers can be re-measured.

## Done criteria

- [ ] `out/perf/measurements.md` has one row per step, with before, after and kept or reverted.
- [ ] Every kept step is its own commit whose body quotes its measurement.
- [ ] `.\scripts\Test.ps1 -Suite unit` exits 0.
- [ ] `hython scripts\run_conformance.py --runtime-config <perf runtime> --only preview_surface/* materialx/* texture/* lux/* render_settings/* volume/*` prints `0 failing runs`.
- [ ] Cornell image check passes in realtime and Path Tracer mode against `out/perf/cornell_base.exr`.
- [ ] `git grep -n "SetMaxSimTime(600)" unreal` returns nothing, if Step 5 was kept.
- [ ] `git diff 6fe3e12 -- unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` shows no change in lines 1332-1344.
- [ ] No files outside the in-scope list are modified (`git status`).
- [ ] `plans/README.md` status row updated, listing kept and reverted steps.

## STOP conditions

Stop and report back (do not improvise) if:

- The code at the locations in "Current state" doesn't match the excerpts.
- A commit message or comment says the `SetMaterialParameters` flush prevents a crash (Step 1).
- Removing the flush (Step 1) crashes or hangs any of the 3 repeated in-Houdini runs.
- `NiagaraComponent.h` has no public way to read the current age or seek state (Step 5).
- Any step seems to need a change to `include/hdue/bridge.h` or the ABI version.
- The image check fails after a change that should not alter pixels, and reverting the change fixes it. Revert, then report, rather than retuning thresholds.
- A step's verification fails twice after a reasonable fix attempt.

## Maintenance notes

- The cached texture list (Step 3) and the cached asset counts (Step 4) must be invalidated by every future ABI entry point that adds, removes or re-materials actors. A reviewer should check the invalidation points against every `Scene->` and `Assets.` mutating call in `Bridge.cpp`.
- The idle tick (Step 8) is a heuristic. Any future feature that needs UE to keep ticking while converged (for example a running simulation) must make the convergence check report "not idle".
- Deferred: render-stats and refiner caching when they measure below 5% (Step 9). The ID-mode switch cost and transform-only edits are in plan 024.
- Rejected on purpose: changing the offline streaming flush. It mirrors Movie Render Queue (`MoviePipelineRendering.cpp:174, 658-664`).
