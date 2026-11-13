# Plan 017: The Houdini and usdview hosts recover from errors, stay responsive and leave the user's scene clean

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- integrations/houdini/python3.13libs/hdue_houdini.py integrations/houdini/python3.13libs/hdue_ue_import.py integrations/houdini/python3.13libs/hdue_op_textures.py integrations/houdini/python3.13libs/hdue_viewport.py scripts/asset_controls.py scripts/usdview_hdue.py scripts/export_usd_materialx.py scripts/Local-Paths.ps1 scripts/Copy-ProjectContent.ps1 scripts/Prepare-ProjectAssets.ps1 scripts/Test.ps1 scripts/Test-Houdini.ps1 tests/asset_controls_test.py tests/houdini_op_textures.py`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: M (nine small, independent fixes)
- **Risk**: LOW
- **Depends on**: none (plan 018 also edits `hdue_ue_import.py`: if 018 landed first, re-read the file and keep its changes)
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

These are nine user-facing bugs in hdUE's Python hosts. Each is small, but
together they make hdUE feel broken in day-to-day use:

1. One temporary error in the Houdini host blocks the "Use Unreal Engine in the
   Viewport" menu until Houdini restarts.
2. The Unreal Asset LOP's **Import** button freezes Houdini for a minute or more,
   and Esc cannot cancel it.
3. Using a Copernicus (COP) texture adds a ROP node named `__hdue_texture_export`
   to the user's `/out` network, which is then saved into their `.hip` and
   multiplies on each reopen.
4. 3-component colour parameters get alpha 0 in the Houdini viewport but alpha 1
   in husk and usdview, so the same material can look different.
5. The **Retry material preparation** button raises an error when the host has
   not started.
6. A prim whose `unreal:asset` attribute is not a string closes usdview.
7. A graph with an unknown surface nodedef fails with a confusing
   `'NoneType' has no attribute 'getType'` instead of the intended message.
8. usdview's camera-help dialog and status messages are in Japanese, while all
   of hdUE is in English.
9. Two scripts pick a project's `.uproject` file differently when a folder holds
   several.

Each step below fixes one of them and has its own check, so they can be
committed one by one.

## Current state

Paths below are relative to the repo root `H:\github\hdUE`. `P` means
`integrations/houdini/python3.13libs`.

### Fix 1 — sticky host failure (`P/hdue_houdini.py`)

Module globals (lines 26-27): `_host = None`, `_failure = None`.

`Host.tick` records any error in the global and stops the timer after 5 in a
row (lines 301-319):

```python
        except BaseException:
            # Shown in Houdini's status bar; the host retries, and stops only after repeated failures.
            _failure = traceback.format_exc()
            self.failures += 1
            if self.failures >= 5:
                self.running = False
                self.timer.stop()
```

A successful tick sets `self.failures = 0` (line 299) but never clears
`_failure`. `start()` (lines 539-573) returns the existing host as is, even when
it has stopped:

```python
def start():
    global _host, _failure
    if _host:
        return _host
```

`activate()` (lines 784-793) refuses whenever `_failure` was ever set:

```python
def activate():
    if not _host:
        start()
    if not _host or _failure:
        raise RuntimeError(_failure or "UE not ready")
```

The menu's "Use Unreal Engine in the Viewport" goes through `activate()`
(`P/hdue_menu.py:174`). `Host.close()` (lines 475-498) shuts UE down for the
process (one UE lifetime per process): a closed host must never be restarted,
but a host stopped by 5 tick errors is not closed (only `running = False` and
the timer stopped). The timer restarts with `self.timer.start(...)` (line 324).

In-Houdini tests read `_failure` to fail fast
(`tests/houdini/python3.13libs/hdue_houdini_test.py:66-67`,
`hdue_selection_test.py:48-49`, `hdue_solaris_probe.py:32`). Keep `_failure` as
"the last error seen" for them.

### Fix 2 — Asset LOP Import blocks the UI (`P/hdue_ue_import.py`)

`import_assets` (lines 115-151) runs the whole cook synchronously:

```python
    result = subprocess.run(
        [
            shutil.which("pwsh")
            or "pwsh",  # PowerShell 7, as the build scripts
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(ROOT / "scripts/Import-UEAssets.ps1"),
            ...
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(
            f"Importing from {project} failed:\n{(result.stdout + result.stderr)[-2000:]}"
        )
```

`import_from_node` (lines 159-180) wraps it in an interruptible operation but
never reports progress:

```python
    if not imported(asset):
        with hou.InterruptableOperation(
            f"Importing {asset.split('.')[-1]} from {Path(project).stem}",
            open_interrupt_dialog=True,
        ):
            stale = import_assets(project, [asset])
```

Houdini 22 docs (`houdini/hom/hou/InterruptableOperation.md`, lines 14-17 of
the local mirror): "The script **only** checks if the user has pressed Escape
when you call the context object's `updateProgress()` method… Calling
`updateProgress()` also gives Houdini a chance to respond to events." If the user
interrupts, the block exits with `hou.OperationInterrupted`.

`scripts/Import-UEAssets.ps1:23` rebuilds the editor on every import unless
`-SkipBuild` is passed: `if (-not $SkipBuild) { Build-HdUEEditor $EditorRoot }`.
The LOP never passes it. The editor module the cook uses is
`unreal/HdUEBackend/Binaries/Win64/UnrealEditor-HdUEContent.dll`, built from
`unreal/HdUEBackend/Source/HdUEContent/`.

### Fix 3 — COP texture ROP saved into the scene (`P/hdue_op_textures.py`)

`Cache.__init__` sets `self.exporter = None` (line 74). On the first Copernicus
export (lines 134-139) it creates a node in the user's `/out` and keeps it until
`Cache.close()` (lines 76-81), at Houdini exit:

```python
            if self.exporter is None:
                self.exporter = hou.node("/out").createNode(
                    "image", "__hdue_texture_export", run_init_scripts=False
                )
            rop = self.exporter
```

Houdini docs (`houdini/hom/hou/OpNode.md:3316-3319`): if a node with that name
exists, Houdini appends a number. So a `.hip` saved with the node gets
`__hdue_texture_export1` on the next session, and so on. HOM has no API that
writes a Copernicus layer to disk without a ROP (`hou.CopNode` and
`hou.ImageLayer` have none), so a ROP is still needed, but only for the
duration of one export.

### Fix 4 — viewport alpha padding (`P/hdue_viewport.py:217-222`)

```python
                else:
                    parameters[p].type = 1
                    vector = list(value)
                    vector.extend([0.0] * (4 - len(vector)))
                    for component in range(4):
                        parameters[p].value[component] = vector[component]
```

The other two paths set alpha to 1 for a 3-vector: `src/hydra/delegate.cpp:656-661`
(`p.value[3] = 1;`) and `scripts/material_api.py:113-114`
(`if len(value) == 3: p.value[3] = 1`). The rest of `Viewport.materials` only
needs `self.set_materials` and `self.runtime.check` (lines 230-237).

### Fix 5 — Retry button with no host (`P/hdue_houdini.py:808-827`)

```python
def configure_live_preparation(node):
    if node.parm("hdue_substrate_auto_prepare"):
        return
    ...
    retry.setScriptCallback(
        "import hdue_houdini; hdue_houdini._host.live_preparation.retry()"
    )
```

`_host` is `None` until a Scene Viewer selects hdUE. The callback string is
saved into the node (and the `.hip`), and the early `return` means nodes made
by older versions keep the old string.

### Fix 6 — usdview closes on a non-string `unreal:asset`

`scripts/asset_controls.py:34-44`:

```python
        for prim in stage.Traverse():
            attr = prim.GetAttribute("unreal:asset")
            value = attr.Get() if attr else None
            if value:
                if isinstance(value, Sdf.AssetPath):
                    value = value.path
                if not isinstance(value, str):
                    raise ValueError(
                        f"unreal:asset requires a UE object path string at {prim.GetPath()}"
                    )
                refs[str(prim.GetPath())] = value
```

`AssetReferences.warnings` (line 45) is already shown to the user by
`scripts/usdview_hdue.py:350-360` (`controller.statusMessage("hdUE project
warning: " + warning)`). But the `ValueError` escapes `sync_stage`, and the
usdview tick (lines 496-499) treats any exception as fatal:

```python
        except BaseException:
            self.failure = traceback.format_exc()
            self.timer.stop()
            QtWidgets.QApplication.instance().closeAllWindows()
```

`tests/asset_controls_test.py` (unit suite) tests `AssetReferences` with a real
in-memory stage and has no test expecting the `ValueError`.

### Fix 7 — `None` nodedef (`scripts/export_usd_materialx.py:266-279`)

```python
        terminal = graph.get("terminal", {}).get("node")
        # A node that is a material itself (Lama Surface's): it is the material UE imports, under the material's name.
        if (
            terminal
            and library.getNodeDef(graph["nodes"][terminal]["id"]).getType()
            == "material"
        ):
            names[terminal] = material_name
        exported = {}
        fields = []
        for path, node in graph["nodes"].items():
            definition = library.getNodeDef(node["id"])
            if definition is None:
                raise ValueError(
                    f"Unknown MaterialX nodedef: {path}: {node['id']}"
                )
```

### Fix 8 — Japanese strings (`scripts/usdview_hdue.py`)

The camera-help dialog (lines 255-266, opened by the "Camera controls..." menu
action) and four status messages (lines 461-490) are in Japanese. No test or
doc matches these strings (`grep -rln 準備 tests docs` finds nothing). English
meanings:

| Line | Japanese | English |
|---|---|---|
| 258 | カメラ操作 | Camera controls |
| 259 | ビューポート上で操作します。 | Use these in the viewport. |
| 260 | メッシュの選択：左クリック（USD／cook済みUEメッシュ） | Select a mesh: left click (USD and cooked UE meshes) |
| 261 | 回転：Alt＋左ドラッグ | Tumble: Alt + left drag |
| 262 | 平行移動：Alt＋中ドラッグ | Track: Alt + middle drag |
| 263 | 平行移動（中ボタンなし）：Ctrl＋Alt＋左ドラッグ | Track (no middle button): Ctrl + Alt + left drag |
| 264 | ズーム／前後移動：Alt＋右ドラッグ、またはホイール | Dolly: Alt + right drag, or the wheel |
| 464 | UEアセット準備待ち {}件 / 配置Prim {}件 — Path Tracer蓄積は準備完了後 | Waiting for {} UE asset(s), {} placed prim(s); the Path Tracer accumulates once they are ready |
| 472 | UEアセット準備完了 — {} Actors / {} Meshes | UE assets ready: {} actors, {} meshes |
| 480 | UDIM準備中 — ページ {}/{}（Path Tracer 0 samples） | Preparing UDIM textures: page {} of {} (Path Tracer at 0 samples) |
| 488 | UDIM準備完了 — Path Tracer蓄積開始 | UDIM textures ready; the Path Tracer starts accumulating |
| 490 | リアルタイム描画 | Rendering in realtime |

### Fix 9 — two `.uproject` resolvers

`scripts/Copy-ProjectContent.ps1:5-10` throws unless a folder holds exactly one
`.uproject`:

```powershell
$SourceProject = if (Test-Path -LiteralPath $ProjectPath -PathType Container) {
    $Projects = @(Get-ChildItem -LiteralPath $ProjectPath -Filter *.uproject)
    if ($Projects.Count -ne 1) { throw 'Specify one .uproject file.' }
    $Projects[0].FullName
}
else { (Resolve-Path -LiteralPath $ProjectPath).Path }
```

`scripts/Prepare-ProjectAssets.ps1:31` takes the first one silently:

```powershell
$SourceProject = if (Test-Path -LiteralPath $ProjectPath -PathType Container) { (Get-ChildItem -LiteralPath $ProjectPath -Filter *.uproject | Select-Object -First 1).FullName } else { (Resolve-Path -LiteralPath $ProjectPath).Path }
```

With `-SkipCopy`, the copy step (which would have thrown) does not run, so the
catalog's `source_project` can name the wrong project. Both scripts dot-source
`scripts/Local-Paths.ps1`, which holds the shared helpers (`Get-HdUELocalPath`,
`New-HdUERun`, `Build-HdUEEditor`).

### Conventions

- Python: Black 24.10.0, Ruff 0.7.4, 80 columns (`pyproject.toml`). Comments
  explain why, one line. Never silence warnings.
- PowerShell: `$ErrorActionPreference = 'Stop'`, check `$LASTEXITCODE` after
  native calls, PSScriptAnalyzer 1.23.0 with `PSScriptAnalyzerSettings.psd1`,
  and `Invoke-Formatter` must leave each file unchanged (CI checks this).
- Unit tests: `unittest`, listed in `scripts/Test.ps1:14` (run with `uv run
  --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python`)
  or, for Houdini-Python 3.13 modules without `hou`, like
  `tests/houdini_menu_test.py` (`uv run --python 3.13 python`, line 19).
  hython tests are listed in `scripts/Test-Houdini.ps1:18` and run with
  `uv run -- <HFS>\bin\hython.exe`. `tests/houdini_op_textures.py` shows how to
  import `hdue_houdini` under hython with a throwaway `HDUE_RUNTIME_CONFIG`
  (lines 13-20).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Unit suite (no GPU, no Houdini) | `.\scripts\Test.ps1 -Suite unit` | exit 0 |
| hython tests (Houdini, no GPU) | `.\scripts\Test-Houdini.ps1` | exit 0, ends with "Verified ..." lines (also launches GUI tests, needs the GPU) |
| COP texture tests (Houdini) | `uv run -- "<houdini_root>\bin\hython.exe" tests\houdini_op_textures.py` | `OK` |
| Python format | `uvx black@24.10.0 --check scripts tests integrations` | exit 0 |
| Python lint | `uvx ruff@0.7.4 check scripts tests integrations` | `All checks passed!` |
| PowerShell lint | `Invoke-ScriptAnalyzer -Path scripts\<file>.ps1 -Settings .\PSScriptAnalyzerSettings.psd1` | no output |
| PowerShell format | `$t=[IO.File]::ReadAllText('scripts\<file>.ps1'); (Invoke-Formatter -ScriptDefinition $t) -eq $t` | `True` |

`<houdini_root>` is `houdini_root` in `.local/paths.json`. When a release hdUE
package is installed, set `$env:HOUDINI_PACKAGE_SKIPLIST = 'hdUE'` before any
Houdini or hython run, so the repo's code is the one loaded.

## Scope

**In scope**:
- `integrations/houdini/python3.13libs/hdue_houdini.py` (Fixes 1, 5)
- `integrations/houdini/python3.13libs/hdue_ue_import.py` (Fix 2)
- `integrations/houdini/python3.13libs/hdue_op_textures.py` (Fix 3)
- `integrations/houdini/python3.13libs/hdue_viewport.py` (Fix 4)
- `scripts/asset_controls.py` (Fix 6)
- `scripts/export_usd_materialx.py` (Fix 7)
- `scripts/usdview_hdue.py` (Fix 8)
- `scripts/Local-Paths.ps1`, `scripts/Copy-ProjectContent.ps1`, `scripts/Prepare-ProjectAssets.ps1` (Fix 9)
- Tests: `tests/asset_controls_test.py`, `tests/houdini_op_textures.py`,
  new `tests/houdini_host_state_test.py`, new `tests/houdini_viewport_test.py`,
  and their entries in `scripts/Test.ps1` / `scripts/Test-Houdini.ps1`

**Out of scope** (do NOT touch):
- `merge_catalog`, `publish` and the content-copy preflight logic in
  `hdue_ue_import.py` / `Copy-ProjectContent.ps1:15-21` — plan 018 owns them.
- `src/hydra/delegate.cpp` and `scripts/material_api.py` — they already pad
  alpha to 1; they are the reference, not the change.
- `scripts/Import-UEAssets.ps1` — only its callers change (`-SkipBuild` is an
  existing switch).
- The `_failure` reads in `tests/houdini/python3.13libs/*` — keep the global's
  meaning ("last error seen") so they keep working.

## Git workflow

- AGENTS.md: work happens on `main` (the only branch). Commit on the branch or
  worktree the operator gave you; if none, `main`. Never push unless told.
- One commit per fix. Subject: one plain English sentence, e.g. "The host
  menu works again after a transient tick error". Body: why, and how you
  verified. End with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Steps

### Step 1 (Fix 1): A stopped host resumes; `activate()` no longer refuses on an old error

In `P/hdue_houdini.py`:

1. Add a `Host.resume()` method next to `close()`: if `self.closed`, return
   False (UE is gone for this process); otherwise set `self.failures = 0`,
   `self.running = True`, `self.timer.start(16)`, return True.
2. In `start()`, replace `if _host: return _host` with: if `_host` and not
   `_host.closed`, call `_host.resume()` when `not _host.running`, then return
   `_host`. A closed host is returned as is (activate refuses it below).
3. In `activate()`, replace `if not _host or _failure:` with
   `if not _host or _host.closed or not _host.running:` and keep the message
   `RuntimeError(_failure or "UE not ready")`.
4. In `start()`'s success path (after `_host = Host()`), set `_failure = None`
   (a start that worked clears an earlier start failure). Do not clear it in
   `tick`: tests use it as "an error happened".

Add `tests/houdini_host_state_test.py` (hython, `unittest`), set up like
`tests/houdini_op_textures.py:13-20`. Replace `hdue_houdini._host` with a stub
(`types.SimpleNamespace(running=False, closed=False, failures=5, resume=...)`
recording calls) and set `hdue_houdini._failure = "old traceback"`. Tests:
- `start()` on a stopped, not-closed stub calls `resume()` once and returns it.
- `start()` on a closed stub does not call `resume()`.
- `activate()` with a running stub and a stale `_failure` does not raise
  `RuntimeError` before it reaches `scene_viewer()` (patch
  `hdue_houdini.scene_viewer` to return a stub with `hydraRenderers()` →
  `("Unreal Engine",)` and a `setHydraRenderer` recorder).
- `activate()` with a closed stub raises `RuntimeError`.
Restore the globals in `tearDown`. Add `'houdini_host_state_test'` to the list
in `scripts/Test-Houdini.ps1:18`.

**Verify**: `uv run -- "<houdini_root>\bin\hython.exe" tests\houdini_host_state_test.py` → `OK` (4 tests).

### Step 2 (Fix 2): The Import button reports progress, can be cancelled and times out

In `P/hdue_ue_import.py`:

1. Give `import_assets` an optional `progress=None` callable. Replace
   `subprocess.run(..., capture_output=True, text=True)` with
   `subprocess.Popen(...)` whose stdout and stderr go to a log file
   `run / "import.log"` (open it before, `stderr=subprocess.STDOUT`) — a pipe
   that nobody reads can fill and hang the child. Poll in a loop:
   `while proc.poll() is None:` call `progress()` if given, then
   `time.sleep(0.1)`; stop with `proc.kill()` and raise `TimeoutError` after
   a deadline (use 1800 s and say why in a one-line comment: one cook took
   "about a minute", a hung editor must not hang Houdini forever). In a
   `finally`, if the process is still running (an interrupt raised inside
   `progress()`), `proc.kill()` and `proc.wait()`.
2. On a non-zero exit, raise `RuntimeError` with the log's last 2000
   characters and its path (read the log file).
3. Pass `-SkipBuild` to `Import-UEAssets.ps1` when
   `ROOT / "unreal/HdUEBackend/Binaries/Win64/UnrealEditor-HdUEContent.dll"`
   exists and is newer than every file under
   `ROOT / "unreal/HdUEBackend/Source/HdUEContent"` (compare `st_mtime`). One
   comment line: the editor is rebuilt only when its source is newer.
4. In `import_from_node`, keep the `with hou.InterruptableOperation(...)
   as operation:` block and pass `progress=operation.updateProgress` to
   `import_assets`. An Escape then raises `hou.OperationInterrupted` inside
   the loop, the `finally` kills the cook, and the exception propagates (do
   not catch it).

**Verify**:
- `uvx ruff@0.7.4 check integrations` → `All checks passed!`
- Unit test without Houdini, in a new `tests/hdue_ue_import_test.py` (plan 018
  may already have created it: then add to it): patch `subprocess.Popen`
  with a fake whose `poll()` returns `None` twice then `0`, and a `progress`
  callable that counts calls. Assert `progress` was called at least twice
  and the command contains `-SkipBuild` when a fake DLL newer than a fake
  source tree exists (point `hdue_ue_import.ROOT` at a temp folder). Add a
  second case where `progress` raises `KeyboardInterrupt` on its first call:
  assert the fake's `kill()` was called. Run it as Houdini-Python without
  `hou`, like `houdini_menu_test` in `scripts/Test.ps1:19`
  (`uv run --python 3.13 python tests\hdue_ue_import_test.py` → `OK`), and
  add that line to the unit block of `scripts/Test.ps1`. `hdue_ue_import`
  imports only `hdue_batch`, which needs no `hou`.
- Manual (needs Houdini, a UE editor and a UE project): in an hdUE Unreal
  Asset LOP, press Import, then Esc within 10 s. The interrupt dialog closes,
  no `UnrealEditor-Cmd.exe` keeps running (`Get-Process UnrealEditor-Cmd`
  finds nothing), and Houdini stays responsive during the import. If you
  cannot run this, say so in your report.

### Step 3 (Fix 3): The COP texture ROP lives only during an export

In `P/hdue_op_textures.py`:

1. Remove `self.exporter` (line 74 and `close()` lines 77-82).
2. In the Copernicus branch (lines 134-159), create the Image ROP, set its
   parameters, render, and destroy it in a `finally`, all inside
   `with hou.undos.disabler():` so the user's undo history does not get the
   temporary node. Keep every `rop.parm(...)` line as it is.
3. In `Cache.__init__`, delete leftovers saved into older `.hip` files: every
   child of `/out` whose name starts with `__hdue_texture_export` and whose
   type name is `image` (`hou.node("/out").children()`), inside
   `hou.undos.disabler()`. Comment: older hdUE versions left this node in
   saved scenes.

In `tests/houdini_op_textures.py`, in `test_copernicus_output_port_and_raw_values`
(line 220), after the existing resolves, add:
`self.assertEqual([n for n in hou.node("/out").children() if n.name().startswith("__hdue_texture_export")], [])`.

**Verify**: `uv run -- "<houdini_root>\bin\hython.exe" tests\houdini_op_textures.py` → `OK`, including the new assertion.

### Step 4 (Fix 4): Viewport colour parameters get alpha 1, as husk and usdview do

In `P/hdue_viewport.py:219-220`, pad a 1- to 3-component value with `0.0` up
to 3 components and then `1.0` for alpha; a 4-component value stays as is.
One comment: alpha 1 as `delegate.cpp` and `material_api.py` send it.

Add `tests/houdini_viewport_test.py` (no Houdini; run like
`houdini_menu_test`, `uv run --python 3.13 python`): put
`integrations/houdini/python3.13libs` and `scripts` on `sys.path`, import
`hdue_viewport`, and call `hdue_viewport.Viewport.materials(stub, 1,
{"/M": "/Game/M.M"}, {"/M": {"tint": (0.5, 0.25, 0.125), "rgba": (1, 2, 3, 4)}})`
where `stub` is a `SimpleNamespace` whose `set_materials` captures the
`ctypes.byref` argument (read `._obj` to get the struct) and returns 0, and
whose `runtime.check` does nothing. Assert the `tint` parameter's
`value[:]` is `[0.5, 0.25, 0.125, 1.0]` and `rgba` is `[1, 2, 3, 4]`. Add the
test to the unit block of `scripts/Test.ps1` next to `houdini_menu_test`.

**Verify**: `uv run --python 3.13 python tests\houdini_viewport_test.py` → `OK`.

### Step 5 (Fix 5): Retry works with no host, also on nodes saved by older versions

In `P/hdue_houdini.py`:

1. Add a module function `retry_preparation()`: if `_host` is None, call
   `start()`; if `_host` is still None, show
   `hou.ui.setStatusMessage("hdUE host is not running: " + (_failure or "select Unreal Engine in a viewport"), hou.severityType.Warning)`
   and return; else `_host.live_preparation.retry()`.
2. Set the button callback to `"import hdue_houdini; hdue_houdini.retry_preparation()"`.
3. In `configure_live_preparation`, when the parms already exist, still set
   `node.parm("hdue_substrate_retry")`'s callback to the new string (via the
   node's parm template group: find the template, `setScriptCallback`,
   `replace` it, `node.setParmTemplateGroup(group)`), then return. Only do
   this when the stored callback differs, so an up-to-date node is not
   re-written on each call.

Extend `tests/houdini_host_state_test.py`: with `_host = None` and
`hdue_houdini.start` patched to return None, `retry_preparation()` does not
raise.

**Verify**: `uv run -- "<houdini_root>\bin\hython.exe" tests\houdini_host_state_test.py` → `OK` (5 tests).

### Step 6 (Fix 6): A bad `unreal:asset` value is a warning, not a crash

In `scripts/asset_controls.py:40-43`, instead of raising, record a message
`f"{prim.GetPath()}: unreal:asset requires a UE object path string"` in a local
list and `continue`. After the loop, put those messages first in
`self.warnings` (line 45 builds it; prepend them). `usdview_hdue.py` already
shows `warnings` in the status bar, so no change is needed there.

In `tests/asset_controls_test.py`, add a test that authors `unreal:asset` as an
`Int` attribute on one prim and a valid string on another, calls `sync()`, and
asserts it does not raise, the valid prim is in the references, and one warning
names the bad prim's path.

**Verify**: `uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\asset_controls_test.py` → `OK`.

### Step 7 (Fix 7): An unknown terminal nodedef gives the intended error

In `scripts/export_usd_materialx.py:268-272`, look up the terminal's definition
first; if it is `None`, raise
`ValueError(f"Unknown MaterialX nodedef: {terminal}: {graph['nodes'][terminal]['id']}")`
(the same message the loop below uses); otherwise compare its `getType()`.

**Verify**: add a test in `tests/materialx_live_test.py` that calls
`export_usd_materialx.write(records, output, name)` (the function holding
lines 258-279, defined at `scripts/export_usd_materialx.py:233`; the existing
test around `tests/materialx_live_test.py:191-213` already calls it — copy how
it builds `records`) with a record whose terminal node id is
`"ND_does_not_exist"`, and asserts `ValueError` with
`"Unknown MaterialX nodedef"`. Run
`uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\materialx_live_test.py` → `OK`.
If that function cannot be called without UE or a full stage, STOP and report
instead of building a large fixture.

### Step 8 (Fix 8): usdview speaks English

Replace the Japanese strings in `scripts/usdview_hdue.py` with the English in
the table above. Keep the `.format(...)` arguments and their order. Title the
dialog "Camera controls" (the menu action already says "Camera controls...").

**Verify**: `python -c "import re,sys; t=open('scripts/usdview_hdue.py',encoding='utf-8').read(); sys.exit(1 if re.search('[\u3040-\u30ff\u4e00-\u9fff]', t) else 0)"` → exit 0 (no Japanese characters left), and Black/Ruff pass.

### Step 9 (Fix 9): One `.uproject` resolver for both scripts

1. In `scripts/Local-Paths.ps1`, add `function Resolve-HdUEUProject { param([Parameter(Mandatory = $true)][string]$ProjectPath) ... }`
   holding the body of `Copy-ProjectContent.ps1:5-10` (throws
   `'Specify one .uproject file.'` unless a folder holds exactly one).
2. In `scripts/Copy-ProjectContent.ps1`, set `$SourceProject = Resolve-HdUEUProject $ProjectPath`.
3. In `scripts/Prepare-ProjectAssets.ps1:31`, do the same.

**Verify**:
- PSScriptAnalyzer and `Invoke-Formatter` (table above) on the three files →
  no findings, `True`.
- `pwsh -NoProfile -Command ". .\scripts\Local-Paths.ps1; $d = New-Item -ItemType Directory (Join-Path $env:TEMP ('up' + [guid]::NewGuid())); 'a','b' | % { New-Item (Join-Path $d \"$_.uproject\") | Out-Null }; try { Resolve-HdUEUProject $d; 'no throw' } catch { $_.Exception.Message }; Remove-Item -Recurse $d"`
  → prints `Specify one .uproject file.`

### Step 10: Whole-suite checks

**Verify**:
- `uvx black@24.10.0 --check scripts tests integrations` → exit 0
- `uvx ruff@0.7.4 check scripts tests integrations` → `All checks passed!`
- `.\scripts\Test.ps1 -Suite unit` → exit 0
- On a machine with Houdini and the GPU runtime: `.\scripts\Test.ps1 -Suite houdini` → exit 0. If you have no such machine, say so.

## Test plan

- New: `tests/houdini_host_state_test.py` (hython; Fixes 1 and 5),
  `tests/houdini_viewport_test.py` (no Houdini; Fix 4),
  `tests/hdue_ue_import_test.py` (no Houdini; Fix 2 — shared with plan 018).
- Extended: `tests/houdini_op_textures.py` (Fix 3), `tests/asset_controls_test.py`
  (Fix 6), `tests/materialx_live_test.py` (Fix 7).
- Patterns: `tests/houdini_menu_test.py` (hou-free Houdini Python),
  `tests/houdini_op_textures.py` (hython with a throwaway runtime profile),
  `tests/asset_controls_test.py` (real in-memory USD stage).

## Done criteria

- [ ] `grep -n "_host.live_preparation.retry()" integrations/houdini/python3.13libs/hdue_houdini.py` finds it only inside `retry_preparation`
- [ ] `grep -n "if not _host or _failure" integrations/houdini/python3.13libs/hdue_houdini.py` returns nothing
- [ ] `grep -n "subprocess.run" integrations/houdini/python3.13libs/hdue_ue_import.py` returns nothing
- [ ] `grep -n "self.exporter" integrations/houdini/python3.13libs/hdue_op_textures.py` returns nothing
- [ ] `grep -n "Select-Object -First 1" scripts/Prepare-ProjectAssets.ps1` returns nothing
- [ ] No Japanese characters in `scripts/usdview_hdue.py` (Step 8 command exits 0)
- [ ] `.\scripts\Test.ps1 -Suite unit` exits 0 with the new tests listed
- [ ] The hython tests of Steps 1, 3 and 5 print `OK`
- [ ] Black, Ruff, PSScriptAnalyzer and Invoke-Formatter pass
- [ ] `git status` shows only in-scope files changed
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back if:

- Any excerpt above does not match the live file.
- `hou.undos.disabler` or `InterruptableOperation.updateProgress` does not
  exist in your Houdini (`hython -c "import hou; print(hou.undos.disabler, hou.InterruptableOperation.updateProgress)"`).
- `Host` gains a restart path that reloads the backend DLL or restarts UE:
  `resume()` must only restart the timer. If resuming needs more, stop.
- Step 2's unit test cannot patch `subprocess.Popen` without importing `hou`
  (that would mean `hdue_ue_import` started importing `hou` at module level).
- Step 7's function needs UE, a GPU or a large stage to call.
- A fix seems to need changes to `merge_catalog`, `publish` or the content-copy
  preflight (plan 018's scope).

## Maintenance notes

- `_failure` now means "the last error seen", used by tests and status
  messages; whether the host is usable is `_host.running and not _host.closed`.
  Code that gates on readiness must use the latter.
- Step 2 keeps the import modal (Houdini shows its interrupt dialog and stays
  responsive). A fully background import (QTimer polling, the LOP flipping
  Imported when done) is a possible follow-up; it changes when `imported`
  turns on, so it was left out.
- Step 3 still marks the scene as modified while an export runs (creating any
  node does). Only the saved leftover is fixed.
- Reviewers: check that every new `subprocess.Popen` path kills its child on
  every exit route (normal, error, timeout, interrupt).
