# Plan 010: Each test tier runs everything it claims, and the upload state machine has unit tests

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- scripts/Test.ps1 scripts/Test-Houdini.ps1 scripts/Test-RenderSettings.ps1 scripts/Launch-Houdini.ps1 scripts/Test-HoudiniReferences.ps1 CMakeLists.txt cmake/Houdini.cmake VERIFICATION.md .github/workflows/ci.yml tests/ src/hydra/delegate.cpp src/hydra/delegate.h src/hydra/prims.cpp scripts/materialx_lowering.py`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: M (six independent parts, A to F)
- **Risk**: LOW (A-E: tests and runners only); MED for Part F (moves delegate code into a header)
- **Depends on**: none. Plans 015, 016, 023 and 024 depend on **Part F** of this plan (and benefit from A and B).
- **Category**: tests
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

`.\scripts\Test.ps1 -Suite all` prints `Suite all passed.`, but it runs no native (C++) tests, no conformance scenes, and about half of the in-Houdini and usdview checks. So "all passed" does not mean the code works. CI runs only one of the native tests that need no SDK, although two more need none. Some in-Houdini checks compare images with thresholds below render noise, so they may never fail; a test that fails in its last step leaves Houdini running until a 420 s timeout hides the cause. And the code most likely to break, the delegate's scene-upload bookkeeping (which meshes are dirty, released, re-pulled) and the new 1,200-line `scripts/materialx_lowering.py`, has no unit test at all. Later plans (015, 016, 023, 024) change exactly that upload code; Part F gives them a safety net first.

Each part below is independent and lands as its own commit.

## Current state

### Runners

`scripts/Test.ps1` (48 lines) has suites `unit`, `hydra`, `houdini`, `all`:

```powershell
param(
    [ValidateSet('unit', 'hydra', 'houdini', 'all')][string]$Suite = 'unit',
    [string]$RuntimeConfig = ''
)
...
if ($Suite -in 'unit', 'all') {
    & uv run --python 3.11 (Join-Path $TaskRoot 'tests\test_repository_scope.py')
    ...
    foreach ($Test in 'live_materials_test', 'materialx_live_test', 'materialx_textures_cache_test', 'scene_space_test', 'substrate_graph_test', 'surface_effects_test', 'asset_controls_test') {
        & uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python (Join-Path $TaskRoot "tests\$Test.py")
        if ($LASTEXITCODE) { throw "$Test failed." }
    }
    ...
}
if ($Suite -in 'hydra', 'all') {
    # Native UE and Hydra renders, usdview, camera and buffer checks, with evidence (no rebuild).
    & (Join-Path $PSScriptRoot 'Verify.ps1') -SkipBuild -WithUsdview
}
if ($Suite -in 'houdini', 'all') {
    & (Join-Path $PSScriptRoot 'Test-Houdini.ps1') -RuntimeConfig $RuntimeConfig
    & (Join-Path $PSScriptRoot 'Test-HoudiniOpTextures.ps1') -RuntimeConfig $RuntimeConfig
    ... houdini_prim_lops.py, husk_prepared_materials.py, husk_streaming.py
}
Write-Output "Suite $Suite passed."
```

- ctest runs only inside `scripts/Build-Host.ps1` and `scripts/Build-Houdini.ps1` (both build first).
- `Verify.ps1:41-42` runs only the usdview `smoke` and `camera` checks; `scripts/Test-Usdview.ps1:3` offers `smoke, camera, scene, materialx, textures, residency, opacity, glass, assets`. `opacity`/`glass` default to a private `.local` scene (`Test-Usdview.ps1:14`); `scene`, `residency`, `assets` require `-InputUsd`; `textures`, `residency` require `-Library` (`:20-21`).
- In-Houdini test kinds: `scripts/Launch-Houdini.ps1:11` lists 20; `tests/houdini/python3.13libs/hdue_tests.py:6-26` maps them to modules. `Test-Houdini.ps1` runs `integration`, `postprocess`, `shared-builds`, `recreation`, `selection` (twice); `Test-HoudiniOpTextures.ps1` runs `optextures`. Not run by any script: `materials`, `lighting`, `surfaces`, `substrate`, `substrate-textures`, `substrate-live`, `materialx-live`, `volume-material`, `decal`, `dlss`, `viewport`, `parity`, `heavy`, and `references` (only via the orphan `scripts/Test-HoudiniReferences.ps1`, which nothing calls).
- `scripts/run_conformance.py` (175 scenes under `samples/conformance/*/`) has no wrapper; AGENTS.md says targeted runs (`--only <area>/*`) are part of every change's verification.
- `Test-Houdini.ps1:18` runs hython tests `houdini_cook, asset_controls_test, scene_space_test, render_outputs_test, shading_change_test, substrate_builder_test, dialog_scripts_test`. `tests/houdini_embedded_materials.py` (imports `hou`; cited by `VERIFICATION.md`) is in no list.
- `scripts/Test-RenderSettings.ps1:13` runs `& python ...generate_render_settings.py`; every other script uses `uv run --python 3.11`. `generate_render_settings.py` imports only the standard library (lines 16-21).
- `scripts/Launch-Houdini.ps1:68-69`:

```powershell
    if ($TestKind) {
        if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) { throw "Houdini verification timed out (PID $($Process.Id)); see $env:HDUE_HOUDINI_LOG_DIR." }
```

  The timed-out Houdini keeps running (holding UE, its DLL and the caches) while the caller starts the next case.
- CI (`.github/workflows/ci.yml`): job `native` builds `cmake -S . -B build -DHDUE_BUILD_HYDRA=OFF` and runs `ctest`; job `scripts` runs `./scripts/Test.ps1 -Suite unit` in a step named "Unit suite (repository scope, local paths, usd-core tests, gallery brightness bands)". `scripts/check_gallery.py` reads the committed `docs/gallery.md`, so that step checks docs consistency, not renders.

### Native test registration

`CMakeLists.txt`: tests for the standalone OpenUSD build are inside `if(HDUE_BUILD_HYDRA)` → `if(BUILD_TESTING)` (lines 62-106). Among them, `hdue_cryptomatte_test` (lines 90-92) and `hdue_aov_table_test` (93-95) include only `src/hydra/cryptomatte.h` (standard headers) and `src/hydra/aov_table.h` (`hdue/bridge.h`). The block after it (lines 108-112) is built even with `-DHDUE_BUILD_HYDRA=OFF`:

```cmake
if(BUILD_TESTING)
    add_executable(hdue_abi_layout tests/abi_layout.c)
    target_include_directories(hdue_abi_layout PRIVATE include)
    add_test(NAME abi_c_layout COMMAND hdue_abi_layout)
endif()
```

When `HDUE_HOUDINI_ROOT` is set, `CMakeLists.txt:28-30` includes `cmake/Houdini.cmake` and returns, so that file registers its own tests (lines 55-100), including `subdivision_refine`, `nurbs_sampling` and `displacement_dice`, which the standalone block does not register. The standalone delegate target already links `pxOsd` (line 48), which `src/hydra/subdivision.h` needs.

`VERIFICATION.md:13` says "Each SDK build has four native tests"; there are 12 (Houdini build) and 9 (standalone).

`tests/render_buffer.cpp:66-71` claims to check concurrent publication but joins the publisher thread before reading:

```cpp
        std::thread publisher([&] {
            for (int i = 0; i < 100; ++i) buffer.publish(frame, std::vector<float>(8, 2));
        });
        publisher.join();
        for (int i = 0; i < 8; ++i) check(pinned[i] == 1, "published frames modified mapped memory");
```

### In-Houdini test robustness

`tests/houdini/python3.13libs/hdue_houdini_test.py:96-116`: the error path writes `test-error.txt` and calls `finish()`; `finish()` can itself raise (`"UE shutdown incomplete"`, line ~107) before scheduling `quit -f`, so Houdini never exits and `Launch-Houdini.ps1` reports only a timeout. `hdue_op_texture_test.py:18-34` has the same shape (`"UE lifetime contract failed"`).

Image-change checks compare a mean with a threshold below render noise, e.g. `hdue_substrate_test.py:59` (`< 1e-6`), `hdue_substrate_texture_test.py:62` (`< 1e-6`), `hdue_materialx_test.py:115` (`< 0.0001`), `hdue_lighting_test.py:100` (`< 0.001`). The robust pattern already in the repo, `hdue_houdini_test.py:247-250`:

```python
        # Count changed pixels, not the mean: ...
        if (np.abs(traced - transformed).max(-1) > 0.05).sum() < 100:
            raise AssertionError("Transform did not change the image")
```

`tests/materialx_textures_cache_test.py:64-70` asserts 2,000 lookups take under 1 s of wall time, in CI.

### Code with no unit test

`scripts/materialx_lowering.py`: entry point `lower(doc, library, fps=24.0)` (line 1178) runs, per document and node graph, `upgrade_legacy`, `lower_nodes`, `merge_displacements`, `lower_integers`, `lower_matrices`, `author_defaults`, `fold_base`, `drop_pinless`, `remove_unused`, `name_definitions`; `lower_flakes` (989) and `lower_primvars` (1065) are called from `export_usd_materialx.py`. Constant inputs are folded into Python floats by `Scalars.op` (135-144); `Scalars.div` folds `x / 0` to `0.0` (169-174). No test imports the module. Test pattern for MaterialX documents: `tests/materialx_live_test.py` (imports from `scripts/` via `sys.path`, uses `MaterialX==1.39.4` from the unit suite's `uv run --with`).

The delegate's upload bookkeeping, `src/hydra/delegate.cpp`:

- `struct ScenePacket` (line 496): `take(HdUEState&, uint64_t meshBudget)` (lines 529-561) moves dirty meshes up to a byte budget and swaps the other dirty sets; `giveBack(HdUEState&)` (564-592) restores them after a failed upload, keeping only prims that still exist.
- `releasable(state, mesh)` (1719-1728) — depends on `selfHost()` (a process global in delegate.cpp) through `host.worker.joinable()`.
- `finishScene(state, packet, surface)` (1730-1779) records what reached UE, releases geometry, queues re-pulls.

`src/hydra/prims.cpp:838-899` (inside the mesh `Sync`) is the change detection: compares each new part with the previous or erased (revived) version, decides `meshChanged`, `moved`, `maskChanged`, releases geometry again for an unchanged released mesh, and updates `dirtyMeshes`, `movedMeshes`, `fullUploads`, `removed`, `repull`, `erasedMeshes`, `pendingBytes`, `revision`.

`HdUEState` is declared in `src/hydra/delegate.h:347` (it derives from `HdRenderParam`); `HdUEMeshData` at line 77; `HdUEMeshBytes` (inline) at 203; `HdUEReleasable`/`HdUEReleaseGeometry` declared at 178-179. Exemplar of a header-only piece of delegate logic with its own test: `src/hydra/geom_subsets.h` + `tests/geom_subsets.cpp` (registered in both CMake files, linked to `hd`).

### Conventions

- C++: `.clang-format` (Google, 4 spaces, 120 columns); comments explain why.
- Python: Black 24.10.0, Ruff 0.7.4 (`pyproject.toml`).
- PowerShell: PSScriptAnalyzer 1.23.0 with `PSScriptAnalyzerSettings.psd1`, and `Invoke-Formatter` (CI checks both).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| Standalone build + native tests | `.\scripts\Build-Host.ps1` | exit 0; ctest `100% tests passed` |
| Houdini build + native tests (Houdini closed) | `.\scripts\Build-Houdini.ps1` | exit 0; ctest `100% tests passed` |
| CI-like native build | `cmake -S . -B build-ci -DHDUE_BUILD_HYDRA=OFF; cmake --build build-ci --config Release; ctest --test-dir build-ci -C Release --output-on-failure` | `100% tests passed, 0 tests failed out of 3` |
| C++ format | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror <files>` | no output |
| Python format / lint | `uvx black@24.10.0 --check scripts tests integrations` ; `uvx ruff@0.7.4 check scripts tests integrations` | pass |
| PowerShell lint | `Invoke-ScriptAnalyzer -Path scripts\Test.ps1 -Settings .\PSScriptAnalyzerSettings.psd1` | no output |

(Delete `build-ci` afterwards; it is a throwaway folder.)

## Scope

**In scope**: `scripts/Test.ps1`, `scripts/Test-Houdini.ps1`, `scripts/Test-RenderSettings.ps1`, `scripts/Launch-Houdini.ps1`, `scripts/Test-HoudiniReferences.ps1` (delete, see C3), `CMakeLists.txt`, `cmake/Houdini.cmake`, `VERIFICATION.md` (native-test sentence only), `.github/workflows/ci.yml` (step name only), `tests/render_buffer.cpp`, `tests/materialx_textures_cache_test.py`, `tests/houdini/python3.13libs/hdue_houdini_test.py`, `tests/houdini/python3.13libs/hdue_op_texture_test.py`, the image-threshold lines of `hdue_substrate_test.py`, `hdue_substrate_texture_test.py`, `hdue_substrate_live_test.py`, `hdue_materialx_test.py`, `hdue_lighting_test.py`, `tests/materialx_lowering_test.py` (create), `src/hydra/scene_bookkeeping.h` (create), `src/hydra/delegate.cpp` and `src/hydra/prims.cpp` (only to move code into that header and call it), `tests/scene_bookkeeping.cpp` (create).

**Out of scope**:

- Any behaviour change in the delegate. Part F moves code; it must not change what it does. Bugs found in it are reported, not fixed (plans 015, 016 fix them).
- `scripts/materialx_lowering.py` logic (tests only; report bugs).
- `scripts/run_conformance.py` internals (plan 009 changes them); here it is only wrapped.
- Unit tests for `hdue_ue_import.py`'s `publish`/`merge_catalog`: plan 018 adds them.
- Making private-scene checks (`opacity`, `glass`, `assets`) public.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, `main` (AGENTS.md).
- One commit per part (A-F), plain English subjects, e.g. `Test.ps1 runs the native tests and a targeted conformance tier`; body with why and verification; end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless told.

## Steps

### Part A. `Test.ps1` tiers that say what they ran

**A1.** Extend `ValidateSet` with `native`, `conformance`, `extended`. Add parameters `[string[]]$Only = @()` (conformance patterns) and `[string]$BuildRoot = ''`.

- `native`: run `ctest --test-dir <dir> -C Release --output-on-failure` for each of `build` and `build\houdini` (use `.local/paths.json`'s `houdini_build_root` via `Get-HdUELocalPath` for the second; skip a folder that has no `CTestTestfile.cmake` and record it as skipped). Throw on a non-zero exit.
- `conformance`: run `<houdini_root>\bin\hython.exe scripts\run_conformance.py --runtime-config <absolute RuntimeConfig or the default profile> --only @Only`. Require `-Only` (throw `"-Suite conformance needs -Only <area>/*; the full set takes over an hour"` without it), matching AGENTS.md's tiering.
- `extended`: the in-Houdini kinds no script runs (`materials`, `lighting`, `surfaces`, `substrate`, `substrate-textures`, `substrate-live`, `materialx-live`, `volume-material`, `decal`, `references`) through `Launch-Houdini.ps1 -TestKind <kind>`, each into its own folder under one `New-HdUERun`, reading its `verified.json` like `Test-Houdini.ps1:25-31` does; plus the usdview `scene` check on `samples\cornell\cornell.usda`. Exclude `dlss` (needs the untracked DLSS plugin), `viewport`, `parity`, `heavy` (benchmarks: they assert nothing) and the private-scene usdview checks; print one line naming each excluded kind and why.

**A2.** Make `all` mean `unit, native, hydra, houdini` (conformance and extended stay explicit because they take long), and print at the end which tiers ran and which were skipped, e.g. `Suite all passed (unit, native, hydra, houdini). Not run: conformance (-Suite conformance -Only ...), extended.`

**Verify**: `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`; `.\scripts\Test.ps1 -Suite conformance` (no `-Only`) → throws the message above; `.\scripts\Test.ps1 -Suite native` after both builds → passes; `Invoke-ScriptAnalyzer -Path scripts\Test.ps1 -Settings .\PSScriptAnalyzerSettings.psd1` → no output; `(Invoke-Formatter -ScriptDefinition (Get-Content scripts\Test.ps1 -Raw)) -eq (Get-Content scripts\Test.ps1 -Raw)` → `True`.

### Part B. Native tests: CI runs all that it can, and the concurrency test races

**B1.** In `CMakeLists.txt`, move the `hdue_cryptomatte_test` and `hdue_aov_table_test` executables and their `add_test` lines from the `HDUE_BUILD_HYDRA` block (lines 90-95) into the always-built `if(BUILD_TESTING)` block at lines 108-112. They need only `src/hydra` and `include` on the include path.

**B2.** In the standalone `HDUE_BUILD_HYDRA` test block, register `hdue_subdivision_test` (`tests/subdivision.cpp`, link `hd pxOsd gf vt tf`, plus `osdCPU` if the link fails without it), `hdue_nurbs_test` (`tests/nurbs.cpp`, link `gf vt`) and `hdue_displacement_test` (`tests/displacement.cpp`, include `include`, link `gf tf`), with `add_test` names matching `cmake/Houdini.cmake` (`subdivision_refine`, `nurbs_sampling`, `displacement_dice`) and the same `ENVIRONMENT_MODIFICATION` PATH setup as the other OpenUSD tests (lines 98-105).

**B3.** `tests/render_buffer.cpp:66-71`: keep the publisher thread running while the main thread repeatedly `Map()`s, reads all 8 values of the mapped image, checks they are all 1 or all 2 (never mixed), and `Unmap()`s; join afterwards. Keep the final checks.

**B4.** `VERIFICATION.md:13`: replace "four native tests covering ..." with the actual list (count the `add_test` lines in each build).

**Verify**:
- The CI-like build in "Commands" → `100% tests passed, 0 tests failed out of 3`.
- `.\scripts\Build-Host.ps1` → ctest passes, test count is 12 for standalone (`ctest --test-dir build -N | Select-String 'Total Tests'`).
- `.\scripts\Build-Houdini.ps1` (Houdini closed) → ctest passes.
- clang-format clean on `tests/render_buffer.cpp`.

### Part C. Orphans and small runner bugs

**C1.** `scripts/Test-Houdini.ps1:18`: add `'houdini_embedded_materials'` to the hython list. Run it once by hand first: `uv run -- <houdini_root>\bin\hython.exe tests\houdini_embedded_materials.py` with `HOUDINI_PATH` set as `Test-Houdini.ps1:16` does; if it fails, STOP (it is a real failure in an unrun test).

**C2.** `scripts/Test-RenderSettings.ps1:13`: `& uv run --python 3.11 python (Join-Path $PSScriptRoot 'generate_render_settings.py') --defaults-json $Settings`.

**C3.** Delete `scripts/Test-HoudiniReferences.ps1`: Part A's `extended` tier runs `-TestKind references` through `Launch-Houdini.ps1`. If its `-MaterialXLibrary` argument is needed by that kind, pass it from `Test.ps1 -Suite extended` the same way (read the deleted script's lines 1-25 first and keep what it did).

**C4.** `scripts/Launch-Houdini.ps1:69`: before throwing on timeout, stop the process tree: `taskkill /PID $Process.Id /T /F | Out-Null` (Houdini starts child processes). Keep the throw message.

**C5.** `.github/workflows/ci.yml`: rename the step "Unit suite (repository scope, local paths, usd-core tests, gallery brightness bands)" to "Unit suite (repository scope, local paths, usd-core tests, gallery docs consistency)".

**Verify**: `git grep -n "Test-HoudiniReferences"` → no match; `git grep -n "& python" scripts/Test-RenderSettings.ps1` → no match; PSScriptAnalyzer and `Invoke-Formatter` clean on the changed `.ps1` files; `actionlint` is CI's; `git diff .github/workflows/ci.yml` shows only the name change.

### Part D. In-Houdini tests fail fast and can fail

**D1.** In `hdue_houdini_test.py` and `hdue_op_texture_test.py`, wrap the body of `finish()` in `try/except BaseException` that writes `test-error.txt` (append, so an earlier error is kept) and re-reports it, and move the `hdefereval.execute_deferred(lambda: hou.hscript("quit -f"))` call into a `finally` so Houdini always quits.

**D2.** Replace the mean-difference thresholds listed in "Current state" with the changed-pixel pattern of `hdue_houdini_test.py:247-250`: `(np.abs(a - b).max(-1) > 0.05).sum() < N`. Pick `N` per test as 1% of the image's pixels (`a.shape[0] * a.shape[1] // 100`), and keep each test's error message. Also check `hdue_substrate_live_test.py:68,81` and `hdue_substrate_texture_test.py:46,56,84,105` and convert those too.

**D3.** `tests/materialx_textures_cache_test.py:64-70`: keep the test but assert the behaviour, not wall time: patch `os.scandir`/`Path.iterdir` (whichever `udim_tiles` uses; read `scripts/materialx_textures.py` first) with a counting wrapper and assert repeated lookups do not list the directory again. Remove the `time.perf_counter()` assertion.

**Verify**: `uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\materialx_textures_cache_test.py` → `OK`. For D1/D2 (GPU, UE, Houdini): `.\scripts\Launch-Houdini.ps1 -InputUsd samples\smoke.usda -LogDirectory out\check-integration -TestKind integration` → exits 0 with `verified.json`; and run one converted test kind, e.g. `-TestKind substrate`, → exits 0.

### Part E. Unit tests for the MaterialX lowering

Create `tests/materialx_lowering_test.py`, modelled on `tests/materialx_live_test.py` (same imports via `sys.path`, `MaterialX==1.39.4`). Load the standard libraries the way `materialx_lowering.lower` expects its `library` argument (read how `scripts/export_usd_materialx.py` builds it and do the same). One small document per pass, asserting the node categories and folded values after `lower()`:

- integers: `add` of two `integer` constants feeding a `convert` to float → no `integer`-typed node remains and the folded value is the sum.
- matrices: `invertmatrix` of `diag(2, 4, 5)` applied by `transformmatrix` to `(0.4, 0.8, 1)` → no matrix-typed node remains; the result folds to `(0.2, 0.2, 0.2)`. Determinant of `diag(0.3, 0.3, 1)` → `0.09`.
- division by zero: pin the current behaviour (`Scalars.div` folds `x / 0` to `0.0`) with an explicit test named `test_divide_by_zero_folds_to_zero`, and a comment that GPUs give inf; changing it later must be deliberate.
- `merge_displacements`, `upgrade_legacy` (a 1.38 `swizzle` becomes 1.39 nodes), `lower_primvars` (a `geompropvalue` becomes a primvar reader with the expected name), `lower_flakes` (a `flake3d` is replaced): one case each; read each function's docstring and its call site in `export_usd_materialx.py` to build a minimal input.

Add `'materialx_lowering_test'` to the unit list in `scripts/Test.ps1:14`.

**Verify**: `uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\materialx_lowering_test.py` → `OK` with at least 7 tests; `.\scripts\Test.ps1 -Suite unit` → passes.

### Part F. Characterization tests for the upload bookkeeping (plans 015, 016, 023, 024 depend on this)

Goal: pin today's behaviour of the dirty-set bookkeeping so later fixes show exactly what they change. Move, do not rewrite.

**F1.** Create `src/hydra/scene_bookkeeping.h`. Move into it, unchanged except for what is needed to compile outside `delegate.cpp`:

- the dirty-set part of `ScenePacket`: `take`, `giveBack`, and the members they use (`dirtyMeshes`, `movedMeshes`, `dirtyMaterials`, `dirtyLights`, `dirtyVolumes`, `dirtyActors`, `dirtyGrooms`, `removedIds`, `volumeParameters`, `materialMeshes`, `bytes`, and `deferred`'s mesh ids). If `ScenePacket` cannot be split cleanly, move the two functions as free functions taking the sets they touch.
- the part-storing logic from `prims.cpp:838-899` as `inline void HdUEStoreMeshPart(HdUEState& state, HdUEMeshData&& part, std::set<std::string>& current)` (the loop body), called from the same place in `prims.cpp`. `HdUEStoreShearCompanion` and `traceStored` stay where they are; call them from the loop before/after as today.
- Replace the hidden `selfHost()` dependency of `releasable` with a parameter (`bool hostOwnsUe`), passed by `delegate.cpp` as `selfHost().worker.joinable()`.

`delegate.cpp` and `prims.cpp` include the new header and call the moved code. No logic change.

**F2.** Create `tests/scene_bookkeeping.cpp` (pattern: `tests/geom_subsets.cpp`, its `check` helper and `main`). Build an `HdUEState` by hand, fill `meshes` with small `HdUEMeshData` values, and assert:

1. `take` with a small budget takes at least one mesh and leaves the rest in `state.dirtyMeshes`; it swaps the other sets whole.
2. `take` adds the meshes of a dirty material to the packet as `materialMeshes`.
3. `giveBack` restores only ids still in `state.meshes` (erase one in between) and re-queues a removed id only if it is still gone.
4. Storing an unchanged part for a released mesh keeps it released and not dirty.
5. Storing a part with a new `geometryHash` marks it dirty and increases `pendingBytes` by `HdUEMeshBytes`.
6. Storing a part whose only change is its transform (placed-only) puts it in `movedMeshes`, not `dirtyMeshes`.
7. A revived part (in `erasedMeshes`, not in `meshes`) with the same hash is not dirty.
8. Today's known gaps, written as tests of current behaviour with a comment pointing to the plan that will change them, so that plan flips the expectation: an error return in mesh Sync leaves the id in `repull` (plan 015), and `HdUEStoreShape` does not add to `pendingBytes` (plan 015). If these live outside the moved code, skip them and say so in the test file.

Register it in both CMake files (standalone block linked to `hd`, like `hdue_subset_test`; `cmake/Houdini.cmake` linked to `hdue_hdk`) as `add_test(NAME scene_bookkeeping ...)`.

**Verify**:
- `.\scripts\Build-Host.ps1` and `.\scripts\Build-Houdini.ps1` → ctest passes including `scene_bookkeeping`.
- No behaviour change: render the Cornell box with husk before and after (same command, same build settings) and compare with `scripts/compare_pfm.py` or the repo's usual Cornell check; the images match within noise. Run the in-Houdini tests that cover uploads: `.\scripts\Test-Houdini.ps1` → passes.
- `git diff --stat src/hydra/delegate.cpp src/hydra/prims.cpp` shows code moved out (more deletions than insertions in those two files).

## Test plan

- New tests: `tests/materialx_lowering_test.py` (unit suite and CI), `tests/scene_bookkeeping.cpp` (ctest, both builds), the racing variant of `tests/render_buffer.cpp`.
- Newly run tests: `cryptomatte_ranks` and `aov_table` in CI; `subdivision_refine`, `nurbs_sampling`, `displacement_dice` in the standalone build; `houdini_embedded_materials.py` in `Test-Houdini.ps1`; ten in-Houdini kinds and the usdview `scene` check in `-Suite extended`.
- Hardened tests: changed-pixel thresholds, `finish()` always quits, cache-lookup test without wall time.

## Done criteria

- [ ] `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`
- [ ] CI-like native build (`-DHDUE_BUILD_HYDRA=OFF`) → `out of 3` tests, all passed
- [ ] `.\scripts\Build-Host.ps1` and `.\scripts\Build-Houdini.ps1` → ctest all passed, including `scene_bookkeeping`
- [ ] `.\scripts\Test.ps1 -Suite all` ends with a line naming the tiers run and not run
- [ ] `git grep -n "Test-HoudiniReferences"` → no match
- [ ] `git grep -n "perf_counter" tests/materialx_textures_cache_test.py` → no match
- [ ] Black, Ruff, clang-format, PSScriptAnalyzer and `Invoke-Formatter` all clean
- [ ] Cornell husk render unchanged after Part F
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back if:

- A test that was never run before (`houdini_embedded_materials.py`, an `extended` kind, a newly registered native test) fails on the current code: that is a real bug to report, not to fix here.
- Part F needs a behaviour change to compile or to be testable, or moving the code touches more than about 200 lines of `delegate.cpp`/`prims.cpp`.
- `HdUEState` cannot be constructed in a test without linking `delegate.cpp` (then test the moved functions on the sets they take instead, and report).
- The Cornell render differs after Part F.
- `subdivision.h` cannot link in the standalone build with the OpenUSD libraries available.

## Maintenance notes

- Plans 015 and 016 change the bookkeeping; they must update the "known gap" tests in `tests/scene_bookkeeping.cpp` as part of their fix, which is the point of writing them now.
- New in-Houdini test kinds must be added either to `Test-Houdini.ps1` or to the `extended` list in `Test.ps1`; a reviewer should reject a kind that no runner calls.
- Not done: running conformance in CI (it needs a GPU, UE and Houdini) and image baselines for the extended tier.
