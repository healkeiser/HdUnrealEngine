# Plan 024: Moving or re-shading instancers, volumes, grooms and placed actors keeps their uploaded data

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/prims.cpp src/hydra/delegate.cpp src/hydra/delegate.h include/hdue/bridge.h unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.h unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition. Plan 015 changes `prims.cpp` and
> `delegate.cpp` too: when it has landed, expect drift there and re-read the
> excerpts.

## Status

- **Priority**: P3
- **Effort**: L
- **Risk**: MED-HIGH
- **Depends on**: plans/010-test-tiers-run-everything-they-claim.md (verification tiers), plans/015-released-mesh-edits-and-repull-convergence.md (released-mesh edits)
- **Category**: perf
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

Plain meshes already have a cheap path: a transform-only edit sends one matrix and keeps the geometry in UE. Five other kinds of object have no such path:

- **Instanced prototypes** (the meshes a PointInstancer copies) rebuild their full geometry on every instancer edit.
- **Volumes** copy their whole density grid on a move.
- **Grooms** (hair) rebuild their hair asset on any edit.
- **Materials**: a material colour edit makes a new UE material instance.
- **Placed actors** (fog volumes, decals) are destroyed and spawned again on a move.

Dragging one of these in Houdini costs time proportional to its data, not to the edit. Animated instancers pay it every frame in husk. This plan adds the cheap path to each kind, one at a time, each measured and each kept only if the image is unchanged.

## Current state

Read at `6fe3e12`.

**Terms.**
- *Prototype*: the mesh an instancer copies.
- *ISM*: UE's `UInstancedStaticMeshComponent`, which draws many copies of one mesh.
- *MID*: UE's `UMaterialInstanceDynamic`, a material instance whose parameters can change at runtime.
- *Released mesh*: a mesh whose CPU arrays the delegate freed after UE received them. It must be re-read from Hydra ("re-pulled") if UE needs it again.

### 1. Instanced prototypes always take the full Sync path

The quick path in `Mesh::Sync` refuses any instanced mesh (`src/hydra/prims.cpp:582-585`):

```cpp
        // A transform, visibility or purpose edit of a mesh without instancer keeps its geometry.
        if (!emitted_.empty() && GetInstancerId().IsEmpty() &&
            !(*bits & ~(HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
                        HdChangeTracker::DirtyRenderTag))) {
```

The "moved" fast send also excludes instanced meshes (`src/hydra/delegate.h:182-185`):

```cpp
inline bool HdUEPlacedOnly(const HdUEMeshData& mesh, const GfMatrix4d& transform) {
    return mesh.transform != transform && !mesh.instanced && mesh.sceneAsset.empty() && !HdUEShears(mesh.transform) &&
           !HdUEShears(transform);
}
```

Every instancer Sync bumps one global epoch, which invalidates the cached placements of *every* instancer (`prims.cpp:322`, `:365`, `:455-470`):

```cpp
std::atomic<uint64_t> instancerEpoch{1};
...
    void Sync(HdSceneDelegate* delegate, HdRenderParam*, HdDirtyBits* bits) override {
        ++instancerEpoch;
...
        const uint64_t epoch = instancerEpoch;
        if (!sampled) {
            std::lock_guard lock(placementsMutex_);
            if (const auto it = placements_.find(prototype); it != placements_.end() && it->second.first == epoch)
                return it->second.second;  // shared, copy on write
        }
```

The backend can already move every instance of one ISM in place, with no ABI change, when the instance count is unchanged (`Scene.cpp:464-483`):

```cpp
void FHdueScene::SetTransforms(const FString& Id, uint32 Count, const double* Matrices) {
    AActor* Actor = Actors.FindRef(Id);
    if (!IsValid(Actor) || !Count) return;
    ...
    if (auto* Instances = Cast<UInstancedStaticMeshComponent>(Actor->GetRootComponent())) {
        if (int32(Count) != Instances->GetInstanceCount()) return;
        ...
        // Not a teleport: UE keeps each instance's previous transform for the velocity buffer (motion vectors).
        Instances->BatchUpdateInstancesTransforms(0, Transforms, true, true, false);
    } else if (Count == 1)
        Actor->SetActorTransform(ToTransform(0), false, nullptr, ETeleportType::TeleportPhysics);
}
```

ABI (`include/hdue/bridge.h:426-430`, `:729`):

```c
typedef struct HdueMeshTransform {
    const char* id;
    uint32_t count;
    const double* matrices;
} HdueMeshTransform;
...
    HdueResult(HDUE_CALL* set_transforms)(HdueSession session, const HdueMeshTransform* meshes, uint32_t count);
```

The delegate sends moved meshes through it today, with `count = 1` (`delegate.cpp:1087-1097`):

```cpp
    // A moved mesh keeps its geometry in UE; only its actor's transform is sent (HdUEPlacedOnly).
    for (const auto& id : packet.movedMeshes) {
        if (packet.dirtyMeshes.count(id)) continue;
        if (packet.firstBatch) {
            packet.dirtyMeshes.insert(id);
            continue;
        }
        std::vector<double> matrix(16);
        rowMajor(inverse * state.meshes.at(id).transform * basis, matrix.data());
        packet.moves.push_back({packet.own(id), 1, packet.own(packet.doubles, std::move(matrix))});
    }
```

The motion-blur sub-frame path already sends N instance matrices this way (`delegate.cpp:2018-2085`; read it before Step 2).

When the full path runs, the backend re-hashes the mesh (`Scene.cpp:2266-2277`), then clears and re-adds every instance and its custom data (`Scene.cpp:2455-2476`):

```cpp
            Instances->ClearInstances();
            ...
            Instances->SetNumCustomDataFloats(4);
            Instances->AddInstances(Transforms, false, true);
            for (uint32 I = 0; I < Desc.instance_count; ++I) {
                float Data[4] = {1.f, 1.f, 1.f, float(Desc.instance_ids ? Desc.instance_ids[I] : int32(I))};
                if (Desc.instance_colors) FMemory::Memcpy(Data, Desc.instance_colors + I * 3, sizeof(float) * 3);
                Instances->SetCustomData(int32(I), MakeArrayView(Data, 4), I + 1 == Desc.instance_count);
            }
```

### 2. Volumes keep their grid on the delegate side, but re-send all of it

The delegate keeps the loaded grid on a placement edit (`prims.cpp:1604-1616`):

```cpp
        // Visibility, purpose, transform, material and instance edits keep the loaded grid instead of reading the file
        // again.
        if (gridToLocal_ && !emitted_.empty() &&
            !(*bits & ~(HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyRenderTag |
                        HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyMaterialId |
                        HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex))) {
            ...
                publish(loaded);
```

But `publish` marks the volume dirty (`prims.cpp:1592`), and the upload copies the whole density, and emission when present, into the packet under the state lock (`delegate.cpp:1375`, `:1384-1385`):

```cpp
        desc.density = packet.own(packet.floats, std::vector<float>(volume.density.begin(), volume.density.end()));
```

The backend then refills the 3D texture voxel by voxel and calls `UpdateResource` (`Scene.cpp:386-411`, the `Fill` lambda).

### 3. Grooms have no quick path

`Groom` Sync (`prims.cpp:1292-1523`) re-reads everything on any dirty bit. The upload converts each point through `state.convert` (`delegate.cpp:1490-1494`):

```cpp
        for (const auto& p : groom.points) {
            const auto ue = state.convert(place.Transform(GfVec3d(p)));
            points.insert(points.end(), {float(ue[0]), float(ue[1]), float(ue[2])});
        }
```

`convert` calls `converter()`, which reads two settings under the settings lock, on every call (`delegate.h:520-523`):

```cpp
    Converter converter() const {
        return {setting<double>("metersPerUnit", 0.01) * 100, setting<std::string>("upAxis", "Y") == "Z"};
    }
    GfVec3d convert(const GfVec3d& point, bool direction = false) const { return converter()(point, direction); }
```

The mesh path hoists it once (`prepareMeshes` builds a `MeshConversion`, `delegate.cpp:1080-1085`). The backend rebuilds the whole groom asset (hair description, build data) on every `upsert_groom` (`Scene.cpp:247-350`).

### 4. A material colour edit makes a new UE material instance

Generated materials always get a fresh MID (`Scene.cpp:2137-2140`):

```cpp
            auto* Parent = LoadObject<UMaterialInterface>(
                nullptr, *FString::Printf(TEXT("/Game/HdUE/%s.%s"), *ParentName, *ParentName));
            if (Parent) {
                auto* Instance = UMaterialInstanceDynamic::Create(Parent, World);
```

The native-asset branch right above already reuses the existing instance when the parent is the same (`Scene.cpp:2097-2103`):

```cpp
                const auto* Entry = Materials.Find(UTF8_TO_TCHAR(Desc.id));
                auto* Existing = Entry ? Cast<UMaterialInstanceDynamic>(Entry->Get()) : nullptr;
                if (Existing && Existing->Parent == Material) {
                    Existing->ClearParameterValues();
                    Material = Existing;
                } else
                    Material = UMaterialInstanceDynamic::Create(Material, World);
```

A new MID makes `BindMaterials` call `SetMaterial` on every mesh using it (`Scene.cpp:1253-1275`), which rebuilds their render state. The retire comment (`Scene.cpp:2219-2220`) records a past crash: "ray-tracing instance gathering crashed on a material edit". Reusing the instance avoids replacing it at all, so it does not reopen that crash; it is the same pattern UE's native branch uses.

### 5. Placed actors are destroyed and respawned on every upsert

`Bridge.cpp:1804-1817` (local fog; post-process volume at `:1874` and decal at `:1936` are the same):

```cpp
// upsert_local_fog_volume: rebuilt on every upsert (the delegate sends a volume only when it changed), with its
// properties set before it registers.
...
    if (const auto* Old = Session->PlacedActors.Find(Id); Old && Old->IsValid()) (*Old)->Destroy();
    Session->PlacedActors.Remove(Id);
    Session->HasRequest = false;  // restart accumulation, as scene batches do
    if (!Desc->visible) return HDUE_OK;
    AActor* Actor = Runtime.World->SpawnActor<AActor>();
```

The delegate sends an actor whenever its `HdUEActorData` changes, and a transform-only edit changes it (`prims.cpp:1993-2001`).

### 6. Optional: ID mode switches walk every mesh

`FHdueScene::SetIdMode` (`Scene.cpp:530-601`) walks every actor and component each time the viewport turns ID passes on or off (after one second of stillness). Do this one only if Step 0 measures it above 100 ms on the test scene.

### Conventions

- C++ is clang-format (Google style, 4 spaces, 120 columns). Comments explain why, at the density of the surrounding code.
- Mirror an existing pattern whenever one exists. Each step names its model: the plain-mesh moved path, `SetTransforms`, the native MID reuse, `MeshConversion`.
- Project rule: do what Unreal does. `BatchUpdateInstancesTransforms` with teleport off keeps motion vectors, as UE's own instance updates do.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build backend | `.\scripts\Build-Backend.ps1` | exit 0 |
| Stage side runtime | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig $PWD\.local\xform-runtime.json` | exit 0 |
| Build delegate + unit tests | `.\scripts\Build-Houdini.ps1` (Houdini closed) | exit 0, ctest passes |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| Conformance | `hython scripts\run_conformance.py --runtime-config <repo>\.local\xform-runtime.json --only instancer/* native_instancing/* volume/* points_curves/* shade/*` | `0 failing runs` |
| In-Houdini | `.\scripts\Launch-Houdini.ps1 -TestKind <kind>` (`integration`, `selection`, `decal`, `volume-material`, `materials`) | exit 0 |
| Timeline | `$env:HDUE_TRACE_TIMING = 1`, `$env:HDUE_TRACE_SCENE = 1` | per-upload lines on stderr |
| Format | `uvx --from clang-format==19.1.5 clang-format -i <file>` | no diff |

Image check: render the same frame before and after with husk, then compare mean absolute difference with the hython snippet in plan 023's "Commands you will need" (`OpenImageIO`, mean |a−b| below 1% of mean |a|).

## Scope

**In scope**:
- `src/hydra/prims.cpp`, `src/hydra/delegate.cpp`, `src/hydra/delegate.h`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp`, `Scene.h`, `Bridge.cpp`
- `include/hdue/bridge.h`, `tests/abi_layout.c`, `scripts/hdue_runtime.py`: **only** if Step 3 needs a placement-only volume or groom call. See STOP conditions: ask first.
- `samples/repro/transform_only/` (create: small test scenes and the hython scripts that edit them)

**Out of scope**:
- The plain-mesh moved path itself (`HdUEPlacedOnly` for non-instanced meshes). It works. Extend it, do not change its existing behaviour.
- Sheared instances (`HdUEStoreShearCompanion`, `delegate.cpp:1848-1929`). Sheared prototypes keep the full path.
- `Scene.cpp:2219-2222` (`RetiredMaterials`). Do not remove the retire step.
- Anything in plan 023 (material flush, wet marking, Niagara).
- Sponza, Jungle Ruins and Da Vinci (AGENTS.md: small scenes only).

## Git workflow

- Work on the operator's branch or worktree; if none, `main` (AGENTS.md).
- One commit per step that is kept. Plain English subject, e.g. "Instancer edits move UE's instances without re-uploading the prototype". Body: why, before and after times, and the conformance areas run. End with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Never push unless told.

## Steps

**Keep rule for every step.** Keep the step only when all three hold:
- its measured edit time drops by at least 50%;
- the image check passes on the step's scene, at the edited frame and the next one;
- its verification passes.

Otherwise revert it with `git checkout -- <files>` and note it in the status row.

### Step 0: Test scenes and baselines

1. Write `samples/repro/transform_only/build.py` (hython, `pxr` only). It writes four small stages:
   - `instancer.usda`: a PointInstancer with 10,000 instances of a 5,000-triangle prototype; the instancer's `xformOp:translate` is time-sampled at frames 1 and 2.
   - `volume.usda`: one 256³ density VDB. Use `samples/docs/` volumes built by `scripts/build_docs_volumes.py` if one is 128³ or more; else generate one with hython's `pyopenvdb` if available, and STOP if neither exists. Its transform is time-sampled.
   - `groom.usda`: BasisCurves with 50,000 curves × 8 points and `primvars:unreal:groom = 1` (check the attribute name in `docs/geometry/hair.md`); transform time-sampled.
   - `fog.usda`: one hdUE local fog volume prim (see `samples/conformance` or `docs/environment/fog.md` for its prim type); transform time-sampled.
   - Plus `materials.usda`: 100 meshes sharing one `UsdPreviewSurface` whose `inputs:diffuseColor` is time-sampled.
   Lights keep `inputs:intensity = 1` and use `inputs:exposure` (repo rule).
2. Build and stage the base code. Render each stage at frames 1-2 with husk, with `HDUE_TRACE_TIMING=1` and `HDUE_TRACE_SCENE=1`. Save `out/xform/<scene>_base.log` and `_base.<frame>.exr`.

**Verify**: `Get-ChildItem out\xform\*_base.log | Measure-Object` → Count = 5.

### Step 1: Item 4, reuse the material instance when its parent is unchanged

1. In `Scene.cpp`'s generated-material branch (around `:2137`), mirror the native branch (`:2097-2103`):
   - If `Materials` already holds a `UMaterialInstanceDynamic` for `Desc.id` whose `Parent` is the parent just loaded, call `ClearParameterValues()` and reuse it.
   - Otherwise create a new instance, as today.
2. Make sure the reused instance is not added to `RetiredMaterials`. The `Previous->Get() != Material` check at `:2221` already handles that.
3. Measure frame 2 of `materials.usda`: the upload time, and the count of `SetMaterial` calls (temporary counter, removed before commit).

**Verify**:
- `.\scripts\Launch-Houdini.ps1 -TestKind materials` → exit 0, run 3 times. Any crash is a STOP.
- Conformance `preview_surface/* materialx/* shade/*` → `0 failing runs`.

### Step 2: Item 1, instance-only edits through `set_transforms`

1. In `Mesh::Sync` (`prims.cpp:582`), add a second quick path for instanced meshes. It applies when the dirty bits are only `DirtyInstancer | DirtyInstanceIndex | DirtyTransform | DirtyVisibility | DirtyRenderTag`, and the stored mesh exists in `state->meshes`. Released meshes are fine: their geometry stays in UE.
   - Recompute only the instance placements, motion and per-instance data, exactly as the full path computes them. Read the full path from `:612` on and reuse its helpers; do not duplicate the maths.
   - Keep the geometry and its hash.
2. Decide by comparing the new instance list with the stored one:
   - Same instance count, same instance IDs and colours, only matrices changed: insert the id into a new set, `state->movedInstances`. Model it on `movedMeshes` (`delegate.cpp:518, 539, 581`; `prims.cpp:604`).
   - Anything else: fall back to the full upload. Insert into `dirtyMeshes` as today, with the stored geometry, no Hydra re-read. If the mesh was released, re-pull it, as plan 015's pattern does.
3. In `prepareMeshes`, next to the `movedMeshes` loop (`delegate.cpp:1087-1097`), send each id in `movedInstances` as one `HdueMeshTransform` with `count = N` and N matrices, built the way the motion sub-frame path builds them (`delegate.cpp:2018-2085`).
4. Make the instancer epoch per instancer. Bump only the edited instancer and its descendants: store the epoch on each `Instancer`, and compare against the sum or max of its ancestors' epochs. Keep `ComputeInstanceTransforms`' cache semantics the same.
5. Measure frame 2 of `instancer.usda`.

**Verify**:
- Conformance `instancer/* native_instancing/*` → `0 failing runs`.
- `.\scripts\Launch-Houdini.ps1 -TestKind selection` → exit 0. Picking goes through instance IDs, which must stay right.
- `.\scripts\Build-Houdini.ps1` → ctest passes, including `tests/instancing.cpp`.

### Step 3: Item 2, move a volume without re-sending its grid

1. Check whether `upsert_volume` can already keep the grid. Read `UpsertVolume` in `Bridge.cpp` (grep `UpsertVolume`) and `FHdueScene::UpsertVolume` in `Scene.cpp` (around `:360-415`). If a null `density` with `width/height/depth` unchanged already means "keep", or can be made to mean that without changing the struct, do that:
   - In the delegate, track placement-only volumes (as Step 2's `movedInstances`), and send `density = nullptr`.
   - In the backend, skip `Fill` when `Desc.density` is null and the texture's size matches. Update only the transform, the material and `extinction_scale`, which depends on the voxel size, `delegate.cpp:1381-1382`.
   - Document the null-density meaning in the `bridge.h` comment for `HdueVolumeDesc`. A comment change only needs no ABI bump.
2. If the backend currently rejects a null `density` (it returns `HDUE_INVALID_ARGUMENT`), accepting it changes the ABI's meaning. Then STOP and ask the operator whether to bump `HDUE_ABI_VERSION`. A bump means updating `bridge.h:22`, `tests/abi_layout.c`, `scripts/hdue_runtime.py:11` and `docs/architecture.md`, and rebuilding both sides.
3. Measure frame 2 of `volume.usda`.

**Verify**:
- Conformance `volume/*` → `0 failing runs`.
- `.\scripts\Launch-Houdini.ps1 -TestKind volume-material` → exit 0.

### Step 4: Item 3, grooms: one conversion per upload, and a transform-only path

1. In the groom upload (`delegate.cpp:1484-1494`), call `const auto convert = state.converter();` once before the loop, and use `convert(...)` inside it. This is the same hoist `prepareMeshes` uses. It needs no measurement threshold and can be committed alone.
2. Transform-only groom edits:
   - If the groom is instanced, send the new instance matrices without rebuilding the asset. That needs a backend path that updates `Groom.Actors` transforms (`Scene.cpp:328-347`).
   - Otherwise update the single actor's transform.
   - Points are baked into UE space with `place` (`delegate.cpp:1488`) for non-instanced grooms. So a non-instanced move changes the points themselves, and cannot skip the rebuild unless the upload stops baking `localToWorld` into points. Do the instanced case only.
   - Note the non-instanced case as deferred in the status row, unless the change to place grooms by actor transform is small. If it would change the image, STOP.
3. Measure frame 2 of `groom.usda`, plus an instanced variant (add one to `build.py`).

**Verify**:
- Conformance `points_curves/*` → `0 failing runs`.
- The `samples/conformance/points_curves/groom_attributes.usda` image check passes.

### Step 5: Item 5, placed actors move without respawning

1. In `UpsertLocalFogVolume`, `UpsertPostProcessVolume` and `UpsertDecal` (`Bridge.cpp:1806`, `:1874`, `:1936`), keep the existing actor when:
   - the visibility is unchanged;
   - the property names and values are unchanged (store the last `names`/`values` per id, beside `PlacedActors`);
   - only the transform differs.
   In that case call `SetWorldTransform` on its root component and set `HasRequest = false`.
2. Keep the full rebuild for any property change. The comment at `:1804-1805` says properties must be set before the component registers.
3. Measure frame 2 of `fog.usda`.

**Verify**:
- `.\scripts\Launch-Houdini.ps1 -TestKind decal` → exit 0.
- Conformance `render_settings/*` → `0 failing runs` (fog and post-process volumes live there; if not, use the area whose scenes author them: `git grep -l "LocalFogVolume\|PostProcessVolume" samples/conformance`).

### Step 6 (optional): Item 6, ID mode without a full walk

Do this only if Step 0's trace, or a 1-second-still viewport on `instancer.usda`, shows `SetIdMode` above 100 ms. Otherwise write "skipped: below 100 ms" in the status row.

If you do it: record which components need an ID swap once per Apply, in a list kept on `FHdueScene`, and have `SetIdMode` iterate only that list.

**Verify**: `.\scripts\Launch-Houdini.ps1 -TestKind selection` → exit 0.

### Step 7: Final regression

**Verify**:
- Backend, runtime and delegate build → exit 0.
- `.\scripts\Test.ps1 -Suite unit` → passed.
- Conformance `instancer/* native_instancing/* volume/* points_curves/* shade/* preview_surface/* render_settings/*` → `0 failing runs`.
- A Cornell husk render matches the base build (image check).
- `-TestKind integration` → exit 0.

## Test plan

- `tests/instancing.cpp`: add a case for the per-instancer epoch. Editing instancer A must not invalidate instancer B's cached placements. Model it on the existing cases in that file. If `Instancer` is not reachable from the test (it is in an anonymous namespace in `prims.cpp`), STOP the test sub-step, note it, and rely on conformance.
- The scenes under `samples/repro/transform_only/` stay in the repo, with a short `README.md` saying how to re-measure.
- The conformance areas listed per step are the regression net. Every step also runs its image check at frames 1 and 2.

## Done criteria

- [ ] Each kept step is one commit whose body has the before and after time of its scene's frame 2.
- [ ] `.\scripts\Test.ps1 -Suite unit` exits 0, and `.\scripts\Build-Houdini.ps1`'s ctest passes.
- [ ] `hython scripts\run_conformance.py --runtime-config <xform runtime> --only instancer/* native_instancing/* volume/* points_curves/* shade/* preview_surface/* render_settings/*` prints `0 failing runs`.
- [ ] `.\scripts\Launch-Houdini.ps1 -TestKind selection`, `-TestKind materials` and `-TestKind integration` each exit 0.
- [ ] `git diff 6fe3e12 -- include/hdue/bridge.h` is empty, or limited to a comment, unless the operator approved an ABI bump in Step 3.
- [ ] No files outside the in-scope list are modified (`git status`).
- [ ] `plans/README.md` status row updated, listing kept, reverted and deferred steps.

## STOP conditions

Stop and report back (do not improvise) if:

- The excerpts in "Current state" don't match the code. Plan 015 may have moved them: re-read and compare first.
- Step 3 or Step 4 needs a new ABI function, a struct change, or a changed meaning for an existing field. Ask before bumping `HDUE_ABI_VERSION`.
- Reusing a MID (Step 1) crashes in any of the 3 repeated `-TestKind materials` runs. That is the past crash the retire comment records.
- An instance-only update (Step 2) changes picking: `-TestKind selection` fails, or instance IDs differ.
- Motion blur on `instancer.usda` changes, comparing the before and after renders with `unreal:motionBlur` on. The sub-frame path depends on the same instance data.
- A step's verification fails twice after a reasonable fix attempt.

## Maintenance notes

- Any future per-instance attribute (a new custom-data float, per-instance material) must decide whether it rides the instance-only path. If it can change without the count changing, `movedInstances` must fall back to the full upload when it changes. A reviewer should check the comparison in Step 2 covers every per-instance field in `HdueMeshDesc`.
- Volume placement without the grid relies on the texture size staying the same. A resample that changes `width/height/depth` must keep sending the grid.
- Deferred: transform-only edits for non-instanced grooms (Step 4) and the ID-mode list (Step 6). Each has a note in the status row if skipped.
- Reviewers should scrutinise the per-instancer epoch (Step 2). A missed ancestor bump shows up as stale nested placements: instances stay where they were after a parent instancer moves.
