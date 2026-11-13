# Plan 016: One bad material parameter, mesh asset or native asset no longer fails the whole scene

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/delegate.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp src/host/usd_render.cpp scripts/Test-Hydra.ps1`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: M
- **Risk**: MED
- **Depends on**: plans/010-test-tiers-run-everything-they-claim.md (test tiers: run its suites before and after this change)
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE (a Hydra render delegate running Unreal Engine 5.8 inside Houdini, husk and usdview) turns Hydra's scene into upload "packets" in `prepareScene` (in the delegate) and applies them in UE with `FHdueScene::Apply` (in the backend). Today a single bad item stops everything:

- In the delegate, one material parameter of an unsupported type (bool, 2-vector, 64-bit int, empty), parameters for a material with no cooked asset, an out-of-range viewer light intensity, or a wet mask that failed to read, throws out of `prepareScene`. The whole packet is handed back and retried on every Execute, so no mesh, light or material uploads until the user finds the one bad value.
- In the backend, one material without an ID, one missing native material, or one missing native static mesh makes `Apply` return false in the middle of a batch (batches are up to ~1 GB). Items after it are dropped, an actor already spawned for the failing mesh stays behind empty, and `BindMaterials()` never runs.
- In the render request, one native asset (`primvars:unreal:asset`) in the FAILED state blanks the whole viewport.

After this plan, a bad item is skipped with a warning that names it, and the rest of the scene uploads and renders. The project rule "never silence warnings" holds: every skipped item warns.

## Current state

Files:

- `src/hydra/delegate.cpp` — the delegate. `materialParameter` (lines 648-671), native parameters in `prepareMaterials` (lines 804-822), `addParameterUpdate` (lines 825-838), `prepareLights` (lines 1231-1235), volume parameters (lines 1388-1397), groom parameters (lines 1530-1541), wet masks in `prepareSurfaceEffects` (lines 1566-1578), the retry path in the scene upload (lines 1785-1825), the asset gate in `executeOnce` (lines 2922-2945), and `batchWarning` (lines 185-188).
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` — `FHdueScene::Apply` (lines 1930-2495).
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` — `RequestRender`'s asset gate (lines 772-779).
- `src/host/usd_render.cpp` and `scripts/Test-Hydra.ps1` — the standalone Hydra self-test (`Test-Hydra.ps1 -SelfTest`).

### Delegate: the throws

`delegate.cpp:648-671`:

```cpp
HdueMaterialParameter materialParameter(ScenePacket& packet, const std::string& name, const VtValue& value) {
    HdueMaterialParameter p{};
    p.name = packet.own(name);
    if (value.IsHolding<float>() || value.IsHolding<double>() || value.IsHolding<int>()) {
        p.type = HDUE_PARAMETER_SCALAR;
        ...
    } else if (value.IsHolding<GfVec3f>() || value.IsHolding<GfVec3d>()) {
        p.type = HDUE_PARAMETER_VECTOR;
        p.value[3] = 1;
        ...
    } else if (value.IsHolding<GfVec4f>()) {
        ...
    } else if (value.IsHolding<std::string>()) {
        p.type = HDUE_PARAMETER_TEXTURE;
        p.texture_asset = packet.own(value.UncheckedGet<std::string>());
    } else
        throw std::runtime_error("Unsupported native material parameter value: " + name);
    return p;
}
```

husk's own JSON conversion of `nativeMaterialParameters` produces exactly the types this rejects (`delegate.cpp:2519-2527`: `GfVec2f` for 2-element arrays, `bool`, and an empty `VtValue` for other array sizes).

`delegate.cpp:809-813`:

```cpp
        if (asset(nativeMaterials, id).empty() || !entry->second.IsHolding<VtDictionary>())
            throw std::runtime_error("Native material parameters require a mapped cooked material and a dictionary");
        std::vector<HdueMaterialParameter> parameters;
        for (const auto& [name, value] : entry->second.UncheckedGet<VtDictionary>())
            parameters.push_back(materialParameter(packet, name, value));
```

`materialParameter` is also called at `delegate.cpp:830-831` (`addParameterUpdate`), `:1393-1394` (volumes), `:1531-1541` (grooms, including a `scalar` lambda that always passes a float).

`delegate.cpp:1232-1235`:

```cpp
    const double viewerIntensity =
        state.setting<double>("viewerLightIntensity", state.setting<float>("viewerLightIntensity", 4.f));
    if (!std::isfinite(viewerIntensity) || viewerIntensity < 0 || viewerIntensity > 100)
        throw std::runtime_error("viewerLightIntensity must be between 0 and 100");
```

`delegate.cpp:1571-1578`:

```cpp
        const auto maskName = HdUEWetValue<std::string>(fields, "maskPrimvar", "");
        const auto mask = data.masks.find(maskName);
        if (!maskName.empty() && mask == data.masks.end()) {
            const auto error = data.maskErrors.find(maskName);
            throw std::runtime_error(
                data.bindingPath + ": " +
                (error != data.maskErrors.end() ? error->second : "Wet mask not synchronized: " + maskName));
        }
```

Two cases hide here. A mask that is **not synchronized yet** is transient: `HdUERefreshWetMasks` (`prims.cpp:1881-1893`) fills it on a later Execute, so the retry is how the delegate waits. A mask with an entry in `maskErrors` (filled at `prims.cpp:712` and `:965` when reading the primvar failed) is permanent: it fails the scene on every Execute forever.

`delegate.cpp:1792-1797`, every throw above lands here and hands the whole packet back:

```cpp
        std::lock_guard lock(state.mutex);
        try {
            prepareScene(state, packet, meshBudget);
        } catch (...) {
            packet.giveBack(state);
            throw;
        }
```

How warnings reach users: `TF_WARN` works in every host. In husk, `HdUEBatchWarnings` (`delegate.cpp:189-210`) forwards hdUE's own Tf warnings to `batchWarning` (stderr), once per message. In Houdini's viewport a `TF_WARN` issued on every Execute would repeat, so a warn-once is needed. The pattern the code already uses for once-per-key warnings is at `delegate.cpp:680-686`:

```cpp
        static std::mutex warnedMutex;
        static std::set<std::string> warned;
        std::lock_guard lock(warnedMutex);
        if (!selfHost().continuous && warned.insert(file).second)
            batchWarning(...);
```

### Backend: `Apply` stops mid-batch

`Scene.cpp:1936-1937` already states the intended behaviour, which invalid meshes follow:

```cpp
    // Validate mesh array bounds before mutating the scene. An invalid mesh is skipped (and its previous version
    // removed) with a warning naming it; the rest of the batch still applies. Arrays are int32-addressed in UE.
```

But these paths return false mid-batch:

`Scene.cpp:2088-2092`:

```cpp
    for (uint32 Index = 0; Index < Batch.material_count; ++Index) {
        const auto& Desc = Batch.materials[Index];
        if (!Desc.id) {
            Error = TEXT("Material has no ID.");
            return false;
        }
```

`Scene.cpp:2215-2218`:

```cpp
        if (!Material) {
            Error = TEXT("Material asset missing. Prepare/cook content or check the native asset path.");
            return false;
        }
```

`Scene.cpp:2365-2398`: the actor is spawned and added to `Actors` first (line 2366-2367), then:

```cpp
        if (Native && Instanced) {
            InstanceMesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (!InstanceMesh) {
                Error = TEXT("Native StaticMesh not found in cooked content.");
                return false;
            }
        } else if (Native) {
            auto* Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(Desc.native_asset));
            if (!Mesh) {
                Error = TEXT("Native StaticMesh not found in cooked content.");
                return false;
            }
            if ((Desc.flags & HDUE_MESH_REQUIRE_NANITE) && !Mesh->HasValidNaniteData()) {
                Error = TEXT("Native asset has no cooked Nanite data.");
                return false;
            }
```

`Scene.cpp:2486` (`BindMaterials();`) only runs when `Apply` reaches its end. The invalid-mesh warning to copy is at `Scene.cpp:1954-1955`:

```cpp
        UE_LOG(LogTemp, Warning, TEXT("hdUE: skipped mesh %s (%u vertices, %u indices): %s."),
               Mesh.id ? UTF8_TO_TCHAR(Mesh.id) : TEXT("without ID"), Mesh.vertex_count, Mesh.index_count, Problem);
```

`UE_LOG(LogTemp, Warning, ...)` reaches the UE log and husk's output (commit `3ff8593` made husk echo UE's warnings).

`FHdueScene::Remove(Id)` (`Scene.cpp:146-160`) destroys a mesh's actor and forgets the id; use it to drop a half-made actor.

### Backend and delegate: one failed native asset blanks the render

`Bridge.cpp:776-779`:

```cpp
    Session->Scene->Assets.SetTime(Request->time_seconds);
    if (Session->Scene->Assets.Loading(AssetFailure)) return Pending(TEXT("native assets"));
    if (!AssetFailure.IsEmpty()) return Fail(HDUE_ENGINE_ERROR, *AssetFailure);
```

`delegate.cpp:2934-2939`, the delegate also stops the frame:

```cpp
                if (assets.failed) {
                    s.setError(assets.message);
                    s.assetRequestPending = true;
                    converged_ = true;
                    return;
                }
```

Which host is interactive: the backend knows it from `IsInteractive()` (`Bridge.cpp:360`, set from `HDUE_INIT_INTERACTIVE` at boot). The delegate passes `host.continuous` as that flag (`delegate.cpp:327`, `src/client/backend.cpp:65`), so in the delegate the same test is `selfHost().continuous` (true in Houdini's UI; false in husk, hython and the standalone host).

### Decision recorded in this plan (native asset failures)

Recommended and implemented here: **interactive hosts render without the failed asset and warn; offline hosts (husk) keep failing the frame**, because a farm render missing an asset should not quietly deliver a wrong image. If the operator wants offline renders to continue too, that is a one-condition change at the two sites above; do not make it without being told.

## Commands you will need

| Purpose | Command | Expected on success |
|---------|---------|---------------------|
| Build the backend | `.\scripts\Build-Backend.ps1` | exit 0 |
| Stage it to a side profile | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\.local\runtime-plan016.json` | exit 0 |
| Build the standalone host (delegate + self-test) | `.\scripts\Build-Host.ps1` | exit 0, ctest passes |
| Build the Houdini delegate | `.\scripts\Build-Houdini.ps1` (Houdini must be closed) | exit 0, ctest passes |
| Hydra self-test | `.\scripts\Test-Hydra.ps1 -SelfTest -RuntimeConfig H:\github\hdUE\.local\runtime-plan016.json` | `Verified Hydra/UE output: ...` |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| Targeted conformance | `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-plan016.json --only shade/*` then `--only volume/*` then `--only points_curves/*` | `0 failing runs` each |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp src/host/usd_render.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` | exit 0 |

`Test-Hydra.ps1` takes `-RuntimeConfig <profile>` (its first lines) and uses `out\runtime` when it is omitted.

## Scope

**In scope** (the only files you should modify):
- `src/hydra/delegate.cpp`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` (`Apply` only)
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp` (`RequestRender`'s asset gate only)
- `src/host/usd_render.cpp` (new self-test steps)
- `scripts/Test-Hydra.ps1` (the self-test frame count only)

**Out of scope** (do NOT touch, even though they look related):
- Failures of individual `upsert_*` calls in `sendScene` (`delegate.cpp:1638-1713`): changing those is a separate decision about backend errors.
- The groom descriptor checks in `Bridge.cpp` `UpsertGroom` — plan 007.
- Native asset loading itself (`NativeAssets.cpp`), its 120 s timeout, or which paths are allowed — plan 019.
- The ABI (`include/hdue/bridge.h`): no struct or version change is needed.
- The transient "Wet mask not synchronized" case: keep it as a retry.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, `main` (per `AGENTS.md`).
- One commit per step. Plain English subjects (e.g. `Logging in the Test backend; the viewport lighting that restarts hdUE`); body with why and verification; end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless told.

## Steps

### Step 0: Record the baseline

Run, before any change, and keep the outputs:

- `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`
- `.\scripts\Test-Hydra.ps1 -SelfTest` with the current default profile → `Verified Hydra/UE output`.

**Verify**: both pass. If either fails before your change, STOP.

### Step 1: A warn-once helper in the delegate

In `delegate.cpp`, right after `batchWarning` (line 188), add a helper that issues `TF_WARN("%s", message.c_str())` once per distinct message per process (a function-local `static std::mutex` and `static std::set<std::string>`, as at lines 680-686). One comment line saying why: these warnings come from scene preparation, which runs on every Execute.

**Verify**: `.\scripts\Build-Host.ps1` → exit 0.

### Step 2: Convert or skip unsupported material parameter values

Change `materialParameter` to return `std::optional<HdueMaterialParameter>`:

- Add: `bool` → scalar 0 or 1; `int64_t` → scalar; `GfVec2f` and `GfVec2d` → vector `(x, y, 0, 1)` (UE material parameters are scalars or 4-vectors; a 2-vector parameter reads the first two components).
- Any other type (including an empty `VtValue`) → call the warn-once helper with `"hdUE skips material parameter " + name + ": unsupported value type " + value.GetTypeName()` and return `std::nullopt`.
- Update every caller (lines 812-813, 830-831, 1393-1394, 1531-1541) to push only when the optional has a value. The groom `scalar` lambda can dereference directly (it always passes a float), or use the same `if`.

**Verify**: `.\scripts\Build-Host.ps1` → exit 0. `grep -n "Unsupported native material parameter value" src/hydra/delegate.cpp` → no match.

### Step 3: Skip parameters for unmapped materials, clamp the viewer light, skip permanently broken wet masks

- `delegate.cpp:810-811`: instead of throwing, warn once (`"hdUE skips the native material parameters of " + id + ": it has no cooked material, or its parameters are not a dictionary"`) and `continue`.
- `delegate.cpp:1234-1235`: if the value is not finite, warn once and use 4 (the declared default, `delegate.cpp:3881` area `GetRenderSettingDescriptors`); else clamp to [0, 100] and warn once when clamping. Keep the 0-100 range: it is the setting's documented range.
- `delegate.cpp:1573-1578`: when `maskErrors` has the mask, warn once with the same text the throw used (`data.bindingPath + ": " + error->second`) and `continue` (no wet binding for that mesh). When it does not (not synchronized yet), keep the existing `throw`.

**Verify**: `.\scripts\Build-Host.ps1` → exit 0. `awk 'NR>=600 && NR<=1730 && /throw /' src/hydra/delegate.cpp` → only the "Wet mask not synchronized" throw remains.

### Step 4: `Apply` skips bad materials and native meshes and finishes the batch

In `Scene.cpp` `FHdueScene::Apply`:

- Material without an ID (line 2090): `UE_LOG(LogTemp, Warning, TEXT("hdUE: skipped a material without an ID."))` and `continue`.
- Material that resolved to null (line 2215): warn naming the material id and its native path (`Desc.native_asset` may be null; print `(none)` then), and `continue`. Do not touch `Materials` for that id, so a previous good material stays bound. The meshes that use it fall back to UE's default material via `BindMaterials`.
- Native mesh not found / no Nanite data (lines 2384-2398): call `Remove(Id)` (it destroys the actor spawned a few lines above and forgets the id), warn naming the mesh id, the asset path and the reason, and `continue` the mesh loop.

Keep `return true;` at the end. Do not add a new error return: like invalid meshes, skipped items warn and the batch succeeds. Update the comment at lines 1936-1937 so it says invalid meshes, materials and native assets are all skipped with a warning.

Check the loop variables before `continue`: a `continue` must not skip bookkeeping that later code reads by index (for example `Keys[Index]` or per-mesh arrays filled in parallel before the loop). If such bookkeeping exists, STOP and report.

**Verify**: `.\scripts\Build-Backend.ps1` → exit 0. `grep -n "return false" unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` → the remaining `return false` lines in `Apply` are only the "Missing scene batch data." check at the top (and any not listed in this plan; report those).

### Step 5: Interactive hosts render without a failed native asset

- `Bridge.cpp:779`: when `!AssetFailure.IsEmpty()` and `IsInteractive()`, log `UE_LOG(LogTemp, Warning, TEXT("hdUE: native asset %s failed; rendering without it."), *AssetFailure)` once per distinct failure text (keep the last logged text in a `static FString`, or in the session if you prefer) and continue the request. Offline: unchanged (`return Fail(...)`).
- `delegate.cpp:2934-2939`: in the `assets.failed` branch, when `selfHost().continuous`, warn once with `assets.message` and fall through to render; otherwise keep today's `setError` and return.

**Verify**: `.\scripts\Build-Backend.ps1` and `.\scripts\Build-Houdini.ps1` → exit 0.

### Step 6: Self-test the skip paths

In `src/host/usd_render.cpp`, in the self-test right after the native-assets captures (the block that sets `nativeMeshAssets` around lines 494-510), add:

1. `raw->SetRenderSetting(TfToken("nativeMaterialParameters"), ...)` with a dictionary for `/Looks/Red` holding a `bool`, a `GfVec2f` and a `VtValue()` (empty) entry, then `capture("native-parameters-mixed")`. It must render (no exception).
2. Map `/World/Cube` to a path that does not exist (`/Game/HdUE/DoesNotExist.DoesNotExist`) in `nativeMeshAssets`, keep `/World/Ground` on `SM_Nanite`, `capture("native-mesh-missing")`. It must render. Then restore the previous mapping.

Each `capture` adds one frame per cycle. In `scripts/Test-Hydra.ps1` line 58, raise `$PerCycle += 15` by the number of captures you added (2 → `+= 17`), so the evidence check still counts frames exactly.

**Verify**: `.\scripts\Build-Host.ps1`, then `.\scripts\Test-Hydra.ps1 -SelfTest -RuntimeConfig H:\github\hdUE\.local\runtime-plan016.json *>&1 | Tee-Object -FilePath $env:TEMP\plan016-selftest.txt` → ends with `Verified Hydra/UE output`. `Select-String -Path $env:TEMP\plan016-selftest.txt -Pattern "skips material parameter"` finds the Tf warning for the empty value. `Select-String -Path "$env:LOCALAPPDATA\hdUE\Saved\Logs\HdUEBackend.log" -Pattern "DoesNotExist"` finds the backend's skipped-mesh warning (the UE log; timestamps are UTC).

### Step 7: Regression runs

**Verify**:
- `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`
- Conformance `--only shade/*`, `--only volume/*`, `--only points_curves/*` against the side profile → `0 failing runs` each.
- A Cornell husk render with the side profile matches the one with the default profile.
- Interactive check of Step 5: `.\scripts\Launch-Houdini.ps1 -RuntimeConfig H:\github\hdUE\.local\runtime-plan016.json -InputUsd <scratch .usda>` where the scratch scene is the Cornell box plus one `Xform` with `string primvars:unreal:asset = "/Game/HdUE/DoesNotExist.DoesNotExist"`. The viewport renders the Cornell box, and `Select-String -Path "$env:LOCALAPPDATA\hdUE\Saved\Logs\HdUEBackend.log" -Pattern "rendering without it"` finds the warning. Close Houdini afterwards. Keep the scratch scene out of the repo.
- The format check exits 0.

## Test plan

- New self-test captures in `src/host/usd_render.cpp` (Step 6): mixed parameter types (bool, vec2, empty) render; a mesh mapped to a missing cooked StaticMesh renders and the rest of the scene with it. Pattern: the existing `native-assets-realtime` captures around `usd_render.cpp:494-510`.
- Frame count in `scripts/Test-Hydra.ps1` updated to match.
- Existing suites that must stay green: unit suite, `shade/*`, `volume/*`, `points_curves/*` conformance, Cornell render.
- Manual-but-scripted: the Houdini viewport check of a failed `unreal:asset` (Step 7).

## Done criteria

- [ ] `.\scripts\Build-Backend.ps1`, `.\scripts\Build-Host.ps1`, `.\scripts\Build-Houdini.ps1` exit 0
- [ ] `.\scripts\Test-Hydra.ps1 -SelfTest` prints `Verified Hydra/UE output` with the two new captures
- [ ] `.\scripts\Test.ps1 -Suite unit` prints `Suite unit passed.`
- [ ] `run_conformance.py --only shade/*`, `volume/*`, `points_curves/*` each print `0 failing runs`
- [ ] `grep -n "Unsupported native material parameter value" src/hydra/delegate.cpp` returns nothing
- [ ] In `Apply`, no `return false` remains for the material-ID, missing-material or missing-native-mesh cases
- [ ] clang-format check exits 0
- [ ] No files outside the in-scope list are modified (`git status`)
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- The baseline (Step 0) fails before any change.
- The code at the locations in "Current state" does not match the excerpts.
- In `Apply`, a `continue` would skip per-index bookkeeping that later code depends on (Step 4).
- A conformance scene that passed before now fails, or a scene that should fail offline (a missing `unreal:asset` in husk) now renders: that means the interactive/offline split leaked.
- The operator has decided offline renders should also continue past failed native assets: that changes Step 5; ask before doing it.
- Skipping a material leaves meshes drawing nothing (invisible) rather than with UE's default material: report it, the material binding needs a closer look.

## Maintenance notes

- New throws in `prepareScene` and its helpers fail the whole scene again. A reviewer should ask: is this error about one item (warn and skip) or about the whole scene (throw)?
- New `return false` paths inside `FHdueScene::Apply` mean "half-applied batch". Prefer warn-and-continue, as invalid meshes and now materials and native meshes do.
- The warn-once helper keeps its messages for the process lifetime. Messages include ids and paths, so the set grows with distinct problems, not with frames.
- Deferred: failures of single `upsert_*` calls (volumes, grooms, decals, fog) still fail the scene upload in `sendScene`. That is a separate decision because those are backend errors, not bad input the delegate could detect.
