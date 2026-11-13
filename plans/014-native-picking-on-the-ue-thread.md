# Plan 014: Native picking works when hdUE runs UE on its own thread, and one unpickable asset no longer breaks every pick

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/delegate.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/NativePicking.cpp include/hdue/bridge.h`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: S-M
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

The `unreal:pickRay` renderer command picks the nearest surface along a ray, including cooked
UE assets ("native" meshes). It calls two backend functions that only accept calls from the
thread that started UE. In usdview that is the calling thread, so it works. In husk and
Houdini's viewport hdUE runs UE on a thread of its own, so the backend answers
`HDUE_WRONG_THREAD` and every native pick fails. Separately, the backend's pick refuses the
whole scene when *any* visible native mesh is translucent or deformed, even far from the ray,
and tests instanced native meshes at the origin instead of at their instances. After this
plan, picks work in every host, an unpickable asset only blocks rays that pass through it, and
instanced native meshes are picked where they are drawn.

## Current state

Files:

- `src/hydra/delegate.cpp` — `HdUERenderDelegate::InvokeCommand` (starts at line 4315); the
  pick branch is lines 4323-4457. `ueRun` (lines 237-244) runs a function on the UE thread.
- `unreal/HdUEBackend/Source/HdUEBackend/Private/NativePicking.cpp` — `FHdueScene::PickNative`
  (lines 101-175) and the material check `InspectMaterial` (lines 16-35).
- `include/hdue/bridge.h` — the C ABI; its threading sentence is at lines 61-63.
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` — `PickNative` (line 2268) and
  `GetPickMaterialInfo` (line 2291) both start with `HDUE_RETURN_UNLESS_READY()`, which returns
  `HDUE_WRONG_THREAD` off the owner thread (`CheckOwner`, lines 331-333). Read only; do not change.

`ueRun` (`delegate.cpp:237-244`):

```cpp
// Runs f on the thread that owns UE: hdUE's own UE thread, or inline.
void ueRun(const std::function<void()>& f) {
    auto& host = selfHost();
    if (host.worker.joinable()) {
        host.run(f);
        return;
    }
    f();
}
```

`host.run` queues the work, blocks until the UE thread ran it, and rethrows its exceptions
(a `std::packaged_task`, `delegate.cpp:133-143`).

The pick branch holds the state lock for its whole length and calls the backend directly
(`delegate.cpp:4324`, `:4372-4390`, `:4405-4408`):

```cpp
    std::lock_guard lock(state_->mutex);
    ...
        const auto& api = state_->backend->api();
        for (const auto& [id, mesh] : state_->meshes) {
            ...
            const auto code = api.get_pick_material_info(path.c_str(), &info);
            ...
        }
        ...
        auto code = api.pick_native(state_->session, &ray, &hit, objectId.data(), uint32_t(objectId.size()));
```

You cannot simply wrap these calls in `ueRun` while holding `state_->mutex`: the UE thread may
be inside `executeOnce`, which takes the same mutex, and then both threads wait for each other
(a deadlock). The lock must be released across the hop and taken again afterwards.

Today only `scripts/picking_controls.py` (usdview) sends `unreal:pickRay`, where UE runs inline,
so the bug is latent in husk and Houdini. `src/host/backend_smoke.cpp:384-387` asserts that the
backend refuses `pick_native` from another thread — that is the contract, keep it.

Backend: the pick fails the whole call on the first unsuitable component, anywhere in the
scene (`NativePicking.cpp:107-131`):

```cpp
    for (const auto& Entry : Actors) {
        ...
        auto* Static = Actor->FindComponentByClass<UStaticMeshComponent>();
        UStaticMesh* Source = Static ? Static->GetStaticMesh() : nullptr;
        if (!Source || !Source->IsAsset()) continue;
        TArray<uint32> MaterialFlags;
        for (int32 I = 0; I < Static->GetNumMaterials(); ++I) {
            ...
            if (!(Flags & HDUE_PICK_MATERIAL_OPAQUE) || (Flags & HDUE_PICK_MATERIAL_DEFORMED)) {
                Error = TEXT("Picking requires opaque materials without vertex/displacement deformation.");
                return HDUE_NOT_SUPPORTED;
            }
        ...
        const FMatrix LocalToWorld = Static->GetComponentTransform().ToMatrixWithScale();
```

Instanced native meshes are spawned with an identity actor transform and their instances live
in a `UInstancedStaticMeshComponent` (`Scene.cpp:2378-2380` and `:2454-2466`).
`FindComponentByClass<UStaticMeshComponent>` returns that component (it derives from
`UStaticMeshComponent`), and its component transform is the identity, so the ray is tested
against one prototype at the world origin and never against the instances.

The ABI's threading sentence (`include/hdue/bridge.h:61-63`) says "All calls except get_api
execute on the thread that called initialize", but the backend accepts these from any thread
(they skip `CheckOwner` in `Bridge.cpp`): `get_status`, `poll_frame`, `poll_aov`,
`copy_aov_buffer`, `copy_frame`, `copy_depth`, `acquire_gpu_frame`, `get_load_status` (some are
marked "Any thread" individually at `bridge.h:363, 637, 745, 760`).

Conventions: C++ Google style, 4 spaces, 120 columns; UE code in `unreal/` follows UE naming
(PascalCase locals). Comments explain why, one line.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build the backend (~40 s incremental) | `.\scripts\Build-Backend.ps1` | exit 0 |
| Stage it without touching the default profile | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\out\plan014\runtime.json` | exit 0, profile written |
| Build the Houdini delegate (Houdini closed) | `.\scripts\Build-Houdini.ps1` | exit 0 |
| Build the standalone delegate + smoke host | `.\scripts\Build-Host.ps1` | exit 0 |
| Backend smoke test (native picking contract) | `.\scripts\Test-Backend.ps1` | exit 0 |
| usdview smoke (includes ten picks) | `.\scripts\Test-Usdview.ps1 -Check smoke` | exit 0 |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/NativePicking.cpp include/hdue/bridge.h` | no output |

## Scope

**In scope**:

- `src/hydra/delegate.cpp` (the `unreal:pickRay` branch only)
- `unreal/HdUEBackend/Source/HdUEBackend/Private/NativePicking.cpp`
- `include/hdue/bridge.h` (comment only — no struct or table change, no ABI version bump)

**Out of scope**:

- `Bridge.cpp`'s thread checks: the owner-thread rule is correct; the delegate must follow it.
- `scripts/picking_controls.py` and the usdview host.
- The delegate's validation of native *materials on generic USD meshes* (`delegate.cpp:4372-4390`
  still fails the pick for any such translucent material in the scene). Note it in your report;
  changing which generic meshes are pickable is a separate decision.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, on `main`. Never push unless told.
- Two commits: delegate (+ header comment), then backend. Subjects are plain English sentences
  describing the result; body says why and how it was verified; last line
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Steps

### Step 1: Delegate — gather under the lock, call the backend on the UE thread, store under the lock

Change `std::lock_guard lock(state_->mutex);` at line 4324 to `std::unique_lock lock(state_->mutex);`.
Inside `if (nativeScene) { ... }`:

1. Under the lock, after the readiness check, collect what the backend calls need: the list of
   native material paths not yet in `state_->pickMaterialFlags` (for visible meshes, as the loop
   does today), the `HdueNativeRay` (built as today), the session handle, and the current
   `state_->revision`.
2. `lock.unlock();` then run every backend call in one `ueRun` (wrapped in `try`/`catch` that turns
   an exception into the pick's `"error"`):
   - `get_pick_material_info` for each collected path, into a local `std::vector<std::pair<std::string, HduePickMaterialInfo>>` plus each call's result code;
   - `pick_native`, including the existing `HDUE_BUFFER_TOO_SMALL` retry.
   `state_->backend->status_message()` is safe from any thread (it calls `get_status`).
3. `lock.lock();` then: if `state_->session` or `state_->revision` changed meanwhile, set
   `result["error"]` to "The scene changed during the pick" and return true. Otherwise apply the
   material results exactly as the old loop did (same error strings, same
   `pickMaterialFlags[path] = info.flags`), then handle the hit as before.

Keep the rest of the function (the CPU pick of generic meshes) unchanged; it runs with the lock
held again. Do not call `ueRun` while `lock` owns the mutex.

**Verify**: `sed -n 4323,4430p src/hydra/delegate.cpp | grep -n "ueRun\|unlock\|lock.lock"` shows
`unlock` before `ueRun` and `lock.lock` after it; `grep -n "api.pick_native\|api.get_pick_material_info" src/hydra/delegate.cpp` shows the calls only inside the `ueRun` lambda.

### Step 2: Header — list the calls that run on any thread

Replace the last sentence of the comment at `include/hdue/bridge.h:61-63` with: calls run on the
thread that called initialize, except `get_api`, `get_status`, `get_load_status`, `poll_frame`,
`poll_aov`, `copy_frame`, `copy_depth`, `copy_aov_buffer` and `acquire_gpu_frame`, which any
thread may call. Before writing it, confirm the list: in `Bridge.cpp`, each of these functions
must start without `HDUE_RETURN_UNLESS_READY()`/`CheckOwner()` (`ReadableSession` is the any-thread
guard).

**Verify**: `grep -n "which any" include/hdue/bridge.h` → the new sentence.

### Step 3: Backend — skip components the ray cannot reach, before judging their materials

In `FHdueScene::PickNative`, before the material loop, test the ray segment against the
component's world bounds and `continue` when it misses:

```cpp
        // Only a component the ray can reach decides the pick: an unpickable asset elsewhere must not fail it.
        const FBox Bounds = Static->Bounds.GetBox();
        if (!FMath::LineBoxIntersection(Bounds, Origin, Origin + Direction * Nearest, Direction * Nearest)) continue;
```

Then keep the existing material and geometry checks: a component the ray *does* reach and that
cannot be picked still returns `HDUE_NOT_SUPPORTED` (the pick cannot know whether it would have
hit it). Add the asset's path to that error message (`Source->GetPathName()`), so the user knows
which asset to fix.

**Verify**: `grep -n "LineBoxIntersection" unreal/HdUEBackend/Source/HdUEBackend/Private/NativePicking.cpp` → 1 match before `InspectMaterial(Static->GetMaterial`.

### Step 4: Backend — test each instance of an instanced static mesh component

Move the per-transform ray test (lines 132-172: from `LocalToWorld` to the end of the hit
handling) into a small lambda that takes one `FMatrix LocalToWorld`. Call it once with
`Static->GetComponentTransform().ToMatrixWithScale()` for plain components. For a
`UInstancedStaticMeshComponent` (`Cast<UInstancedStaticMeshComponent>(Static)`), call it for each
instance with `GetInstanceTransform(I, Transform, /*bWorldSpace*/ true)` and
`Transform.ToMatrixWithScale()`. The `Nearest` distance carries across instances as it does
across actors. Add `#include "Components/InstancedStaticMeshComponent.h"`.

**Verify**: `grep -n "GetInstanceTransform" unreal/HdUEBackend/Source/HdUEBackend/Private/NativePicking.cpp` → 1 match.

### Step 5: Build, format, test

**Verify**:
- clang-format check from the table → no output.
- `.\scripts\Build-Backend.ps1` → exit 0; `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\out\plan014\runtime.json` → exit 0.
- `.\scripts\Build-Host.ps1` → exit 0; `.\scripts\Test-Backend.ps1` → exit 0 (its native-picking
  checks, including "Native picking ignored the UE owner thread", still pass).
- `.\scripts\Build-Houdini.ps1` → exit 0.
- `.\scripts\Test-Usdview.ps1 -Check smoke -RuntimeConfig H:\github\hdUE\out\plan014\runtime.json` → exit 0
  (the inline UE path still picks).
- A Cornell husk render matches the previous build's.

## Test plan

- `Test-Backend.ps1` (`src/host/backend_smoke.cpp`) already covers native picking on the owner
  thread: hits, near clipping, hidden actors, buffer sizes. It must stay green.
- `Test-Usdview.ps1 -Check smoke` covers the delegate's pick command with UE inline.
- No existing harness sends `unreal:pickRay` while hdUE runs UE on its own thread (husk,
  Houdini). If you can add one cheaply, extend `src/host/backend_smoke.cpp` with a pick through
  an instanced native mesh (two instances away from the origin, a ray through the second one →
  a hit at that instance, not at the origin) — only if the smoke host already loads a native
  StaticMesh fixture; otherwise list it as a follow-up in your report.

## Done criteria

- [ ] `api.pick_native` and `api.get_pick_material_info` are only called inside a `ueRun` lambda in `delegate.cpp`.
- [ ] The pick branch never holds `state_->mutex` across `ueRun` (review the diff; `unlock` before, `lock` after).
- [ ] `NativePicking.cpp` has the bounds test and the per-instance loop.
- [ ] `bridge.h` lists the any-thread calls; `git diff include/hdue/bridge.h` changes comments only.
- [ ] Builds, `Test-Backend.ps1` and `Test-Usdview.ps1 -Check smoke` exit 0; clang-format clean.
- [ ] `git status --short` lists only the three in-scope files and `plans/README.md`.

## STOP conditions

- `Test-Backend.ps1` fails on a native-picking check after Step 3 or 4.
- You find another caller of `api.pick_native` or `api.get_pick_material_info` outside this branch.
- The list of any-thread functions in `Bridge.cpp` differs from the one in Step 2 (a function
  you expected to be any-thread calls `CheckOwner`): report the difference instead of guessing.
- Making the delegate change requires changing `HdueApi` or any struct in `bridge.h`.

## Maintenance notes

- Pattern for any future delegate code that calls the backend from a host thread: copy what you
  need under `state_->mutex`, release it, call through `ueRun`, retake it and re-check
  `session`/`revision`.
- The per-instance loop is O(instances) per component per pick. Fine for interactive picks; if a
  scene scatters millions of native instances, add a per-instance bounds test before the BVH query.
- Deferred: generic USD meshes with translucent native materials still fail the whole pick
  (`delegate.cpp:4372-4390`). Whether to skip them instead is a product decision.
