# Over-engineering audit (ponytail)

Scope: tracked code, scripts, tests and build files only (no docs, samples or generated .hda contents). Date: 2026-10-29. Read-only: nothing below has been applied. Every "unused" claim was checked with `git grep` across the repo, including docs, CI and other scripts.

## C++ delegate

- `yagni:` 12 extension tables, each separately versioned (`hdue_get_{aov,asset,depth,gpu,lighting,material,motion,native_pick,postprocess,residency,surface,volume}_api`). Each has its own ABI version, `struct_size`/capacity handshake and `GetProcAddress` probe. The delegate and backend ship as a pair and `HDUE_ABI_VERSION` is already 8. Fold the function pointers into `HdueApi` behind the single version check. If they stay separate, one `template<class T> bool load(const char* name, uint32_t version, T& out)` replaces the 12 copy-pasted probe blocks. [src/client/backend.cpp:19] (-75)
- `yagni:` per-extension boilerplate in 11 headers (export macro, `Hdue*Fn` typedef, `*_ABI_VERSION`, `struct_size`/`abi_version` header fields). This goes away with the fold above. [include/hdue/aov_api.h:18, asset_api.h:34, depth_api.h:19, gpu_api.h:51, lighting_api.h:46, material_api.h:51, motion_api.h:22, postprocess_api.h:35, residency_api.h:36, surface_api.h:37, volume_api.h:31] (-35)
- `native:` `HDUE_TRACE_TIMING`, `_PASS`, `_SCENE` and `_SETTINGS` are hand-rolled env switches, each with its own `static TfGetenvBool`. Use USD `TF_DEBUG_CODES(HDUE_TIMING, HDUE_PASS, HDUE_SCENE, HDUE_SETTINGS)` with `TF_DEBUG(...).Msg()`, enabled via `TF_DEBUG=HDUE_*`. Keep `HDUE_TRACE_FILE` only for Houdini's missing stderr. [src/hydra/delegate.cpp:57, 1062, 1314, 1344, 1478, 1693, 1741; src/hydra/prims.cpp:363, 755] (-15)
- `native:` five copies of the "find key, check `IsHolding<T>`, else fallback" helper. Use USD `VtDictionaryGet<T>(dict, key, VtDefault = fallback)` for `HdUEWetValue`, `surfaceField` and `stat`. Use `VtValue::GetWithDefault<T>(fallback)` for the `prims.cpp` `value()`. [src/hydra/surface_settings.h:8; src/host/surface_settings.h:8; src/host/usd_render.cpp:115; src/hydra/prims.cpp:44] (-15)
- `delete:` `HdUETraceThread`, the `HDUE_TRACE_THREADS` env var and its `std::set` dedup. It is exported with `HDUE_HYDRA_API`, but nothing outside `delegate.cpp` calls it and nothing sets or documents the variable. [src/hydra/delegate.cpp:67; src/hydra/delegate.h:225] (-12)
- `yagni:` `hdue_resize_session` is a lone export outside every table and is looked up with `GetProcAddress` on each call. Put it in `HdueApi` and drop the `HDUE_INVALID_STATE` fallback branch. [src/client/backend.cpp:128] (-5)

## UE backend

- `yagni:` 12 near-identical `hdue_get_*_api` exports that check version, check capacity and fill a struct. They fold into the one `hdue_get_api` table with the first C++ delegate finding. [unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp:1197-1290] (-70)

## Houdini integration

- `shrink:` the 30-line `if/elif` chain mapping `HDUE_HOUDINI_TEST` to test modules. Shorter form: `importlib.import_module({'substrate-live':'hdue_substrate_live_test', ..., 'viewport':'hdue_solaris_probe'}.get(test,'hdue_houdini_test')).start(_host)`. [integrations/houdini/python3.13libs/hdue_houdini.py:249] (-25)
- `yagni:` `getattr(self.library,'hdue_run_ui_tasks',None)` and `getattr(...,'hdue_set_viewport_surfaces',None)`, with the `if` guards after them. Both are always exported by the same DLL (`delegate.cpp:1985`, `diagnostics.cpp:24`). Bind them directly, like the other five exports. [integrations/houdini/python3.13libs/hdue_viewport.py:43, 51] (-3)
- `yagni:` `-Test` and `-TestKind 'integration'` are two flags for one choice. Use `-TestKind ''` (empty means no test). [scripts/Launch-Houdini.ps1:10, 53] (-2)
- `yagni:` nine in-Houdini test/probe modules (`hdue_*_test.py`, `hdue_solaris_probe.py`, about 1,000 lines) live in `python3.13libs`, which the package ships to users. Move them to `tests/houdini/` and put that folder on `HOUDINI_PATH` only under `-Test`. [integrations/houdini/python3.13libs/; scripts/Package-Houdini.ps1:54] (0 repo lines; -1,000 shipped)

## Scripts

- `yagni:` nine fixture generators that no script, CI job or runner invokes. Their outputs are already committed under `samples/`. This one is a judgement call: delete them, or keep them only if fixtures are still regenerated (git history keeps them). Files: `generate_asset_scene`, `generate_inline_materialx_stage`, `generate_lighting_stage`, `generate_materialx_samples`, `generate_materialx_stage`, `generate_normal_udim_stage`, `generate_residency_stage`, `generate_substrate_sample`, `generate_wet_stage`. [scripts/generate_*.py] (-649)
- `delete:` `test_usd_materialx_export.py`. It is a 166-line test that no runner, CI job or `Verify.ps1` executes; its only mention is in `docs/audit/05`. Wire it into CI or delete it. [scripts/test_usd_materialx_export.py:1] (-166)
- `shrink:` nine usdview wrappers with the same 10-line skeleton: fresh timestamped dir, `Launch-Usdview.ps1 --hdue-X-check`, read `verified.json`, re-assert counts, grep the log. Files: `Test-Glass`, `Test-Opacity`, `Test-Assets`, `Test-Textures`, `Test-Scene`, `Test-Residency`, `Test-MaterialX`, `Test-Camera`, `Test-Usdview`. Replace them with one `Test-Usdview.ps1 -Check glass|opacity|...`. The count assertions belong in each Python check's `passed`, which those checks already compute. [scripts/Test-Glass.ps1:1 and siblings] (-120)
- `shrink:` 20 copies of the run-directory boilerplate: `$Run=if ($OutputDirectory) {...} else {Join-Path $TaskRoot ('out\x\'+(Get-Date -Format ...))}`, then `if (Test-Path) {throw}` and `New-Item`. Replace with one `New-HdUERun -Name x -Path $OutputDirectory` in the existing `Local-Paths.ps1`. [scripts/Local-Paths.ps1:1] (-40)
- `delete:` `preview_pfm.py`, a one-off viewer that nothing calls. It is the only user of Pillow. [scripts/preview_pfm.py:1] (-27, -1 dep: pillow)
- `shrink:` `Prepare-SubstrateLive.ps1` re-implements `Prepare-Substrate.ps1 -GraphJob`: the `export-job` call, the `HdUESubstrate` commandlet loop over `export.json` and the log checks. Keep only the mutex and progress wrapper, and have it call `Prepare-Substrate.ps1 -GraphJob -SkipBuild`. [scripts/Prepare-SubstrateLive.ps1:1] (-25)
- `shrink:` 8 boolean `--hdue-*-check` flags plus an 8-way `if/elif` in `LaunchProcess`. Replace with `parser.add_argument('--hdue-check', choices=CHECKS)` and one `{name: (module, class)}` lookup. [scripts/usdview_hdue.py:56-71, 295-325] (-20)
- `native:` `Copy-ProjectContent.ps1` hand-rolls a collision preflight and "copy only new files" loop. Use `robocopy $Src $Dst /E /XC /XN /XO /L` for the dry run, then the same without `/L`. robocopy never overwrites with those flags, and its log is the receipt. [scripts/Copy-ProjectContent.ps1:14-35] (-20)
- `delete:` `houdini_capabilities.py` and `houdini_probe.py`, one-off HOM capability probes that nothing references. [scripts/houdini_capabilities.py:1; scripts/houdini_probe.py:1] (-20)
- `native:` `Build-Host.ps1` and `Build-Houdini.ps1` are configure, build and ctest wrappers. Replace them with `CMakePresets.json` `configurePresets`, `buildPresets`, `testPresets` and `workflowPresets` (`cmake --workflow --preset houdini`). [scripts/Build-Host.ps1:1; scripts/Build-Houdini.ps1:1] (-15)
- `shrink:` three hand-rolled PFM readers. Share one: `np.fromfile(f, dtype, offset=header_len).reshape(h,w,3)` in `compare_pfm.read`, and import it from the test. [scripts/compare_pfm.py:12; tests/substrate_texture_render.py:63] (-10)
- `shrink:` the `Build.bat HdUEBackendEditor Win64 Development -Project=... -gather -WaitMutex -NoHotReload` line is copied in four scripts. Use one `Build-HdUEEditor` function in `Local-Paths.ps1`. [scripts/Import-MaterialX.ps1:22; Prepare-Content.ps1:11; Prepare-ProjectAssets.ps1:23; Prepare-Substrate.ps1:28] (-6)
- `yagni:` `[ValidateSet('Release')][string]$Configuration` is a parameter with one allowed value. Hard-code `Release`. [scripts/Build-Host.ps1:2] (-3)
- `yagni:` `Package-Houdini.ps1` robocopies every `scripts/*.py *.ps1` into the user package, about 100 files including `Test-*`, generators and gallery tools. Whitelist the modules that the runtime imports. [scripts/Package-Houdini.ps1:55] (0 repo lines)

## Tests and build

- `delete:` `tests/substrate_vop_test.py`, a 187-line unittest that no script, CI job or doc runs. Add it to `Test-Houdini.ps1` or delete it. [tests/substrate_vop_test.py:1] (-187)
- `delete:` `tests/houdini_baseline.py`, a "UI-only Storm baseline" that nothing references. [tests/houdini_baseline.py:1] (-40)
- `shrink:` `Test-Houdini.ps1` runs four hython calls, each with its own `if ($LASTEXITCODE) {throw}`. Shorter form: `foreach ($t in 'houdini_cook','asset_controls_test','scene_space_test','render_outputs_test') { & uv run -- $Hython "$TaskRoot\tests\$t.py"; if ($LASTEXITCODE) {throw "$t failed"} }`. [scripts/Test-Houdini.ps1:12-19] (-5)

net: -1630 lines, -1 deps possible.
