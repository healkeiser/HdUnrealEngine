# Plan 001: A scene file can no longer choose which backend DLL or UE project hdUE loads

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/delegate.cpp src/host/usd_render.cpp docs/architecture.md`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: security
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE is a Hydra render delegate: a plugin that Houdini, husk (Houdini's command-line renderer) and usdview load to
render a USD scene. When the delegate starts, it loads a second DLL, the "backend", which contains Unreal Engine.
Today the path of that DLL can come from the scene's render settings. husk copies every attribute of the scene's
`RenderSettings` prim into the settings map it gives the delegate, so a USD file that authors
`unreal:backendLibrary = "\\server\share\anything.dll"` makes hdUE load and run that DLL with the user's rights (or a
render farm's). The same holds for `runtimeProject`, the UE project UE boots. After this plan, both come only from the
environment (`HDUE_BACKEND_DLL`) and the runtime profile (`HDUE_RUNTIME_CONFIG`), which the user or the install
controls, never from scene content.

## Current state

Files involved:

- `src/hydra/delegate.cpp` — the render delegate. Its constructor (`HdUERenderDelegate::HdUERenderDelegate`, from line
  ~3555) copies the settings map into `state_->settings`, then reads `backendLibrary` and `runtimeProject` from it.
- `src/host/usd_render.cpp` — `hdue_usd_render`, a standalone test host. It is the only code in the repo that sets
  `backendLibrary` as a setting (line 286).
- `docs/architecture.md` — line 71 documents the `backendLibrary` render setting as a way to pick the DLL.

How settings reach the delegate. Every key of the map is copied; `unreal:<name>` keys also override `<name>`
(`src/hydra/delegate.cpp:3564-3577`):

```cpp
    state_->settings = settings;
    // As SetRenderSetting does: aliases, then unreal:<name> over both.
    for (const bool namespacedPass : {false, true})
        for (const auto& [key, value] : settings) {
            if (HdUETraceSettings() && !namespacedPass)
                std::fprintf(stderr, "hdUE initial setting %s = %s\n", key.GetText(), TfStringify(value).c_str());
            bool namespaced = false;
            const auto canonical = HdUECanonicalKey(key, namespaced);
            if (canonical == key || namespaced != namespacedPass) continue;
            if (namespaced) state_->namespaced.insert(canonical);
            state_->settings[canonical] = value;
        }
```

The DLL and project are then read from those settings first (`src/hydra/delegate.cpp:3607-3610` and `:3633`):

```cpp
    auto dll = state_->setting<std::string>("backendLibrary", TfGetenv("HDUE_BACKEND_DLL"));
    if (dll.empty()) dll = runtimeConfig("backend_dll");
    if (dll.empty()) throw std::runtime_error("hdUE requires backendLibrary, HDUE_BACKEND_DLL or HDUE_RUNTIME_CONFIG");
    state_->backend = std::make_shared<hdue::Backend>(utf8Path(dll));
...
            startSelfHost(dll, state_->setting<std::string>("runtimeProject", runtimeConfig("runtime_project")), wait);
```

`runtimeConfig(key)` (`src/hydra/delegate.cpp:266-284`) reads `backend_dll` / `runtime_project` from the JSON file
named by `HDUE_RUNTIME_CONFIG`. `hdue::Backend` (`src/client/backend.cpp`) calls `LoadLibraryExW` on whatever absolute
path it gets, and the DLL's `DllMain` runs at load.

Evidence that scene `RenderSettings` attributes reach the delegate: the conformance stage authors render settings this
way and they take effect (`samples/conformance/common/stage.usda:41-51`, e.g. `token unreal:renderMode = "realtime"`
on `def RenderSettings "Settings"`), and Houdini's HDK docs say "When rendering with `husk`, the initial render
settings are sent en mass" to `CreateRenderDelegate`.

The test host (`src/host/usd_render.cpp:285-287`) passes the DLL as a setting. It boots UE itself first
(`hdue::Backend runtime(options.dll);` at line 543), so the delegate only needs the path to load the same module:

```cpp
    for (const auto& [name, value] : prepared)
        if (name != "wetProfiles") settings[TfToken(name)] = value;
    settings[TfToken("backendLibrary")] = VtValue(utf8(options.dll));
```

Nothing else sets `backendLibrary` or `runtimeProject` (`git grep -n "backendLibrary\|runtimeProject"` lists only the
two code sites, `docs/architecture.md:71`, and `delegate.cpp:3609`'s message). No `.ds` parameter file or HDA exposes
them. The Houdini host already sets `HDUE_BACKEND_DLL` from the profile (`integrations/houdini/python3.13libs/hdue_houdini.py:73-74`)
and usdview's launcher does too (`scripts/usdview_hdue.py:164`).

Conventions: C++ is Google style, 4 spaces, 120 columns (`.clang-format`). Comments explain why, at the density of the
surrounding code. Warnings use `TF_WARN` (see `delegate.cpp:3547` for a once-only warning with `std::call_once`).
Never silence a warning to clean a log.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build delegate (Houdini closed) | `.\scripts\Build-Houdini.ps1` | exit 0; its ctest run passes |
| Build standalone host | `.\scripts\Build-Host.ps1` | exit 0; ctest passes |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp src/host/usd_render.cpp` | no output, exit 0 |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | ends with `Suite unit passed.` |
| Hydra suite (GPU) | `.\scripts\Test.ps1 -Suite hydra` | exit 0 (runs `usd_render` through `Verify.ps1`) |
| Targeted conformance | `& "<houdini_root>\bin\hython.exe" scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only render_settings/*` | `0 failing runs` |

`<houdini_root>` is the `houdini_root` value in `.local/paths.json` (for example
`C:/Program Files/Side Effects Software/Houdini 22.0.368`). husk is `<houdini_root>\bin\husk.exe`.

## Scope

**In scope** (the only files you should modify):
- `src/hydra/delegate.cpp`
- `src/host/usd_render.cpp`
- `docs/architecture.md` (line 71 only)

**Out of scope** (do NOT touch):
- `src/client/backend.cpp` — loading itself is sound (absolute path, safe search flags); the problem is only who picks
  the path.
- `integrations/houdini/python3.13libs/*.py`, `scripts/usdview_hdue.py` — they already use `HDUE_BACKEND_DLL`.
- Any other setting. Other scene-driven risks (UE properties, console variables) are plan 019; do not mix them in.
- `include/hdue/bridge.h` — no ABI change is needed.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, `main` (AGENTS.md: `main` is the only branch).
- One commit per step that changes code. Subject: a plain English sentence describing the result, as in this repo's
  log (examples: "Logging in the Test backend; the viewport lighting that restarts hdUE", "UE's Saved folder in local
  app data; the menu opens the versioned guide"). Body: why, and how it was verified. Last line:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Do NOT push unless the operator told you to. Never push to Epic's or the upstream fork's repositories.

## Steps

### Step 1: Confirm the scene path reaches the constructor (before changing code)

This proves the problem on this machine. Use a scratch folder outside the repo, e.g. `$env:TEMP\hdue-plan001`.
Write `probe.usda` there:

```usda
#usda 1.0
(
    subLayers = [@H:/github/hdUE/samples/conformance/common/stage.usda@]
)

over "Render"
{
    over "Settings"
    {
        string unreal:backendLibrary = "C:/hdue-plan001-does-not-exist/probe.dll"
    }
}
```

Run husk on it with the settings trace on (the profile path is the `runtime_config` value in `.local/paths.json`):

```powershell
$env:HDUE_TRACE_SETTINGS = '1'
$env:HDUE_RUNTIME_CONFIG = 'H:\github\hdUE\.local\runtime-houdini.json'
& "<houdini_root>\bin\husk.exe" --renderer HdUnrealEngine -f 1 -n 1 -o "$env:TEMP\hdue-plan001\probe.exr" "$env:TEMP\hdue-plan001\probe.usda" 2>&1 | Tee-Object "$env:TEMP\hdue-plan001\before.log"
```

**Verify**: `Select-String -Path "$env:TEMP\hdue-plan001\before.log" -Pattern 'initial setting unreal:backendLibrary'`
→ one match, and the log shows a load failure naming `probe.dll` (husk fails the frame). This confirms the scene picks
the DLL. If the setting line is absent, STOP (see STOP conditions).

### Step 2: Read the DLL and the UE project only from the environment and the profile

In `src/hydra/delegate.cpp`, change the two reads:

- `dll`: `TfGetenv("HDUE_BACKEND_DLL")`, else `runtimeConfig("backend_dll")`. Do not read `backendLibrary`.
- the project passed to `startSelfHost`: `runtimeConfig("runtime_project")` only. Do not read `runtimeProject`.
- If the settings map holds `backendLibrary` or `runtimeProject` (check `state_->settings` after the copy loop, so
  `unreal:`-prefixed forms are caught too), emit one `TF_WARN` naming the key and saying it is ignored because the
  backend comes from `HDUE_BACKEND_DLL` or `HDUE_RUNTIME_CONFIG`. A warning, not silence: a user who relied on it must
  learn why.
- Update the error message at line 3609 to "hdUE requires HDUE_BACKEND_DLL or HDUE_RUNTIME_CONFIG".
- Add a one-line comment saying why: scene render settings must not choose a DLL to load (a USD file could run code).

**Verify**: `git grep -n '"backendLibrary"\|"runtimeProject"' -- src/hydra` → only the warning check lines;
the clang-format command above → exit 0.

### Step 3: Make the test host pass the DLL through the environment

In `src/host/usd_render.cpp`, replace `settings[TfToken("backendLibrary")] = VtValue(utf8(options.dll));` with
`TfSetenv("HDUE_BACKEND_DLL", utf8(options.dll));` (from `pxr/base/tf/setenv.h`; add the include if missing). It must
run before the delegate is created. `TfSetenv` updates the C runtime environment that `TfGetenv` reads; do not use
`SetEnvironmentVariableW`, which `getenv` does not see.

**Verify**: `git grep -n backendLibrary -- src` → only the warning check in `delegate.cpp`.
`.\scripts\Build-Host.ps1` → exit 0, ctest passes.

### Step 4: Update the architecture doc

`docs/architecture.md:71` says the path comes from the profile "unless the `backendLibrary` render setting or
`HDUE_BACKEND_DLL` names one". Change it to "unless `HDUE_BACKEND_DLL` names one" and add one sentence: render
settings cannot choose the backend or the UE project, so a scene file cannot load a DLL.

**Verify**: `Select-String -Path docs/architecture.md -Pattern 'backendLibrary'` → no match.

### Step 5: Build and confirm the fix

`.\scripts\Build-Houdini.ps1` (Houdini closed). Then repeat the Step 1 husk command, writing `after.log`.

**Verify**:
- `Select-String -Path "$env:TEMP\hdue-plan001\after.log" -Pattern 'backendLibrary.*ignored'` → one match (the warning).
- husk exits 0 and `$env:TEMP\hdue-plan001\probe.exr` exists (it rendered with the profile's DLL).
- `.\scripts\Test.ps1 -Suite hydra` → exit 0 (the standalone host still finds its DLL).
- Targeted conformance `--only render_settings/*` → `0 failing runs`.
- A Cornell husk render compared with the previous build (AGENTS.md "Verification is tiered") looks the same.

Delete `$env:TEMP\hdue-plan001` afterwards. Never commit the probe scene.

## Test plan

- No unit test can cover the constructor without UE; the regression check is Step 5's husk run with the probe scene.
- `Test.ps1 -Suite hydra` covers `hdue_usd_render` (the only in-repo user of the old setting).
- Targeted conformance `render_settings/*` checks other render settings still reach the delegate.

## Done criteria

- [ ] `git grep -n 'setting<std::string>("backendLibrary"\|setting<std::string>("runtimeProject"' -- src` returns nothing
- [ ] `git grep -n backendLibrary -- src/host` returns nothing
- [ ] `.\scripts\Build-Houdini.ps1` and `.\scripts\Build-Host.ps1` exit 0 with ctest passing
- [ ] Step 5's probe husk run exits 0 and prints the "ignored" warning
- [ ] `.\scripts\Test.ps1 -Suite hydra` exits 0
- [ ] `run_conformance.py --only render_settings/*` reports `0 failing runs`
- [ ] clang-format check exits 0
- [ ] `git status` shows changes only in the three in-scope files (and `plans/README.md`)
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Step 1 shows no `initial setting unreal:backendLibrary` line: husk does not forward the attribute, the threat model
  is different, and the change needs a new decision.
- Any code other than `usd_render.cpp` turns out to rely on `backendLibrary` or `runtimeProject` as a setting (for
  example a test, a `.ds` file or an HDA you find).
- `TfSetenv` is unavailable in the OpenUSD 26.03 build or the Houdini 22 HDK, or `Test.ps1 -Suite hydra` fails to
  find the backend after Step 3.
- The excerpts above do not match the code.

## Maintenance notes

- Rule for the future: no render setting may name a file that hdUE executes or loads as code. Reviewers should reject
  new settings that do.
- `HDUE_BACKEND_DLL` and `HDUE_RUNTIME_CONFIG` are now the only ways to pick the backend. Docs that list ways to
  choose it (`docs/architecture.md`, `README.md` environment table) must stay in step.
- Deferred: signature checks on the backend DLL (Authenticode). Out of scope; the environment is already trusted.
