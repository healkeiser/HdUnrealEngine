# Plan 007: Groom uploads reject bad descriptors, and groom and volume actors die with their scene

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.h include/hdue/bridge.h src/host/backend_smoke.cpp`
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

hdUE (a Hydra render delegate that runs Unreal Engine 5.8 inside Houdini and husk) uploads hair as UE "grooms" through the C ABI call `upsert_groom`. The backend checks points and curve counts, but not the instance array or the attribute array. A descriptor that says "2 instances" or "3 attributes" with a null pointer makes the backend read through null and crash the whole host process (Houdini or husk). A huge `instance_count` spawns one actor per instance on the UE thread with no limit.

Separately, the scene object (`FHdueScene`) destroys only its mesh actors when it is destroyed. Groom and volume actors outlive their session and keep rendering and holding GPU memory until something else sweeps them. When the scene is reset (`reset_scene`), the groom actors are destroyed by the reset sweep, but the `Grooms` map keeps raw pointers to them; a later `IsValid()` on those pointers, after UE's garbage collector frees the actors, reads freed memory.

After this plan: bad groom descriptors are rejected with a message instead of crashing, and every groom and volume actor is destroyed with its scene or on reset.

## Current state

Files (all under `unreal/HdUEBackend/Source/HdUEBackend/Private/` unless noted):

- `Bridge.cpp` — the C ABI entry points. `UpsertGroom` validates the descriptor (lines 1551-1566).
- `Scene.cpp` — the scene: `FHdueScene::UpsertGroom` builds the groom (lines 247-351), the destructor (lines 69-84), `Remove` (lines 146-160), and the `reset_scene` branch of `Apply` (lines 1965-1992).
- `Scene.h` — the `Volumes` and `Grooms` maps (lines 128-138).
- `include/hdue/bridge.h` — the ABI: `HdueGroomDesc` (lines 520-545).
- `src/host/backend_smoke.cpp` — the native smoke test of the backend (`Test-Backend.ps1` runs it).

`Bridge.cpp:1551-1566`, today's validation:

```cpp
HdueResult HDUE_CALL UpsertGroom(HdueSession Handle, const HdueGroomDesc* Desc) {
    HDUE_RETURN_UNLESS_READY();
    if (!ValidSession(Handle) || !Sized(Desc) || !Desc->id || !Desc->point_count || !Desc->curve_count ||
        Desc->point_count > (1u << 26) || !Desc->points || !Desc->widths_cm || !Desc->curve_point_counts)
        return HDUE_INVALID_ARGUMENT;
    uint64 Sum = 0;
    for (uint32 I = 0; I < Desc->curve_count; ++I) Sum += Desc->curve_point_counts[I];
    if (Sum != Desc->point_count) return HDUE_INVALID_ARGUMENT;
    for (uint64 I = 0; I < uint64(Desc->point_count) * 3; ++I)
        if (!FMath::IsFinite(Desc->points[I])) return HDUE_INVALID_ARGUMENT;
    const auto Session = Runtime.Session;
    FString Error;
    if (!Session->Scene->UpsertGroom(*Desc, Error)) return Fail(HDUE_ENGINE_ERROR, *Error);
    Session->HasRequest = false;  // restart accumulation, as scene batches do
    return HDUE_OK;
}
```

`Scene.cpp:255-262` and `Scene.cpp:332-339` read the arrays the validation never checked:

```cpp
        SetHairVertexAttribute(Description, FVertexID(int32(I)), HairAttribute::Vertex::Width,
                               FMath::Max(0.f, Desc.widths_cm[I]));       // NaN passes through FMath::Max
    ...
    for (uint32 A = 0; A < Desc.attribute_count; ++A) {
        const HdueGroomAttribute& Attribute = Desc.attributes[A];         // null when attributes == nullptr
    ...
    const uint32 Count = FMath::Max(1u, Desc.instance_count);
    for (uint32 I = 0; I < Count; ++I) {
        AActor* Actor = World->SpawnActor<AActor>();                      // one actor per instance, no cap
        AddRootComponent<UGroomComponent>(Actor, [&](UGroomComponent& Component) {
            if (Desc.instance_count) {
                FMatrix M;
                for (int32 R = 0; R < 4; ++R)
                    for (int32 C = 0; C < 4; ++C) M.M[R][C] = Desc.instance_transforms[I * 16 + R * 4 + C];
```

`Scene.cpp:69-84`, the destructor destroys `Actors` only:

```cpp
FHdueScene::~FHdueScene() {
    ...
    GHdueLoad.MeshBuildsDone += uint32(PendingStatic.Num());
    for (auto& Entry : Actors)
        if (IsValid(Entry.Value)) World->DestroyActor(Entry.Value);
}
```

`Scene.cpp:1965-1992`, the reset branch: the "Fixtures" sweep destroys every persistent-level actor with a `UMeshComponent` (a `UGroomComponent` is one: `Engine/Plugins/Runtime/HairStrands/Source/HairStrandsCore/Public/GroomComponent.h:29`, `class UGroomComponent : public UMeshComponent`), then `Volumes` is emptied but `Grooms` is not:

```cpp
        for (auto* A : Fixtures) World->DestroyActor(A);
        Actors.Empty();
        ...
        for (const auto& Entry : Volumes)
            if (IsValid(Entry.Value.Actor)) World->DestroyActor(Entry.Value.Actor);
        Volumes.Empty();
        WetBindings.Empty();
        GeometryRevisions.Empty();
    }
```

`Scene.h:128-138`:

```cpp
    struct FVolume {
        AActor* Actor = nullptr;
        ...
    };
    TMap<FString, FVolume> Volumes;
    struct FGroom {
        TArray<AActor*> Actors;  // one per instance
        TStrongObjectPtr<class UGroomAsset> Asset;
    };
    TMap<FString, FGroom> Grooms;  // upsert_groom, by id
```

The existing per-id removal already does it right, in `Scene.cpp:146-160` (`FHdueScene::Remove`): it calls `RemoveVolume(Id)` and destroys each groom actor, then `Grooms.Remove(Id)`. Reuse that loop shape.

`include/hdue/bridge.h:543-544`, a field the backend never reads (nothing under `unreal/HdUEBackend/Source` references `simulate`; `docs/geometry/hair.md` says hair physics does not run):

```c
    /* ABI 24: UE's groom physics (the groom asset's simulation, guides from every strand): 1 simulates it. */
    uint32_t simulate;
```

Conventions:
- Validation failures that should explain themselves use `Fail(Code, Message)` (`Bridge.cpp:327-330`), which stores the message for `get_status`. Example: `Fail(HDUE_ENGINE_ERROR, *Error)` above.
- Comments explain why, at the density of the surrounding code. C++ style is `.clang-format` (Google, 4 spaces, 120 columns).
- The smoke test checks results with `check(bool, message)` (`backend_smoke.cpp:95`); see `backend_smoke.cpp:366-368` for an invalid-argument check:

```cpp
            backend.require(backend.api().destroy_session(session), "destroy_session");
            check(backend.api().destroy_session(session) == HDUE_INVALID_ARGUMENT,
                  "Destroyed session remained accessible");
```

Do not change the ABI layout or `HDUE_ABI_VERSION`. This plan only adds checks and fixes lifetimes.

## Commands you will need

| Purpose | Command | Expected on success |
|---------|---------|---------------------|
| Build the backend | `.\scripts\Build-Backend.ps1` | exit 0 (~40 s incremental) |
| Stage it without touching the default profile | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\.local\runtime-plan007.json` | exit 0, the profile file exists |
| Build the standalone host and smoke test | `.\scripts\Build-Host.ps1` | exit 0, ctest passes |
| Native backend smoke test | `.\scripts\Test-Backend.ps1 -RuntimeConfig H:\github\hdUE\.local\runtime-plan007.json` | prints `Verified real UE output: ...` |
| Groom conformance | `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-plan007.json --only points_curves/*` | `0 failing runs` |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp src/host/backend_smoke.cpp` | exit 0, no output |

## Scope

**In scope** (the only files you should modify):
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` (`UpsertGroom` only)
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` (destructor and the reset branch of `Apply` only)
- `include/hdue/bridge.h` (the comment on `simulate` only)
- `src/host/backend_smoke.cpp` (new invalid-groom checks)

**Out of scope** (do NOT touch, even though they look related):
- The `simulate` field itself: do not remove or rename it. Removing it changes the ABI (a version bump that rebuilds both sides); whether to implement groom physics or drop the field is the maintainer's decision.
- Mesh, volume or light validation in `Bridge.cpp` — plan 019 owns finite checks on meshes and other scene limits.
- What the delegate does when `upsert_groom` fails (`src/hydra/delegate.cpp`) — plan 016 owns "one bad item does not fail the scene".
- `Scene.h` — no struct changes are needed.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, `main` (the repo's only branch, per `AGENTS.md`).
- One commit per step. Subjects are plain English sentences, for example `Logging in the Test backend; the viewport lighting that restarts hdUE`. The body says why and how it was verified, and ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless the operator told you to.

## Steps

### Step 1: Reject groom descriptors with missing or non-finite arrays, and cap the instance count

In `Bridge.cpp` `UpsertGroom`, after the existing point checks and before `const auto Session = Runtime.Session;`, add:

- `Desc->attribute_count && !Desc->attributes` → return `Fail(HDUE_INVALID_ARGUMENT, TEXT("Groom attributes are missing."))`.
- `Desc->instance_count && !Desc->instance_transforms` → `Fail(HDUE_INVALID_ARGUMENT, TEXT("Groom instance transforms are missing."))`.
- `Desc->instance_count > 65536` → `Fail(HDUE_INVALID_ARGUMENT, TEXT("Too many groom instances (at most 65536)."))`. Add a one-line comment saying why: each instance is a spawned actor on the host's UE thread (`Scene.cpp` `UpsertGroom`).
- Every `widths_cm[I]` for `I < point_count` must be finite: `!FMath::IsFinite(...)` → `HDUE_INVALID_ARGUMENT`, as the points loop does.
- Every one of the `instance_count * 16` values of `instance_transforms` must be finite, same pattern.

Keep the existing bare `return HDUE_INVALID_ARGUMENT;` lines as they are; only the new checks get messages.

**Verify**: `.\scripts\Build-Backend.ps1` → exit 0. Then `git diff --stat` → only `Bridge.cpp` changed in this step.

### Step 2: Destroy groom and volume actors when the scene is destroyed or reset

In `Scene.cpp`:

1. In `FHdueScene::~FHdueScene`, after the `Actors` loop, destroy every `Volumes` entry's `Actor` and every actor of every `Grooms` entry, with the same `if (IsValid(Actor)) World->DestroyActor(Actor);` pattern. Comment why: a session's grooms and volumes must not outlive it (Houdini recreates its renderer, and the next session's actors would draw beside them).
2. In the `reset_scene` branch of `Apply`, next to the existing `Volumes` loop, add a loop that destroys every groom actor (same pattern), then `Grooms.Empty();`. Comment why: the Fixtures sweep above already destroys groom actors (a `UGroomComponent` is a `UMeshComponent`), and `Grooms` would otherwise keep raw pointers to actors the garbage collector frees.

Do not change `Remove`; it is already correct.

**Verify**: `.\scripts\Build-Backend.ps1` → exit 0. `grep -n "Grooms.Empty" unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` → exactly one match.

### Step 3: Say in the header that `simulate` is not implemented

In `include/hdue/bridge.h`, replace the comment above `uint32_t simulate;` with:

```c
    /* ABI 24: reserved for UE's groom physics; not implemented, the backend ignores it (docs/geometry/hair.md). */
```

Do not change the field. Do not change `HDUE_ABI_VERSION`.

**Verify**: `grep -n "simulate" include/hdue/bridge.h` → one line with "not implemented" and the `uint32_t simulate;` line. `ctest --test-dir build -C Release --output-on-failure` (after `.\scripts\Build-Host.ps1`) → the `abi` layout test passes (the struct size is unchanged).

### Step 4: Add smoke-test checks for bad groom descriptors and for reset

In `src/host/backend_smoke.cpp`, inside the per-cycle session block, before `backend.require(backend.api().destroy_session(session), "destroy_session");` (around line 366), add a small block that runs only in the first cycle (`cycle == 0`):

1. Build a valid one-strand groom: `HdueGroomDesc` with `struct_size = sizeof(...)`, `id = "smoke_groom"`, `visible = 1`, 2 points (e.g. `(0,0,0)` and `(0,0,10)` cm), `widths_cm = {0.1f, 0.1f}`, `curve_count = 1`, `curve_point_counts = {2}`. `backend.require(backend.api().upsert_groom(session, &groom), "valid groom")`.
2. Same descriptor with `attribute_count = 1, attributes = nullptr` → `check(... == HDUE_INVALID_ARGUMENT, "Groom without its attribute array was accepted")`.
3. With `instance_count = 2, instance_transforms = nullptr` → `HDUE_INVALID_ARGUMENT`.
4. With `instance_count = 70000` and a valid transforms array → `HDUE_INVALID_ARGUMENT` (build the array with `std::vector<double>(70000 * 16, 0.0)` and set the diagonal to 1 per instance; or leave all zeros, since the count check must reject it first).
5. With a `NaN` width (`std::nanf("")`) → `HDUE_INVALID_ARGUMENT`.
6. Apply an empty scene batch with `reset_scene = 1` (see how `empty_batch` is built near line 136 and set its `reset_scene` field), then upsert the valid groom again with the same id → `backend.require(..., "groom after reset")`. This exercises the reset path that used to leave dangling pointers.

Find the exact field names in `include/hdue/bridge.h` (`HdueGroomDesc`, lines 520-545; `HdueSceneBatch`). Include `<cmath>` is already present.

**Verify**: `.\scripts\Build-Host.ps1` → exit 0. Then `.\scripts\Build-Backend.ps1`, `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\.local\runtime-plan007.json`, and `.\scripts\Test-Backend.ps1 -RuntimeConfig H:\github\hdUE\.local\runtime-plan007.json` → `Verified real UE output: ...`, no `check` failure message.

### Step 5: Run the groom conformance scenes and a Cornell render

**Verify**:
- `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-plan007.json --only points_curves/*` → `0 failing runs` (this includes `groom.usda` and `groom_attributes.usda`, whose instanced curtain uses `instance_transforms`).
- Render the Cornell box with husk against the new profile and against the previous profile (`.local\runtime-houdini.json`), as `AGENTS.md` asks for every change. The two images match (no geometry in Cornell uses grooms).
- The format check in "Commands you will need" exits 0.

## Test plan

- New checks in `src/host/backend_smoke.cpp` (Step 4): valid groom accepted; null attribute array, null instance transforms, 70,000 instances and a NaN width each rejected with `HDUE_INVALID_ARGUMENT`; a groom upserted again after a `reset_scene` batch is accepted. Pattern: the existing `destroy_session` invalid-argument check at `backend_smoke.cpp:366-368`.
- Existing coverage that must stay green: `points_curves/groom.usda` and `points_curves/groom_attributes.usda` conformance scenes (both modes).
- Not covered by an automated test: the destructor change (a destroyed session's grooms no longer linger). The next session's first batch resets the scene and sweeps them anyway, so no image test can see the old leak; review the diff instead.

## Done criteria

- [ ] `.\scripts\Build-Backend.ps1` exits 0
- [ ] `.\scripts\Build-Host.ps1` exits 0 and ctest passes (ABI layout unchanged)
- [ ] `.\scripts\Test-Backend.ps1 -RuntimeConfig <plan profile>` prints `Verified real UE output`
- [ ] `run_conformance.py --only points_curves/*` prints `0 failing runs`
- [ ] `grep -n "Grooms.Empty" unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` returns one line
- [ ] `grep -n "not implemented" include/hdue/bridge.h` returns the `simulate` comment
- [ ] `git diff 6fe3e12 -- include/hdue/bridge.h` changes only that comment (no `HDUE_ABI_VERSION` change)
- [ ] clang-format check exits 0
- [ ] No files outside the in-scope list are modified (`git status`)
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- The code at the locations in "Current state" does not match the excerpts.
- The delegate (`src/hydra/delegate.cpp`) is found to send `attribute_count > 0` with a null `attributes`, or more than 65,536 instances, in a normal scene (for example a conformance scene starts failing with the new messages). That would mean the cap or the check breaks real content; report the scene.
- `Test-Backend.ps1` crashes inside the new reset step (step 4.6): that is the use-after-free this plan is about, and it needs a closer look, not a workaround.
- A fix seems to need a change to `HdueGroomDesc`'s layout or `HDUE_ABI_VERSION`.

## Maintenance notes

- If groom physics is ever implemented, `simulate` becomes live again: update the header comment and `docs/geometry/hair.md` together.
- Any new per-id actor map added to `FHdueScene` (like `Volumes` and `Grooms`) needs the same three places: `Remove`, the destructor and the reset branch. A reviewer should check all three when one is added.
- The 65,536 instance cap is a guard, not a measured limit. If someone needs more groom instances, move grooms to one component per prototype (as meshes use instanced static meshes) rather than raising the cap.
- When `upsert_groom` now fails, the delegate's scene upload fails as a whole; plan 016 changes that to skip the item with a warning.
