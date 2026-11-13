# Plan 004: Cooked MaterialX libraries load again when their textures are recorded by size and time

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- scripts/materialx_controls.py scripts/materialx_textures.py scripts/build_materialx_library.py tests/materialx_live_test.py scripts/Test.ps1`
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

A cooked MaterialX library is a JSON file (`library.json`) that maps MaterialX
materials to UE assets that were imported and cooked ahead of time. Each library
records its "dependencies": the source files (textures, `.mtlx` documents) and a
fingerprint of each, so hdUE can refuse a library whose sources changed.

Commit `e37311d` (2026-11-04) changed the fingerprint of texture dependencies
from a SHA-256 hash of the file's bytes to `stat:<size>:<mtime_ns>`, because
hashing 60 GB of textures froze Houdini's UI for minutes. Every reader of these
records was switched to the helper `file_unchanged()`, which accepts both forms
— except `MaterialXLibrary.__init__` in `scripts/materialx_controls.py`, which
still compares a SHA-256 hash against the record. A hash never equals a
`stat:` string, so **every library built by `Import-UsdMaterialX.ps1` that has a
texture fails to load**: usdview stops at startup, the Houdini viewport shows a
material error, and every husk frame that uses the library fails. After this
plan, those libraries load, and libraries from `Import-MaterialX.ps1` stop
hashing every texture on Houdini's UI thread.

## Current state

- `scripts/materialx_controls.py` — `MaterialXLibrary`, the class every host
  uses to load a cooked library (usdview `usdview_hdue.py:191`, Houdini
  `integrations/houdini/python3.13libs/hdue_materialx.py:59`, husk
  `integrations/houdini/python3.13libs/hdue_batch.py:93`,
  `scripts/prepare_substrate.py:58`, `scripts/prepare_renderer_settings.py:16`).
  The broken check, `scripts/materialx_controls.py:85-93`:

  ```python
          for name, expected in library["dependencies"].items():
              path = Path(name)
              if (
                  not path.is_file()
                  or hashlib.sha256(path.read_bytes()).hexdigest() != expected
              ):
                  raise ValueError(
                      f"MaterialX source changed; import/cook again: {path}"
                  )
  ```

  Its imports (`scripts/materialx_controls.py:3-8`): `import hashlib`, `import
  json`, `from pathlib import Path`, `from types import SimpleNamespace`, `from
  materialx_textures import validate_texture_sets`. `hashlib` is still used
  later in the file (line 99, `_hdue_identity`), so keep that import.

- `scripts/materialx_textures.py:115-133` — the shared helpers added by
  `e37311d`:

  ```python
  def file_identity(path):
      """A texture dependency's identity: its size and modification time, ..."""
      stat = Path(path).stat()
      return f"stat:{stat.st_size}:{stat.st_mtime_ns}"


  def file_unchanged(path, expected):
      """Whether a recorded dependency still holds: a file_identity, or a SHA-256 of the bytes (older records)."""
      path = Path(path)
      if not path.is_file():
          return False
      if expected.startswith("stat:"):
          return file_identity(path) == expected
      import hashlib

      return hashlib.sha256(path.read_bytes()).hexdigest() == expected
  ```

- Who writes `stat:` records into a library:
  `scripts/export_usd_materialx.py:186-197` stores `file_identity(key)` per
  texture; `scripts/bind_usd_materialx_library.py:88` copies them into the
  cooked library (`library["dependencies"].update(export["dependencies"])`).

- The other readers already use the helper (checked, no change needed):
  `scripts/bind_usd_materialx_library.py:21-22` and
  `scripts/substrate_textures.py:110-116` (`validate_dependencies`, also used by
  `scripts/live_materials.py:108`). The remaining `sha256(...read_bytes())`
  calls in `scripts/` check other records, not library dependencies:
  `export_usd_materialx.py:91` (embedded `opdef:` image entries, a field named
  `sha256`), `glass_controls.py:86` (`source_sha256`), `houdini_resources.py:38`
  (content-addressed cache). Leave them.

- `scripts/build_materialx_library.py:46-56` — the `Import-MaterialX.ps1`
  library builder still records SHA-256 for every texture and the source
  `.mtlx`. It already reads each file once here to check the UE import receipt's
  SHA-1, so this is cheap at build time; the cost is at load time, where
  `MaterialXLibrary` re-hashes every texture on the host's thread:

  ```python
          for name, expected in report["dependencies_sha1"].items():
              file = Path(name).resolve()
              data = file.read_bytes() if file.is_file() else None
              if data is None or hashlib.sha1(data).hexdigest() != expected:
                  raise ValueError(
                      f"Imported texture/source changed; import again: {file}"
                  )
              dependencies[str(file)] = hashlib.sha256(data).hexdigest()
          dependencies[str(source)] = hashlib.sha256(source_bytes).hexdigest()
  ```

- Tests: `tests/materialx_live_test.py` already constructs
  `MaterialXLibrary(None, view)` with `OfflineView` (lines 10-11 import them,
  `LibraryTests` at line 144). It runs in the unit suite
  (`scripts/Test.ps1:14`, with `--with usd-core --with numpy --with
  MaterialX==1.39.4`). Model the new test on `LibraryTests`.

Conventions: Black 24.10.0 and Ruff 0.7.4 at 80 columns (`pyproject.toml`);
Google-style docstrings; comments explain why. Tests are plain `unittest`.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Unit suite (no GPU) | `.\scripts\Test.ps1 -Suite unit` | exit 0, ends without a "failed" throw |
| One test file | `uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\materialx_live_test.py` | `OK` |
| Format check | `uvx black@24.10.0 --check scripts tests integrations` | exit 0 |
| Lint | `uvx ruff@0.7.4 check scripts tests integrations` | `All checks passed!` |

## Scope

**In scope** (the only files you should modify):
- `scripts/materialx_controls.py`
- `scripts/build_materialx_library.py`
- `tests/materialx_live_test.py`

**Out of scope** (do NOT touch):
- `scripts/materialx_textures.py` — the helpers are correct; reuse them.
- `scripts/export_usd_materialx.py`, `scripts/glass_controls.py`,
  `scripts/houdini_resources.py` — their SHA-256 checks are for other records
  (embedded images, glass profiles, a content-addressed cache), not library
  dependencies.
- Any change to the library JSON schema (`version` stays 1). `file_unchanged`
  already reads both record forms, so old libraries keep loading.

## Git workflow

- AGENTS.md: work happens on `main` (the only branch). Commit on the branch or
  worktree the operator gave you; if none, `main`. Never push unless told.
- One commit per step. Subject: one plain English sentence, e.g. "Cooked
  MaterialX libraries accept size-and-time texture records". Body: why, and how
  you verified. End with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Steps

### Step 1: Write the failing test first

In `tests/materialx_live_test.py`, add a test class (e.g.
`LibraryDependencies`) with two tests. Each builds a library JSON in a
`tempfile.TemporaryDirectory()`:

1. `test_stat_dependency_loads`: write a small file `tex.png` (any bytes), write
   `library.json` as
   `{"version": 1, "materials": [], "dependencies": {str(tex): file_identity(tex)}}`
   (import `file_identity` from `materialx_textures`), then
   `MaterialXLibrary(str(library_json), OfflineView(stage, Usd.TimeCode.Default(), object()))`
   with `stage = Usd.Stage.CreateInMemory()`. It must not raise.
2. `test_changed_stat_dependency_is_refused`: same, but rewrite `tex.png` with
   different-length bytes after recording; constructing must raise
   `ValueError` whose message contains `"MaterialX source changed"`.
3. `test_sha256_dependency_still_loads`: record
   `hashlib.sha256(tex.read_bytes()).hexdigest()` instead; constructing must not
   raise (older libraries keep loading).

**Verify**: `uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\materialx_live_test.py`
→ `test_stat_dependency_loads` FAILS with `ValueError: MaterialX source changed`
(this proves the bug); the other two pass.

### Step 2: Use the shared helper in `MaterialXLibrary`

In `scripts/materialx_controls.py`, change the import to
`from materialx_textures import file_unchanged, validate_texture_sets` and
replace lines 85-93 with:

```python
        for name, expected in library["dependencies"].items():
            if not file_unchanged(name, expected):
                raise ValueError(
                    f"MaterialX source changed; import/cook again: {name}"
                )
```

Keep `import hashlib` (still used for `_hdue_identity`).

**Verify**: the Step 1 command → `OK`, all tests pass, including the 3 new ones.

### Step 3: Record `Import-MaterialX.ps1` texture dependencies by size and time

In `scripts/build_materialx_library.py:55`, record textures with
`file_identity(file)` instead of `hashlib.sha256(data).hexdigest()` (import
`file_identity` from `materialx_textures`, which the file already imports
from). Keep the SHA-1 receipt check above it (that guards the import itself).
Leave line 56 (the source `.mtlx`, a small file) as SHA-256. Add one comment
line: the load-time check runs on the host's UI thread, so textures are
identified by size and time as `export_usd_materialx.record` does.

Check that line 159 (`name not in dependencies`) still works: it tests key
membership only, so it is unaffected.

**Verify**: `uvx ruff@0.7.4 check scripts` → `All checks passed!`, and
`uv run --python 3.11 python -c "import sys; sys.path.insert(0, 'scripts'); import build_materialx_library"` → exit 0.

### Step 4: Format and run the unit suite

**Verify**:
- `uvx black@24.10.0 --check scripts tests integrations` → exit 0
- `uvx ruff@0.7.4 check scripts tests integrations` → `All checks passed!`
- `.\scripts\Test.ps1 -Suite unit` → exit 0

### Step 5 (needs the GPU machine; skip if you have no UE runtime)

If `.local/paths.json` exists and a library built by `Import-UsdMaterialX.ps1`
with textures is available under `out/`, run usdview's MaterialX check with it:
`.\scripts\Test-Usdview.ps1 -Check materialx -Library <that library.json>`
(without `-Library` the script uses `out\materialx\library.json`, an
`Import-MaterialX.ps1` library, which exercises Step 3 instead). It must exit 0.
If no such library or runtime exists, say so in your report instead.

## Test plan

- New tests in `tests/materialx_live_test.py` (Step 1): a `stat:` record loads,
  a changed file with a `stat:` record is refused, an old SHA-256 record still
  loads.
- Pattern: `LibraryTests` in the same file.
- Verification: `.\scripts\Test.ps1 -Suite unit` → exit 0, with the 3 new tests
  passing inside `materialx_live_test`.

## Done criteria

- [ ] `grep -n "sha256(path.read_bytes())" scripts/materialx_controls.py` returns nothing
- [ ] `grep -n "file_unchanged" scripts/materialx_controls.py` returns the import and the call
- [ ] `grep -n "file_identity" scripts/build_materialx_library.py` returns the import and the call
- [ ] `.\scripts\Test.ps1 -Suite unit` exits 0
- [ ] Black and Ruff (pinned versions above) pass
- [ ] `git status` shows changes only in the three in-scope files (and `plans/README.md`)
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back if:

- `scripts/materialx_controls.py:85-93` no longer matches the excerpt (someone
  fixed it, or the class moved).
- `file_unchanged` is missing from `scripts/materialx_textures.py` or no longer
  accepts both `stat:` and SHA-256 records.
- The Step 1 test does NOT fail before Step 2 (the bug is gone or the test is
  wrong — find out which before continuing).
- Another reader of library `dependencies` turns up that still hashes bytes
  directly (`grep -rn "dependencies" scripts integrations` and read each hit);
  report it rather than widening scope.

## Maintenance notes

- Every new reader of a library's `dependencies` must use
  `materialx_textures.file_unchanged`; every new writer of texture records
  should use `file_identity`. A test like Step 1 catches a reader that forgets.
- `stat:` records change when a file is copied without keeping its timestamp
  (some sync tools). That makes a library refuse to load with "MaterialX source
  changed", which is the safe direction.
- Reviewers: check that the error message still names the file, since users
  act on it.
