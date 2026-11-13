# Plan 019: Scene content can only set editable UE properties, and its sizes are capped

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.h unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/NativeAssets.cpp src/hydra/prims.cpp src/hydra/delegate.cpp`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: M
- **Risk**: MED
- **Depends on**: plans/001-scene-cannot-choose-backend-dll.md (same threat model; land it first)
- **Category**: security
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE renders USD files that often come from other people (asset packs, other studios), inside Houdini and husk on
artists' machines and render farms. Several values in those files reach Unreal Engine almost unchecked:

1. Attributes like `unreal:heightFog:<Property>` set any reflected UE property by name, including internal,
   non-editable ones (object pointers, attachment state). Only the light path checks UE's "editable" flag.
2. Object-valued properties load any object path the scene names.
3. Some sizes have no upper bound: a subdivision level of 10 multiplies faces by 4^10 and hangs Houdini; a
   displacement image header of 100000 x 100000 allocates tens of GB before reading.
4. Mesh positions and matrices are never checked for NaN/infinity, while every other upload (grooms, volumes, lights,
   assets) is.
5. Light and post-process image textures can exceed the GPU's maximum texture size (16384 on D3D12).
6. Curve indices are read without checking they are inside the index array.

Each can crash or stall a render from a scene file. After this plan, scene content can only set what UE's Details
panel lets a user set, within limits UE itself uses, and bad data is skipped with a warning naming it.

## Current state

### A. Property import (backend)

One helper imports a property from text, `ImportPropertyValue`
(`unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp:25-33`, declared in `Lighting.h:19`):

```cpp
bool ImportPropertyValue(FProperty* Property, const FString& Text, void* Container) {
    void* Value = Property->ContainerPtrToValuePtr<void>(Container);
    if (const auto* Object = CastField<FObjectPropertyBase>(Property)) {
        const bool None = Text.IsEmpty() || Text == TEXT("None");
        UObject* Loaded = None ? nullptr : LoadObject<UObject>(nullptr, *Text);
        if (!None && (!Loaded || !Loaded->IsA(Object->PropertyClass))) return false;
        Object->SetObjectPropertyValue(Value, Loaded);
        return true;
    }
```

Callers. Only the light path checks the editable flag (`Lighting.cpp:464-466`):

```cpp
    for (const auto& [Name, Text] : Definition.Properties) {
        FProperty* Property = Light->GetClass()->FindPropertyByName(*Name);
        if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || !ImportPropertyValue(Property, Text, Light))
```

The others look the property up by name and import it directly:

- `Bridge.cpp:1773-1787` (`SetSceneComponent`: height fog, sky atmosphere, volumetric cloud, sky light; also
  `<Struct>:<Field>` into a struct property):
  `FProperty* Property = Class->FindPropertyByName(*Name); ... if (!Property || !ImportPropertyValue(Property, Value.Value, Container))`
- `Bridge.cpp:1822-1825` (local fog volume): `FProperty* Property = ULocalFogVolumeComponent::StaticClass()->FindPropertyByName(*Name);`
- `Bridge.cpp:1846-1848` (`ImportPostProcessSetting`, `FPostProcessSettings` fields; it already refuses `bOverride_` names).
- `Bridge.cpp:1911-1913` (post-process volume component properties other than `Settings`).
- `Bridge.cpp:1957-1958` (decal component; refuses `DecalSize`).

Console variables. In `set_post_process` (`Bridge.cpp:2017-2020`) any name containing `.` sets that console variable:

```cpp
        if (Name.Contains(TEXT("."))) {
            SetConsoleVariable(Name, Text);
            Variables.Add(Name);
            continue;
        }
```

`SetConsoleVariable` (`Bridge.cpp:1591-1608`) refuses only unknown and read-only variables. This "any cvar" contract is
documented in `include/hdue/bridge.h` and is by design for the host. The scene reaches it through `unreal:cvar:*` and
`postProcess:*` render settings and through actor-prim dictionaries (`src/hydra/prims.cpp:1964-1983`). Read
`include/hdue/bridge.h` for the `set_post_process` comment before Step 3.

### B. Texture size (backend)

`FHdueLighting::LoadImageFile` (`Lighting.cpp:474-494`) accepts images up to 32768 x 16384 for every light. That is
right for dome lights (they are only sampled into a cubemap) but not for rect-light source textures and light functions,
which become a 2D GPU texture in `ImageTexture` (`Lighting.cpp:495-505`) with no null check:

```cpp
UTexture2D* FHdueLighting::ImageTexture(const FString& File) {
    if (const auto* Existing = LightTextures.Find(File)) return Existing->Get();
    const auto& Image = *Images[File].Image;
    TStrongObjectPtr<UTexture2D> Texture(UTexture2D::CreateTransient(Image.SizeX, Image.SizeY, PF_FloatRGBA));
    Texture->SRGB = false;
```

Its callers: `Lighting.cpp:430` (`Rect->SetSourceTexture(ImageTexture(Definition.File))`) and `:444` (light function).
`PostProcessTexture` (`Bridge.cpp:1659-...`) checks `CreateTransient` for null (`if (!Texture) return nullptr;` near
line 1736) but not the size. The limit UE uses is `GetMax2DTextureDimension()` (already used in `Bridge.cpp:591-593`:
"D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, 16384, on D3D12").

### C. Finite geometry (backend)

Mesh validation (`Scene.cpp:1938-1957`) checks counts and index ranges, not values:

```cpp
        const TCHAR* Problem = !Mesh.id               ? TEXT("no ID")
                               : Mesh.index_count % 3 ? TEXT("index count is not a multiple of 3")
                               : ...
                                   ? TEXT("missing positions or indices")
                                   : nullptr;
        for (uint32 I = 0; !Problem && I < Mesh.index_count; ++I)
            if (Mesh.indices[I] >= Mesh.vertex_count) Problem = TEXT("index out of range");
```

`FHdueScene::SetTransforms` (`Scene.cpp:464-481`) builds `FTransform`s from the matrices with no check; its ABI entry
`SetTransforms` (`Bridge.cpp:1576-1585`) checks only null pointers. By contrast, native assets reject non-finite
matrices (`NativeAssets.cpp:65-68`: `if (!FMath::IsFinite(D.transform[R * 4 + C])) return HDUE_INVALID_ARGUMENT;`).

### D. Native asset paths (backend)

`FHdueNativeAssets::Upsert` (`NativeAssets.cpp:71-75`) accepts any path starting with `/` without `'`. An unregistered
package that exists is loaded (`:145-148`), and if it is a non-abstract `AActor` class it is spawned (`:212-221`).
It is not known whether `/Script/...` (engine C++ classes) passes `FPackageName::DoesPackageExist`.

### E. Delegate-side caps

Subdivision level (`src/hydra/prims.cpp:549-565`), no upper bound:

```cpp
        if (const int level = asset ? 0
                                    : value(delegate->Get(GetId(), TfToken("unreal:subdivisionLevel")),
                                            state.setting<int>("subdivisionLevel", 0));
            level > 0 && ...
            shape.subdivision = HdUESubdivision::Create(shape.topology, level, std::move(fvarIndices));
```

UE's own maximum: `static int32 GMaxSubdivLevel = 6;` with cvar `USD.Subdiv.MaxSubdivLevel`
(UE 5.8.4 `Engine/Plugins/Runtime/USDCore/Source/USDUtilities/Private/USDGeomMeshConversion.cpp:118-119`; the engine
root is `engine_root` in `.local/paths.json`). The import UI clamps to it (`USDStageImportContext.cpp:74-75`).

Warm-up frames (`src/hydra/delegate.cpp:2975`): `request.warmup_frames = uint32_t(std::max(1, s.setting<int>("warmupFrames", 64)));`
No upper bound; the backend checks only samples (`Bridge.cpp:690`: `samples_per_pixel > 65536` rejected with a reason).

Displacement image (`src/hydra/delegate.cpp:694-704`) allocates the header's size before reading:

```cpp
    spec.width = source->GetWidth();
    spec.height = source->GetHeight();
    spec.format = source->GetFormat();
    std::vector<uint8_t> raw(size_t(spec.width) * spec.height * source->GetBytesPerPixel());
```

Curve indices (`src/hydra/prims.cpp:1392-1399`):

```cpp
            for (size_t i = 0; i < count; ++i) {
                const size_t vertex = indices.empty() ? offset + i : size_t(indices[offset + i]);
                if (vertex >= points.size()) {
                    TF_RUNTIME_ERROR("Invalid curve index at %s", GetId().GetText());
                    return;
                }
```

`indices[offset + i]` is read even when `offset + i >= indices.size()` (out-of-bounds read).

Already handled, do not redo: sheared-instance baking is capped by `HdUEBakesShear` (`src/hydra/delegate.h:162-168`).

### Conventions

- Warnings name the item and what was skipped, as the existing ones do:
  `UE_LOG(LogTemp, Warning, TEXT("hdUE: decal setting %s = '%s' skipped (unknown property or value)"), ...)`.
  In the delegate, `TF_WARN` / `TF_RUNTIME_ERROR` with the prim path.
- "Unreal is the benchmark" (AGENTS.md): use UE's own limits and flags (its Details panel shows `CPF_Edit` properties
  and hides `EditConst` / `DisableEditOnInstance` ones on instances; its USD importer caps subdivision at 6).
- C++ Google style, 4 spaces, 120 columns. Comments explain why.
- Never silence a warning to clean a log.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build backend | `.\scripts\Build-Backend.ps1` | exit 0 (~40 s incremental) |
| Stage backend for a side profile | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\.local\runtime-plan019.json` | exit 0; the default profile untouched |
| Stage as default (at the end) | `.\scripts\Update-HoudiniRuntime.ps1 -SetDefault` | exit 0 |
| Build delegate (Houdini closed) | `.\scripts\Build-Houdini.ps1` | exit 0; ctest passes |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| Targeted conformance | `& "<houdini_root>\bin\hython.exe" scripts\run_conformance.py --runtime-config <absolute profile path> --only <area>/*` | `0 failing runs` |
| Format | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror <changed .cpp/.h>` | exit 0 |

Areas to run (all exist under `samples/conformance/`): `render_settings/*`, `lux/*`, `points_curves/*`, `mesh/*`
(includes the `subdiv_*` scenes), `geom/*`, and `volume/*` (holds the `unreal:asset` scenes
`sparse_volume_texture.usda` and `sparse_volume_sequence.usda`). `<houdini_root>` comes from `.local/paths.json`.

## Scope

**In scope**:
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp`, `Lighting.h`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/NativeAssets.cpp` (Step 6 only, after its investigation)
- `src/hydra/prims.cpp`, `src/hydra/delegate.cpp`
- `tests/subdivision.cpp` or a new small test if you extract a pure helper (optional)

**Out of scope**:
- The backend DLL / project selection — plan 001.
- Process launches and auto-preparation — plan 020. Cache file validation — plan 021.
- The ABI (`include/hdue/bridge.h`): no struct or table change. If a fix seems to need one, STOP.
- The groom upload validation (`UpsertGroom`) — plan 007 owns it.
- Generated `.ds` files and HDAs: never hand-edit them.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, `main`.
- One commit per step. Subject: a plain English sentence describing the result, as in `git log` (e.g. "Logging in the
  Test backend; the viewport lighting that restarts hdUE"). Body: why and how verified. Last line:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Do NOT push unless told. Never push to Epic's or the upstream fork's repositories.

## Steps

### Step 1: One shared check for which properties scene text may set

In `Lighting.cpp`, next to `ImportPropertyValue`, add `bool HdueSceneEditable(const FProperty* Property)` (declare it in
`Lighting.h`): true when the property has `CPF_Edit` and has none of `CPF_EditConst`, `CPF_DisableEditOnInstance`,
`CPF_Transient`. Comment: the properties UE's Details panel lets a user edit on an instance.

Use it at every scene-facing call site: the light loop (`Lighting.cpp:466`, replacing the bare `CPF_Edit` check), the
scene components (`Bridge.cpp:1778-1785`, checking the outer struct property and, for `<Struct>:<Field>`, the field),
local fog (`:1824`), `ImportPostProcessSetting` (`:1848`), the post-process volume (`:1911-1913`) and the decal
(`:1957-1958`). A refused property logs the existing "skipped" warning.

**Verify**: `git grep -n "FindPropertyByName" -- unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp`
→ each scene-facing hit is followed (same statement) by `HdueSceneEditable`. `.\scripts\Build-Backend.ps1` → exit 0.

Then check every property hdUE's own generated settings use is still accepted: the generated parameter files list
them. Run, read-only:
`Select-String -Path integrations\houdini\soho\parameters\HdUnrealEngine_*.ds -Pattern 'name\s+"xn__unreal' | Measure-Object`
and, after staging to a side profile, run the targeted conformance areas `render_settings/*` and `lux/*`.
**Verify**: `0 failing runs`, and the backend log (`%LOCALAPPDATA%\hdUE\Saved\Logs\HdUEBackend.log`) of those runs
contains no new `skipped` line for a property hdUE generates (compare with a run on the previous build).

### Step 2: Restrict object values

In `ImportPropertyValue`'s object branch: accept only paths starting with `/Game/` or `/Engine/` (the comment above the
function already says objects are "cooked paths (/Game/..., /Engine/...)"), and for an `FClassProperty` check the loaded
class `IsChildOf(MetaClass)`, not only `IsA(PropertyClass)`. Refuse otherwise (return false; the caller warns).

**Verify**: build exits 0; `render_settings/*` conformance → `0 failing runs`.

### Step 3: Decide the scene console-variable rule (ask first)

The host-facing contract "set_post_process sets any console variable" is documented in `include/hdue/bridge.h` and is
by design. But scene files reach it. Two options; ask the operator which, and STOP until answered:

- **Option A (recommended)**: keep the ABI contract; in the delegate, pass only console variables named in the
  generated settings (the `cvar:` keys in `integrations/houdini/soho/parameters/HdUnrealEngine_Global.ds`) when they
  come from the scene, and warn once for others. This needs a generated list readable by the delegate; if there is none,
  generating it belongs in `scripts/generate_render_settings.py` — then STOP and report, because that is a larger change.
- **Option B**: leave console variables as they are and document in `docs/getting-started/` that scenes can set any
  writable cvar for the life of the process.

**Verify** (Option A): a probe scene authoring `unreal:cvar:r.ScreenPercentage` (generated) still applies, and
`unreal:cvar:r.SomethingNotGenerated` logs a warning. (Option B): the doc sentence exists.

### Step 4: Cap texture sizes to what the GPU accepts

- `ImageTexture` (`Lighting.cpp:495`): if `Image.SizeX` or `SizeY` exceeds `GetMax2DTextureDimension()`, log a warning
  naming the file and return `nullptr`; also return `nullptr` when `CreateTransient` returns null. Domes do not call
  `ImageTexture`, so their 32768 limit stays.
- `PostProcessTexture` (`Bridge.cpp`): reject 2D images larger than `GetMax2DTextureDimension()` with the existing
  "cannot read the image" warning style. Cubemaps are already capped at 2048 per face.

**Verify**: build exits 0; `lux/*` conformance → `0 failing runs`.

### Step 5: Skip meshes and transforms with non-finite values

- In the `Scene.cpp:1938-1957` validation lambda, after the index loop, check every `positions` component (and the
  `transform` and any `instance_transforms` matrices) with `FMath::IsFinite`; problem text "non-finite position or
  matrix". It runs inside the existing `ParallelFor`, so cost stays parallel.
- In `FHdueScene::SetTransforms`, return without change if any matrix element is non-finite (log one warning with the
  id).

**Verify**: build exits 0; `mesh/*` (or the mesh/xform areas that exist) conformance → `0 failing runs`; a Cornell
husk render matches the previous build.

### Step 6: Investigate, then restrict native asset paths

First investigate: in a small backend session (any conformance scene with an `unreal:asset` prim, e.g.
`git grep -l "unreal:asset" samples/conformance`), check whether a path like `/Script/Engine.StaticMeshActor` passes
`FPackageName::DoesPackageExist` (read UE's `PackageName.cpp` for `/Script` handling; the engine root is in
`.local/paths.json`). Write the answer in the commit body.

- If `/Script` paths can pass: in `FHdueNativeAssets::Upsert`, refuse paths that do not start with `/Game/`, with the
  existing error text style ("Use a cooked UE object path such as /Game/HdUE/SM_Nanite.SM_Nanite.").
- If they cannot: make no code change; record the finding as "not reachable" in `plans/README.md`.

**Verify**: build exits 0; the conformance area that holds the `unreal:asset` scenes → `0 failing runs`.

### Step 7: Delegate caps

In `src/hydra/prims.cpp` and `src/hydra/delegate.cpp`:

- Subdivision: clamp the level to 6 (UE's `GMaxSubdivLevel`, cite `USDGeomMeshConversion.cpp:119` in the comment) and
  `TF_WARN` once per prim when clamped.
- Warm-up frames: clamp to 65536, as the backend does for samples; warn once when clamped.
- Displacement image: before allocating, return an empty image (with a warning naming the file) when width or height is
  above 16384 (the D3D12 2D limit the backend uses) or the byte count overflows.
- Curves: before reading `indices[offset + i]`, check `offset + count <= indices.size()` when `indices` is not empty;
  on failure use the existing `TF_RUNTIME_ERROR("Invalid curve index at %s", ...)` and return.

**Verify**: `.\scripts\Build-Houdini.ps1` → exit 0, ctest passes; `points_curves/*` and the subdivision conformance
scenes (`git grep -l subdivisionLevel samples/conformance`) → `0 failing runs`.

### Step 8: Stage and check the whole set

`.\scripts\Update-HoudiniRuntime.ps1 -SetDefault`, then run every area touched above once more, and a Cornell husk render
compared with the previous build.

**Verify**: all targeted runs `0 failing runs`; the Cornell image matches.

## Test plan

- Conformance areas `render_settings/*`, `lux/*`, `points_curves/*`, the mesh areas and the `unreal:asset` scenes are the
  regression net (they exercise every property path that legitimately reaches UE).
- If you extract the subdivision clamp or the curve-range check into a header helper, add a case to
  `tests/subdivision.cpp` (pattern: its existing `main` with asserts) — optional.
- Manual probe for Step 1: a scratch `.usda` (outside the repo, never committed) sublayering
  `samples/conformance/common/stage.usda` and authoring an internal, non-editable fog property; the backend log must say
  "skipped".

## Done criteria

- [ ] `HdueSceneEditable` exists and every scene-facing `FindPropertyByName` call in `Bridge.cpp`/`Lighting.cpp` uses it
- [ ] `ImportPropertyValue` refuses object paths outside `/Game/` and `/Engine/`
- [ ] Step 3 decision recorded in the commit body; its verification holds
- [ ] `git grep -n "GetMax2DTextureDimension" -- unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp` returns a hit
- [ ] Mesh validation mentions "non-finite"; `SetTransforms` checks finiteness
- [ ] Step 6 investigation result recorded
- [ ] `git grep -n "GMaxSubdivLevel\|USD.Subdiv.MaxSubdivLevel" -- src/hydra` returns the clamp's comment
- [ ] Backend and delegate builds exit 0; ctest passes; `.\scripts\Test.ps1 -Suite unit` passes
- [ ] Every targeted conformance area run reports `0 failing runs`
- [ ] clang-format check exits 0 on changed files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Step 1 makes any property that hdUE's generated settings (`.ds` files) use get refused: the generator relies on a
  non-editable property, and the allowlist needs a decision.
- Step 3: the operator has not chosen A or B, or Option A needs a generated cvar list that does not exist.
- A fix needs a change to `include/hdue/bridge.h` or to a generated file.
- Conformance shows a new failure you cannot trace to a deliberately refused input.
- The excerpts above do not match the code.

## Maintenance notes

- New code that imports a UE property from scene text must call `HdueSceneEditable` first. Reviewers: grep for
  `FindPropertyByName` in any change.
- When UE changes `USD.Subdiv.MaxSubdivLevel`'s default, update the clamp to match.
- Deferred: limits on actor and instance counts (plan 007 caps grooms; ISM counts are capped by 32-bit checks).
