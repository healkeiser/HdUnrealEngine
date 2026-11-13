# Plan 018: Unreal asset imports update shared files atomically, and re-importing from a changed project works

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- integrations/houdini/python3.13libs/hdue_ue_import.py scripts/Copy-ProjectContent.ps1 scripts/Import-UEAssets.ps1 scripts/Test.ps1 tests/hdue_ue_import_test.py`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition. (Plan 017 Step 2 also edits
> `hdue_ue_import.py` and may have created `tests/hdue_ue_import_test.py`;
> that is expected — keep its changes and add to them.)

## Status

- **Priority**: P2
- **Effort**: M
- **Risk**: MED (the content-copy guard protects hdUE's own UE content; Part B changes what it allows)
- **Depends on**: none (coordinate with plan 017, which edits the same Python file)
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

The hdUE Unreal Asset LOP imports an asset from a user's UE project: it cooks
the asset, copies the cooked files into the running hdUE runtime, and records
the asset in an "asset catalog" (a JSON file listing the assets a prim can
name). It has two robustness problems:

- **Part A — shared files are rewritten in place.** The import rewrites the
  asset catalog and the runtime profile (the JSON file named by the
  `HDUE_RUNTIME_CONFIG` environment variable, which every husk, Houdini and
  delegate reads at startup) with a plain `write_text`. A process that starts
  while the file is half written reads broken JSON. The delegate then cannot
  find its backend DLL ("hdUE requires backendLibrary, HDUE_BACKEND_DLL or
  HDUE_RUNTIME_CONFIG"), and `scripts/Update-HoudiniRuntime.ps1` treats a
  profile it cannot parse as not naming its DLL and may delete that DLL. Two
  imports at once also drop each other's catalog entries. The import also
  stores an absolute catalog path in a profile that packages write with
  relative paths, so a moved package breaks.
- **Part B — re-importing from a project whose assets changed always fails.**
  The content-copy step refuses any file that already exists in hdUE's UE
  project with different bytes. After a user edits any asset in a project
  they imported from before, every later import from that project fails with
  "Different asset already exists". The import's own "N package(s) changed"
  warning, written for exactly this case, can therefore never show.

## Current state

### Part A — `integrations/houdini/python3.13libs/hdue_ue_import.py:95-112`

```python
def merge_catalog(catalog, config_path, config):
    """Adds the import's catalog entries to the runtime's asset catalog (made, and named in the profile, if it has
    none), so the Native Asset menus and the bounds proxies know them.
    """
    path = config.get("asset_catalog")
    if path:
        target = Path(path)
        current = json.loads(target.read_text(encoding="utf-8-sig"))
    else:
        target = Path(config["runtime_project"]).parent / "hdue_catalog.json"
        current = {"version": 1, "assets": {}}
    added = json.loads(Path(catalog).read_text(encoding="utf-8-sig"))["assets"]
    current["assets"] = {**added, **current["assets"]}
    target.write_text(json.dumps(current, indent=1), encoding="utf-8")
    if not path:
        profile = json.loads(config_path.read_text(encoding="utf-8-sig"))
        profile["asset_catalog"] = str(target)
        config_path.write_text(json.dumps(profile, indent=2), encoding="utf-8")
```

Called from `import_assets` (line 150) as
`merge_catalog(run / "catalog.json", config_path, config)`, where
`config_path = Path(os.environ["HDUE_RUNTIME_CONFIG"]).resolve()` (line 119) and
`config = runtime_config()` (line 120).

- `runtime_config()` (`integrations/houdini/python3.13libs/hdue_batch.py:18-38`)
  returns the profile with `asset_catalog` (and other paths) made absolute,
  resolving relative values against the profile's folder. The delegate does
  the same (`src/hydra/delegate.cpp:266-284`, `runtimeConfig`: "Relative paths
  resolve against the profile's folder"). So a relative `asset_catalog` works
  for every reader.
- Other readers of `asset_catalog`: `hdue_houdini.py:852`, `scripts/usdview_hdue.py:129-130`,
  `src/hydra/delegate.cpp:393`. All go through the resolvers above.
- The repo's atomic-write pattern, `scripts/houdini_resources.py:34-37`:

  ```python
      if not file.exists():
          pending = file.with_suffix(extension + ".tmp")
          pending.write_bytes(content)
          pending.replace(file)
  ```

  and in the same module, `publish` (`hdue_ue_import.py:89-91`):
  `partial = target.with_name(target.name + ".partial")`,
  `shutil.copyfile(source, partial)`, `os.replace(partial, target)  # a reader never sees half a package`.
- `scripts/Update-HoudiniRuntime.ps1:49-56` writes profiles through a temp file
  and a move for the same reason (its comment: a husk starting while
  `Set-Content` had truncated the file read half a profile).
- Catalog precedence: `{**added, **current["assets"]}` keeps an existing entry
  over a re-imported one. That matches `publish`, which never replaces a file
  the runtime already has. Keep it.
- Houdini runs on Windows only here; the stdlib `msvcrt.locking` is available
  for a lock file.

### Part B — `scripts/Copy-ProjectContent.ps1:11-28`

```powershell
$SourceContent = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $SourceProject) 'Content'))
$Destination = [IO.Path]::GetFullPath((Join-Path $TaskRoot 'unreal\HdUEBackend\Content'))
if ($SourceContent.TrimEnd('\') -eq $Destination.TrimEnd('\')) { throw 'Source and destination content must be different.' }
$Run = New-HdUERun -Name content-copy -Path $OutputDirectory -Create
# Preflight every collision before copying; never overwrite workspace assets. robocopy lists the existing files whose
# size or time differ; those must still hold the same bytes.
foreach ($Line in robocopy $SourceContent $Destination /E /L /XX /NJH /NJS /NDL /NP /NS /FP) {
    if ($Line -notmatch '^\s*(Newer|Older|Changed)\s+(.+)$') { continue }
    $File = $Matches[2].Trim(); $Target = Join-Path $Destination ([IO.Path]::GetRelativePath($SourceContent, $File))
    if ((Get-FileHash -LiteralPath $File).Hash -ne (Get-FileHash -LiteralPath $Target).Hash) { throw "Different asset already exists: $Target" }
}
if ($LASTEXITCODE -ge 8) { throw 'Cannot enumerate source content.' }
# Only new files (/XC /XN /XO); the log is the receipt.
& robocopy $SourceContent $Destination /E /XC /XN /XO /NP /NDL /FP "/LOG:$(Join-Path $Run 'robocopy.log')" | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Content copy failed; see $Run\robocopy.log." }
@{source_project = $SourceProject; project_sha256 = (Get-FileHash -LiteralPath $SourceProject).Hash.ToLowerInvariant(); destination = $Destination } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'copied-content.json') -Encoding utf8
```

Facts that shape the fix:

- The destination is hdUE's own UE project content,
  `unreal/HdUEBackend/Content`, shared by hdUE's own assets (under `HdUE/`)
  and every project ever imported. The guard exists so an import never
  overwrites hdUE's own assets or another project's.
- `copied-content.json` records only the source project and its hash. It does
  **not** list the files that were copied (the robocopy log is the only
  receipt, in a per-run folder under `out/`).
- Imported packages keep their `/Game/...` paths, because cooked packages
  reference each other by those paths. Moving a project's files to another
  folder (a "namespace") would break those references unless UE redirectors
  are created; that is a content-layout change.
- The script is called by `scripts/Import-UEAssets.ps1:22` (per import, a new
  `content-copy-<timestamp>` folder) and `scripts/Prepare-ProjectAssets.ps1:19`.
- After the copy, `hdue_ue_import.publish` (lines 63-92) copies cooked files
  into the runtime but never replaces a file the runtime has (a running UE may
  have it loaded) and returns those as "stale"; `import_from_node`
  (lines 174-179) warns about them. That part is by design and stays.

### Two options for Part B (recommendation: Option 2)

- **Option 1 — per-project namespace.** Copy each project into its own folder
  under `Content` and fix up references. Rejected for this plan: packages
  reference each other by `/Game/...` paths, so this needs redirectors or a
  re-save in the editor, which is a content-layout change.
- **Option 2 — a manifest of files hdUE copied (recommended).** Keep one
  manifest at `unreal/HdUEBackend/Saved/hdue-imported-content.json` (the
  `Saved` folder is UE's per-project scratch folder; check `.gitignore` covers
  it): for each destination file, the source project it came from and the hash
  it had when copied. The preflight then allows overwriting a destination file
  only if the manifest says the same source project copied it and its current
  hash still equals the recorded one (nobody edited it in hdUE's project
  since). Everything else keeps today's refusal: hdUE's own assets, files from
  another project with the same `/Game` path, and files changed in hdUE's
  workspace. Files copied before this change are not in the manifest, so the
  first re-import after the change still refuses them, with a message that says
  how to fix it (delete the listed files from `unreal/HdUEBackend/Content` or
  re-import once with a new `-Adopt` switch that records them in the manifest
  after checking their source project's `copied-content.json` receipts). Keep
  `-Adopt` only if it stays under ~15 lines; otherwise just improve the message.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| New unit tests | `uv run --python 3.13 python tests\hdue_ue_import_test.py` | `OK` |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | exit 0 |
| Python format/lint | `uvx black@24.10.0 --check scripts tests integrations`; `uvx ruff@0.7.4 check scripts tests integrations` | exit 0; `All checks passed!` |
| PowerShell lint | `Invoke-ScriptAnalyzer -Path scripts\Copy-ProjectContent.ps1 -Settings .\PSScriptAnalyzerSettings.psd1` | no output |
| PowerShell format | `$t=[IO.File]::ReadAllText('scripts\Copy-ProjectContent.ps1'); (Invoke-Formatter -ScriptDefinition $t) -eq $t` | `True` |

## Scope

**In scope**:
- `integrations/houdini/python3.13libs/hdue_ue_import.py` (`merge_catalog`; a
  small atomic-write helper; `publish` unchanged unless a test shows a bug)
- `scripts/Copy-ProjectContent.ps1` (Part B)
- `tests/hdue_ue_import_test.py` (create, or extend if plan 017 created it)
- `scripts/Test.ps1` (add the test to the unit block)

**Out of scope** (do NOT touch):
- `scripts/Update-HoudiniRuntime.ps1` — it already writes atomically; the lock
  in this plan does not cover it (see Maintenance notes).
- `publish`'s rule of never replacing a runtime file — by design.
- `import_assets`' process launch and `import_from_node` — plan 017 Step 2.
- `scripts/Prepare-ProjectAssets.ps1`, `scripts/Import-UEAssets.ps1` — they
  call `Copy-ProjectContent.ps1` with the same arguments; no change needed.
- Moving imported content to per-project folders (Option 1).

## Git workflow

- AGENTS.md: work happens on `main` (the only branch). Commit on the branch or
  worktree the operator gave you; if none, `main`. Never push unless told.
- One commit per part. Subjects, e.g. "Asset imports write the catalog and
  the runtime profile atomically" and "Re-importing from a changed project
  replaces the files hdUE copied from it". Body: why, how verified. End with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Steps

### Step 1: Characterization tests for `project_assets`, `publish` and `merge_catalog`

Create (or extend) `tests/hdue_ue_import_test.py`, modelled on
`tests/houdini_menu_test.py` (puts `integrations/houdini/python3.13libs` on
`sys.path`, no `hou`). Use `tempfile.TemporaryDirectory()` for everything. Tests:

- `project_assets`: a fake project folder with `Content/Meshes/SM_Rock.uasset`,
  `Content/BP_Door.uasset`, `Content/Maps/L_Test.umap`, `Content/T_Albedo.uasset`
  → returns `/Game/BP_Door.BP_Door_C`, `/Game/Maps/L_Test.L_Test`,
  `/Game/Meshes/SM_Rock.SM_Rock` and not the texture.
- `publish`: a cooked tree with a new file (copied), an existing identical
  file (left, not stale), an existing different `.uasset` under
  `HdUEBackend/Content/Game` (left and listed as stale), and a file at depth < 3
  (skipped). No `.partial` file remains.
- `merge_catalog` without `asset_catalog` in the profile: creates
  `hdue_catalog.json` next to the runtime project and adds `asset_catalog` to
  the profile file.
- `merge_catalog` with an existing catalog: an entry present in both keeps the
  existing value; a new entry is added.

Build `config` the way `runtime_config()` does (absolute paths), and pass the
profile path as `config_path`.

**Verify**: `uv run --python 3.13 python tests\hdue_ue_import_test.py` → `OK`
(all pass against today's code: these pin current behaviour before Step 2).

### Step 2 (Part A): Atomic writes, a relative catalog path, and a lock

In `hdue_ue_import.py`:

1. Add a helper `_write_atomic(path, text)`: write `path.with_name(path.name + ".pending")`
   with `encoding="utf-8"`, then `os.replace(pending, path)`. One comment: readers
   (husk, the delegate, `Update-HoudiniRuntime.ps1`) must never see half a file.
2. Wrap the read-modify-write of the catalog and the profile in a lock on a
   file next to the profile (`config_path.with_name(config_path.name + ".lock")`):
   open it with `open(lock, "a+b")`, then `msvcrt.locking(f.fileno(),
   msvcrt.LK_LOCK, 1)` (blocks, retrying for about 10 s, then raises
   `OSError`), unlock in `finally` (`LK_UNLCK`). Re-read the catalog and the
   profile inside the lock, not before it.
3. Use `_write_atomic` for both writes.
4. Store `asset_catalog` relative to the profile's folder when the catalog is
   on the same drive: `os.path.relpath(target, config_path.parent)`; else
   absolute (relpath raises `ValueError` across drives on Windows — catch it).

Extend the tests:
- After `merge_catalog`, no `*.pending` file remains and the profile's
  `asset_catalog` is relative (`not Path(value).is_absolute()`) and resolves,
  via `hdue_batch.runtime_config()` with `HDUE_RUNTIME_CONFIG` set to the temp
  profile, to the created catalog.
- Two `merge_catalog` calls from two threads with different `added` entries:
  both entries end up in the catalog.

**Verify**: `uv run --python 3.13 python tests\hdue_ue_import_test.py` → `OK`.

### Step 3 (Part B): A manifest of copied files lets a project replace its own earlier copies

In `scripts/Copy-ProjectContent.ps1`, implement Option 2:

1. Read `$Manifest = Join-Path $TaskRoot 'unreal\HdUEBackend\Saved\hdue-imported-content.json'`
   if it exists (a JSON object: destination path relative to `Content` →
   `{ source_project, sha256 }`), else start empty.
2. In the preflight loop, when hashes differ: allow the file (collect it in
   `$Replace`) only if the manifest entry for that relative path has
   `source_project` equal to `$SourceProject` and `sha256` equal to the
   current destination hash. Otherwise throw as today, with a message that
   names the reason: "hdUE's own asset", "copied from <other project>", or
   "changed in hdUE's workspace since it was copied".
3. Never allow anything under `Content\HdUE\` (hdUE's own assets), whatever
   the manifest says.
4. After the existing robocopy of new files, copy each `$Replace` file with
   `Copy-Item -LiteralPath $File -Destination $Target -Force`.
5. Record every file this run copied or replaced (new files: parse the run's
   `robocopy.log`, or list them in the preflight with robocopy `/L` lines
   matching `New File`) with its new hash and `$SourceProject`, and write the
   manifest through a temp file and `Move-Item -Force` (atomic, as
   `Update-HoudiniRuntime.ps1:49-56` does).
6. Keep writing `copied-content.json` as today.

**Verify** (no UE needed; uses temp folders, never the real `Content`): add a
`-Destination` parameter defaulting to today's path so the test can point
elsewhere, then run in pwsh:

```powershell
$t = Join-Path $env:TEMP ('cpc' + [guid]::NewGuid()); $src = "$t\P\Content"; $dst = "$t\Dest"
New-Item -ItemType Directory -Force "$src\Meshes", $dst | Out-Null; New-Item "$t\P\P.uproject" | Out-Null
'v1' | Set-Content "$src\Meshes\SM_A.uasset"
.\scripts\Copy-ProjectContent.ps1 -ProjectPath "$t\P\P.uproject" -OutputDirectory "$t\run1" -Destination $dst
'v2' | Set-Content "$src\Meshes\SM_A.uasset"
.\scripts\Copy-ProjectContent.ps1 -ProjectPath "$t\P\P.uproject" -OutputDirectory "$t\run2" -Destination $dst
Get-Content "$dst\Meshes\SM_A.uasset"   # → v2
```

Then a second project `Q` with its own `Meshes\SM_A.uasset` containing `q` must
throw with "copied from" in the message, and editing `$dst\Meshes\SM_A.uasset`
by hand then re-running for `P` must throw with "changed in hdUE's workspace".
Keep this as a script `tests/copy_project_content.ps1` modelled on
`tests/local_paths.ps1`, and call it from the unit block of `scripts/Test.ps1`
next to `tests\local_paths.ps1` (line 13). The manifest location must follow
`-Destination` in the test (put it next to the destination folder, e.g.
`<Destination>\..\Saved\hdue-imported-content.json`, so tests never touch the
real one).

**Verify**: `pwsh -NoProfile -File tests\copy_project_content.ps1` → exit 0;
PSScriptAnalyzer and `Invoke-Formatter` on both `.ps1` files → clean, `True`.

### Step 4: Wire the tests in and run everything

Add `tests\hdue_ue_import_test.py` to the unit block of `scripts/Test.ps1`,
run as `uv run --python 3.13 python` (like `houdini_menu_test`, line 19), unless
plan 017 already added it.

**Verify**:
- `.\scripts\Test.ps1 -Suite unit` → exit 0
- Black and Ruff (pinned) → pass
- Manual, on a machine with the UE editor and a test UE project (optional; say
  if skipped): import an asset with the hdUE Unreal Asset LOP, edit that asset
  in the source project, import again → the import succeeds and Houdini shows
  the "N package(s) of this project changed" warning.

## Test plan

- `tests/hdue_ue_import_test.py`: `project_assets`, `publish`, `merge_catalog`
  (create, keep-existing precedence, atomic, relative path, two concurrent
  merges).
- `tests/copy_project_content.ps1`: re-copy from the same project replaces its
  own file; another project's file and a workspace-edited file are refused with
  the right reason; `Content\HdUE\` is never replaced.
- Patterns: `tests/houdini_menu_test.py` (Python without `hou`),
  `tests/local_paths.ps1` (PowerShell test called from `Test.ps1`).

## Done criteria

- [ ] `grep -n "write_text" integrations/houdini/python3.13libs/hdue_ue_import.py` shows no direct write of the catalog or profile (only inside `_write_atomic`, if at all)
- [ ] `grep -n "msvcrt" integrations/houdini/python3.13libs/hdue_ue_import.py` shows the lock
- [ ] `tests/hdue_ue_import_test.py` and `tests/copy_project_content.ps1` exist and are called from `scripts/Test.ps1`
- [ ] `.\scripts\Test.ps1 -Suite unit` exits 0
- [ ] Black, Ruff, PSScriptAnalyzer and `Invoke-Formatter` pass
- [ ] `git status` shows only in-scope files changed
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back if:

- The excerpts above do not match the live files.
- `unreal/HdUEBackend/Saved/` is tracked by git or not ignored
  (`git check-ignore unreal/HdUEBackend/Saved/x` prints nothing): the manifest
  location must then be agreed with the maintainer.
- Part B seems to need moving imported content to other folders, redirectors,
  or any editor-side change (Option 1): stop and ask; that is a content-layout
  change.
- A test shows `publish` misbehaving (beyond what Step 1 pins): report it
  rather than changing `publish`.
- `msvcrt.locking` is unavailable (non-Windows Python) in the environment the
  tests run in.

## Maintenance notes

- `scripts/Update-HoudiniRuntime.ps1` also rewrites profiles (atomically) and
  does not take this lock. A profile update and an import at the same moment
  can still lose one of the two writes; it is rare (a developer staging while
  importing). If it matters, give the PowerShell script the same lock file.
- The manifest only knows files copied after this change. Its first use on an
  existing workspace will refuse earlier copies until they are deleted or
  adopted; the error message must say so.
- Reviewers: check that `Content\HdUE\` can never be overwritten, and that the
  manifest is written atomically and only after the copy succeeded.
