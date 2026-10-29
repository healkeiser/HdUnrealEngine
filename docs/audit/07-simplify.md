# Code-quality review (/simplify, whole repo)

2026-10-29, `feature/package` at `3d65669`. Four read-only reviews of the whole tree (reuse, simplification, efficiency, altitude), merged and deduplicated. **Nothing was applied**: another session is editing the code. Spot-checked against the code while merging: the per-tick `opdef:` hashing, single-threaded OCIO, the duplicated `houdinicore.exe`, the `/Game/HdUETest` map in both boot paths, the 600 x `Sleep(10)` boot loop and the 26 `if (CheckReady() != HDUE_OK) return CheckReady();` sites. Correctness bugs are in [01](01-hydra-delegate.md)–[05](05-build-packaging-quality.md); over-engineering cuts are in [06](06-ponytail-audit.md).

IDs: `ALT` root cause at the wrong depth, `EFF` wasted work, `SIM` simpler form, `REU` reuse an existing helper. Each line: where · problem · fix.

## Do first

These remove whole classes of special cases or the biggest per-frame costs.

| ID | Where | Problem | Fix |
|----|-------|---------|-----|
| ALT-01 | `hdue_houdini.py:153-157`, `src/houdini/diagnostics.cpp:103-118`, `hdue_viewport.py:75` | A ctypes side channel polled every 16 ms pushes renderMode, samples, upAxis, metersPerUnit, time into the delegate; Hydra already delivers them (RenderSettings prim, `stageMetersPerUnit`, `houdini:frame`/`houdini:fps`). Two sources of truth; stale once the timer dies. | Read them from the render settings map; add the up axis like `stageMetersPerUnit`; delete the options ABI. |
| ALT-02 | `hdue_houdini.py:134-152, 158-163, 187-211`, `hdue_op_textures.py`, `hdue_materialx.py` | Material preparation, `op:` textures and `hdue:asset` proxies are done by the Python timer, so they exist only in the Houdini UI (same root as HOU-02). | Move them behind the delegate: material Sprim sync requests preparation; resolve `op:` through `ArGetResolver().OpenAsset` / Houdini's image API in the Houdini build; turn `hdue:asset` into native-asset descriptors in the delegate (`nativeMeshAssets` already exists, `delegate.cpp:1725`). |
| ALT-03 | `hdue_houdini.py:164-171` | A permanent 16 ms timer polls frames and forces `curViewport().draw()`. | Let Hydra's progressive contract drive redraws: `IsConverged()` false plus render-buffer invalidation keeps the viewport calling Execute. |
| ALT-04 | `delegate.cpp:233-239, 248, 1517`; `hdue_houdini.py:53` | Threading model chosen by matching exe names (misses `hindie.exe`/`hescape.exe`, lists `houdinicore.exe` twice) OR an env var that arrives too late. | Decide on the first Execute from what Hydra passes (`houdini:interactive`/viewport settings, OpenGL Hgi in `SetDrivers`). Drops the exe list and `HDUE_HOUDINI_UI`. |
| EFF-01 | `hdue_op_textures.py:111-118` from `hdue_houdini.py:202` | Every tick reads and SHA-256s every bound `opdef:` image (~60x/s). | Key on the HDA definition / library file mtime; hash only when it changes. |
| EFF-02 | `color_management.cpp:56-58` from `delegate.cpp:1282` | OCIO runs single-threaded over the full float frame every publish (~10–30 ms at 1080p when scene-linear isn't Rec.709). | `WorkParallelForN` over row chunks, or fold the 3x3 matrix into the readback conversion. |
| EFF-03 | `Bridge.cpp:686-703, 968-974`, `render_buffer.cpp:57-62` | Beauty goes half → float → 33 MB memcpy → OCIO → back to half for Houdini's `Float16Vec4` buffer: three full-frame passes. | Expose the half readback (`HDUE_RGBA16_FLOAT`) and do alpha, OCIO and flip in one pass into the buffer. |
| EFF-04 | `delegate.cpp:497-545` | `prepareMeshes` converts serially under the state lock, double-precision transform per vertex, no `reserve`, copies `indices` even when no winding swap. | `WorkParallelForEach` over dirty meshes, `reserve`, copy indices only for negative determinants. |
| SIM-01 | `include/hdue/*_api.h` (12 headers), `src/client/backend.cpp:19-98`, `Bridge.cpp:1197-1290`, `scripts/*_api.py` | 12 separately versioned extension tables all shipped from the one DLL built with the delegate: 12 copy-pasted loaders, 12 exports (already drifted: `hdue_get_asset_api` returns a different error), 5 Python loaders, plus the stray unversioned `hdue_resize_session` (`backend.cpp:129-133`). | One API table grown by `struct_size` under one ABI version; failing that, one template loader + one export macro + `Runtime.get_extension()` in `hdue_runtime.py`. ~400 lines. |

## Root causes at the wrong depth (ALT)

| ID | Where | Problem | Fix |
|----|-------|---------|-----|
| ALT-05 | `Scene.cpp:415, 817, 829, 876`; `usd_render.cpp:405` | Mesh build policy (card timing, Nanite) keyed on the `-hdUEInteractive` command-line flag plus `HDUE_NANITE`. | Per-session build-policy flags in the ABI, set from render settings. |
| ALT-06 | `delegate.cpp:1646` | Zero-copy GPU frames gated on Houdini UI + `#if HDUE_HOUDINI` + `HDUE_GPU_FRAMES`. | Choose from Hgi capability for every GL host; CPU path as automatic fallback. |
| ALT-07 | `delegate.cpp:1529-1532, 1582-1631, 1670-1672, 1695-1696` (+~15 `#if HDUE_HOUDINI`) | Volumes, field Bprims and the depth AOV exist only in the Houdini build. | Support the standard Hydra types everywhere; keep only Houdini's field token and viewport registration in a host shim. |
| ALT-08 | `color_management.cpp:4-101` | Colour management only in the Houdini build, UE working space found by guessing names (`:36`), HDR textures re-encoded to `%TEMP%/hdue_ocio`, never evicted. | Matrix from the OCIO scene_linear role applied in UE; OCIO in every build; no converted copies on disk. |
| ALT-09 | `Bridge.cpp:1081-1087`; `delegate.cpp:280` | A static destructor rewrites `GGameThreadId` so UE's scheduler check passes at exit. | Shut UE down on its own thread before static destruction (atexit joins the UE thread after a full engine exit). |
| ALT-10 | `MeshCards.cpp:1-21` | Nine `#pragma include_alias` to compile UE private `.cpp` files into the game DLL. | Keep the card builder as an engine-fork patch/module (see the EULA note in [04](04-unreal-backend.md) UE-01). |
| ALT-11 | `scripts/Apply-ResidencyPatch.ps1:9-28` | String-anchor edits to engine sources; silently no-ops if anchors "already contain" the text. | Commit the patch to the engine fork branch; drop the rewriter. |
| ALT-12 | `hdue_houdini.py:213-233` | Quit choreography in Python: switch viewers to Houdini VK, then ctypes shutdown. | Delegate destroys live sessions from its viewport registry on shutdown; paused delegates recreate on Resume. |
| ALT-13 | `delegate.cpp:1490-1495, 1694, 1699-1700, 1675-1680` | Setting aliases (`hdUE:` strip, `stageMetersPerUnit`) handled in two places; only 6 of ~25 settings in `GetRenderSettingDescriptors`. | One `canonicalKey()`; declare every setting in the descriptors. |
| ALT-14 | `delegate.cpp:1744-1747` | Hand-maintained OR chain of keys that bump the request revision, plus per-key dirty blocks. | One table key/prefix → dirty effect. |
| ALT-15 | `delegate.cpp:272-279` | Boot readiness = 600 x tick + `Sleep(10)`, a hard 6 s budget. | Backend signals ready, or loop to a wall-clock timeout from settings. |
| ALT-16 | `hdue_houdini.py:46, 55, 283` | Shipped module defaults to repo paths (`out/houdini-22.0.466/session`, `build/houdini-22.0.466/Release/...`). | Plugin path from the runtime config or `PlugRegistry`; logs under `$HOUDINI_TEMP_DIR`. |
| ALT-17 | `hdue_houdini.py:249-280`, `hdue_houdini_test.py:274`, `delegate.cpp:1113` | Startup dispatches 11 test modules; a test enables ID AOVs via `os.environ['HDUE_VIEWPORT_ID_AOVS']`. | Tests launch via their own `123.py` / `importlib`; ID AOVs become a render setting. |
| ALT-18 | `delegate.cpp:259`, `usd_render.cpp:400, 403`, `delegate.cpp:204, 266` | Both boot paths hard-code the map `/Game/HdUETest` and assemble `-RenderOffscreen`/`HDUE_SHOW_WINDOW`/`HDUE_UE_ARGS` separately (already differ on `-stdout`). | Map in the runtime config; one `bootArguments()` in `src/client`. |
| ALT-19 | `usd_render.cpp:409-414` | Two renderer plugin names by `#if HDUE_HOUDINI`, hard-coded in host tool, `UsdRenderers.json`, HDAs. | One id, separated by `PXR_PLUGINPATH_NAME`. |
| ALT-20 | `delegate.cpp:1513-1526`; `scripts/usdview_hdue.py` | usdview needs a custom launcher that owns UE (third ownership mode). | Always self-host; stock usdview then works with `PXR_PLUGINPATH_NAME` only. |
| ALT-21 | `scripts/render_gallery.py:160-166` | 20 x 0.5 s retry around `os.replace` for locked PNGs. | Fail fast or write a fresh name. |

## Wasted work (EFF)

| ID | Where | Problem | Fix |
|----|-------|---------|-----|
| EFF-05 | `hdue_materialx.py:58-60` | Re-hashes every embedded texture on every material sync, which runs every frame change. | Cache `(uri, hda mtime) → digest`. |
| EFF-06 | `hdue_houdini.py:132, 136, 291` | `_settings_values(node)` evaluated twice per tick (hundreds of `parm.eval()`). | Once per tick, or a `ParmTupleChanged` callback dirty flag. |
| EFF-07 | `hdue_houdini.py:153, 527` | `render_options(stage)` recomputed every tick. | Only when the stage key changes. |
| EFF-08 | `delegate.cpp:1299`, `Bridge.cpp:713` | AOVs allocate and zero 33 MB per AOV per frame; UE side steals the spare buffer. | `takeSpare` as the beauty does; a spare AOV buffer in the session. |
| EFF-09 | `delegate.cpp:340-349, 429, 469, 661` | `setting<VtDictionary>` copied per dirty prim under the settings mutex. | Snapshot once per `prepareScene` (or `shared_ptr<const VtDictionary>` like `wetBindings`). |
| EFF-10 | `delegate.cpp:1264-1269` | Motion-blur accumulate then divide: two serial full-frame passes per sub-frame. | One fused parallel loop. |
| EFF-11 | `delegate.cpp:466, 552, 618, 1314, 1344`; `prims.cpp:363, 755` | `TfGetenvBool` per prim/request (see SIM-08). | `static const bool`, as lines 68, 76, 1113 do. |
| EFF-12 | `delegate.cpp:1128-1129, 1333-1337, 1348-1349, 1436, 954` | Same settings fetched several times per Execute (mutex + `TfToken` construction + string copy). | Static tokens; one settings snapshot per Execute. |
| EFF-13 | `Bridge.cpp:1135, 1031, 768` | `FlushRenderingCommands()` on every scene apply and material parameter edit. | Cancel in-flight readbacks by generation; flush only when releasing resources they use. |
| EFF-14 | `Bridge.cpp:337-339, 1245-1247` | Two back-to-back flushes around one reset. | Enqueue, flush once. |
| EFF-15 | `hdue_op_textures.py:54-60, 102` | `opdef:` re-extracted every `prepare_stage`; COP exports written then read back to hash. | Cache on mtime; hash while writing or key on `(uri, stamp)`. |
| EFF-16 | `color_management.cpp:79-86` | Two filesystem calls per float texture per material pack. | Memoize `(file, mtime) → path`. |

## Simpler forms (SIM)

| ID | Where | Problem | Fix |
|----|-------|---------|-----|
| SIM-02 | `delegate.cpp:997-~1482` | `Pass::_Execute` is ~485 lines (resize, submit, poll, AOVs, motion blur, denoise, camera deferral, request, tracing). | Extract `pollFrame`, `scheduleAovs`, `buildRequest`, `submitRequest`; state already lives on `Pass`. |
| SIM-03 | `Bridge.cpp:449` (`Draw`, ~224 lines) | View family, post-process, debug views and readbacks in one function. | `BuildViewFamily`, `ApplyPostProcess`, `EnqueueReadbacks`. |
| SIM-04 | `prims.cpp:240` (`Mesh::Sync`, ~188 lines) | Points, topology, primvars, wet masks, material, motion in one Sync. | One function per dirty bit (also enables HYD-07). |
| SIM-05 | `delegate.h:162-166`, `delegate.cpp:835, 1174, 1332, 1435, 1453` | Five counters for one idea; `submittedRequestSettingsRevision` written, never read; `~uint64_t(0)` sentinel. | Delete the unread field; a dirty bool, one settings counter, one monotonic generation, a `forceRequest_` flag. |
| SIM-06 | `Bridge.cpp` (26 sites, e.g. 1053, 1058, 1086) | `if (CheckReady() != HDUE_OK) return CheckReady();` plus the same session/`struct_size` checks. | `if (auto r = CheckReady()) return r;` or one `GUARD(Handle, Ptr)`. |
| SIM-07 | `delegate.cpp:434-640`, `delegate.h:143-147` | `prepareMaterials/Meshes/Lights` repeat the same drain loop over four dirty sets. | One `drain(dirtySet, map, fn)` helper. |
| SIM-08 | `delegate.cpp:76, 1062, 1314, 1478, 1693`; `prims.cpp:755` | Seven `HDUE_TRACE_*` env vars, raw `fprintf(stderr)` next to the `trace()` sink. | One `HDUE_TRACE=scene,pass,timing` bitmask read once, all through `trace()`. |
| SIM-09 | `delegate.h:132-147` | Five mutexes + a recursive one, each guarding a field or two. | One `uiMutex` for UI-read fields; plain settings mutex unless recursion is needed (verify threading first). |
| SIM-10 | `delegate.h:145, 150` | `geometryRevisions` and `erasedMeshes` both answer "changed since submit". | One submitted hash per mesh. |
| SIM-11 | `delegate.cpp:838-839` | Comment for `executeQueued_` sits on `skippedIdSize_`. | Move it. |
| SIM-12 | `hdue_houdini.py:69, 73, 116, 181, 214, 233` | `closed` and `running` always checked together. | One `running`. |
| SIM-13 | `hdue_houdini.py:58-66`, `hdue_materialx.py:15-27` | Paired last/current caches for textures, materials, options; `PreparedMaterials` dedupes again. | One `pushed` dict by kind; let `PreparedMaterials` own material dedupe. |
| SIM-14 | `Bridge.cpp:1062-1065, 1121` | Two ways to read the same cvars. | One `CVarInt()` helper. |
| SIM-15 | `hdue_houdini_test.py:225-230` | Pixel counts by nested Python loops next to a numpy array. | Vectorized masks. |

## Reuse an existing helper (REU)

| ID | Where | Problem | Fix |
|----|-------|---------|-----|
| REU-01 | `lighting_api.py:4`, `surface_api.py:3`, `material_api.py:4`, `residency_api.py:4` | Re-declare `U32, U64, Result`. | `from hdue_runtime import ...` as `asset_runtime.py:3` does. |
| REU-02 | `residency_api.py:16-31` | Hand-written checks and raw `CFUNCTYPE`. | `runtime.check` / `runtime.bind` like the other four. |
| REU-03 | `generate_materialx_samples.py:15`, `generate_normal_udim_stage.py:13`, `generate_substrate_textures.py:15`, `generate_residency_stage.py:11` | Four hand-rolled PNG writers. | One `write_png()`. |
| REU-04 | `compare_pfm.py:12`, `preview_pfm.py:14-19`, `tests/substrate_texture_render.py:63-66`; writers in `hdue_houdini_test.py:88-91`, `hdue_viewer.py:116` | Three PFM readers, two writers. | Import `compare_pfm.read`; one `write_pfm`. |
| REU-05 | `prepare_renderer_settings.py:16-20`, `prepare_substrate.py:35-39`, `hdue_materialx.py:34-38` | Same fake usdview `View` class three times. | One shared class. |
| REU-06 | `build_render_settings_hda.py:88-100, 90`, `build_substrate_hda.py:95-105, 96`, `generate_render_settings.py:467` | `relaunch()` and `.local/paths.json` reading duplicated; fallback keys drift. | One Python `local_paths()` twin of `Get-HdUELocalPath`. |
| REU-07 | `build_render_settings_hda.py:100`, `build_substrate_hda.py:144` | Two HDA help-page builders. | One help-card builder. |
| REU-08 | 11 `python3.13libs/*_test.py`, `tests/houdini_baseline.py:10` | Same `start(host)` / run / capture / flipbook boilerplate in each. | A base `Test` and `flipbook()` in `hdue_houdini_test.py`. |
| REU-09 | `asset_test.py:15`, `glass_test.py:16, 21`, `opacity_test.py:16, 20`, `texture_test.py:68`, `residency_test.py:38`, `materialx_test.py:40` | Per-test `image()`/`material()` wrappers. | Default `previous` in `SmokeTest.wait_image`; `SmokeTest.material_stats()`. |
| REU-10 | `asset_controls.py:27-30`, `glass_controls.py:48-50`, `materialx_controls.py:38-68`, `substrate_library.py:31` | Parallel dirty-flag control classes. | One base class (check first). |
| REU-11 | `backend_smoke.cpp:18-36, 74`, `usd_render.cpp:39-55` | Identical `pump()`, `check()`, window proc. | Shared header in `src/host`. |
| REU-12 | `NativeAssets.cpp:17`, `VirtualTextureResidency.cpp:34-37`, `Bridge.cpp:256-258` | FString → `char[512]` copied three ways (Bridge relies on zeroed memory for NUL). | Promote `Message()` to a shared private header. |
| REU-13 | `Apply-ResidencyPatch.ps1:5`, `Build-Backend.ps1:10`, `Import-MaterialX.ps1:15`, `Prepare-Content.ps1:7` | Engine 5.8.4 pin parsed in four places. | `Assert-HdUEEngineVersion` in `Local-Paths.ps1`. |
| REU-14 | `generate_lighting_stage.py:42`, `generate_normal_udim_stage.py:38`; `substrate_graph.py:99, 223`, `usd_materialx_graph.py:49`, `hdue_substrate.py:65` | Repeated UsdPreviewSurface authoring and graph-walk closures (unverified beyond names). | One stage helper; one walker. |

## Overlaps with earlier reports

ALT-02 = HOU-02 · ALT-04 ⊃ HOU-04 · ALT-07/ALT-08 relate to COL findings in 03 · ALT-10 = UE-01's code side · EFF-11 = SIM-08.
