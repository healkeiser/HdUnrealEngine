# Build, packaging and quality audit

## Scope & sources

Branch `feature/package`, commit `e8fe1f4`. This audit covers `CMakeLists.txt`, `cmake/Houdini.cmake`, `resources/plugInfo.json.in`, `packaging/houdini/*`, `scripts/*`, `tests/*`, `.github/workflows/ci.yml`, `.gitignore`, `.gitattributes`, `README.md`, `docs/*.md`, `VERIFICATION.md`, `THIRD_PARTY_NOTICES.md` and `LICENSE`. I also inspected the built release folder `out/package/hdUE-v0.2.0-houdini22.0-win64` (1.3 GB, 911 MB zipped) and the local Houdini 22.0.368 install.

Checks I ran (I changed no code):
- `git ls-files`, `git status --ignored` and `git ls-files --eol` (repository hygiene and line endings).
- I ran `Install.ps1 -PackagesDirectory <scratch>` under Windows PowerShell 5.1 to see the `hdUE.json` it really writes.
- I compared DLL names and versions between the package and `$HFS/bin`. I also checked `embree4.dll`'s `tbb12.dll` imports against Houdini's `tbb12.dll` exports with `pefile`.
- I ran the Python unit tests from `tests/` outside Houdini with `uv run --python 3.11 --with usd-core --with numpy`.

Reference documentation used:
- SideFX packages: https://www.sidefx.com/docs/houdini/ref/plugins.html
- SideFX Python startup scripts: https://www.sidefx.com/docs/houdini/hom/locations.html
- HDK compatibility: https://www.sidefx.com/docs/hdk/_h_d_k__intro__compatibility.html
- HDK compiling / CMake: https://www.sidefx.com/docs/hdk/_h_d_k__intro__compiling.html
- Microsoft DLL search order: https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order
- oneTBB version information: https://oneapi-spec.uxlfoundation.org/specifications/oneapi/latest/elements/onetbb/source/configuration/version_information
- Redshift Houdini package configuration: https://help.maxon.net/r3d/houdini/en-us/Content/html/Houdini+Plugin+Configuration.html
- Maxon support article on Redshift for Solaris: https://support.maxon.net/hc/en-us/articles/20785973614236-How-to-install-Redshift-for-Solaris-on-Linux-and-MacOS
- SideFX forum thread on plugins across builds: https://www.sidefx.com/forum/topic/49648/
- Arnold answers on HtoA and Houdini builds: https://answers.arnoldrenderer.com/questions/5362/last-htoa-in-a-more-recent-version-of-houdini.html
- Unreal Engine EULA: https://www.unrealengine.com/eula/unreal

## Summary

| ID | Severity | Finding | Effort |
| --- | --- | --- | --- |
| BLD-01 | High | The delegate is built against one Houdini daily build, but the package loads into any 22.0.x build without an ABI check. The repository cannot agree on which build that is (22.0.466 or 22.0.368). | M |
| PKG-01 | High | The release zip redistributes the UE runtime and third-party DLLs without `THIRD_PARTY_NOTICES.md` or any upstream license texts, against the repository's own redistribution rule. | M |
| TST-01 | High | CI runs none of the 23 Python unit tests that already pass outside Houdini with `usd-core`. It also runs none of the 3 C++ unit tests besides the ABI layout test. | S |
| TST-02 | High | `Verify.ps1` writes `verified.json` with a hard-coded `passed = $true` and constant frame counts, and never checks the usdview, camera or buffer suites. | S |
| PKG-02 | Medium | The release ships a Development UE runtime with linker artefacts, a test executable, Vulkan validation layers, `d3d12SDKLayers.dll`, 9 Houdini test modules and 98 development scripts. | M |
| PKG-03 | Medium | UE writes `Saved/` (shader cache, logs, PSO data) inside the package folder. The documented studio install (one shared folder) therefore makes every user and farm process write to the same directory. | M |
| BLD-02 | Medium | There is no release pipeline. The version comes from `git describe --dirty` on a workstation, the CMake version is 0.1.0 while the tag is v0.2.0, the DLLs carry no version resource and no symbols are archived. | M |
| TST-03 | Medium | There is no stored golden-image baseline. `compare_pfm.py` checks one global relative RMSE between two UE paths, and `render_gallery.py` publishes results without thresholds: the IES scene renders almost black (0.00 brightness) and nothing fails. | M |
| TST-04 | Medium | Tests are spread over three directories. 15 scripts are referenced nowhere, and two suites need a private scene. | M |
| PKG-04 | Low | The package `enable` expression has no `houdini_os` or `houdini_python` guard, uses a `'22.0.999'` sentinel, and comes out `'`-escaped. `load_package_once` is not set. | S |
| BLD-03 | Low | Houdini's older `tbb12.dll` (2022.1) is kept for UE's Embree, which ships with 2022.3. All imports resolve today, but nothing guards against a future build. | S |
| BLD-04 | Low | `.gitattributes` forces LF on everything, including `.cmd` files shipped to Windows users. Two files are already CRLF in the worktree. | S |
| TST-05 | Low | CI parses `Install.ps1` only with PowerShell 7, but `Install.cmd` runs it with Windows PowerShell 5.1. The installer is never run in CI. | S |
| TST-06 | Low | The GUI Solaris test depends on exact counts (10 frames, 6 captures) and a 420 s wall clock, and `Verify.ps1` does not include it. | S |
| BLD-05 | Low | CI actions are pinned to mutable tags. | S |
| DOC-01 | Medium | `docs/architecture.md` states base ABI version 7; the code is at 8. | S |
| DOC-02 | Medium | The documented Houdini build disagrees between `building.md`, `VERIFICATION.md`, CMake messages and the machine that built v0.2.0. | S |
| DOC-03 | Low | README lists Displacement as "Not yet", but `rendering.md` documents Preview Surface displacement as supported. | S |
| DOC-04 | Low | The Sponza numbers in `VERIFICATION.md` contradict the README table. | S |
| DOC-05 | Low | `architecture.md` says shared GPU images are a "future optimization", but they ship in the Solaris viewport. | S |
| DOC-06 | Low | `docs/houdini.md:107` contains a mangled log path (`HdUEBackendSavedogs`). | S |
| DOC-07 | Low | The `uiready.py` claim in `docs/houdini.md:15` contradicts SideFX's documentation. | S |
| DOC-08 | Low | The generated Sponza text says "hdUE runs no denoiser", while the Features table lists Open Image Denoise. The two GPU requirement statements also differ. | S |
| PKG-05 | Low | The `HDUE_hduecryptomatte.svg` icon is shipped, but no node or code uses it. | S |

## Findings

### BLD-01 (High): no Houdini build guard for a binary built against one daily build

**What we do**
- `cmake/Houdini.cmake:2` accepts any `find_package(Houdini 22.0)`.
- `cmake/Houdini.cmake:4-7` checks only that `pxr.h` reports USD 26.05.
- `packaging/houdini/Install.ps1:19` enables the package for `houdini_version >= '22.0' and < '22.0.999'`, so for every 22.0 daily build.
- The delegate never compares the Houdini it was compiled against with the Houdini that loads it. Grep finds no `HDK_API_VERSION` or `SYS_Version` use in `src/`.
- The zip name says only `houdini22.0` (`scripts/Package-Houdini.ps1:35`).
- The build that produced `out/package/hdUE-v0.2.0-*` used `.local/paths.json` → `Houdini 22.0.368`. The build directory, CMake messages and docs all say 22.0.466 (`CMakeLists.txt:25`, `cmake/Houdini.cmake:6`, `scripts/Build-Houdini.ps1:7`).

**What the reference says**
- SideFX: "the HDK's ABI can change from time to time, requiring plugins to be recompiled". `HDK_API_VERSION` "only changes when ABI compatibility is broken so that it can be as a signal to rebuild plugins" (https://www.sidefx.com/docs/hdk/_h_d_k__intro__compatibility.html).
- A SideFX developer on loading a plugin in another build: it is possible but "inconsistent between builds" (https://www.sidefx.com/forum/topic/49648/).
- Redshift installs per-build plugin folders selected with `${HOUDINI_VERSION}` in its package (Maxon support article above).
- Arnold users are told to stay on the build HtoA was built against (Arnold answers link above).

**Impact.** A user on a later 22.0 production build with an `HDK_API_VERSION` bump gets crashes or silent misbehaviour inside Houdini, with nothing pointing at the cause. Hydra/USD symbols come from Houdini's `libpxr_*`, so any USD patch in a daily build is also exposed.

**Recommendation**
- Record the Houdini build and `HDK_API_VERSION` at configure time (read `UT_HDKVersion.h` / `SYS_Version.h`) into a generated header and into `runtime.json`.
- At delegate creation, compare the recorded build with the running Houdini (`SYS_Version` at runtime, or `HOUDINI_VERSION` in Python) and print a clear warning on mismatch, or refuse on an `HDK_API_VERSION` mismatch.
- Name the zip `...-houdini22.0.<build>-win64`.
- In the package, either pin `houdini_version >= '22.0.<build>'` or follow Redshift's layout: `plugin/22.0.<build>/` selected by `${HOUDINI_VERSION}`.
- Rename the build directory to `build/houdini` (or derive its name from the detected build) instead of hard-coding `22.0.466` in about 12 places, including `hdue_houdini.py:1,46,55,283`, `hdue_viewer.py:64` and `Launch-Houdini.ps1:32,34`.

### PKG-01 (High): binary release without third-party notices

**What we do**
- `scripts/Package-Houdini.ps1:57` copies only `LICENSE`, with `-ErrorAction SilentlyContinue`.
- The built package has no `THIRD_PARTY_NOTICES.md` (checked: `Test-Path` returns False).
- The package still contains:
  - the UE Development runtime;
  - `embree4.dll`, `tbb12.dll` and `tbbmalloc.dll`;
  - `D3D12Core.dll` and `d3d12SDKLayers.dll`;
  - Ogg and Vorbis, `dbghelp.dll`, NVIDIA Aftermath, Vulkan layers and XAudio2.
- `THIRD_PARTY_NOTICES.md:7` says "This source repository does not bundle those engines", which is true for the repository but not for the zip.
- `THIRD_PARTY_NOTICES.md:35` itself requires that "any release that includes third-party code, binaries, runtimes… include the complete applicable license texts".
- `LICENSE:3` credits only "2026 K240". The fork's own copyright holder is absent.

**What the reference says**
- UE EULA §7(a)/(b) on notices and third-party software supplied with the engine (https://www.unrealengine.com/eula/unreal), as `THIRD_PARTY_NOTICES.md:15` summarises.
- Whether `d3d12SDKLayers.dll` (a debug layer) may be redistributed at all is **unverified**: check the DirectX Agility SDK license.

**Impact.** Each published zip is out of compliance with the project's own policy and with upstream notice requirements. This is a legal and reputational risk for a public GitHub release.

**Recommendation**
- Copy `THIRD_PARTY_NOTICES.md` and a `licenses/` folder (UE `Engine/Source/ThirdParty/*` license files for every staged DLL, plus oneTBB, Embree, Ogg/Vorbis and Aftermath) into the package.
- Make `Package-Houdini.ps1` fail when any of these files is missing (drop `SilentlyContinue`).
- Add the fork author to `LICENSE`.

### TST-01 (High): CI skips tests that already run without Houdini

**What we do**
- `ci.yml:36` runs only `tests/test_repository_scope.py`.
- `ci.yml:21` configures with `-DHDUE_BUILD_HYDRA=OFF`, so `ctest` runs `abi_c_layout` only (`CMakeLists.txt:84-88`).
- I ran these unchanged with `uv run --python 3.11 --with usd-core --with numpy python tests/<name>.py`, and all passed:
  - `substrate_live_test`: 4 tests
  - `scene_space_test`: 2
  - `substrate_graph_test`: 10
  - `surface_effects_test`: 4
  - `asset_controls_test`: 3
- `scene_space_test` and `asset_controls_test` currently run only through `hython` in `Test-Houdini.ps1:14-17`.
- `tests/picking.cpp` needs only `gf` (`CMakeLists.txt:65-67`), and `geom_subsets.cpp`/`render_buffer.cpp` need only `hd`.

**What the reference says.** Not applicable (internal practice). README:264 describes CI accurately but narrowly.

**Impact.** About 23 deterministic checks of the substrate graph, coordinate conversion and proxy logic only run when a developer launches the Houdini suites by hand.

**Recommendation**
- Add a CI step: `uv run --python 3.11 --with usd-core --with numpy python -m unittest discover -s tests -p "*_test.py"`. Exclude the Houdini-only modules, or have them `skipUnless` `hou` can be imported.
- Medium-term, cache an OpenUSD 26.03 build (or a prebuilt USD SDK) on the runner so the 3 C++ contract tests build and run in CI too.

### TST-02 (High): `Verify.ps1` evidence is partly hard-coded

**What we do**
- `scripts/Verify.ps1:33-44` writes `passed = $true`, `native_frames = 9`, `hydra_frames = 34` and `path_tracing_samples = 64` as literals, not values read from `metrics.json`.
- `Verify.ps1:29-31` runs `Test-Usdview.ps1`, `Test-Camera.ps1` and `Test-BufferPerf.ps1` but never checks `$LASTEXITCODE` or their `verified.json`. Only the `compare_pfm` exit code is checked (`:27`).
- `Verify.ps1:17-25` redirects each step with `*>`. This works only because each child throws; a native non-zero exit inside a child that does not throw would go unnoticed.

**What the reference says.** `VERIFICATION.md:9`: "Exit code zero is insufficient." The script contradicts the policy it documents.

**Impact.** A `verified.json` that reads "passed" can exist after a usdview regression. It also cannot be trusted as evidence across commits.

**Recommendation**
- Read the counts from the produced metrics files.
- Assert each sub-suite's `verified.json` the way `Test-Houdini.ps1:21-25` does.
- Derive `passed` from those checks, and fail on `$LASTEXITCODE` after each call.

### PKG-02 (Medium): release contains development and test payload

**What we do**
- `scripts/Build-Backend.ps1:18` builds `Development`, and `Stage-Runtime.ps1:8` stages `-clientconfig=Development`.
- The package `Binaries/Win64` contains `HdUEBackend.lib`, `HdUEBackend.exp` and `hdue_backend_smoke.exe` (copied by `Stage-Runtime.ps1:26`).
- `Engine/Binaries/ThirdParty/Vulkan/*VkLayer_*` (the renderer is D3D12-only) and `d3d12SDKLayers.dll` are included.
- `Package-Houdini.ps1:41` copies all of `integrations/houdini`, including 9 `hdue_*_test.py` modules and `hdue_solaris_probe.py`, onto every user's `HOUDINI_PATH`.
- `Package-Houdini.ps1:42` copies every `scripts/*.py` and `*.ps1` (98 files). Only the modules imported through `sys.path.insert(0, ROOT/'scripts')` are needed at runtime.
- The robocopy calls at `:41-43` do not check `$LASTEXITCODE` (only `:50` does).

**Impact**
- The package is larger than it needs to be.
- Test modules are importable in production sessions.
- A Development runtime exposes the console and debug cvars, and runs with extra checks.
- A failed copy of `integrations/` or `scripts/` still produces a "Package ready" zip.

**Recommendation**
- Stage a `Test` or `Shipping` client config for releases, or document why Development is required (for example console variables used by `HDUE_UE_ARGS`).
- Exclude `*.lib`, `*.exp`, `hdue_backend_smoke.exe`, `VkLayer_*` and `d3d12SDKLayers.dll`.
- Keep the test modules out of the package (move them to `tests/houdini/` and add them to `HOUDINI_PATH` only in `Launch-Houdini.ps1`).
- Copy an explicit allow-list of runtime scripts.
- Check robocopy's `$LASTEXITCODE -ge 8` after every call.

### PKG-03 (Medium): shared install writes UE state into the package folder

**What we do**
- README:207 and `packaging/houdini/README.txt:7-8` say UE writes its shader cache and logs into `runtime\Windows\HdUEBackend\Saved`.
- README:211 and `docs/houdini.md:13` recommend a studio-wide `-PackagesDirectory <shared HOUDINI_PACKAGE_DIR>`, which points every machine at one `$HDUE` folder.
- `src/` contains no `-userdir`/`Saved` redirection (grep finds none for `userdir|SavedDir|LOCALAPPDATA`).
- The Houdini host's default log directory is also inside the package (`hdue_houdini.py:46`, `ROOT/'out/houdini-22.0.466/session'`).

**Impact.** On a read-only or shared network install, UE either fails to write or many workstations and farm husk processes write logs, PSO caches and DDC into the same tree. That causes contention, corrupted caches and logs nobody can attribute.

**Recommendation**
- Pass `-userdir=%LOCALAPPDATA%/hdUE/<version>` (or an `HDUE_USER_DIR` override) on the UE command line built in `src/hydra/delegate.cpp`, so `Saved/` is per user and per machine.
- Default `HDUE_HOUDINI_LOG_DIR` to `$HOUDINI_TEMP_DIR`.
- Then state that the package folder may be read-only.

### BLD-02 (Medium): versioning and release process

**What we do**
- `CMakeLists.txt:2` has `VERSION 0.1.0`; the tags are `v0.1.0` and `v0.2.0`.
- The package version is `git describe --tags --always --dirty` on the developer machine (`Package-Houdini.ps1:33`), with no check against a dirty tree.
- No `.rc` VERSIONINFO exists for `hdUE_Houdini22.dll` or the backend.
- `runtime.json` has `version=1` but no hdUE version.
- `ci.yml:6` triggers on `v*` tags but has no release job.
- PDBs are deliberately excluded (`Package-Houdini.ps1:49`, `docs/houdini.md:11`) and not archived anywhere.
- I could not see the GitHub Releases page (gh returned nothing): **unverified** whether a v0.2.0 zip is published, although README:206 links to it.

**Impact**
- A bug report cannot be tied to an exact build: there is no version in the DLL properties, `hdUE.json` or the UI.
- A crash dump from a user cannot be symbolised.

**Recommendation**
- Make `project(VERSION)` the single source and feed it into a VERSIONINFO resource and into `runtime.json`/`hdUE.json` (`"hdue_version"`).
- Refuse `-dirty` in `Package-Houdini.ps1` unless `-AllowDirty` is passed.
- Archive the PDBs (delegate and backend) as a separate `-symbols.zip` per release.
- Add a manual release checklist to `docs/building.md`; GitHub runners cannot build UE, so the release itself stays local.

### TST-03 (Medium): image regression without baselines

**What we do**
- `scripts/compare_pfm.py:24,30-33` compares native UE with Hydra UE from the same run, using one global relative RMSE ≤ 1%. There is no per-pixel or max-error check, no stored baseline and no tiled metric.
- `scripts/render_gallery.py:333-345` computes brightness against Karma but has no thresholds; it only writes docs.
- The published table shows `IES profile | 0.00 / 0.00` (README:92). `docs/images/gallery/ies.png` is nearly black in both hdUE modes while Karma is fully lit, and nothing failed.

**Impact**
- Global RMSE between two UE paths cannot catch a regression shared by both (lighting units, tonemapping, materials).
- Localised errors under 1% of total energy pass.
- The gallery is the only cross-renderer check, and it is advisory.

**Recommendation**
- Store a small set of 64-spp low-resolution reference PFM/EXR files per scene, in Git LFS or as a release asset.
- Compare against them with a tiled metric (per-tile relative error, plus a percentile cap such as FLIP or SSIM).
- Give `render_gallery.py` a `--check` mode that fails when a brightness ratio leaves a per-scene band (for example Path Tracer 0.9–1.1 except scenes marked experimental), so a 0.00 ratio fails loudly.

### TST-04 (Medium): script sprawl and unreferenced files

**What we do**
- Tests live in `tests/` (13), in `scripts/*_test.py` (8: `asset_test`, `glass_test`, `materialx_test`, `opacity_test`, `residency_test`, `texture_test`, `usdview_test`, `test_usd_materialx_export`) and in `integrations/houdini/python3.13libs/hdue_*_test.py` (9).
- There are 15 `Test-*.ps1` wrappers.
- These are referenced by no file, doc or script (`git grep` on file and module name):
  - `Test-Assets.ps1`, `Test-Scene.ps1` and `Test-Textures.ps1`;
  - 8 generators: `generate_asset_scene`, `generate_inline_materialx_stage`, `generate_lighting_stage`, `generate_materialx_samples`, `generate_materialx_stage`, `generate_residency_stage`, `generate_substrate_sample` and `generate_wet_stage`;
  - `houdini_capabilities.py`, `houdini_probe.py`, `preview_pfm.py` and `test_usd_materialx_export.py`.
- `Test-Opacity.ps1` and `Test-Glass.ps1` need the private `review_scene` (`docs/building.md:41`), so nobody else can reproduce them.
- `create_asset_gallery.py`, `create_glass_gallery.py` and `create_opacity_gallery.py` overlap with `render_gallery.py`.
- Three near-identical 2.7 MB generated USD files are committed (`samples/materialx/inline.usda`, `showcase.usda`, `showcase_solaris.usda`).

**Recommendation**
- Move every test into `tests/`, split into `unit/` (pure Python and C++), `hydra/`, `houdini/` (hython) and `gui/`.
- Have one `scripts/Test.ps1 -Suite unit|hydra|houdini|gui|all` replace the 15 wrappers.
- Delete the unreferenced scripts, or document each generator next to the sample it produces (a header comment `# generates samples/...`).
- Fold the `create_*_gallery.py` scripts into `render_gallery.py --set`.

### PKG-04 (Low): package `enable` expression and keywords

**What we do.** `Install.ps1:19` writes `houdini_version >= '22.0' and houdini_version < '22.0.999'`. Under Windows PowerShell 5.1 (`Install.cmd:2` calls `powershell`), `ConvertTo-Json` produces:

```
"enable":  "houdini_version >= '22.0' and houdini_version < '22.0.999'"
```

This is valid JSON but unreadable for admins who audit package files. The expression also has no `houdini_os == 'windows'` check, no `load_package_once`, and `Install.cmd` ends with `pause`, which blocks scripted or remote deployment.

**What the reference says**
- "The expression language supports string values only"; the available variables include `houdini_os` and `houdini_python`.
- `load_package_once` makes Houdini "ignore any other packages with the same file name in the search path" (https://www.sidefx.com/docs/houdini/ref/plugins.html).

**Recommendation**
- Use `houdini_version >= '22.0.<build>' and houdini_version < '22.1' and houdini_os == 'windows'`.
- Set `"load_package_once": true`.
- Write the JSON with `ConvertTo-Json | ForEach-Object { [regex]::Unescape($_) }`, or from a template.
- Add `-NoPause` (or drop `pause` when `%1` is given).

### BLD-03 (Low): `tbb12.dll` shared between Houdini and UE

**What we do.** `Bridge.cpp:277-291` preloads `tbb12.dll` and `embree4.dll` from the backend folder by full path, but keeps an already loaded `tbb12` ("as Houdini does"). Houdini 22.0.368 ships `tbb12.dll` 2022.1.0; the package ships 2022.3.0. I checked with `pefile`: all 29 `tbb12` imports of `embree4.dll` resolve in Houdini's 2022.1 DLL, so this works today. Backend loading itself is correct: full path plus `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | DEFAULT_DIRS` (`src/client/backend.cpp:7-8`).

**What the reference says**
- Microsoft: the loader checks the "Loaded-module list… a DLL with the same module name is already loaded into memory (no matter which folder it was loaded from)" (Microsoft link above).
- oneTBB: "the run-time value must be greater than or equal to the compile-time value of TBB_INTERFACE_VERSION. Otherwise, the application may fail to resolve all symbols" (oneTBB link above).

**Impact.** A Houdini build with an older TBB, or a UE update whose Embree uses newer TBB entry points, would make `embree4.dll` fail to load. The log would then show a warning, and Nanite and Lumen cards would silently disappear. `GetModuleFileNameW` into `MAX_PATH` (`Bridge.cpp:283-284`) also truncates for deep install paths.

**Recommendation**
- Add a packaging-time check (the same `pefile` comparison) that fails when a staged UE DLL imports a symbol missing from the matching `$HFS/bin` DLL.
- Log `TBB_runtime_interface_version()` at startup.
- Use a long-path-safe buffer.

### BLD-04 (Low): line endings

**What we do**
- `.gitattributes:1` is `* text=auto eol=lf`, so `packaging/houdini/Install.cmd` and `Uninstall.cmd` are LF in the index and in the zip.
- `git ls-files --eol` shows `scripts/Test-RenderSettings.ps1` and `samples/cornell/beam.ies` as `i/lf w/crlf`, so they will show as modified after a renormalise.
- HDA and VDB files are correctly auto-detected as binary today, but only by heuristic.

**Recommendation.** Add `*.cmd text eol=crlf`, `*.bat text eol=crlf`, `*.hda binary`, `*.vdb binary` and `*.ies text`, then run `git add --renormalize .`.

### TST-05 (Low): installer not exercised in CI

**What we do.** `ci.yml:39-48` parses with `pwsh` (7.x), but `Install.cmd:2` runs `powershell` (5.1), where `ConvertTo-Json` and the encoding defaults differ (see PKG-04). No CI step creates a fake package folder and runs `Install.ps1`/`Uninstall.ps1`.

**Recommendation.** Add a CI step that:
- creates the three required files;
- runs `powershell -File Install.ps1 -PackagesDirectory $RUNNER_TEMP`;
- validates the JSON keys (`enable`, `hpath`, `PXR_PLUGINPATH_NAME.method == 'append'`);
- runs `Uninstall.ps1`.

### TST-06 (Low): GUI test brittleness

**What we do.** `Test-Houdini.ps1:23` requires exactly `frames.Count -eq 10` and `captures.Count -eq 6`. `Launch-Houdini.ps1:69` waits a fixed 420 s. The suite needs an interactive desktop session and is not part of `Verify.ps1`.

**Recommendation.** Assert minimums and named states instead of exact counts. Make the timeout a parameter. Add `-WithHoudini` to `Verify.ps1` so one command covers every suite.

### BLD-05 (Low): CI hygiene

**What we do**
- `actions/checkout@v7` and `astral-sh/setup-uv@v10.0.1` are pinned to tags. Both versions exist: checked via the GitHub API, the latest releases are v7.0.1 and v10.3.3.
- `.ruff_cache/` is ignored only by its own inner `.gitignore`.
- There is no `pyproject.toml` or lock file, so script dependencies (PEP 723 headers) are resolved freshly on every `uv run` (`ci.yml:31` comment).

**Recommendation.** Pin actions by commit SHA (Dependabot can bump them). Add `.ruff_cache/` to `.gitignore`. Consider a minimal `pyproject.toml` with a `dev` dependency group (`usd-core`, `numpy`) for TST-01.

### DOC-01 (Medium): ABI version is wrong in the docs

`docs/architecture.md:28` says "`HDUE_ABI_VERSION` 7" and describes 7 as the latest change. `include/hdue/bridge.h:22` is `#define HDUE_ABI_VERSION 8u` (raised in `6aa4919`, "Indexed meshes, runtime MaterialX…"). The "96 bytes" function table claim is correct (`tests/abi_layout.c:38`). **Recommendation:** update the docs and list what version 8 changed. Better, generate the version line from the header, or have the ABI test assert the documented value.

### DOC-02 (Medium): which Houdini build is supported

The repository gives four different answers:
- `docs/building.md:11`: "upstream verified 22.0.466, and the features in this fork were tested on 22.0.368".
- `VERIFICATION.md:7`: "Houdini 22.0.466".
- `cmake/Houdini.cmake:6`: "Houdini 22.0.466 is expected".
- The machine that built v0.2.0 uses 22.0.368 (`.local/paths.json`).

README:143 says only "Houdini versions other than 22.0". **Recommendation:** add one "Supported Houdini builds" table, with the build each release was compiled against and the builds it was tested on (see BLD-01).

### DOC-03 (Low): Displacement listed as "Not yet" while documented as supported

README:145 lists "Displacement" under Not yet. `docs/rendering.md:60,115-117` documents Preview Surface `displacement` as World Position Offset, which the code supports: `src/hydra/materials.cpp:190` reads it and `src/hydra/delegate.cpp:449-453` converts it. Meanwhile `rendering.md:151` says "displacement is not supported" (meaning on subdivided meshes). **Recommendation:** change the README entry to "true/adaptive displacement (WPO along normals only)", and reword `rendering.md:151`.

### DOC-04 (Low): Sponza numbers disagree

`VERIFICATION.md:102` says "Path Tracer took 2.4 s against 84 s for Karma XPU and 405 s for Karma CPU, at 1.03x… (XPU 1.04x, Lumen 0.79x)". The generated README:131 says 3.3 s, 24.7 s and 508.3 s, with brightness 0.83 / 0.96. **Recommendation:** have `VERIFICATION.md` link to the generated tables instead of restating numbers.

### DOC-05 (Low): GPU frames described as future work

`docs/architecture.md:32` says "Shared GPU images remain a future optimization". `docs/houdini.md:107` says frames "come from GPU readback as before". But `docs/houdini.md:46`, `include/hdue/gpu_api.h` and `src/hydra/gpu_frames.cpp` ship shared D3D12→GL frames for the Solaris viewport, enabled by default (`delegate.cpp:1646`). **Recommendation:** update both sentences.

### DOC-06 (Low): mangled path

`docs/houdini.md:107` contains `` `HdUEBackendSavedogs` `` (confirmed with `od -c`). The backslash sequences `\S` and `\L` were lost. It should read `HdUEBackend\Saved\Logs`, as `rendering.md:177` has it.

### DOC-07 (Low): `uiready.py` claim

`docs/houdini.md:15` says the package's `uiready.py` "replaces Houdini's own". SideFX documents `pythonX.Ylibs/uiready.py` as: "Houdini will find any files matching this pattern in the Houdini path and run them after the UI is ready". Only `123.py` is first-match-only (https://www.sidefx.com/docs/houdini/hom/locations.html). **I did not confirm the actual behaviour empirically.** **Recommendation:** correct the sentence, or verify in Houdini 22 and cite the result.

### DOC-08 (Low): denoiser and GPU requirement wording

- The README Sponza block (generated by `scripts/render_gallery.py:312`) says "hdUE runs no denoiser". README:150 lists "Open Image Denoise for the Path Tracer", which is opt-in and built only into the Houdini delegate (`src/hydra/delegate.cpp:39-40`). **Recommendation:** say "the gallery renders without hdUE's optional OIDN denoiser", and note that OIDN is Houdini-build only.
- README:204 requires "a DirectX 12 GPU with hardware ray tracing". `packaging/houdini/README.txt:20` says "SM6 (ray tracing for the Path Tracer mode)". **Recommendation:** make the two match.

### PKG-05 (Low): orphan icon

`integrations/houdini/config/Icons/HDUE_hduecryptomatte.svg` is tracked and shipped, but no HDA, shelf tool or code references cryptomatte, and README:150 lists cryptomatte as "Not yet". **Recommendation:** remove it until a cryptomatte node exists.

## Documentation spot-check

| # | Claim | Location | Verdict |
| --- | --- | --- | --- |
| 1 | The nine env vars in the README table exist and behave as described | README:239-247 | Correct: `HDUE_GPU_FRAMES` (`delegate.cpp:1646`), `HDUE_TRACE_PASS` (`delegate.cpp:1314`), etc. Undocumented in the README: `HDUE_NANITE`, `HDUE_HOUDINI_UI`, `HDUE_MATERIALX_LIBRARY`, `HDUE_SUBSTRATE_LIBRARY` |
| 2 | "The package file sets the first three" | README:235 | Correct (`Install.ps1:22-24`) |
| 3 | `HDUE_ABI_VERSION` 7 | architecture.md:28 | **Wrong**: 8 (DOC-01) |
| 4 | Function table is 96 bytes | architecture.md:28 | Correct (`abi_layout.c:38`) |
| 5 | Displacement is "Not yet" | README:145 | **Contradicted** by rendering.md:115 (DOC-03) |
| 6 | Cryptomatte is "Not yet" | README:150 | Correct, but an orphan icon ships (PKG-05) |
| 7 | OIDN for the Path Tracer | README:150 | Houdini build only (DOC-08) |
| 8 | CI contents | README:264 | Correct (`ci.yml`); omits `tests/local_paths.ps1` |
| 9 | Package about 1.2 GB unpacked, 0.9 GB zipped | houdini.md:11 | Correct: measured 1.3 GB / 911 MB |
| 10 | Package JSON sets these vars, appends `PXR_PLUGINPATH_NAME`, adds to `HOUDINI_PATH` | houdini.md:13 | Correct (`Install.ps1:20-27`; output checked) |
| 11 | `uiready.py` replaces Houdini's own | houdini.md:15 | **Contradicts the SideFX docs** (DOC-07) |
| 12 | UE log path | houdini.md:107 | **Mangled** (DOC-06) |
| 13 | Houdini build tested | building.md:11, VERIFICATION.md:7 | **Inconsistent** (DOC-02) |
| 14 | Four native tests per SDK build | VERIFICATION.md:13 | Correct (`CMakeLists.txt:59-88`, `Houdini.cmake:36-55`) |
| 15 | Sponza timings | VERIFICATION.md:102 | **Contradicts** README:131 (DOC-04) |
| 16 | Shared GPU images are future work | architecture.md:32 | **Stale** (DOC-05) |
| 17 | `HDUE_VIEWPORT_ID_AOVS=1` re-enables viewport IDs | houdini.md:49 | Correct (`delegate.cpp`) |
| 18 | MSVC/UE pin `f45729c…` | building.md:5 | Consistent with `Verify.ps1:40` |

## What is already right

- **ABI isolation.** The standalone build (OpenUSD 26.03) and the Houdini build (USD 26.05) are separate CMake paths. Each pins its USD version with a hard `FATAL_ERROR` (`CMakeLists.txt:39-41`, `Houdini.cmake:4-7`). The DLL names differ (`hdUE.dll` and `hdUE_Houdini22.dll`), and the plugin names differ (`hdUE` and `hdUEHoudini22`), so both can coexist on `PXR_PLUGINPATH_NAME`.
- **Uses SideFX's own CMake config.** The build uses `find_package(Houdini)` from `$HFS/toolkit/cmake` and links the `Houdini` imported target (`Houdini.cmake:2-9`), as the HDK documents (https://www.sidefx.com/docs/hdk/_h_d_k__intro__compiling.html).
- **`plugInfo.json` is correct and relocatable.** `Root: ".."` with a relative `LibraryPath`, `bases: ["HdRendererPlugin"]`, `displayName` and `priority`. The implicit-surface scene index is scoped with `loadWithRenderer` (`resources/plugInfo.json.in`).
- **The package uses current keywords.** It uses `hpath` (not the deprecated `path`), appends to `PXR_PLUGINPATH_NAME` instead of replacing Houdini's value, and gates `UsdRenderers.json` and `uiready.py` on `HDUE_HOUDINI_ENABLE`. `HOUDINI_USER_PREF_DIR` with `__HVER__` is honoured (`Install.ps1:13`).
- **Safe DLL loading.** The backend loads by absolute path with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, and is never unloaded (`src/client/backend.cpp:7-18`). Python uses `winmode=0x1100`. Every C ABI extension is negotiated by version and struct size.
- **No conflicting OpenImageDenoise.** The UE runtime stages no OIDN, so the Houdini delegate uses Houdini's OIDN only.
- **Repository hygiene.**
  - `out/`, `build/`, `.local/`, `unreal/*/Intermediate|Binaries|Saved|DerivedDataCache|Content` and `__pycache__` are all ignored, and none of them is tracked (`git ls-files` holds 341 files, no `.pyc`).
  - `test_repository_scope.py` guards against machine paths and private content.
  - Local paths flow through one resolver (`Local-Paths.ps1`), which is itself tested (`tests/local_paths.ps1`).
- **Real C contract tests.** There are static layout assertions for every extension struct (`tests/abi_layout.c`), plus focused unit tests for the render buffer, picking BVH and GeomSubsets.
- **Honest scope statements.** `VERIFICATION.md` and `docs/houdini.md:40,119` say clearly what is untested (click picking, the File COP path, running Live Render and the viewport at the same time).
