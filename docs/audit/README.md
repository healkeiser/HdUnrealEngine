# hdUE audit (2026-10-29)

An audit of hdUE (`feature/package`, `e8fe1f4`) against the OpenUSD/Hydra, SideFX Houdini 22 and Unreal Engine 5.8 documentation and the public source of reference delegates (HdEmbree, HdStorm, HdArnold, hdPrman, hdCycles). Every finding cites the code (`path:line`) and the document it is measured against; points that could not be checked by reading code or running a test are marked *unverified* or *inferred* in the reports.

| Report | Scope | Findings |
|--------|-------|----------|
| [01 Hydra delegate](01-hydra-delegate.md) | `HdRenderDelegate`, render pass, buffers/AOVs, camera, geometry, instancing, motion blur | 18 (1 Critical, 3 High) |
| [02 Houdini / Solaris](02-houdini-solaris.md) | `UsdRenderers.json`, HDAs, parameter files, Python host, husk, USD Render ROP | 18 (3 High) |
| [03 Materials, lights, color](03-materials-lights-color.md) | UsdPreviewSurface, MaterialX, UsdLux units, IES, OCIO, the Lumen brightness gap | 20 (1 Critical, 4 High) |
| [04 Unreal backend](04-unreal-backend.md) | UE as a library, Path Tracer, Lumen, PSOs, Nanite, **EULA** | 18 (1 Critical, 4 High) |
| [05 Build, packaging, quality](05-build-packaging-quality.md) | CMake, Houdini package, release zip, CI, tests, doc accuracy | 24 (4 High) |

## Act first

Spot-checked against the code while consolidating; all held.

1. **Licensing: pull or privatize the public release zips (UE-01, UE-02, UE-03, PKG-01).** v0.1.0 and v0.2.0 are public GitHub releases (~950 MB each). The backend DLL contains Developer ("Engine Tools") code: `bBuildDeveloperTools = true` (`unreal/HdUEBackend/Source/HdUEBackend.Target.cs:16`) and `NaniteBuilder` (`HdUEBackend.Build.cs:10`). EULA §6(d) restricts distribution of such products to Fab or forks of Epic's UnrealEngine GitHub network. The zip also carries no EULA end-user terms, no Epic notices and no third-party licences, next to a bare MIT `LICENSE`. Not legal advice: confirm with Epic before the next release.
   *Status (2026-10-29):* both zip assets deleted, release notes say why; `Package-Houdini.ps1` now ships `LICENSE-PACKAGE.txt` (end-user terms), `THIRD_PARTY_NOTICES.md` and the engine's third-party licenses, drops the smoke exe, import libraries and debug layers, and refuses to package Developer code without `-UnrealLicenseesOnly`. Open: removing the Developer code itself, and the seat status of the developing entity (§6(a)).
2. **Crash risk in the Solaris viewport (HYD-01).** The Execute posted to the UE thread keeps a raw `HdCamera*` and render buffer pointers (`src/hydra/delegate.cpp:986-1000`) and reads them after Houdini may have synced or deleted them. Copy the camera values and own the buffers in the snapshot.
3. **IES lights ~1000x too dark (LGT-01, LGT-03, UE-12).** `bUseIESBrightness=false` (`Lighting.cpp:214`) discards UE's candela peak, and `TextureMultiplier` is always 1, so `normalize` is a no-op. Multiply by the profile's brightness. One-line fix; explains the 0.00 in the gallery.
4. **Purpose and render tags ignored (HYD-02).** `executeOnce` drops its tags (`delegate.cpp:1060`): proxy/guide geometry renders, toggles do nothing.
5. **Silent black outputs (HYD-03, HYD-06).** A `color3f` beauty (UsdRenderVar's default type) or any AOV with a channel-count mismatch is dropped without a message. `lpe:` vars get a copy of the beauty.
6. **husk robustness (HOU-03, HOU-06, UE-05).** UE cannot restart after the last delegate goes away outside the GUI (`delegate.cpp:1567-1579`), `huskErrorStatus` is never set so failed frames exit 0, and frames can be written with placeholder materials while PSOs compile.
7. **Path Tracer motion blur wastes samples (UE-04).** The seed is locked (`Bridge.cpp:442`) and each sub-frame restarts the view state: 512 spp over 16 sub-frames is 32 spp on static pixels. Offset the seed per sub-frame as Movie Render Graph does.
8. **Cost on every Houdini session (HOU-01).** The package loads both DLLs and runs a 16 ms timer and a 2 s disk write even when hdUE is never picked. Start the host when a viewport selects hdUE.
9. **Houdini build pinning (BLD-01).** The plugin is built against one 22.0 daily build but loads into any 22.0.x without an ABI check, and the repo disagrees on which build (22.0.466 vs 22.0.368).
10. **Tests that lie or don't run (TST-01, TST-02, TST-03).** `Verify.ps1` writes `passed = $true` and constant counts; 23 Python tests that pass outside Houdini are not in CI; no image baselines, which is how a black IES scene shipped in the gallery.

## Then

- **Lumen 0.6x Karma (LGT-02).** Units and exposure match the Path Tracer; the deficit is indirect light. Hypothesis: zero-thickness Cornell walls and a light 1 mm under the ceiling starve Lumen's surface cache. Test with the Lumen Scene debug view and a light moved 5 cm down before changing code. Also: offline Lumen runs at game quality (UE-06, UE-08).
- **Viewport/husk parity (HOU-02).** The cooked material library, live Substrate, `hdue:asset` proxies and `op:` images exist only in the GUI host. Anything that changes the picture must live in USD or the delegate.
- **Geometry correctness (HYD-04, HYD-07, HYD-10).** Subdiv tags and face-varying topology, dirty-bit aware Sync (transform edits re-triangulate and re-upload today), curve widths/wrap/normals.
- **Framing (HYD-05 = HOU-08).** Data window, pixel aspect and framing are ignored: crops, overscan and husk tiling render wrong.
- **Materials and lights robustness (MAT-01, LGT-05).** One missing texture or one unsupported light attribute fails the whole batch; fall back per spec and warn instead. Light linking (LGT-04) is the largest missing feature.
- **Color (COL findings).** With UE's tonemapper on, a display-referred image is published as scene-linear and Houdini transforms it again.
- **Denoising (UE-07).** `r.PathTracing.Denoiser 0` is forced on every request, so UE's own denoisers never run.
- **Package hygiene (PKG-02, PKG-03 = HOU-05, HOU-12).** Ship a Test (not Development) runtime without `.lib`/validation layers/test modules; move logs and UE `Saved/` out of the install folder; stop putting `scripts/` first on `sys.path`.

## Docs out of date

`docs/architecture.md` says ABI 7 (`bridge.h:22` is 8) and calls shared GPU frames future work; README lists displacement as "Not yet" while `rendering.md` documents it; README and `houdini.md` say UE runs on the UI thread and that `uiready.py` replaces Houdini's own; the selection highlight is credited to `primId`. Full list: [05 DOC findings](05-build-packaging-quality.md) and HOU-13.

## Duplicates across reports

LGT-01 = UE-12 (IES) · UE-02 ≈ PKG-01 (licence texts) · HYD-05 = HOU-08 (data window) · PKG-03 = HOU-05 (logs in install dir) · HOU-13 ⊂ DOC findings.
