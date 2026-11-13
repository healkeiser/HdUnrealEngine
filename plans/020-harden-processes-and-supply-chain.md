# Plan 020: Child processes start only trusted programs, and builds use pinned tools

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- scripts/live_materials.py integrations/houdini/python3.13libs/hdue_ue_import.py integrations/houdini/python3.13libs/hdue_batch.py src/hydra/delegate.cpp tests/live_materials_test.py .github/workflows scripts/check_shared_dlls.py integrations/houdini/UsdRenderers.json scripts/generate_render_settings.py`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: M
- **Risk**: MED (Step 4 changes a default; it needs the operator's OK)
- **Depends on**: plans/001-scene-cannot-choose-backend-dll.md (same threat model)
- **Category**: security
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE starts a few child processes: PowerShell scripts that run Unreal's editor to prepare materials and to import UE
assets. Four weaknesses let a scene file or its folder influence what runs:

1. PowerShell is found with Python's `shutil.which`, which on Windows searches the **current directory first**. husk's
   current directory is often the scene's folder, so an asset pack that ships a `pwsh.exe` or `pwsh.bat` next to its
   USD gets that program run.
2. In husk, material preparation (a full UE editor run that writes content into the shared runtime) starts by default,
   on any scene that contains an unprepared material.
3. The husk Python bridge builds Python source text from scene values with hand-written escaping. It is safe today,
   but one careless edit turns it into code execution.
4. CI and packaging use tools fetched by tag or unpinned version (a moved tag changes what runs), and every EXR husk
   writes stores the full command line and USD path (usernames, farm layout) in its metadata.

After this plan, PowerShell comes only from known install folders or PATH (never the current directory), scene values
reach Python as data, preparation in batch renders follows the operator's decision, CI is pinned, and EXRs no longer
leak paths.

## Current state

### PowerShell lookup

`scripts/live_materials.py:428-458` (`LivePreparation._start`):

```python
    def _start(self, key, records):
        executable = shutil.which("pwsh") or shutil.which("powershell")
        if not executable:
            self.phase = "failed"
            self.message = (
                "PowerShell is required for automatic Substrate preparation"
            )
            ...
        command = [
            executable,
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(self.root / "scripts/Prepare-LiveMaterials.ps1"),
            ...
        ]
```

There is no `-ExecutionPolicy Bypass`, so Windows PowerShell 5.1 (the fallback) with its default Restricted policy, or
scripts unzipped with a Mark-of-the-Web under RemoteSigned, refuse to run the script. The Asset LOP and the installer
pass it already:

`integrations/houdini/python3.13libs/hdue_ue_import.py:125-131`:

```python
    result = subprocess.run(
        [
            shutil.which("pwsh")
            or "pwsh",  # PowerShell 7, as the build scripts
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
```

(`packaging/houdini/Install.cmd:2` also uses `-ExecutionPolicy Bypass`.)

Why `which` is unsafe here. Houdini 22's Python 3.13 `shutil.which`
(`<houdini_root>/python313/lib/shutil.py:1545-1549`) does:

```python
        if sys.platform == "win32" and _win_path_needs_curdir(cmd, mode):
            curdir = os.curdir
            ...
            path.insert(0, curdir)
```

`_win_path_needs_curdir` is true unless the environment variable `NoDefaultCurrentDirectoryInExePath` is set. The bare
name `"pwsh"` passed to `subprocess.run` is searched by Windows' `CreateProcess`, which also looks in the current
directory. `scripts/` is on Houdini's `sys.path` (`hdue_batch.py:105` does `from live_materials import LivePreparation`),
so `hdue_ue_import.py` can import a helper from `live_materials`.

### Auto-preparation in husk

The delegate reads `substrateAutoPrepare` from render settings (default `True`) and passes it to Python
(`src/hydra/delegate.cpp:2464-2466`):

```cpp
        const auto libraries = quoted(s.setting<std::string>("materialxLibrary", "")) + ", " +
                               quoted(s.setting<std::string>("substrateLibrary", "")) + ", " +
                               (s.setting<bool>("substrateAutoPrepare", true) ? "True" : "False");
```

`integrations/houdini/python3.13libs/hdue_batch.py:121-147` (`prepare`) then starts `LivePreparation` and waits up to
30 minutes per frame. The setting is generated in `scripts/generate_render_settings.py:958-970` (`"substrateAutoPrepare"`,
"Prepare Edited Materials", default `True`). Render settings come from the scene's `RenderSettings` prim as well as from
the hdUE Render Settings LOP, and the delegate cannot tell them apart, so a scene file can turn preparation on.

### Python source built from scene values

`src/hydra/delegate.cpp:2445-2491` (`python()` and `resolvePreparedMaterials()`):

```cpp
    static std::string python(const std::string& expression, std::string& error) {
        const auto result = PYrunPythonExpression(expression.c_str(), PY_Result::STRING);
        ...
        const auto quoted = [](const std::string& text) {
            std::string out = "'";
            for (const char c : text) out += c == '\\' || c == '\'' ? std::string("\\") + c : std::string(1, c);
            return out + "'";
        };
```

The quoted strings (`materialxLibrary`, `substrateLibrary`, both scene-controllable settings) are pasted into
`"__import__('hdue_batch').configured(" + libraries + ")"` and `"...resolve_json(" + id + ", " + frame + ", " + libraries + ")"`.

### CI and packaging

- `.github/workflows/ci.yml` uses actions by tag: `actions/checkout@v7` (lines 19, 31, 59, 69, 85),
  `astral-sh/setup-uv@v10.0.1` (32, 86), `ilammy/msvc-dev-cmd@v1.13.0` (70). Line 62 pipes a script into bash:
  `bash <(curl -sSL https://raw.githubusercontent.com/rhysd/actionlint/v1.7.4/scripts/download-actionlint.bash) 1.7.4`
- `.github/workflows/docs.yml` has `permissions: contents: write` and uses `actions/checkout@v7` (line 20) and
  `astral-sh/setup-uv@v10.0.1` (line 23). Its mike install is already pinned by commit SHA (line 27).
- `scripts/check_shared_dlls.py:1-4` (run by `scripts/Package-Houdini.ps1:97` when building licensee packages):
  `# dependencies = ["pefile>=2023.2.7"]` — an open range. `uv.lock` is git-ignored (`.gitignore:26`).

### EXR metadata

`integrations/houdini/UsdRenderers.json:36-39`:

```json
    "husk.metadata": {
      "float OpenEXR:frame": "$frame",
      "float OpenEXR:FramesPerSecond": "$fps",
      "string OpenEXR:husk:command": "$command_line",
      "string OpenEXR:husk:usd_file": "$usdfile",
```

No doc or code reads these two keys (`git grep -n "husk:command\|husk:usd_file"` finds only this file).

### Conventions

- Python: Black 24.10.0 and Ruff 0.7.4 (`pyproject.toml`, line length 80). Google docstrings, short.
- Tests: `tests/live_materials_test.py` is a `unittest` module that puts `scripts/` on `sys.path` and uses
  `unittest.mock.patch` (see its header, lines 1-15). It runs in `scripts/Test.ps1 -Suite unit` (the `foreach` at
  `Test.ps1:14`).
- C++: Google style, 4 spaces, 120 columns; comments explain why.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | `Suite unit passed.` |
| One unit test | `uv run --python 3.11 --with usd-core --with numpy --with MaterialX==1.39.4 python tests\live_materials_test.py` | `OK` |
| Python format | `uvx black@24.10.0 --check scripts tests integrations` ; `uvx ruff@0.7.4 check scripts tests integrations` | exit 0 |
| Build delegate (Houdini closed) | `.\scripts\Build-Houdini.ps1` | exit 0, ctest passes |
| Houdini suite (GPU) | `.\scripts\Test.ps1 -Suite houdini` | exit 0 (includes `tests/husk_prepared_materials.py`) |
| Workflow lint | `actionlint` (download as in Step 5) | no output, exit 0 |
| Format C++ | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp` | exit 0 |

## Scope

**In scope**:
- `scripts/live_materials.py`
- `integrations/houdini/python3.13libs/hdue_ue_import.py`
- `integrations/houdini/python3.13libs/hdue_batch.py` (Step 4 only)
- `src/hydra/delegate.cpp` (`resolvePreparedMaterials` only)
- `tests/live_materials_test.py`
- `.github/workflows/ci.yml`, `.github/workflows/docs.yml`
- `scripts/check_shared_dlls.py` (its PEP 723 header only)
- `integrations/houdini/UsdRenderers.json` (the two metadata keys)
- Step 4 option 1 only: `scripts/generate_render_settings.py` (the setting's help text), then regenerate with
  `scripts/generate_render_settings.py` and `scripts/build_render_settings_hda.py` (never hand-edit `.ds` or HDAs), and
  `docs/` pages that describe the setting.

**Out of scope**:
- `scripts/Prepare-LiveMaterials.ps1`, `Import-UEAssets.ps1` and the commandlets — what they do is unchanged.
- Which DLL the delegate loads — plan 001. UE properties and cvars from scenes — plan 019.
- The Asset LOP's UI freeze and its profile writes — plans 017 and 018.
- Other scripts' unpinned PEP 723 dependencies (`build_docs_hair.py`, `compare_pfm.py`): developer tools, plan 025.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, `main`.
- One commit per step. Subject: a plain English sentence describing the result, as in `git log` (e.g. "UE's Saved
  folder in local app data; the menu opens the versioned guide"). Body: why, and how verified. Last line:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Do NOT push unless told. Never push to Epic's or the upstream fork's repositories.

## Steps

### Step 1: One PowerShell lookup that never uses the current directory

In `scripts/live_materials.py`, add `powershell_executable()`. Return the first that exists:

1. `%ProgramFiles%\PowerShell\7\pwsh.exe`
2. `shutil.which("pwsh")`, only if the result's folder is not the current directory (compare
   `Path(found).resolve().parent` with `Path.cwd().resolve()`)
3. `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`

Return `None` when none exists. Comment why: `shutil.which` on Windows searches the current directory first, and husk's
current directory is often the scene's folder.

Use it in `_start` (replacing `shutil.which("pwsh") or shutil.which("powershell")`) and add `"-ExecutionPolicy",
"Bypass"` after `"-NonInteractive"`, as `hdue_ue_import.py` and `Install.cmd` do. In `hdue_ue_import.import_assets`,
replace `shutil.which("pwsh") or "pwsh"` with the helper (import it inside the function:
`from live_materials import powershell_executable`), and raise `RuntimeError("PowerShell 7 or Windows PowerShell is
required to import UE assets")` when it returns `None`.

**Verify**: `git grep -n 'shutil.which("pwsh")\|or "pwsh"' -- scripts integrations` → only the line inside
`powershell_executable`. Black/Ruff → exit 0.

### Step 2: Test the lookup

In `tests/live_materials_test.py` add `test_powershell_never_from_current_directory`: create a temp folder holding an
empty `pwsh.exe`, `os.chdir` into it (restore in `finally`), patch `os.environ` so `ProgramFiles` and `SystemRoot`
point at empty temp folders and `PATH` is empty, and assert `powershell_executable()` returns `None`. Add a second case
where `SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe` exists in the patched folder and assert it is
returned.

**Verify**: the one-unit-test command → `OK` with the 2 new tests; `.\scripts\Test.ps1 -Suite unit` passes.

### Step 3: Pass scene values to Python as data

In `resolvePreparedMaterials`, replace `quoted(text)` with a function that hex-encodes the UTF-8 bytes and returns
`bytes.fromhex('<hex>').decode('utf-8')` as the Python expression. Hex digits cannot close a string or start code, so
no escaping is needed. Keep the rest of the expressions as they are. Comment why in one line.

**Verify**: `.\scripts\Build-Houdini.ps1` → exit 0; `.\scripts\Test.ps1 -Suite houdini` → exit 0
(`tests/husk_prepared_materials.py` runs husk with cooked MaterialX and Substrate libraries through this code).
clang-format → exit 0.

### Step 4: Decide preparation in batch renders (ask the operator first)

The delegate cannot tell a render setting authored by the hdUE Render Settings LOP from one authored in a downloaded
scene. Ask the operator to choose, and STOP until answered:

- **Option 1 (recommended)**: in husk, preparation runs only when the environment variable
  `HDUE_BATCH_PREPARE=1` is set (the farm or the user decides); the scene setting can still turn it off. In the
  delegate, `prepare = s.setting<bool>("substrateAutoPrepare", true) && TfGetenvBool("HDUE_BATCH_PREPARE", false)` for
  batch hosts only (the viewport keeps today's behaviour: the user is watching it). Without preparation a Substrate
  Builder graph fails the frame with the library's message and MaterialX graphs keep their render-time translation
  (`hdue_batch.py:189-199` already handles both). Update the setting's help text in
  `scripts/generate_render_settings.py`, regenerate `.ds` files and HDAs with the generator scripts, document the
  variable in `docs/getting-started/environment-variables.md` and the README environment table.
- **Option 2**: keep today's behaviour and add one sentence to the docs saying that husk prepares unprepared materials
  in any scene it renders, which runs Unreal's editor.

**Verify** (Option 1): `.\scripts\Test.ps1 -Suite houdini` passes with `HDUE_BATCH_PREPARE=1` set in that shell
(`husk_prepared_materials.py` prepares graphs), and a husk render of the same test scene without the variable reports
the library's "not prepared" message instead of starting UE's editor. (Option 2): the doc sentence exists.

### Step 5: Pin CI actions and tools

- Replace each `uses: <action>@<tag>` in both workflows with `uses: <action>@<full commit SHA> # <tag>`. Get each SHA
  with `gh api repos/<owner>/<repo>/commits/<tag> --jq .sha` (e.g. `gh api repos/actions/checkout/commits/v7 --jq .sha`).
- Replace the actionlint `curl | bash` line with a download of the v1.7.4 release archive
  (`https://github.com/rhysd/actionlint/releases/download/v1.7.4/actionlint_1.7.4_linux_amd64.tar.gz`) verified with
  `sha256sum -c` against the checksum from that release's `actionlint_1.7.4_checksums.txt` (write the checksum into the
  workflow).
- In `scripts/check_shared_dlls.py`, pin `pefile==<exact version>`: the newest release at the time (check
  `uv pip index versions pefile` or PyPI) that runs the script.

**Verify**: `actionlint` on the repo → no output; `git grep -n "@v[0-9]" -- .github/workflows` → only comments;
`uv run scripts/check_shared_dlls.py --help` (or a run on a staged runtime folder) → exit 0.

### Step 6: Stop writing the command line and USD path into EXRs

Remove the `"string OpenEXR:husk:command"` and `"string OpenEXR:husk:usd_file"` lines from
`integrations/houdini/UsdRenderers.json`. Keep the others (frame, fps, Software, Cryptomatte).

**Verify**: `python -c "import json; json.load(open('integrations/houdini/UsdRenderers.json'))"` → no error;
`git grep -n "husk:command" -- integrations` → nothing. Render one conformance scene with husk and check the EXR header
(e.g. `& "<houdini_root>\bin\iinfo.exe" -v beauty.0001.exr`) has no `husk:command`.

## Test plan

- New: 2 unit tests in `tests/live_materials_test.py` for the PowerShell lookup (Step 2).
- Existing: `tests/husk_prepared_materials.py` (houdini suite) covers the Python bridge and preparation in husk.
- CI: `actionlint` validates the edited workflows.

## Done criteria

- [ ] `git grep -n 'shutil.which("pwsh")' -- scripts integrations` → one hit, inside `powershell_executable`
- [ ] `git grep -n '"-ExecutionPolicy"' -- scripts/live_materials.py` → one hit
- [ ] New unit tests pass; `.\scripts\Test.ps1 -Suite unit` passes
- [ ] `git grep -n "bytes.fromhex" -- src/hydra/delegate.cpp` → hit; the old `quoted` escaping is gone
- [ ] Step 4 decision recorded in the commit body and its verification holds
- [ ] Workflows use SHA-pinned actions; actionlint passes; `check_shared_dlls.py` pins pefile exactly
- [ ] `UsdRenderers.json` has no `husk:command` / `husk:usd_file`
- [ ] Black, Ruff, clang-format pass; `.\scripts\Test.ps1 -Suite houdini` passes
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Step 4: the operator has not chosen an option.
- `%ProgramFiles%\PowerShell\7\pwsh.exe` does not exist on the machine and `pwsh` is only on PATH inside the repo or the
  current folder (the lookup would fail on a real setup): report the install layout.
- `husk_prepared_materials.py` fails after Step 3 with a Python syntax or decoding error.
- `gh` cannot resolve an action's SHA (network or permissions).
- The excerpts above do not match the code.

## Maintenance notes

- Any new child process must use an absolute executable path (`powershell_executable()` for PowerShell). Reviewers:
  grep for `shutil.which` and bare executable names in `subprocess` calls.
- Never build Python or shell source text from scene values; pass data.
- Bump the pinned action SHAs deliberately (Dependabot can do it) rather than switching back to tags.
- Deferred: signing the PowerShell scripts, and a lock file for all developer scripts (plan 025 covers the dev tools).
