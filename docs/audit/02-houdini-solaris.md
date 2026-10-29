# Houdini / Solaris integration audit

## Scope & sources

Branch `feature/package` at `e8fe1f4`. Read: `README.md`, `docs/houdini.md`, `docs/rendering.md`, `integrations/houdini/**` (UsdRenderers.json, soho parameter files, shader translator, `python3.13libs/*.py`, shelf, icons), the HDA builders `scripts/build_render_settings_hda.py` and `scripts/build_substrate_hda.py`, `scripts/generate_render_settings.py`, `scripts/prepare_renderer_settings.py`, `scripts/Package-Houdini.ps1`, `packaging/houdini/*`, and the Houdini parts of `src/hydra/delegate.cpp` and `src/hydra/prims.cpp` that implement the Houdini contract. The `.hda` files are binary (`INDX` header), so I audited them through their generator scripts and did not open them. I ran nothing in Houdini: no runtime claim here was reproduced, and the ones inferred from code are marked "inferred" or "unverified".

Reference material:

- SideFX's own files in the locally installed Houdini 22.0.368 (`$HFS`):
  - `houdini/UsdRenderers.json` (the Karma and Storm entries)
  - `houdini/python3.13libs/usdrenderers.py` (the parser)
  - `houdini/soho/parameters/BRAY_HdKarma_*.ds` and `Husk_Aov.ds`
  - `houdini/python3.13libs/loputils.py`
  - `houdini/python3.13libs/uiready.py`
  - the `bin/*.exe` list
- HDK: Hydra integration, UsdRenderers.json keys, `.ds` naming and `houdini:*` settings: https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html
- HDK: `HUSD_RendererInfo` header: https://www.sidefx.com/docs/hdk/_h_u_s_d___renderer_info_8h_source.html
- husk: https://www.sidefx.com/docs/houdini/ref/utils/husk.html
- Python startup scripts: https://www.sidefx.com/docs/houdini/hom/locations.html
- `hou.ui`: https://www.sidefx.com/docs/houdini/hom/hou/ui.html
- Packages: https://www.sidefx.com/docs/houdini/ref/plugins.html
- Asset namespaces: https://www.sidefx.com/docs/houdini/assets/namespaces.html
- Shader translators: https://www.sidefx.com/docs/houdini/solaris/shader_framework.html (already cited in `docs/houdini.md`)

## Summary

| ID | Severity | Finding | Effort |
| --- | --- | --- | --- |
| HOU-01 | High | Every interactive Houdini session with the package loads the UE backend DLL and the delegate DLL, then runs a 16 ms Python timer and writes a state file every 2 s, even if hdUE is never selected | M |
| HOU-02 | High | Viewport and husk render different things: the cooked MaterialX/Substrate library, live Substrate preparation, `hdue:asset` on non-Mesh prims and `op:` images all come from the Python host, outside USD | L |
| HOU-03 | High | Outside the GUI, UE is shut down with the last delegate and cannot restart. `husk --restart-delegate`, or any re-creation of the delegate, cannot work (inferred) | M |
| HOU-04 | Medium | The executable allow-list for "Houdini UI" omits `hindie.exe`, `hescape.exe`, `hedu.exe` and others. Indie or Escape users who open a .hip at startup get the blocking, one-shot husk mode | S |
| HOU-05 | Medium | The host's logs and `runtime-state.json` go into the install folder by default, and UE's `Saved` too. A studio-shared install documented by `Install.ps1 -PackagesDirectory` makes every user write into the same folder | S |
| HOU-06 | Medium | No `huskErrorStatus`: a failed frame ("renders none of the requested render vars", UE boot failure) does not set husk's exit code | S |
| HOU-07 | Medium | UsdRenderers.json has no `husk.stats_metadata` or `husk.metadata`. With the documented `*` default, husk would store every render stat, including per-mesh dictionaries, as EXR metadata, while standard metadata (Software, renderTime, worldToCamera/NDC) is missing | S |
| HOU-08 | Medium | Render framing (`dataWindowNDC`, pixel aspect) is not read: husk `--tile-count`/`--autotile` and Render Settings data windows are unsupported (inferred from code) | M |
| HOU-09 | Medium | The render-settings HDAs are not namespaced or versioned (`hdue_rendersettings`, ...), unlike the Substrate HDAs, and are rebuilt in place from the `.ds` | S |
| HOU-10 | Medium | When the host fails it stops for good, and the only trace is stdout and a log file, never Houdini's UI | S |
| HOU-11 | Medium | Only `_Global`, `_Viewport` and `_Light` `.ds` files exist. Without `_Geometry`, `_Camera`, `_Product` and `_Aov`, per-prim `hdue:*` and per-var settings have no UI on the standard LOPs | M |
| HOU-12 | Medium | `sys.path.insert(0, <package>/scripts)` puts 62 development modules (tests, generators, generic names) ahead of everything in Houdini's Python. Test modules ship in `python3.13libs` | S |
| HOU-13 | Low | Docs contradict the code and SideFX: they say UE runs on the UI thread, that `uiready.py` "replaces Houdini's own", and that the selection highlight comes from `primId` | S |
| HOU-14 | Low | `"valid": "env:HDUE_HOUDINI_ENABLE"` only checks that the variable exists, while `uiready.py` checks `== '1'`. Renderer availability (ROP menu, farm) is tied to the flag that starts the GUI host | S |
| HOU-15 | Low | Several documented `houdini:*` settings are not handled: `houdini:interactive`, `houdini:cop_texture_changed`. `pauseonupdate` is false | S |
| HOU-16 | Low | Attribute namespace inconsistencies: settings use `hdUE:`, prims and lights use `hdue:`. The delegate strips `hdUE:` and accepts bare names. Menus are `string`, not `token`. Every control menu is typed `'int'` | S |
| HOU-17 | Low | Optional UsdRenderers.json features are unused: `restartrendersettings`, `husk.verbose_callback`, `renderProgressAnnotation`/`renderStatsAnnotation` | S |
| HOU-18 | Low | Hard-coded build strings (`22.0.466`), a hard-coded `'Houdini VK'` fallback renderer label, and no `load_package_once` | S |

---

## HOU-01: The always-on Python host costs every Houdini session (High, M)

**What we do**

- `packaging/houdini/Install.ps1:22` writes `HDUE_HOUDINI_ENABLE=1` into the package, so it is set in every session.
- `integrations/houdini/python3.13libs/uiready.py:2-5` then calls `hdue_houdini.start` deferred on every GUI startup.
- `Host.__init__` (`hdue_houdini.py:39-83`):
  - loads the UE backend DLL with `ctypes.CDLL` through `Runtime(..., start=False)` (`scripts/hdue_runtime.py:53`);
  - loads the delegate DLL (`hdue_viewport.py:38`);
  - starts `LivePreparation`;
  - arms a `QTimer` at 16 ms that re-arms after every run (`hdue_houdini.py:76-80`, `:185`).
- Each `tick()` (`hdue_houdini.py:114-185`) runs a ctypes `status()` call, `live_substrate.tick()`, `current_settings_node()` (a pane lookup plus a walk up the input chain), and `hou.ui.paneTabOfType` lookups. It also writes `runtime-state.json` to disk every 2 s (`:177-178`, `:95-105`).
- None of this is gated on hdUE being the active renderer. The UE thread itself is now owned by the delegate (`hdue_houdini.py:51-53`, `src/hydra/delegate.cpp:242-300`), so this timer only does housekeeping, yet it still runs at 60 Hz.

**What SideFX documents**

- `uiready.py`: "Houdini only runs this script (if it exists) in interactive sessions" (https://www.sidefx.com/docs/houdini/hom/locations.html).
- The sanctioned idle hook, `hou.ui.addEventLoopCallback`, "is called approximately every 50ms, unless Houdini is busy processing events" (https://www.sidefx.com/docs/houdini/hom/hou/ui.html). So SideFX's own idle cadence is about 3x slower than hdUE's.
- No SideFX delegate needs a Python companion. Karma, Storm and HdEmbree run only through the delegate, plus the `houdini:*` render settings Houdini sends it (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html).

**Impact**

- Every artist on a machine with the package pays a large native DLL load at startup and a 60 Hz Python callback on the UI thread, even when they never select hdUE. The startup-time and memory cost was not measured (unverified).
- The DLL load also exposes Houdini to any DLL-conflict risk in the UE backend's imports.
- The 2 s file write is a constant disk touch. Combined with HOU-05, it goes to a possibly shared folder.

**Recommendation**

1. Start lazily, in two stages. `uiready.py` should only register a cheap watcher, for example `hou.ui.addEventLoopCallback`, or a 250-500 ms `QTimer` that checks `viewer.currentHydraRenderer()`. Build `Host` (DLL loads, `LivePreparation`) only when a Scene Viewer first selects hdUE. The delegate already knows that moment (`houdini:viewport` in `delegate.cpp:1696`) and could flag it for Python through `hdue_get_viewport_status`.
2. While no hdUE viewport is active, drop the cadence to 250 ms or more, and run at 16 ms only while a frame is pending (`published_frames` changes).
3. Write `runtime-state.json` only when `HDUE_HOUDINI_LOG_DIR` is set (a debug mode), not by default.
4. Longer term, move the redraw-on-new-frame job into the delegate. Houdini polls `IsConverged()` and the stats, and most third-party delegates rely on that rather than on a host-side `draw()`. Then the Python host is left with the jobs in HOU-02 only.

## HOU-02: Viewport and husk/ROP render different things (High, L)

**What we do**

- **Cooked MaterialX / Substrate library selection.** These are spare parameters on a Python-built pass-through subnet, `hdUE_settings` (`hdue_houdini.py:377-399`). The subnet is found by walking `inputs()[0]` until a node has `hdue_materialx_library` (`:357-365`). The parameters are read only by the GUI host (`:133`, `:142-146`). They are not USD and never reach husk.
- **Live Substrate preparation.** `LivePreparation` (`hdue_houdini.py:57-59`, `scripts/substrate_live.py:13`) is a host-side subprocess. Nothing documents what husk renders for a Substrate Builder material (unverified; `docs/substrate-builder.md` never mentions husk).
- **`hdue:asset` on Xform/Scope.** The asset needs the `__HdUEAssetProxy` bounds mesh. The host authors that mesh only in the LOP network's viewport overrides (`hdue_houdini.py:461-475`), and the delegate places assets only through a mesh (`src/hydra/prims.cpp:266-268`). `docs/houdini.md:84` itself says these overrides "are not part of what husk or a USD ROP sees". So an Xform with `hdue:asset` renders in the viewport and is missing in husk (inferred; `docs/materials.md:49` advertises Xform/Scope support).
- **`op:` images.** These are exported by the host only. `docs/houdini.md:141` says "husk does not evaluate `op:` paths."

**What SideFX documents**

- The USD Render ROP and husk render the same delegate from the USD stage. Render settings travel as attributes on the render settings prim, which reach the delegate through `HdRenderSettingsMap` and `SetRenderSetting()` (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html).
- Delegates that support COP textures are expected to handle `houdini:cop_texture_changed` and set `pauseonupdate` (same page).
- husk renders what is in the stage: there is no host-side hook (https://www.sidefx.com/docs/houdini/ref/utils/husk.html).

**Impact**

- This breaks the core Solaris contract: what you look at in the viewport is not what the farm renders.
- An artist sets the cooked MaterialX library on `hdUE_settings`, approves the look in the viewport, submits through the USD Render ROP, and gets Preview Surface fallbacks and missing native assets. Nothing warns them.

**Recommendation**

- Make every input that changes the image USD data:
  - Cooked library paths become render settings, for example `hdUE:materialxLibrary` and `hdUE:substrateLibrary` on the `UsdRenderSettings` prim, read by the delegate in every host. Retire the `hdUE_settings` subnet, or keep it only as a UI shim that authors those attributes.
  - The asset bounds proxy is authored by a real LOP (an "hdUE Native Asset" HDA) in the graph, so husk sees it. Alternatively, the delegate resolves non-Mesh `hdue:asset` prims itself, through a Hydra scene-index plugin that synthesizes the proxy. That works in husk, usdview and Live Render alike.
- For `op:` images, document clearly that they are viewport-only, and add a husk-side warning when a material references `op:` (husk can at least report it). Whether Houdini exposes its `op:` image evaluation to third-party delegates is unverified; check Karma's path before building more host-side export.
- Until this is done, document the viewport/husk differences in one table in `docs/houdini.md`.

## HOU-03: One UE lifetime per process vs husk delegate restarts (High, M)

**What we do**

- In non-GUI processes (`continuous == false`), the destructor of the last delegate shuts UE down and joins its thread (`src/hydra/delegate.cpp:1554-1579`). Per the in-code comment, "UE allows one lifetime per process" (`:1566`).
- A later delegate then finds the worker not joinable. It either goes down the `ueRun` inline path or throws "hdUE requires a ready runtime" (`:1505-1527`). Inferred from code; not run.

**What SideFX documents**

- "`--restart-delegate ‹n›` restarts the delegate every n frames" (https://www.sidefx.com/docs/houdini/ref/utils/husk.html).
- `destroyifdeactivated` and Houdini's viewport may also tear down and re-create delegates (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html).

**Impact**

- `husk --restart-delegate`, a common farm setting to cap memory growth, would fail after the first chunk.
- Any hython or tool process that creates a second hdUE delegate after the first is gone (two Live Render sessions in a hython-driven tool; usdview "reload" outside the launcher) would hit the same wall.

**Recommendation**

- Keep UE alive until process exit in every mode, the same as `continuous`: destroy only the session when the last delegate goes, and shut UE down from the existing `atexit` path.
- Add "husk with `--restart-delegate 1` over 3 frames" to the test suite.
- If UE truly cannot survive between delegates in a given mode, return a clear `huskErrorStatus` (HOU-06) instead of a generic failure.

## HOU-04: Incomplete "Houdini UI" executable list (Medium, S)

**What we do**

`houdiniUi()` (`src/hydra/delegate.cpp:233-238`) recognizes `houdini.exe`, `houdinifx.exe`, `houdinicore.exe` (listed twice) and `happrentice.exe`. The Houdini 22.0.368 `bin` folder also ships `hindie.exe`, `hescape.exe`, `hedu.exe`, `heducation.exe` and `hexper.exe`.

The fallback is `HDUE_HOUDINI_UI`, which the Python host sets only after the UI is ready (`hdue_houdini.py:53`). The comment above `houdiniUi()` notes that "a .hip opened at startup creates the viewport's delegate before hdue_houdini can set HDUE_HOUDINI_UI".

**What SideFX documents**

The product executables are those in `$HFS/bin`. `hindie` is how Houdini Indie is launched.

**Impact**

In Houdini Indie or Escape with a .hip that opens on an hdUE viewport, the delegate takes the husk path (inferred):

- `wait = true` (`delegate.cpp:1517`), which blocks the UI thread during UE boot;
- `continuous = false` (`:248`), so UE shuts down with the last delegate and, per HOU-03, cannot come back for the rest of the session.

**Recommendation**

- Detect the GUI by capability rather than by executable name. One option is the presence of `houdini:viewport` before the first `Execute`. Another is the module `libHoudiniUI` / `HoudiniUI.dll` being loaded (unverified name).
- At minimum, add the missing executables and remove the duplicate entry.

## HOU-05: Logs and state written into the install folder (Medium, S)

**What we do**

- `Host.__init__` defaults `log_dir` to `ROOT/'out/houdini-22.0.466/session'` (`hdue_houdini.py:46`). The same default appears in `start()` for `startup-error.txt` (`:283`).
- `ROOT` is the package root (`:14`; the layout is kept by `scripts/Package-Houdini.ps1:40-41`). `Install.ps1:20-26` does not set `HDUE_HOUDINI_LOG_DIR`.
- UE's `Saved` (shader cache, logs) also lives in the package (`docs/houdini.md:11`).
- `Install.ps1 -PackagesDirectory <shared HOUDINI_PACKAGE_DIR>` is documented for studio-wide installs (`docs/houdini.md:13`).

**What SideFX documents**

Packages are meant to be referenced from shared locations; per-user state belongs under `$HOUDINI_USER_PREF_DIR` or `$HOUDINI_TEMP_DIR` (https://www.sidefx.com/docs/houdini/ref/plugins.html). The page documents the variables; it does not state this practice explicitly.

**Impact**

- A shared or read-only install either fails to write, or has many users and husk farm processes interleaving `integration.log` and overwriting `runtime-state.json` in one folder.
- UE's `Saved` on a network share has the same problem at larger scale (DDC/PSO caches).

**Recommendation**

- Default `HDUE_HOUDINI_LOG_DIR` to `$HOUDINI_TEMP_DIR/hdUE/<pid>` (or `$HOUDINI_USER_PREF_DIR/hdUE`).
- Pass UE a per-user `-UserDir=`/`-ProjectSavedDir` (or the backend equivalent) under `$HOUDINI_USER_PREF_DIR`.
- Have the package JSON set both variables.

## HOU-06: Failed frames don't set husk's exit status (Medium, S)

**What we do**

- When no requested render var is one hdUE renders, the delegate logs `TF_RUNTIME_ERROR` and sets `converged_ = true` (`src/hydra/delegate.cpp:1157-1158`).
- UE boot errors surface only as exceptions or the `error` stat (`:1795`, `:1828`).
- `huskErrorStatus` appears nowhere in `src/`.

**What SideFX documents**

"huskErrorStatus: A non-zero value tells husk to terminate early with that exit code" (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html).

**Impact**

Farm managers (Deadline, Tractor, PDG) judge husk by its exit code. A misconfigured AOV set or a failed UE boot would likely produce "successful" frames that are black or missing. The exit code itself was not tested (unverified).

**Recommendation**

Return `huskErrorStatus` (for example 1, or distinct codes for boot, ABI and AOV errors) from `GetRenderStats()` whenever `state.error` is set in a non-GUI process.

## HOU-07: husk metadata keys unset (Medium, S)

**What we do**

- `integrations/houdini/UsdRenderers.json:1-19` defines neither `husk.metadata` nor `husk.stats_metadata`.
- `renderStats()` returns about 60 keys (`src/hydra/delegate.cpp:1814-1863`), including `nativeAssetIds` (a dictionary with one entry per mesh), `pick`, `sessionHandle` and the `wet*`/`vt*` debug counters.

**What SideFX documents**

- `husk.stats_metadata` "Defaults to `*`" (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html). The header says "(meaning all the stats from GetRenderStats will be stored as metadata)" (https://www.sidefx.com/docs/hdk/_h_u_s_d___renderer_info_8h_source.html).
- Karma sets `"husk.stats_metadata": ""` and a `husk.metadata` map with Software, renderTime, worldToCamera/worldToNDC, clipNear/Far and renderMemory (`$HFS/houdini/UsdRenderers.json`, the `BRAY_HdKarma_base` entry).

**Impact**

- EXR headers can grow with scene size, and they carry debugging internals. That the debug stats actually land in the EXR headers was not checked against an output file (unverified).
- Compositing loses the standard camera matrices that Nuke and other tools read from Karma output.

**Recommendation**

- Set `"husk.stats_metadata": ""`, or a short pattern such as `percentDone totalClockTime completedSamples`.
- Add a `husk.metadata` map modelled on Karma's: `$frame`, `$fps`, `$command_line`, Software `hdUE <version> (UE 5.8)`, `worldToCamera`/`worldToNDC` (which husk computes itself), and render time.

## HOU-08: Render framing / data window ignored (Medium, M)

**What we do**

A search for `dataWindow`, `framing`, `aspect` or `conform` in `src/` finds only a comment that "Hydra's projection already holds the conformed window" (`src/hydra/delegate.cpp:1351`). `HdRenderPassState::GetFraming()` / `dataWindowNDC` is never read. The hdUE Render Settings HDA nevertheless exposes `pixelAspectRatio` (`scripts/build_render_settings_hda.py:64-69`).

**What SideFX documents**

husk provides `--autotile`, `--tile-count`, `--tile-index` and `--tile-suffix` (https://www.sidefx.com/docs/houdini/ref/utils/husk.html). These, and the Render Settings LOP's data window, reach delegates through Hydra framing.

**Impact**

Tiled farm renders and region renders would produce full frames per tile, or wrong crops, and non-square pixels are ignored. Inferred from code, not run.

**Recommendation**

- Read the framing from the render pass state and render only the data window, or render full frames and copy the window.
- Until then, reject unsupported framing with an error rather than writing wrong images, and list it under "Not supported" in `docs/houdini.md`.

## HOU-09: Render-settings HDAs are neither namespaced nor versioned (Medium, S)

**What we do**

- `NAME = "hdue_rendersettings"` (`scripts/build_render_settings_hda.py:28`). `hdue_standardrendervars` and `hdue_renderproducts` are similar (`:296`, `:348`).
- Each `.ds` edit regenerates the asset in place (`:236-256`, `hda.unlink`).
- The Substrate HDAs, by contrast, follow `hdue::substrate_<kind>::1.3` (`hdue_substrate.py:12-24`).

**What SideFX documents**

The type name form is "`[‹namespace›::]‹node_name›[::‹version›]`". Namespaces let you "name your assets without worrying about using the same name as a built-in Houdini node", and versions allow "multiple independent versions of an asset without having to change the 'main name'" (https://www.sidefx.com/docs/houdini/assets/namespaces.html).

**Impact**

The render settings asset mirrors 262 generated parameters. Any rename in `generate_render_settings.py` (for example after a UE update) silently changes the interface of every saved .hip that uses it. There is no way to keep both the old and the new definition.

**Recommendation**

- Rename to `hdue::rendersettings::1.0`, `hdue::standardrendervars::1.0` and `hdue::renderproducts::1.0`. Keep the old names as hidden aliases (`hou.hda` deprecation, or a hidden 0.x definition) so existing hips load.
- Bump the version whenever a parameter is removed or renamed.

## HOU-10: Host failures are invisible (Medium, S)

**What we do**

- Any exception in `tick()` sets `running=False` and stops the timer for good (`hdue_houdini.py:179-183`).
- The traceback goes to `print`, to `integration.log`, and to the optional image viewer's label (`self.viewer` exists only with `HDUE_HOUDINI_IMAGE_VIEWER=1`, `:345`).
- A startup failure only writes `startup-error.txt` (`:281-285`).

**What SideFX documents**

For user-visible errors, `hou.ui.setStatusMessage(..., hou.severityType.Error)` and `hou.ui.displayMessage` are the standard UI channels (https://www.sidefx.com/docs/houdini/hom/hou/ui.html). The host already uses `setStatusMessage` for material errors (`:111`).

**Impact**

Once the host dies, op: textures, asset proxies, material preparation and the frame-arrival redraws all stop silently. The viewport may appear frozen, and the user has no hint why.

**Recommendation**

- Report fatal host and startup errors through `hou.ui.setStatusMessage(..., Error)`, and through the delegate's `renderProgressAnnotation` stat (HOU-17), which Houdini shows in the viewport.
- Restart the timer after transient errors instead of stopping permanently. Keep the hard stop only for repeated failures.

## HOU-11: Missing soho parameter files for other LOPs (Medium, M)

**What we do**

`integrations/houdini/soho/parameters/` contains only `HdUEHoudiniRendererPlugin_Global.ds`, `_Viewport.ds` and `_Light.ds`. Per-prim attributes such as `hdue:asset` and `hdue:surface:wet:*` (`docs/materials.md:49-54`, `hdue_houdini.py:441`) have no UI. Per-render-var options have none either.

**What SideFX documents**

Houdini picks up `<name>_Product.ds` (Render Product LOP), `<name>_Aov.ds` (Render Var LOP), `<name>_Camera.ds` (Camera LOP) and `<name>_Geometry.ds` (Render Geometry Settings LOP) in the same way (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html). Karma ships all seven (`$HFS/houdini/soho/parameters/BRAY_HdKarma_*.ds`).

**Impact**

Users have to author `hdue:*` primvars with wrangles or Edit Properties by hand. The setting names are only discoverable from the docs.

**Recommendation**

- Add `_Geometry.ds` for `hdue:asset`, the wet surface attributes and the subdivision override, if per-prim.
- Add `_Camera.ds` if UE camera settings (DOF overrides, exposure) are intended per camera.
- Add `_Aov.ds` if AOV options exist.
- Generate these files with the same script as `_Global.ds`.

## HOU-12: `scripts/` prepended to `sys.path`; tests shipped (Medium, S)

**What we do**

- `sys.path.insert(0,str(ROOT/'scripts'))` (`hdue_houdini.py:15`) runs on startup in every session (HOU-01).
- The package copies all 62 `scripts/*.py` (`Package-Houdini.ps1:42`). They include generic names such as `scene_check`, `scene_space`, `material_api`, `lighting_api`, `surface_api`, `materialx_textures` and `render_gallery`.
- Ten `*_test.py`/`*_probe.py` modules ship in `integrations/houdini/python3.13libs/`, and a test dispatcher lives in production `start()` (`hdue_houdini.py:249-280`).

**What SideFX documents**

"Houdini automatically adds any `$HOUDINI_PATH/pythonX.Xlibs` directories to the Python path"; other modules should go through `$PYTHONPATH` or `sys.path` (https://www.sidefx.com/docs/houdini/hom/locations.html). Position 0 overrides Houdini's own modules and every other package's.

**Impact**

A studio module named like any of the 62 is shadowed in every session, and the reverse is also possible. The failure mode is confusing import errors far from hdUE.

**Recommendation**

- Move the runtime modules into one package, for example `python3.13libs/hdue/` (`hdue.runtime`, `hdue.asset_proxy`, ...), and stop editing `sys.path`.
- Keep generators and tests out of the shipped package.
- Move the test dispatch into a separate `hdue_test_entry.py` that `Launch-Houdini.ps1` triggers.

## HOU-13: Docs contradict the code and SideFX (Low, S)

**What we do**

- **Threading model.** These places say UE runs on and is ticked from Houdini's UI thread: `README.md:199` ("in Houdini that is the UI thread, which hdUE's Python host ticks"), `README.md:240` ("starts hdUE's UI-thread host"), `docs/houdini.md:117` ("UE belongs to Houdini's UI thread"), `hdue_houdini.py:1` and `:242` ("UE 5.8.4 ready on the Houdini UI thread"). The code runs UE on hdUE's own thread in the GUI as well: `hdue_houdini.py:51-53`, `delegate.cpp:161-170` and `:248`.
- **`uiready.py`.** `docs/houdini.md:15` says the package's `uiready.py` "replaces Houdini's own".
- **Selection highlight.** `docs/houdini.md:40` says that because the delegate provides `primId`/`instanceId`, "Solaris draws its native selection highlight on hdUE images". `docs/houdini.md:49` says those AOVs are off by default, and `UsdRenderers.json:13` sets `needsselection: true`, which asks Houdini's GL renderer for the highlight overlay.

**What SideFX documents**

- `uiready.py`: "Houdini will find any files matching this pattern in the Houdini path and run them after the UI is ready" (https://www.sidefx.com/docs/houdini/hom/locations.html). Both files run.
- `needsselection`: "Whether this renderer needs the Houdini OpenGL renderer to draw the currently selected primitives in an overlay" (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html).

**Recommendation**

Rewrite those passages:

- UE owns its thread; the Python host does housekeeping only.
- `uiready.py` runs alongside Houdini's.
- The selection highlight is Houdini's GL overlay (`needsselection`); `primId` AOVs are opt-in.

## HOU-14: `valid` tied to the GUI-host flag (Low, S)

**What we do**

- `UsdRenderers.json:3` has `"valid": "env:HDUE_HOUDINI_ENABLE"`, while `uiready.py:2` checks `== '1'`.
- `README.md:240` documents the variable as "starts hdUE's UI-thread host".
- The source-checkout husk instructions have to set it just to make the renderer valid (`docs/houdini.md:107`).

**What SideFX documents**

`usdrenderers._checkEnvironment` tests existence only: "Check existence of environment variable: env:NAME or env:-NAME" (`$HFS/houdini/python3.13libs/usdrenderers.py:38-44`). The HDK describes `valid` as "Whether or not this render delegate should be available in Houdini" (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html).

**Impact**

- `HDUE_HOUDINI_ENABLE=0` still lists the renderer but disables the host. That gives a half-working viewport.
- A farm environment that installs only the delegate must also set a variable whose documented meaning is a GUI feature.

**Recommendation**

- Use `"valid": "env:HDUE_RUNTIME_CONFIG"`: without a runtime config the delegate cannot start anyway.
- Keep `HDUE_HOUDINI_ENABLE` for the host only, or drop it once the host starts lazily (HOU-01).

## HOU-15: Unhandled `houdini:*` settings (Low, S)

**What we do**

- `SetRenderSetting` handles `houdini:viewport`, `houdini:render_pause`, `stageMetersPerUnit`, `houdini:frame` and `houdini:fps` (`src/hydra/delegate.cpp:1696-1750`).
- It does not handle `houdini:interactive` or `houdini:cop_texture_changed`.
- `UsdRenderers.json:9` sets `"pauseonupdate": false`.

**What SideFX documents**

- `houdini:interactive` describes the render's purpose (`tumbling`, `scrubbing`, `playing`, ...).
- `houdini:cop_texture_changed` signals that "rendering should stop and caches be flushed".
- `pauseonupdate` "Should be true if the renderer supports COP textures".

All three are from https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html. The header adds: "to avoid potential deadlocks" (https://www.sidefx.com/docs/hdk/_h_u_s_d___renderer_info_8h_source.html).

**Impact**

- The Path Tracer mode cannot drop to cheaper settings while tumbling.
- COP texture changes depend entirely on the Python host's polling (HOU-01, HOU-02).

**Recommendation**

- Use `houdini:interactive` to skip warmup and AOVs and to cap samples while it is not `normal`.
- Handle `houdini:cop_texture_changed` by re-requesting host textures.
- Re-check whether `pauseonupdate: true` is needed once `op:` support moves into the delegate.

## HOU-16: Namespace and type conventions (Low, S)

**What we do**

- Render settings use `hdUE:*` (`docs/rendering.md:5-24`). Lights and prims use `hdue:*` (`HdUEHoudiniRendererPlugin_Light.ds:1`, `hdue:asset`, `hdue:surface:*`). USD namespaces are case-sensitive.
- The delegate strips `hdUE:` and re-dispatches the bare name (`src/hydra/delegate.cpp:1694`), so un-namespaced keys such as `renderMode` are accepted from any source.
- The 27 menu settings are `usdvaluetype string`; Karma uses `token` (`BRAY_HdKarma_Global.ds`, `xn__karmaglobalimagemode_m8ag`).
- The `HDUE_CONTROL` macro calls `createEditPropertiesControlMenu(kwargs, 'int')` for every type (`HdUEHoudiniRendererPlugin_Global.ds:10-23`). Karma passes the real type ('string', 'float', 'bool'), and `loputils.createEditPropertiesControlMenu` branches on it (`$HFS/houdini/python3.13libs/loputils.py:1207-1240`).

**What SideFX documents**

Parameter names must be `hou.text.encodeParm()` of the full attribute name (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html). Every third-party delegate namespaces its settings: `karma:global:*`, `arnold:*`, `ri:*`, `redshift:*`.

**Recommendation**

- Pick one prefix. Lowercase `hdue:` matches the prim attributes. Accept the other casing as a deprecated alias.
- Stop accepting bare keys from the render settings map. Keep them only for the internal C ABI.
- Emit `token` for menus. hdUE already accepts both (`docs/rendering.md:5`).
- Pass each parameter's real type to `createEditPropertiesControlMenu`.

## HOU-17: Optional UsdRenderers.json features unused (Low, S)

**What we do**

- `UsdRenderers.json` has no `restartrendersettings` or `restartcamerasettings`, and no `husk.verbose_callback`.
- The stats provide `rendererStage` and `percentDone`, but not `renderProgressAnnotation`/`renderStatsAnnotation` (`delegate.cpp:1795`, `:1860`).
- `totalClockTime` is the last frame's request-to-frame time (`:1862`), not the total.

**What SideFX documents**

All of these keys and stats are documented (https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html). Karma uses `restartrendersettings` for engine switches, `husk.verbose_callback: karma_stats.py`, and the annotations (`$HFS/houdini/UsdRenderers.json`).

**Recommendation**

- Add `renderProgressAnnotation` (for example "Lumen warmup 12/16", "PT 48/64 spp", or the UE boot stage) so the viewport shows progress natively.
- Add a small `husk.verbose_callback` that prints samples and frame time.
- Add `restartrendersettings` for settings the delegate cannot apply live, if any. `hdUE:cvar:*` read-only cvars are candidates.

## HOU-18: Build strings, renderer label, package options (Low, S)

**What we do**

- Hard-coded `22.0.466` paths: `hdue_houdini.py:46`, `:55`, `:283`; `Package-Houdini.ps1:13`; `build_render_settings_hda.py:93`. The test machine has 22.0.368 installed.
- `Host.close()` switches viewers to `'Houdini VK'` (`hdue_houdini.py:223`). The stock `UsdRenderers.json` lists only "Houdini GL" for the native renderer. The call is wrapped in try/except, and "Houdini VK" may come from elsewhere in 22 (unverified).
- `Install.ps1:18-28` writes no `load_package_once`.

**What SideFX documents**

`load_package_once` "Prevents a package from being loaded multiple times" (https://www.sidefx.com/docs/houdini/ref/plugins.html).

**Recommendation**

- Derive build folders from `hou.applicationVersionString()` or the runtime config.
- Fall back to the first renderer in `viewer.hydraRenderers()` that is not hdUE.
- Add `"load_package_once": true`, so a user copy plus a studio copy do not append `PXR_PLUGINPATH_NAME` twice.

---

## What is already right

- **UsdRenderers.json basics** (`UsdRenderers.json`) match Karma's entry:
  - `depthstyle: linear` with `needsdepth: false`, because the delegate returns real depth;
  - `aovsupport: true`, which puts it in the USD Render ROP list;
  - `defaultpurposes`;
  - `drawmodesupport: false`;
  - `statsdatapaths` mapping `percentDone` and `rendererStage`, plus custom `viewstats`.
- **`.ds` naming and structure** follow the HDK convention `<plugin>_Global/_Viewport/_Light.ds`:
  - `xn__`-encoded attribute names;
  - Karma-style Set/Block/Do Nothing control menus;
  - `usdvaluetype` tags;
  - `uiscope viewport` for display options.

  Render Settings LOPs therefore get an "Unreal Engine" tab with no custom node. This is the sanctioned mechanism, the same as Karma and Arnold.
- **Settings as `UsdRenderSettings` attributes** under a renderer prefix. This is the right model for farm portability, and husk and the ROP need no hdUE-specific wiring.
- **The hdUE Render Settings / Standard Render Vars / Render Products HDAs** mirror Karma's LOP trio:
  - they are built only from stock LOPs, so the USD is plain;
  - they use `driver:parameters:aov:husk:format`, matching `Husk_Aov.ds`;
  - they default the output to `$HIP/render/$HIPNAME.$OS.$F4.exr`;
  - they call `loputils.updateResolutionParameters` in OnCreated;
  - they have help cards and icons referenced by name.

  A wrapper HDA next to the plain `.ds` tab is the same convention Karma uses. It is not a deviation (only the type naming is, HOU-09).
- **Shader translator.** The Substrate translator lives in `husdplugins/shadertranslators/` and is selected by render mask, as SideFX documents for custom translators. The Substrate VOP HDAs are namespaced and versioned (`hdue::substrate_*::1.3`).
- **`houdini:*` handling.** The delegate handles `houdini:render_pause`/`Pause()`, `houdini:viewport`, `stageMetersPerUnit` and `houdini:fps`. It restores husk's working directory and fails fast on unrenderable AOV sets instead of hanging.
- **Host-side edits stay session-only.** The host uses `LopNetwork.viewportOverrides()`, Houdini's own session layer. It never cooks from its timer (`hdue_houdini.py:197-199`) and re-arms a single-shot timer instead of keeping a permanently due one.
- **Package.** The package JSON is relocatable. It uses `hpath`, appends `PXR_PLUGINPATH_NAME`, and gates on `houdini_version`, the same as HtoA and Redshift.

## Gaps vs other Solaris renderers (Karma, Arnold, RenderMan, Redshift)

| Area | Karma | Arnold (HtoA / arnold-usd) / RenderMan / Redshift | hdUE |
| --- | --- | --- | --- |
| Viewport/husk parity | Full: everything is USD | Full: renderer settings and shaders are USD | Partial: cooked libraries, asset proxies, `op:` images and live Substrate prep are GUI-only (HOU-02) |
| Python host in GUI | None | None (pure plugin + `.ds`) | Always-on 60 Hz host, loading DLLs at startup (HOU-01) |
| `.ds` coverage | Global, Viewport, Light, Camera, Geometry, Product, Aov | Global, Light, Camera, Geometry; Arnold and RenderMan also Aov/Product (from their public plugin layouts, unverified per version) | Global, Viewport, Light (HOU-11) |
| husk tiling / data window | Yes | Yes | No (HOU-08) |
| Delegate restart / multiple lifetimes | Yes | Yes | No outside the GUI (HOU-03) |
| husk exit status / metadata | `huskErrorStatus`, `husk.metadata`, `stats_metadata ""` | Comparable metadata | None (HOU-06, HOU-07) |
| Delegate-written products (checkpoint, deep, cryptomatte) | `karma:checkpoint`, `karma:deep` via `diskproducttypes`; cryptomatte | Arnold/RenderMan: deep and cryptomatte drivers | None. `config/Icons/HDUE_hduecryptomatte.svg` ships without a feature |
| Snapshots / checkpoint-resume | Yes | Renderer-dependent | Not applicable to realtime; Path Tracer resume not supported |
| LPE / light-path AOVs | Yes | Yes | No: GBuffer AOVs only; any `lpe` source is mapped to the beauty |
| Viewport progress UI | QML stats template, annotations, verbose callback | Annotations | `percentDone`/`rendererStage` only (HOU-17) |
| Operator naming | SideFX built-ins | Vendor-namespaced (`arnold::`, `pxr`, `redshift::`) | Mixed: Substrate namespaced, render HDAs not (HOU-09) |
| Interactive degradation (`houdini:interactive`) | Yes | Yes (Arnold progressive, RenderMan) | No (HOU-15) |
