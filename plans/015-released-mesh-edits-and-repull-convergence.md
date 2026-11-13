# Plan 015: Edits to released meshes always reach UE, re-pulls always finish, and points and curves count toward streaming

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/delegate.cpp src/hydra/prims.cpp src/hydra/materials.cpp samples/conformance/preview_surface`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: S-M
- **Risk**: LOW
- **Depends on**: plans/010-test-tiers-run-everything-they-claim.md (test tiers: run this plan's verification through `Test.ps1` and the
  conformance wrapper once 010 lands; if 010 is not done, use the direct commands below)
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

To save host memory, hdUE frees ("releases") a mesh's vertex arrays once UE has its copy.
Later edits to a released mesh normally travel through `update_meshes` (material, visibility,
sidedness), and when UE needs the geometry again the mesh is re-read from Hydra ("re-pulled").
Two edits that need the geometry are not routed that way: mapping a mesh to a cooked UE asset
(`nativeMeshAssets`) and giving its material a displacement. Both silently do nothing for a
released mesh. Separately, a re-pull whose mesh then fails to triangulate never leaves the
re-pull set, so the render pass never reports convergence (husk waits, the viewport keeps
redrawing). And points and curves never add their bytes to the streaming counter, while
uploads subtract them, so large point and curve scenes never stream. This plan fixes all three
in the delegate.

## Current state

Files:

- `src/hydra/delegate.cpp` — scene upload (`prepareMeshes` loop at 1098-1117; `releasable` at
  1719-1728; `finishScene` at 1730-1779), render settings (`SetRenderSetting`'s
  `nativeMeshAssets` block at 4002-4010; scene-space block at 4080-4092), convergence
  (`IsConverged` at 2377-2383).
- `src/hydra/materials.cpp` — `Material::Sync`'s change handling at 625-651.
- `src/hydra/prims.cpp` — `Mesh::Sync` (error returns at 727-730, 740-743, 818-821; store at
  846-901), `HdUEStoreShape` at 992-1037, `HdUERepullMeshes` at 1898-1913.
- `src/hydra/delegate.h` — `HdUEState`: `repull`, `fullUploads`, `pendingBytes` (lines 392-402).
- `include/hdue/bridge.h:280-289` — `HdueMeshUpdate.needs_upload` is set only "when the change
  needs the geometry again (an unknown or native mesh, a static/editable switch, a Nanite mesh
  given a translucent material or the reverse)". `update_meshes` carries no `native_asset`.

The vocabulary used below (from `delegate.h:392-397`):

- `repull`: Rprims (by `sourceId`) of released meshes an upload needs again. The render pass
  re-syncs them (`HdUERepullMeshes`) and that Sync erases them. Frames do not converge meanwhile.
- `fullUploads`: released meshes whose next Sync must keep its arrays and upload in full.

**The correct pattern exists in two places.** A unit or up-axis change
(`delegate.cpp:4083-4089`):

```cpp
        for (const auto& [id, mesh] : state_->meshes) {
            state_->dirtyMeshes.insert(id);
            // UE holds a released mesh's vertices in the old units and axes: its arrays come back from Hydra.
            if (mesh.released) {
                state_->fullUploads.insert(id);
                state_->repull.insert(mesh.sourceId);
            }
        }
```

and a material whose UV or colour primvars changed (`materials.cpp:642-648`).

**Bug A — cooked-asset mapping** (`delegate.cpp:4002-4010`):

```cpp
    if (key == TfToken("nativeMeshAssets")) {
        std::set<std::string> changed;
        dirtyChangedEntries(previousDictionary, currentDictionary, state_->meshes, changed);
        for (const auto& id : changed) {
            state_->dirtyMeshes.insert(id);
            // Whether its sheared instances bake (HdUEStoreShearCompanion) is decided at Sync.
            if (state_->meshes.at(id).instanced) state_->repull.insert(state_->meshes.at(id).sourceId);
        }
```

A non-instanced released mesh only becomes dirty. In `prepareMeshes` a dirty released mesh
takes the `update_meshes` path and `continue`s (`delegate.cpp:1098-1117`):

```cpp
    for (const auto& id : packet.dirtyMeshes) {
        const auto& data = state.meshes.at(id);
        if (data.released || packet.materialMeshes.count(id)) {
            // UE keeps its geometry: the changed fields go through update_meshes, and the mesh is uploaded again
            // (re-pulled from Hydra if released) only when UE needs it in full (sendScene, finishScene).
            ...
            continue;
        }
```

The backend knows the mesh as a generic one, so it does not set `needs_upload`, and the mesh
stays generic in UE.

**Bug B — displacement** (`materials.cpp:637-640`):

```cpp
        if (displacement(previous == state->materials.end() ? HdUEMaterialData() : previous->second) !=
            displacement(data))
            for (const auto& [id, mesh] : state->meshes)
                if (mesh.material == data.id) state->dirtyMeshes.insert(id);
```

`releasable()` keeps the arrays of meshes whose material displaces (`delegate.cpp:1726-1727`),
because displacement is diced on the CPU from those arrays. A mesh released *before* its
material gained displacement has no arrays, takes the `update_meshes` path, and is never
displaced. Once re-pulled, `Mesh::Sync` sees it is no longer releasable and uploads it in full
(the `unreleased` flag, `prims.cpp:883-892`), so adding it to `fullUploads` and `repull` is enough.

**Bug C — re-pull never finishes after a Sync error.** `Mesh::Sync` erases the id from `repull`
only on its success path (`prims.cpp:890`: `state->repull.erase(state->meshes[id].sourceId);`).
Its error returns skip that, e.g. `prims.cpp:727-730`:

```cpp
        } catch (const std::invalid_argument& error) {
            TF_RUNTIME_ERROR("%s at %s", error.what(), GetId().GetText());
            return;
        }
```

(and the same at `:740-743`, `:818-821`). `HdUERepullMeshes` (`prims.cpp:1898-1913`) only erases
ids whose Rprim no longer exists. `IsConverged` requires `repull.empty()` (`delegate.cpp:2382`).

**Bug D — streaming counter.** `Mesh::Sync` adds bytes when it marks a mesh dirty
(`prims.cpp:893`: `if (state->dirtyMeshes.insert(id).second) state->pendingBytes += HdUEMeshBytes(state->meshes[id]);`)
and then calls `HdUEStreamMeshes(*state)` after unlocking (`prims.cpp:912`). `HdUEStoreShape`
(points and curves) only does `state.dirtyMeshes.insert(key);` (`prims.cpp:1032-1034`) and never
calls `HdUEStreamMeshes`. But `ScenePacket::take` counts every dirty mesh's bytes
(`delegate.cpp:531-538`) and `finishScene` subtracts them (`delegate.cpp:1774-1775`). Note that
`HdUEStoreShape` takes `state.mutex` for its whole body (`prims.cpp:993`), and
`HdUEStreamMeshes` takes it too (`delegate.cpp:1969`), so the stream call must come after the
lock is released.

**Confidence.** Bugs C and D are certain from the code. Bugs A and B are certain on the delegate
side; whether they show depends on the backend behaving as `bridge.h:286-288` documents, so the
first step reproduces bug B before any change.

Conventions: C++ Google style, 4 spaces, 120 columns; comments explain why, one line, as in the
excerpts above.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build the Houdini delegate (Houdini closed; runs ctest) | `.\scripts\Build-Houdini.ps1` | exit 0 |
| Build the standalone delegate | `.\scripts\Build-Host.ps1` | exit 0 |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp src/hydra/prims.cpp src/hydra/materials.cpp` | no output |
| One conformance scene | `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only preview_surface/displacement_animated` | "0 failing runs" |
| Touched areas | `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only preview_surface/* points_curves/* mesh/* native_instancing/*` | "0 failing runs" |
| Streaming test (husk) | `hython tests\husk_streaming.py` (see its docstring for arguments) | exit 0 |
| In-Houdini (scene upload) | `.\scripts\Launch-Houdini.ps1 -TestKind integration`, then `-TestKind recreation` | each exits 0 |

`hython` is `<houdini_root>\bin\hython.exe` (`houdini_root` in `.local/paths.json`).

## Scope

**In scope**:

- `src/hydra/delegate.cpp`
- `src/hydra/materials.cpp`
- `src/hydra/prims.cpp`
- `samples/conformance/preview_surface/displacement_animated.usda` (create)

**Out of scope**:

- `unreal/` (backend) and `include/hdue/bridge.h`: no ABI change; the fix is to stop relying on
  `needs_upload` for edits the backend cannot detect.
- `releasable()`'s rules: which meshes may be released stays as is.
- Moving `ScenePacket` into a header for unit tests: it is large and entangled with
  `HdUEState`; the conformance scene below is the regression test.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, on `main`. Never push unless told.
- One commit per bug (A+B together is fine). Subject: a plain English sentence describing the
  result; body: why and how verified; last line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Steps

### Step 1: Reproduce bug B with an animated displacement (before changing code)

Create `samples/conformance/preview_surface/displacement_animated.usda` from
`samples/conformance/preview_surface/displacement.usda` (same quad, same subdivision override,
same regions), with these differences:

- `"frames": 2` and a top-level `"husk": ["--restart-delegate", "0"]` entry in the conformance
  JSON, as `samples/conformance/volume/sparse_volume_sequence.usda` has, so one delegate renders
  both frames (otherwise each frame gets a new delegate and nothing is released).
- `endTimeCode = 2`.
- `float inputs:displacement.timeSamples = {1: 0, 2: 1}` instead of the constant.
- Checks: frame 1, region `[2.05, -1, 2.18, 1]`, `"stat": "coverage"`, `"max": 0.1` (not
  displaced yet); frame 2, same region, `"min": 0.9` (displaced); frame 2, body region
  `[-1, -1, 1, 1]`, `"min": 0.9`. Put `"frame": N` inside each `"value"` probe.
- The doc string says: frame 1 uploads the quad without displacement and hdUE frees its arrays;
  frame 2 gives its material displacement 1, which must reach the already-uploaded quad.

Run it on the current build with `HDUE_TRACE_SCENE=1` set in the environment:

`hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only preview_surface/displacement_animated`

**Verify**: the frame-2 "displaced" check FAILS in at least one mode, and husk's output (in the
run's log under `out/conformance/`) contains "hdUE released the geometry of" for frame 1.

### Step 2: Route geometry-changing edits of released meshes through a full re-pull (bugs A and B)

In `delegate.cpp`'s `nativeMeshAssets` block (4005-4009), for each changed id, add the mesh to
`fullUploads` and its `sourceId` to `repull` when it is released, keeping the existing instanced
rule:

```cpp
        for (const auto& id : changed) {
            state_->dirtyMeshes.insert(id);
            const auto& mesh = state_->meshes.at(id);
            // A released mesh mapped to (or from) a cooked asset uploads in full: update_meshes carries no asset.
            if (mesh.released) state_->fullUploads.insert(id);
            // Whether its sheared instances bake (HdUEStoreShearCompanion) is decided at Sync.
            if (mesh.instanced || mesh.released) state_->repull.insert(mesh.sourceId);
        }
```

In `materials.cpp:637-640`, do the same for meshes of a material whose displacement changed:

```cpp
            for (const auto& [id, mesh] : state->meshes)
                if (mesh.material == data.id) {
                    state->dirtyMeshes.insert(id);
                    // Displacement is diced from the mesh's arrays: a released mesh comes back from Hydra.
                    if (mesh.released) {
                        state->fullUploads.insert(id);
                        state->repull.insert(mesh.sourceId);
                    }
                }
```

**Verify**: `grep -n "fullUploads.insert" src/hydra/delegate.cpp src/hydra/materials.cpp` shows the
two new sites next to the existing ones (`delegate.cpp` scene-space block, `materials.cpp:646`).

### Step 3: Let a failed re-pull leave the re-pull set (bug C)

In `Mesh::Sync`, every early `return` after a `TF_RUNTIME_ERROR` (at 727-730, 740-743, 818-821,
and any other error return in the function body you find with
`awk 'NR>=579 && NR<=913 && /return;/' src/hydra/prims.cpp`) must erase this Rprim from
`repull` before returning, under the state lock. `Mesh::Sync` already names its state pointer
`state` (`prims.cpp:581`: `auto* state = static_cast<HdUEState*>(param);`), and a mesh's
`sourceId` is its Rprim path (`prims.cpp:614`: `mesh.sourceId = mesh.id;`). Add one local lambda
right after line 581 and call it before each error return:

```cpp
        // An error ends a re-pull too: IsConverged waits for repull to empty.
        const auto giveUp = [&] { state->locked([&] { state->repull.erase(GetId().GetString()); }); };
```

```cpp
        } catch (const std::invalid_argument& error) {
            TF_RUNTIME_ERROR("%s at %s", error.what(), GetId().GetText());
            giveUp();
            return;
        }
```

**Verify**: each error return in `Mesh::Sync` is preceded by the erase (review
`git diff src/hydra/prims.cpp`).

### Step 4: Count points and curves toward streaming (bug D)

In `HdUEStoreShape` (`prims.cpp:992-1037`), add the bytes when the mesh becomes dirty and stream
after the lock is released. Put the existing body in a block so the `lock_guard` ends before the
stream call:

```cpp
void HdUEStoreShape(HdSceneDelegate* delegate, const SdfPath& id, HdUEState& state, HdUEMeshData mesh) {
    {
        std::lock_guard lock(state.mutex);
        ... existing body, with:
        if (changed) {
            if (state.dirtyMeshes.insert(key).second) state.pendingBytes += HdUEMeshBytes(state.meshes[key]);
            ++state.revision;
        } else if (relinked)
            ++state.revision;
    }
    HdUEStreamMeshes(state);  // as Mesh::Sync: a large point or curve scene uploads while Hydra still syncs it
}
```

**Verify**: `sed -n 992,1045p src/hydra/prims.cpp | grep -n "pendingBytes\|HdUEStreamMeshes"` → both present.

### Step 5: Build and re-run the scene from Step 1

**Verify**:
- clang-format check → no output.
- `.\scripts\Build-Houdini.ps1` → exit 0; `.\scripts\Build-Host.ps1` → exit 0.
- `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only preview_surface/displacement_animated` → "0 failing runs" in both modes; with `HDUE_TRACE_SCENE=1` the frame-2 log shows "hdUE re-pulls" or a full upload of `/Quad`.

### Step 6: Regression runs

**Verify**:
- `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only preview_surface/* points_curves/* mesh/* native_instancing/*` → "0 failing runs".
- `hython tests\husk_streaming.py ...` → exit 0 (read its docstring for the arguments; it checks
  streaming uploads during Sync).
- `.\scripts\Launch-Houdini.ps1 -TestKind integration` and `-TestKind recreation` → exit 0.
- A Cornell husk render matches the previous build's.

## Test plan

- New: `samples/conformance/preview_surface/displacement_animated.usda` — fails before (Step 1),
  passes after (Step 5). It is the regression test for bug B and, through the shared code path,
  for the re-pull route bug A also uses.
- Bug A (cooked-asset remap) has no cheap automated repro: it needs a cooked native asset and a
  `nativeMeshAssets` change while meshes are released. Covered by review and by
  `native_instancing/*` staying green. If you have time, extend the in-Houdini `integration` test
  with a remap; otherwise list it as a follow-up in your report.
- Bugs C and D: covered by review plus `husk_streaming.py` and the in-Houdini runs staying green.

## Done criteria

- [ ] `displacement_animated` failed before your change and passes after, in both modes.
- [ ] The `nativeMeshAssets` block and the material displacement block both add released meshes to `fullUploads` and `repull`.
- [ ] Every error return in `Mesh::Sync` erases the Rprim from `repull`.
- [ ] `HdUEStoreShape` adds `pendingBytes` and calls `HdUEStreamMeshes` outside the lock.
- [ ] Builds exit 0; clang-format clean; the Step 6 runs pass.
- [ ] `git status --short` lists only the in-scope files and `plans/README.md`.

## STOP conditions

- Step 1's frame-2 check passes on the current build (bug B does not reproduce). Report the log
  (is the mesh released in frame 1? does frame 2 re-pull?) and stop: the rest of the plan rests
  on that mechanism.
- Step 1's scene fails in frame 1 (the undisplaced quad already reaches the region): the scene is
  wrong, not the delegate; fix the scene, and stop if you cannot.
- After Step 2, frame 2 still fails although the log shows the re-pull: the backend is not
  rebuilding the mesh from a full upload; report and stop.
- Any Step 6 run that passed before fails after.

## Maintenance notes

- Rule for future edits: anything that changes what UE must build from a mesh's arrays
  (geometry, asset mapping, displacement, wetness) must put a released mesh into `fullUploads`
  and its `sourceId` into `repull`; `update_meshes` only carries material, visibility, flags and
  prim id.
- Reviewers: check that no new early return in `Mesh::Sync` forgets the `repull` erase.
- A unit-level characterization of `ScenePacket`/`finishScene` would catch this class of bug
  earlier; it needs `HdUEState` decoupled from Hydra and is out of scope here.
