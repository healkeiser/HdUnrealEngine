# Plan 009: Conformance, gallery and evidence checks fail when the result is wrong

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- scripts/run_conformance.py scripts/generate_materialx_conformance.py samples/conformance/materialx/lowered_nodes.usda scripts/render_gallery.py scripts/check_gallery.py scripts/materialx_coverage.py scripts/Test-MaterialXCoverage.ps1 scripts/usdview_hdue.py tests/husk_prepared_materials.py tests/substrate_texture_render.py scripts/Test.ps1`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: M (seven small, independent parts)
- **Risk**: LOW
- **Depends on**: none
- **Category**: tests
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

Several of hdUE's automated checks report success when the result is wrong. The worst: 8 of the 11 conformance checks for the MaterialX lowering (the newest, least-tested code, `scripts/materialx_lowering.py`) expect a grey, but the colour check divides by the sum of R, G and B, so any grey passes, right or wrong. Also: a NaN pixel makes the gallery's regression gate pass; a conformance scene with no checks reports `pass`; `--only` with a typo reports `0 failing runs`; two scripts write `passed: true` constants; the MaterialX coverage script can never fail. Each part below makes one of these checks able to fail, and has its own verification, so they can land one at a time.

## Current state

Files and roles:

- `scripts/run_conformance.py` — renders each `samples/conformance/<area>/<scene>.usda` with husk in both modes and runs the checks stored in the scene's `customLayerData["conformance"]` JSON.
- `scripts/generate_materialx_conformance.py` — writes `samples/conformance/materialx/lowered_nodes.usda` (11 emissive quads, one per lowered MaterialX node).
- `scripts/render_gallery.py` — gallery renders, docs tables, and the Cornell tile baselines (`check_baselines`).
- `scripts/check_gallery.py` — CI check of the committed `docs/gallery.md` tables.
- `scripts/materialx_coverage.py` + `scripts/Test-MaterialXCoverage.ps1` — which MaterialX nodedefs reach UE's importer.
- `scripts/usdview_hdue.py` — usdview host; writes the usdview tests' result JSON.
- `tests/husk_prepared_materials.py`, `tests/substrate_texture_render.py` — GPU tests.

**A. The colour check ignores brightness.** `run_conformance.py:261-270`:

```python
    if "color" in check:
        rgb = np.array(measure(frames, {**check["color"], "stat": "rgb"}))
        want = np.array(check["rgb"], dtype=np.float64)
        if rgb.sum() <= 1e-6:
            return [0, 0, 0], False
        got = rgb / rgb.sum()
        distance = float(np.abs(got - want / want.sum()).max())
        return [round(float(v), 3) for v in got], distance <= check.get(
            "tol", 0.12
        )
```

The generator gives every case only a colour check (`generate_materialx_conformance.py:268-276`). The expected values (the `expected` tuple, last element of each `CASES` entry) are: MatrixTransform (0.6, 0.2, 0.9), MatrixInverse (0.2,)*3, MatrixDeterminant (0.09,)*3, Integers (0.5,)*3, Booleans (0, 1, 1), Swizzle (0.9, 0.5, 0.1), CurveAdjust (0.4,)*3, Atan2 (π/4,)*3, Frame (0.1,)*3, AmbientOcclusion (0.7,)*3, Flake3D (0.27,)*3. So 8 grey cases only check "is it grey".

A "ratio" check already exists (`run_conformance.py:271-273`): `"ratio": [probeA, probeB]` divides two probe lumas and compares with `min`/`max`. Example: `samples/conformance/camera/exposure.usda:11-30`. Probes are `{"region": [x0, y0, x1, y1]}` in view units (-5..5).

The quad layout (`generate_materialx_conformance.py:254-256`): `column, row = index % 4, index // 4`, `x, y = -3.75 + 2.5 * column, 3.75 - 2.5 * row`. 11 cases use rows 0-2; row 3 (y = -3.75) is free.

**A2. Wrong declared output types.** `generate_materialx_conformance.py:223-229` declares `float outputs:out` for every node whose id does not end in `color3`, so matrix, vector3, integer and boolean nodes declare `float` outputs. A per-case override already exists: a shader tuple may carry a third element, a dict of output names to types (`Flake3D` uses `{"presence": "float"}`, line ~191; read by `for shader, (identity, inputs, *outputs)` at line 215).

**B. Empty runs pass.** `run_conformance.py:317-323`:

```python
def status(entry):
    if "error" in entry and not entry["checks"]:
        return "error"
    passed = sum(c["pass"] for c in entry["checks"])
    if passed == len(entry["checks"]) and "error" not in entry:
        return "pass"
```

With zero checks for a mode (all checks have `"modes"` excluding it, `:298-299`), `0 == 0` → `pass`. In `main` (`:438-473`), `scenes(args.only)` yielding nothing leaves `failures = 0` and exits 0.

**C. NaN and missing renders pass the gallery gate.** `render_gallery.py:449-489`: `luminance` is a plain `.mean()`; in `check_baselines`, a missing EXR is skipped (`if not path.exists(): continue`), and a NaN `p95` makes `p95 > limit` False. `render_gallery.py:985-987` writes `json.dumps(data, ...)`, which emits bare `NaN`. `render_gallery.py:431-446` (`hardware`) runs `nvidia-smi` and raises `FileNotFoundError` where it is missing. `render_gallery.py:188-196`: if fewer than 3 `hdUE setting changed: houdini:frame` lines are found, `husk()` silently returns wall time including UE startup instead of per-frame time. `render_gallery.py` imports `numpy` and `OpenImageIO` at module level (lines 28-29) and `local_paths` (line 30).

`check_gallery.py:23-30` only matches rows whose last cell is `\d+\.\d+ / \d+\.\d+`, so a `nan / 1.01` row is silently skipped. Rows ending in `n/a` are legitimately skipped (display-referred images; `docs/gallery.md:38`).

**D. Constant success flags.** `usdview_hdue.py:548-551`:

```python
        result = dict(
            launcher.test_result,
            passed=True,
```

This overwrites any `passed` a check put in `test_result`. `tests/substrate_texture_render.py:125-166` (`check`): with zero frames in `metrics.json`, `results` stays empty and it writes `passed=True` and prints "Passed 0 numerical texture checks".

**E. Silent skips.** `tests/husk_prepared_materials.py:227`: `if ARGS.materialx_library:` skips the MaterialX and wet-surface checks when no library exists under `out/materialx` (its docstring, lines 8-9, says so). It still exits 0 (`:337`).

**F. MaterialX coverage cannot fail.** `scripts/materialx_coverage.py:243-347` (`main`) always returns normally; failing categories are only printed. `Test-MaterialXCoverage.ps1:14` throws only on a non-zero exit.

Conventions: Python is Black 24.10.0 / Ruff 0.7.4 (`pyproject.toml`); scripts with dependencies are run with `uv run`; tests are `unittest` files (exemplar: `tests/houdini_menu_test.py`). Unit tests run from the list in `scripts/Test.ps1:14-17` and in CI (`.github/workflows/ci.yml`, job `scripts`).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Regenerate MaterialX conformance | `uv run --python 3.11 python scripts\generate_materialx_conformance.py` | prints the `.usda` path |
| Targeted conformance (GPU, UE, Houdini) | `& "<houdini_root>\bin\hython.exe" scripts\run_conformance.py --runtime-config <absolute path of .local\runtime-houdini.json> --only materialx/*` | ends `0 failing runs` |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| Gallery unit test (new) | `uv run --python 3.11 --with numpy --with OpenImageIO python tests\gallery_checks_test.py` | `OK` |
| Format / lint | `uvx black@24.10.0 --check scripts tests integrations` ; `uvx ruff@0.7.4 check scripts tests integrations` | pass |

`<houdini_root>` is the `houdini_root` value in `.local/paths.json`.

## Scope

**In scope**: `scripts/run_conformance.py`, `scripts/generate_materialx_conformance.py`, `samples/conformance/materialx/lowered_nodes.usda` (regenerated only, never hand-edited), `scripts/render_gallery.py`, `scripts/check_gallery.py`, `scripts/materialx_coverage.py`, `scripts/Test-MaterialXCoverage.ps1`, `scripts/materialx_coverage_baseline.json` (create), `scripts/usdview_hdue.py` (lines ~548-551 only), `tests/husk_prepared_materials.py`, `tests/substrate_texture_render.py`, `tests/gallery_checks_test.py` (create), `tests/conformance_status_test.py` (create), `scripts/Test.ps1` (unit list only).

**Out of scope**:

- `scripts/materialx_lowering.py`: if the new ratio checks fail, that is a real lowering bug for another plan; report it, don't fix it here.
- Other conformance scenes and their checks.
- `tests/gallery_baselines.json` values, and every committed gallery image.
- The gallery's display encoding (gamma 2.2 vs sRGB): a look change that needs a re-render; not this plan.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, `main` (AGENTS.md).
- One commit per part (A, B, C, D, E, F). Plain English subjects, e.g. `MaterialX lowering checks measure the value, not only the hue`; body with why and verification; end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless told.

## Steps

### Part A. MaterialX lowering checks measure the value

**A1.** In `generate_materialx_conformance.py`, add a reference quad in the free row 3 (e.g. column 0: x = -3.75, y = -3.75) with a material whose emission colour is a constant `(1, 1, 1)` (same Standard Surface as `material()`'s, with `color3f inputs:emission_color = (1, 1, 1)` instead of a connection). Name it `Reference`.

For every case whose expected colour is grey (all three components equal and > 0), add a second check:

```python
{"name": f"{name}: value", "ratio": [{"region": case_box}, {"region": reference_box}],
 "min": v * (1 - tol), "max": v * (1 + tol)}
```

where `v` is the grey value, and `tol` is 0.05 (0.1 for `Flake3D`, whose mean is statistical). Keep the existing colour checks. Luma of a grey equals the grey (the Rec.709 weights sum to 1), so the ratio is the value itself.

**A2.** Give each intermediate shader its real output type, using the per-shader third tuple element that already exists: matrix nodes (`ND_creatematrix_*_matrix33`, `ND_invertmatrix_matrix33`, `ND_transpose_matrix33`, matrix multiply) → `{"out": "matrix3d"}`; `ND_transformmatrix_vector3` → `{"out": "float3"}`; integer nodes → `{"out": "int"}`; boolean nodes → `{"out": "bool"}`. Read each case's node ids in `CASES` and its MaterialX nodedef name to choose. Leave `float` for float outputs.

**A3.** Regenerate: `uv run --python 3.11 python scripts\generate_materialx_conformance.py`.

**Verify**: `git diff --stat samples/conformance/materialx/lowered_nodes.usda` shows changes; `uv run --python 3.11 --with usd-core python -c "from pxr import Usd; Usd.Stage.Open('samples/conformance/materialx/lowered_nodes.usda')"` exits 0. Then (needs GPU, UE, Houdini) the targeted conformance run `--only materialx/*` → `0 failing runs`.

### Part B. Empty conformance runs fail

In `run_conformance.py`, `status`: an entry with no checks and no error returns `"error"` (treat "no check applies to this mode" as a spec error). If some scenes intentionally have no check in one mode, the spec should list `"modes"` at the scene level instead (`spec.get("modes", MODES)` is already honoured at `:441`); count how many scenes are affected first (see Verify) and STOP if more than 3.

In `main`, after building the list of `(name, path)` from `scenes(args.only)`, print `no scene matches --only <patterns>` to stderr and exit 2 when it is empty.

Add `tests/conformance_status_test.py` that imports `run_conformance` (it imports `numpy`; run it with `--with numpy --with usd-core`) and checks: `status({"checks": []}) == "error"`, all-pass → `"pass"`, one failing → `"partial 1/2"`. Add it to the unit list in `scripts/Test.ps1:14`.

**Verify**: `uv run --python 3.11 --with numpy --with usd-core python tests\conformance_status_test.py` → `OK`. And a scan for scenes that would now error: `uv run --python 3.11 --with usd-core python -c "<loop over samples/conformance/*/*.usda except common, load spec, print scene/mode pairs where no check applies>"` → expected: none (if any, STOP condition above).

### Part C. NaN, missing renders and missing tools fail the gallery

In `render_gallery.py`:

- `luminance` and `tiles`: raise `ValueError(f"non-finite pixels in {path}")` when `not np.isfinite(image).all()` (pass the path in, or check in `pixels`).
- `check_baselines`: a missing EXR for a scene/key that has a stored baseline is a failure (`f"{name} {key}: render missing"`); a non-finite `p95` is a failure.
- The `gallery.json` write: `json.dumps(data, indent=1, allow_nan=False)`.
- `hardware`: wrap each query in `try/except OSError` and return the parts it found.
- `husk`: when `frames > 2` and fewer than 3 trace lines were found, raise `RuntimeError` naming the missing `hdUE setting changed: houdini:frame` trace instead of returning wall time.

In `check_gallery.py`, `rows`: match any summary row ending in `| <a> / <b> |`; skip it only when the cell is exactly `n/a`; otherwise convert with `float()` and record a failure (`"<title>: brightness cell '<cell>' is not a finite number"`) for `ValueError` or a non-finite value.

Add `tests/gallery_checks_test.py`:

- `check_gallery.rows`/`check` on a small Markdown string with a `nan / 1.01` row → failure reported; an `n/a` row → skipped; a normal row → parsed.
- `render_gallery.check_baselines` with `render_gallery.OUT` and `BASELINES` patched to a temp folder and `render_gallery.pixels` patched to return arrays: a NaN image → failure; a missing render with a stored baseline → failure; an identical image → no failure.

Run it with `--with numpy --with OpenImageIO` (`render_gallery` imports both at module level) and add it to `scripts/Test.ps1` with those dependencies (its own line, like `houdini_menu_test` at line 19).

**Verify**: `uv run --python 3.11 --with numpy --with OpenImageIO python tests\gallery_checks_test.py` → `OK`; `uv run --python 3.11 python scripts\check_gallery.py` → still `Gallery: N scenes within their brightness bands.` on the committed page.

### Part D. No constant success flags

- `usdview_hdue.py:548-551`: build the result with `passed` taken from `launcher.test_result.get("passed", True)` (a check that sets `passed: False` must win).
- `tests/substrate_texture_render.py`, `check`: raise `AssertionError("no frames to check")` when `metrics["frames"]` is empty or `results` ends empty.

**Verify**: `uv run --python 3.11 python -m py_compile scripts\usdview_hdue.py tests\substrate_texture_render.py` → exit 0; `git grep -n "passed=True" scripts/usdview_hdue.py tests/substrate_texture_render.py` → only the substrate result line after the emptiness check.

### Part E. Skipped checks are visible

In `tests/husk_prepared_materials.py`, when `ARGS.materialx_library` is empty, print `SKIPPED: MaterialX library and wet-surface checks (no library under out/materialx)` and add a `--require-all` flag: with it, a skip makes the script exit 1. Default behaviour (exit 0 with the printed skip) stays, so `Test.ps1 -Suite houdini` keeps working on machines without a library; plan 010's extended tier passes `--require-all`.

**Verify**: `uv run --python 3.11 python tests\husk_prepared_materials.py --help` lists `--require-all`.

### Part F. MaterialX coverage fails on a regression

In `materialx_coverage.py`: add `--baseline <file>` (default `scripts/materialx_coverage_baseline.json`) and `--bless`. With `--bless`, write the sorted list of nodedef names whose status is `ok`. Without it, load the baseline and exit 1 listing every baselined nodedef that is no longer `ok` (new `ok` nodedefs are fine; print them as "newly ok, run --bless"). `Test-MaterialXCoverage.ps1` passes `-Bless` through as `--bless`.

Create the baseline by running the script once with `--bless` on the current tree (needs the UE engine root and OpenUSD from `.local/paths.json`): `.\scripts\Test-MaterialXCoverage.ps1 -Bless`.

**Verify**: `.\scripts\Test-MaterialXCoverage.ps1` → exit 0 after blessing. Then temporarily add a fake name `ND_not_real` to the baseline → the run exits 1 naming it; restore the file.

## Test plan

- New: `tests/conformance_status_test.py` (Part B), `tests/gallery_checks_test.py` (Part C). Both in the unit list, so CI runs them.
- Updated scene: `samples/conformance/materialx/lowered_nodes.usda` (ratio checks, correct types), checked by the targeted conformance run.
- Manual negative checks in Parts A (optional: temporarily change one expected grey in the generator, regenerate, see the ratio check fail, revert) and F.

## Done criteria

- [ ] `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.` with the two new tests in its list
- [ ] `samples/conformance/materialx/lowered_nodes.usda` contains `"ratio"` checks for the 8 grey cases (`git grep -c '"ratio"' samples/conformance/materialx/lowered_nodes.usda` → at least 8) and no `float outputs:out` on matrix nodes
- [ ] Targeted conformance `--only materialx/*` → `0 failing runs` (or a STOP report if a ratio check fails)
- [ ] `run_conformance.py --only no_such_scene` exits 2
- [ ] `scripts/materialx_coverage_baseline.json` exists and `.\scripts\Test-MaterialXCoverage.ps1` exits 0
- [ ] Black and Ruff pass
- [ ] Only in-scope files changed; `plans/README.md` status row updated

## STOP conditions

Stop and report back if:

- A new ratio check fails on the current build (Part A): that is a real MaterialX lowering bug, which this plan only exposes. Report the case, the measured ratio and the expected value.
- More than 3 conformance scene/mode pairs have no applicable check (Part B): the rule needs a decision.
- The committed `docs/gallery.md` fails the stricter `check_gallery.py` (Part C).
- `render_gallery` cannot be imported in a unit test without Houdini or the GPU.

## Maintenance notes

- New MaterialX lowering cases should be added with a numeric expectation (ratio to the reference quad), never with a colour check alone when the value is a grey.
- The coverage baseline must be re-blessed after deliberate changes to UE's importer version or to the lowering; review the "newly ok" and "no longer ok" lists in that commit.
- Deferred: the gallery's gamma-2.2 display encoding, non-deterministic SVG charts, and the other generator drift (separate plan on generators).
