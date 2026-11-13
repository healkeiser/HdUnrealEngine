# Plan 008: The Python copies of the C ABI match bridge.h, and CI checks that they do

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- scripts/asset_runtime.py scripts/hdue_runtime.py scripts/surface_api.py scripts/lighting_api.py scripts/material_api.py scripts/residency_api.py integrations/houdini/python3.13libs/hdue_viewport.py include/hdue/bridge.h include/hdue/viewport_api.h tests/abi_layout.c scripts/Test.ps1`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug + tests
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE's C ABI (application binary interface: the structs and the function table in `include/hdue/bridge.h` that the delegate and the UE backend DLL share) is mirrored by hand in Python with `ctypes`, for usdview, hython and Houdini's UI. Nothing compares the two. One mirror is already wrong: the Python `AssetDesc` is 160 bytes, the C struct is 168 since ABI 23, and the Python size check expects the wrong 160, so it passes. Every Python `upsert_asset` call is rejected by the backend (which requires `struct_size == sizeof(T)`). The function table has the same risk: Python checks only its length, so a function inserted in the middle of the C table would make Python call the wrong function. This plan fixes `AssetDesc` and adds a unit test, run in CI, that compares every Python mirror with the C header.

## Current state

Files:

- `include/hdue/bridge.h` — the C ABI. `#define HDUE_ABI_VERSION 24u` (line 22); `typedef struct HdueApi {` (line 665) to `} HdueApi;` (line 762); each entry is written `HdueResult(HDUE_CALL* <name>)(...)`.
- `include/hdue/viewport_api.h` — the Houdini viewport structs (`HdueViewportStatus` line 14, `HdueViewportMaterial` 30, `HdueViewportMaterials` 36, `HdueViewportWet` 45, `HdueViewportSurfaces` 53, `HdueViewportTexture` 62, `HdueViewportTextures` 66).
- `tests/abi_layout.c` — C-side size asserts, one per line: `_Static_assert(sizeof(HdueAssetDesc) == 168, "...");` (line 45). Line 53: `_Static_assert(sizeof(HdueApi) == 8 + 45 * 8, ...)`. It is built and run by ctest (CI's `native` job).
- `scripts/hdue_runtime.py` — `ABI_VERSION = 24` (line 11); `class Api(C.Structure)` (lines 46-96) lists the 45 function names in order; the only check (lines 108-114) compares `sizeof(Api)` with `8 + 8 * (len(Api._fields_) - 2)`, i.e. with itself, so it can never fail.
- Python mirrors and their own size checks:
  - `scripts/asset_runtime.py`: `AssetDesc`, `AssetStatus`, `AssetSceneStatus`; check `(160, 584, 544)` at lines 48-52.
  - `scripts/surface_api.py`: `Binding`, `Batch`, `Status`; check `(64, 16, 48)` at line 47.
  - `scripts/lighting_api.py`: `Status`; check `56` at line 31.
  - `scripts/material_api.py`: `RendererInfo`, `Parameter`, `Update`, `MaterialStats`; check `(20, 40, 24, 32)` at lines 59-62.
  - `scripts/residency_api.py`: `Options`, `Status`; no size check.
  - `integrations/houdini/python3.13libs/hdue_viewport.py`: `Frame`, `Status`, `Material`, `Materials`, `Texture`, `Textures`, `Wet`, `Surfaces`; checks at lines 120, 140, 145, 147. It imports only `ctypes`, `pathlib`, `hdue_runtime` and `material_api` at module level (no `hou`), so a test can import it.

The broken mirror, `scripts/asset_runtime.py:8-17`:

```python
class AssetDesc(C.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("visible", U32),
        ("id", C.c_char_p),
        ("asset_path", C.c_char_p),
        ("transform", C.c_double * 16),
        ("frame", C.c_float),
        ("reserved", U32),
    ]
```

The C struct, `include/hdue/bridge.h:404-416`:

```c
typedef struct HdueAssetDesc {
    uint32_t struct_size, visible;
    const char* id;
    const char* asset_path; /* Cooked UE object path; input copied during the call. */
    double transform[16];   /* Native asset cm -> UE world cm, row-major. */
    float frame;
    float seconds_per_frame;
    double start_seconds;
} HdueAssetDesc;
```

The mapping from Python class to C struct (check each in the headers before trusting it):

| Python | C | `abi_layout.c` size |
|---|---|---|
| `hdue_runtime.InitDesc` | `HdueInitDesc` | 48 |
| `hdue_runtime.Status` | `HdueStatus` | 552 |
| `hdue_runtime.Api` | `HdueApi` | 8 + 45*8 |
| `asset_runtime.AssetDesc` / `AssetStatus` / `AssetSceneStatus` | `HdueAssetDesc` / `HdueAssetStatus` / `HdueAssetSceneStatus` | 168 / 584 / 544 |
| `surface_api.Binding` / `Batch` / `Status` | `HdueWetBinding` / `HdueSurfaceBatch` / `HdueSurfaceStatus` | 64 / 16 / 48 |
| `lighting_api.Status` | `HdueLightingStatus` | 56 |
| `material_api.RendererInfo` / `Parameter` / `Update` / `MaterialStats` | `HdueRendererInfo` / `HdueMaterialParameter` / `HdueMaterialUpdate` / `HdueMaterialStats` | 20 / 40 / 24 / 32 |
| `residency_api.Options` / `Status` | `HdueResidencyOptions` / `HdueResidencyStatus` | 16 / 584 |
| `hdue_viewport.Frame` | `HdueFrameInfo` | 72 |
| `hdue_viewport.Status` | `HdueViewportStatus` | 648 |
| `hdue_viewport.Material` / `Materials` / `Texture` / `Textures` / `Wet` / `Surfaces` | `HdueViewportMaterial` / `HdueViewportMaterials` / `HdueViewportTexture` / `HdueViewportTextures` / `HdueViewportWet` / `HdueViewportSurfaces` | 32 / 32 / 16 / 32 / 48 / 40 |

How unit tests run: `scripts/Test.ps1:14-17` runs each name in a list with `uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\<name>.py`; CI runs `./scripts/Test.ps1 -Suite unit` on `windows-2022` (`.github/workflows/ci.yml`, job `scripts`). Exemplar test file: `tests/houdini_menu_test.py` (plain `unittest`, inserts its import path into `sys.path` at the top).

Conventions: Black 24.10.0 / Ruff 0.7.4 (`pyproject.toml`, line length 80); comments explain why.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| New test | `uv run --python 3.11 python tests\abi_mirror_test.py` | `OK` |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| Format / lint | `uvx black@24.10.0 --check scripts tests integrations` ; `uvx ruff@0.7.4 check scripts tests integrations` | exit 0 / `All checks passed!` |

## Scope

**In scope**:

- `scripts/asset_runtime.py`
- `scripts/hdue_runtime.py` (the self-referential check only)
- `tests/abi_mirror_test.py` (create)
- `scripts/Test.ps1` (add the test to the unit list)

**Out of scope**:

- `include/hdue/bridge.h`, `viewport_api.h`, `tests/abi_layout.c`: the C side is the source of truth. Do not change any C struct.
- The other Python mirrors: only fix one if the new test proves it wrong, and then STOP first (see STOP conditions).
- Removing the per-module size checks in the `*_api.py` files: they still catch a wrong DLL at run time.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, on `main` (AGENTS.md).
- Two commits: the `AssetDesc` fix, then the test. Plain English subjects, e.g. `Python's AssetDesc matches ABI 23's effect timing`; body with why and verification; end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless told.

## Steps

### Step 1: Fix `AssetDesc`

In `scripts/asset_runtime.py`, replace `("reserved", U32)` with `("seconds_per_frame", C.c_float)` and add `("start_seconds", C.c_double)` after it. Change the expected tuple at lines 48-52 from `(160, 584, 544)` to `(168, 584, 544)`. Grep for any caller that sets `reserved` (`git grep -n "reserved" -- scripts integrations tests`); none is expected.

**Verify**: `uv run --python 3.11 python -c "import sys; sys.path.insert(0,'scripts'); import ctypes, asset_runtime as a; print(ctypes.sizeof(a.AssetDesc))"` → `168`.

### Step 2: Make `hdue_runtime.py`'s table check meaningful

At `scripts/hdue_runtime.py:108-114`, keep the check, but compare `C.sizeof(Api)` with the literal the C side asserts (`8 + 45 * 8`, written as a named constant `API_FUNCTIONS = 45` next to `ABI_VERSION`), not with `len(Api._fields_)`. The new unit test (Step 3) keeps that constant honest.

**Verify**: same command as Step 1 with `import hdue_runtime as h; print(ctypes.sizeof(h.Api), h.API_FUNCTIONS)` → `368 45`.

### Step 3: Add `tests/abi_mirror_test.py`

A `unittest` file that needs no DLL, no Houdini and no extra packages. It reads the headers as text:

1. **Version**: parse `#define HDUE_ABI_VERSION (\d+)u` from `include/hdue/bridge.h` and assert it equals `hdue_runtime.ABI_VERSION`.
2. **Function table**: take the text between `typedef struct HdueApi {` and `} HdueApi;`, extract names with `re.findall(r"HDUE_CALL\*\s*(\w+)\)", block)`, and assert the list equals `[name for name, _ in hdue_runtime.Api._fields_[2:]]` (same order) and its length equals `hdue_runtime.API_FUNCTIONS`.
3. **Sizes**: parse `_Static_assert\(sizeof\((\w+)\) == ([^,]+),` from `tests/abi_layout.c` (evaluate the right side with a tiny safe evaluator that accepts only digits, `+`, `*` and spaces, e.g. `re.fullmatch(r"[\d+* ]+", expr)` then `eval` with empty builtins). Assert `ctypes.sizeof(<mirror>) == size` for every pair in the table in "Current state". Keep the mapping as a dict in the test, e.g. `{"HdueAssetDesc": asset_runtime.AssetDesc, ...}`.
4. **Coverage**: for every C struct name in that mapping, assert it appears in `abi_layout.c` (so a mirror whose struct loses its C assert is noticed). For `HdueResidencyOptions` and `HdueResidencyStatus`, which already have asserts, include them.

Import paths: insert `scripts` and `integrations/houdini/python3.13libs` into `sys.path` (pattern: `tests/houdini_menu_test.py:9-11`).

**Verify**: `uv run --python 3.11 python tests\abi_mirror_test.py` → `OK`. Then temporarily change `seconds_per_frame` back to `reserved, U32` without the double: the test must fail on `HdueAssetDesc`. Restore it.

### Step 4: Run it in the unit suite

Add `'abi_mirror_test'` to the list in `scripts/Test.ps1:14`.

**Verify**: `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`; `uvx black@24.10.0 --check scripts tests integrations` and `uvx ruff@0.7.4 check scripts tests integrations` pass.

## Test plan

- New `tests/abi_mirror_test.py` with four tests (version, function table names and order, struct sizes, assert coverage). Structural pattern: `tests/houdini_menu_test.py`.
- Negative check done by hand in Step 3 (reintroduce the old `AssetDesc`, see the failure).
- Run: `.\scripts\Test.ps1 -Suite unit`.

## Done criteria

- [ ] `uv run --python 3.11 python tests\abi_mirror_test.py` → `OK`
- [ ] `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`
- [ ] `git grep -n '"reserved", U32' scripts/asset_runtime.py` → no match
- [ ] `git grep -n "len(Api._fields_)" scripts/hdue_runtime.py` → no match
- [ ] Black and Ruff checks pass
- [ ] Only in-scope files changed (`git status`)
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back if:

- The test finds another mirror whose size differs from `abi_layout.c` (it means a second live ABI bug; report which, do not guess a fix).
- The function-name list in Python differs from `bridge.h` in order or content.
- A C struct in the mapping has no `_Static_assert` in `abi_layout.c` (adding C asserts is out of scope here; report it).
- `hdue_viewport.py` cannot be imported without `hou` (then leave its structs out of the test and report).

## Maintenance notes

- Every ABI change now has three places to update in step: `bridge.h`, `tests/abi_layout.c` and the Python mirror. The new test fails in CI if the last two disagree, so a version bump that forgets Python is caught on push.
- A reviewer should check the struct mapping dict in the test against the headers, since a wrong mapping would hide a bug.
- Not done: field offsets (only sizes are compared). Two fields swapped with the same types keep the size; checking `offsetof` would need more C asserts.
