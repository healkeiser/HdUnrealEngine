# Plan 006: Cache clearing can only delete hdUE's own cache files, and its test can never delete anything

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- integrations/houdini/python3.13libs/hdue_menu.py tests/houdini_menu_test.py unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug (data loss)
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

Three pieces of code delete files from a folder whose path can come from an environment variable, `HDUE_VIRTUAL_TEXTURE_CACHE`. A typo in that variable (for example a project folder, or `Documents`) makes hdUE delete the user's own files.

1. The Houdini menu **Unreal > Caches > Clear...** deletes everything inside the folder. Its only guard refuses a drive root and the home folder.
2. The UE backend, at startup, deletes every file older than one day in that folder that is not a `.manifest`. There is no guard at all.
3. A unit test calls the real delete function on the home folder and on the root of the current drive. It relies on the guard to raise an error first. If a future edit breaks the guard, running the unit suite (locally, or in CI) empties the home folder and the drive that holds the repository.

After this plan, all three only ever delete files hdUE itself wrote, and the test cannot delete anything even when the guard is broken.

## Current state

Files:

- `integrations/houdini/python3.13libs/hdue_menu.py` — the Unreal main menu's actions; `cache_directories` resolves the four cache folders, `clear_folder` empties one.
- `tests/houdini_menu_test.py` — unit tests for `hdue_menu`, run by `scripts/Test.ps1 -Suite unit` (so also by CI).
- `unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp` — the runtime virtual texture cache; `Evict` trims it once per process.

`hdue_menu.py:37-66` resolves the folders. Only the virtual texture cache can be moved by an environment variable; the others are fixed sub-folders:

```python
    folders = {}
    local = env.get("LOCALAPPDATA")
    if local:
        folders["texture"] = Path(local) / "hdUE" / "TextureCache"
    virtual = env.get("HDUE_VIRTUAL_TEXTURE_CACHE")
    if virtual or local:
        folders["virtual_texture"] = (
            Path(virtual)
            if virtual
            else Path(local) / "hdUE" / "VirtualTextureCache"
        )
    saved = saved_directory(env, runtime_project)
    if saved:
        folders["nanite"] = saved / "NaniteCache"
    temp = env.get("TMP") or env.get("TEMP") or env.get("USERPROFILE")
    if temp:
        folders["ocio"] = Path(temp) / "hdue_ocio"
    return folders
```

Note: `saved_directory` (`hdue_menu.py:23-34`) also uses `HDUE_USER_DIR`, so the Nanite cache is `<HDUE_USER_DIR>/Saved/NaniteCache`. That folder name is fixed, so it is not at risk the same way.

`hdue_menu.py:91-116`, the delete:

```python
def clear_folder(folder):
    """Delete everything inside folder and keep the folder. Returns the paths that could not be deleted (files another
    process still holds open: Windows refuses to delete them).
    """
    folder = Path(folder)
    # HDUE_VIRTUAL_TEXTURE_CACHE is user input: never empty a drive root or the home folder it might name by mistake.
    if folder.resolve() in (
        Path(folder.anchor).resolve(),
        Path.home().resolve(),
    ):
        raise ValueError(f"Refusing to clear {folder}")
    failed = []
    if not folder.is_dir():
        return failed
    for entry in folder.iterdir():
        try:
            if entry.is_dir() and not entry.is_symlink():
                # onexc (Python 3.12+; Houdini 22 runs 3.13): keep going past held files.
                shutil.rmtree(
                    entry, onexc=lambda _, path, __: failed.append(path)
                )
            else:
                entry.unlink()
        except OSError:
            failed.append(str(entry))
    return failed
```

It is called from `clear_caches` (`hdue_menu.py:242-282`) after a confirmation dialog that shows each folder's path and size.

`tests/houdini_menu_test.py:96-99`, the test that aims the real function at real folders:

```python
    def test_refuses_roots(self):
        for folder in (Path.home(), Path(Path.cwd().anchor)):
            with self.assertRaises(ValueError):
                hdue_menu.clear_folder(folder)
```

`VirtualTextureBuild.cpp:56-81`, the backend eviction (runs once per process, from `CacheDirectory()` at `:83-100`):

```cpp
void Evict(const FString& Directory) {
    const FDateTime Now = FDateTime::UtcNow();
    auto& Files = IFileManager::Get();
    TArray<FString> Manifests, Chunks;
    Files.FindFiles(Manifests, *(Directory / TEXT("*.manifest")), true, false);
    ...
    // A day's grace: another process writes a set's chunks before its manifest.
    Files.FindFiles(Chunks, *(Directory / TEXT("*")), true, false);
    for (const FString& Name : Chunks)
        if (!Name.EndsWith(TEXT(".manifest")) && !Named.Contains(Name) &&
            Now - Files.GetTimeStamp(*(Directory / Name)) > FTimespan::FromDays(1))
            Files.Delete(*(Directory / Name), false, false, true);
}
const FString& CacheDirectory() {
    static const FString Directory = [] {
        FString Path = FPlatformMisc::GetEnvironmentVariable(TEXT("HDUE_VIRTUAL_TEXTURE_CACHE"));
        if (Path.IsEmpty())
            Path = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA")), TEXT("hdUE"),
                                   TEXT("VirtualTextureCache"));
```

What the backend writes into that folder (read from the same file):

- chunks: one file per chunk, named `Chunk.BulkDataHash.ToString()` (`:74`, `:196`, `WriteChunk` at `:115-118`). The hash is UE's chunk hash; its `ToString()` is a fixed-length hexadecimal string (expected 40 hex characters for an SHA-1; Step 4 checks the real length).
- manifests: `vt_<key>_<tile>_<border>.manifest` (`:108`).
- temporary files: `FPaths::CreateTempFilename(*CacheDirectory(), TEXT("chunk"))` (`:115`) and `TEXT("manifest")` (`:444`). `CreateTempFilename` defaults its extension to `.tmp` (UE `Engine/Source/Runtime/Core/Public/Misc/Paths.h:685`), so these are `chunk<GUID>.tmp` and `manifest<GUID>.tmp`.

So every file hdUE puts there matches one of: 40 hex characters, `vt_*.manifest`, `chunk*.tmp`, `manifest*.tmp`.

Repo conventions:

- Python: Black 24.10.0 (line length 80) and Ruff 0.7.4 (`pyproject.toml`); Google-style docstrings; comments say why.
- C++: `.clang-format` (Google style, 4 spaces, 120 columns), UE naming in `unreal/` (PascalCase locals).
- Tests are plain `unittest` files run by `scripts/Test.ps1` (`uv run --python 3.13 python tests\houdini_menu_test.py`, line 19).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Menu unit test | `uv run --python 3.13 python tests\houdini_menu_test.py` | `OK` |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | last line `Suite unit passed.` |
| Python format | `uvx black@24.10.0 --check integrations tests` | exit 0 |
| Python lint | `uvx ruff@0.7.4 check integrations tests` | `All checks passed!` |
| C++ format | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp` | exit 0, no output |
| Backend build | `.\scripts\Build-Backend.ps1` | exit 0 (~40 s incremental) |
| Stage backend | `.\scripts\Update-HoudiniRuntime.ps1 -SetDefault` | exit 0 |

## Scope

**In scope** (the only files you should modify):

- `integrations/houdini/python3.13libs/hdue_menu.py`
- `tests/houdini_menu_test.py`
- `unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp`

**Out of scope** (do NOT touch):

- The texture, OCIO and Nanite caches' own writers (`Scene.cpp`, `color_management.cpp`): their folders have fixed names and are only emptied by the menu.
- The confirmation dialog text in `clear_caches`: it already shows the paths.
- Any change to where caches live (`cache_directories`'s results): `tests/houdini_menu_test.py:32-66` pins them.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, on `main` (AGENTS.md: `main` is the only branch).
- One commit per step that changes code. Subjects are plain English sentences, e.g. `Cache clearing deletes only the files hdUE wrote`. The body says why and how it was verified, and ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless the operator told you to.

## Steps

### Step 1: Make `test_refuses_roots` unable to delete anything

Rewrite `test_refuses_roots` in `tests/houdini_menu_test.py` so that, even if `clear_folder` stopped raising, nothing on disk could be removed. Use `unittest.mock.patch` to replace `hdue_menu.shutil.rmtree` and `pathlib.Path.unlink` with mocks that raise `AssertionError("would delete")`, then call `clear_folder` on the same two folders and still `assertRaises(ValueError)`. Shape:

```python
    def test_refuses_roots(self):
        boom = mock.Mock(side_effect=AssertionError("would delete"))
        with mock.patch.object(hdue_menu.shutil, "rmtree", boom), mock.patch.object(Path, "unlink", boom):
            for folder in (Path.home(), Path(Path.cwd().anchor)):
                with self.assertRaises(ValueError):
                    hdue_menu.clear_folder(folder)
        boom.assert_not_called()
```

(Black will wrap the long line; run it.) Add `from unittest import mock` to the imports.

**Verify**: `uv run --python 3.13 python tests\houdini_menu_test.py` → `OK`.

### Step 2: Make the menu delete only hdUE's own files

Replace the "refuse roots" guard with an allow-list: `clear_folder(folder, owned)` deletes only entries for which `owned(entry)` is True, and still refuses a drive root and the home folder (keep that check; it is cheap). Then give each cache its rule in one dictionary next to `CACHE_LABELS`:

- `virtual_texture`: files only (no folders), whose name is 40 hexadecimal characters (`re.fullmatch(r"[0-9A-Fa-f]{40}", name)`), or matches `vt_*.manifest`, `chunk*.tmp` or `manifest*.tmp`. If Step 4 finds a different hash length, use that length here too.
- `texture`, `nanite`, `ocio`: their folders have fixed hdUE names (`hdUE/TextureCache`, `Saved/NaniteCache`, `hdue_ocio`). Allow everything inside them, but only when the folder's last name is the expected one (`folder.name in ("TextureCache", "NaniteCache", "hdue_ocio")`); otherwise raise `ValueError`.

`clear_caches` (`hdue_menu.py:273-275`) passes each cache's rule. Keep the function's return value (the list of paths that could not be deleted) and the `onexc` handling.

Entries that do not match are left in place. Count them, and after clearing, if any were left, include "N files hdUE did not write were left in <folder>" in the existing warning message, so the user learns the folder is shared.

**Verify**: `uv run --python 3.13 python tests\houdini_menu_test.py` → `OK` (the existing `test_size_and_clear_keep_the_folder` will need its fixture files renamed to owned names in Step 3; if it fails here only for that reason, continue).

### Step 3: Test the allow-list

In `tests/houdini_menu_test.py`, class `Clearing`:

- Update `test_size_and_clear_keep_the_folder` so its folder is named `hdue_ocio` (an owned name) and keeps passing.
- Add `test_virtual_texture_keeps_foreign_files`: a temp folder holding a 40-hex file, `vt_x_128_4.manifest`, `chunkABC.tmp`, `manifestABC.tmp`, `notes.txt` and a sub-folder `project/`. After clearing with the virtual texture rule, only `notes.txt` and `project/` remain.
- Add `test_fixed_caches_refuse_other_names`: clearing a temp folder named `Documents` with the texture rule raises `ValueError` and deletes nothing.

**Verify**: `uv run --python 3.13 python tests\houdini_menu_test.py` → `OK`, and the test count printed (`Ran N tests`) is 2 higher than before Step 3.

### Step 4: Restrict the backend's eviction to hdUE's file names

In `VirtualTextureBuild.cpp`, `Evict`: delete a non-manifest file only when its name is a chunk name (all hexadecimal, of the hash length) or a temporary file this code writes (`chunk*.tmp`, `manifest*.tmp`). Also delete only `vt_*.manifest` files in the manifest pass (not every `*.manifest`). First confirm the hash length: find `BulkDataHash`'s type in the engine (`.local/paths.json` key `engine_root`; search `Engine/Source/Runtime/Engine` for `struct FVirtualTextureDataChunk`) and check its `ToString()` length. A small helper is enough:

```cpp
// Only names this file writes: a chunk's hash, or a temporary file (CreateTempFilename, .tmp). The folder can be any
// path HDUE_VIRTUAL_TEXTURE_CACHE names, so nothing else in it is ever deleted.
bool IsCacheFile(const FString& Name) { ... }
```

Then guard `CacheDirectory()`: if the resolved path is a drive root (`FPaths::IsDrive` or a path of length <= 3 like a bare drive root) or the user's profile folder (`FPlatformProcess::UserDir()`'s parent, or `USERPROFILE`), log a warning and do not call `Evict` (the cache still works; it is just not trimmed).

**Verify**:
- `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp` → no output, exit 0.
- `.\scripts\Build-Backend.ps1` → exit 0.

### Step 5: Check the backend on a real run

Make a scratch folder with a 40-hex file, a `notes.txt` and a `vt_old_128_4.manifest`, all with modification dates two weeks old (`(Get-Item f).LastWriteTime = (Get-Date).AddDays(-14)`). Set `$env:HDUE_VIRTUAL_TEXTURE_CACHE` to it, stage the backend, and render the Cornell box once with husk (any small husk render the repo uses, e.g. the one in `docs/houdini/husk.md`). Unset the variable afterwards.

**Verify**: after the render, `notes.txt` still exists; the old 40-hex file and the old manifest are gone.

## Test plan

- `tests/houdini_menu_test.py`: the rewritten `test_refuses_roots` (patched deleters, asserts they were never called), `test_virtual_texture_keeps_foreign_files`, `test_fixed_caches_refuse_other_names`, and the updated `test_size_and_clear_keep_the_folder`. Pattern: the existing `Clearing` class.
- The backend change has no unit test harness; Step 5 is its manual check.
- Run: `.\scripts\Test.ps1 -Suite unit` → `Suite unit passed.`

## Done criteria

- [ ] `uv run --python 3.13 python tests\houdini_menu_test.py` prints `OK`
- [ ] `.\scripts\Test.ps1 -Suite unit` ends with `Suite unit passed.`
- [ ] `grep -n "rmtree\|unlink" tests/houdini_menu_test.py` shows them only inside `mock.patch` calls
- [ ] `uvx black@24.10.0 --check integrations tests` and `uvx ruff@0.7.4 check integrations tests` pass
- [ ] `.\scripts\Build-Backend.ps1` exits 0 and clang-format reports nothing
- [ ] Step 5's `notes.txt` survived a render
- [ ] `git status` shows changes only in the three in-scope files (and `plans/README.md`)
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- The excerpts above do not match the live code (drift).
- `BulkDataHash.ToString()` is not a fixed-length hexadecimal string, or chunk files turn out to be named some other way (then the allow-list in Steps 2 and 4 needs a different rule).
- Some other hdUE code writes files into the virtual texture cache folder with names not listed in "Current state" (search `CacheDirectory()` and `HDUE_VIRTUAL_TEXTURE_CACHE` across `unreal/` and `scripts/`).
- Step 5's render fails for a reason unrelated to the cache.

## Maintenance notes

- Any new file kind written into a cache folder must be added to that cache's allow-list in both `hdue_menu.py` and `Evict`, or it will never be cleared.
- A reviewer should check that the drive-root/home refusal is still there in `clear_folder` (defence in depth) and that no code path deletes a directory inside the virtual texture cache.
- Not done here: making `HDUE_VIRTUAL_TEXTURE_CACHE` per-user or locked down (the security plan on cache validation covers shared caches).
