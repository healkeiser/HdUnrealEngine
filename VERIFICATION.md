# Verification

This document covers repository-owned code and synthetic fixtures. Local configuration, private projects, separately supplied scenes, runtime profiles, and their evidence are excluded from version control and public documentation.

## Environments and evidence

Validation targets Windows x64, UE 5.8.4, DirectX 12 hardware ray tracing, OpenUSD 26.03 for standalone/usdview, and Houdini 22.0.466 with bundled USD 26.05/Python 3.13. The delegate builds are ABI-isolated. All Python execution uses `uv`.

Each rendering run must create fresh images, metrics, logs, and `verified.json` in an ignored output directory. Exit code zero is insufficient. One UE initialization per process and final STOPPED state are required.

## C/C++ contracts

Each SDK build has four native tests covering ABI layout, render-buffer snapshots, picking, and GeomSubset partitioning. These have passed for both builds.

```powershell
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build/houdini-22.0.466 -C Release --output-on-failure
```

Coverage includes structure sizes, immutable AOV publication, repeated Map/Unmap, pointer lifetime, concurrent publication, reallocation rejection while mapped, finalization, row orientation, scalar depth, convergence, picking bounds, clipping, backfaces, transformed geometry, negative/nonuniform scales, and BVH traversal.

`tests/geom_subsets.cpp` checks quad-to-triangle ownership, UV/normal preservation, unassigned faces, subset removal, invalid/overlapping faces, native mesh slot preservation, area-weighted normals, shared boundary normals, and degenerate geometry. The viewport material descriptor also has fixed C layout assertions.

## Native rendering and Hydra

`Test-Backend.ps1` validates a real UE runtime in a hidden Win32 host: World/RHI readiness, readback, 64 Path Tracer samples, camera and resolution changes, repeated sessions, invalid calls, and shutdown.

`Test-Hydra.ps1 -SelfTest` validates the synthetic scene through the delegate: transforms, vertices, topology, materials, lights, visibility, deletion, time, native fixtures, Lumen toggling, and delegate re-creation.

`Test-Hydra.ps1 -InputUsd samples/lighting/lighting.usda -LightingTest -Mode both` exercises the owned HDR environment and 30-light fixture. Two delegate cycles produced 20 finite HDR images, including environment exposure, rotation, tint, visibility, restoration, deletion, rect dimensions, temperature, and normalization changes. Render-thread diagnostics confirmed active MegaLights for real-time frames and its absence in Path Tracer. All Path Tracer captures reached 64 samples after environment preparation. The native and generic Hydra smoke regression also passed, with normalized HDR RGB RMSE of 0.0849% for this run.

The lighting extension has fixed C layout assertions and rejects invalid descriptors. The Solaris `lighting` test kind verifies real compositor output, HDR range, readiness, Y-up/meters, thread/session/size rejection, environment edits, and 64-sample Path Tracer recovery. `tests/houdini_embedded_materials.py` also extracts a repository-owned HDRI from a temporary HDA and resolves a composed DomeLight without editing the source.

The Solaris lighting run produced five converged renderer states and two compositor captures, then shut UE down normally. A separate native MaterialX/GeomSubset regression also passed with the updated runtime. An EXR converted from the owned RGBE fixture retained identical decoded source pixels and rendered in both modes. Its 64-sample environment-only background comparison was below 1% relative RMSE; geometry includes independent Monte Carlo noise and is not asserted pixel-identical between the two runs.

`Verify.ps1` combines these checks with native-versus-Hydra HDR comparison. The historical synthetic comparison achieved normalized RGB RMSE of 0.0778%, below the 1% threshold. This is a fixture-specific result, not a general equivalence guarantee.

## usdview

`Test-Usdview.ps1` covers native presentation, convergence, edits, resizing, camera navigation, renderer switching, stage reopening, selection, and shutdown. The synthetic regression includes 24 captured frames and ten picking operations.

`Test-Camera.ps1` measures continuous interaction and verifies coalescing rather than unbounded queuing. `Test-BufferPerf.ps1` measures CPU AOV map behavior. Performance depends on resolution, hardware, content, cache state, and readback cost.

The custom picking path does not imply standard Hydra ID AOV or selection-outline support.

## Solaris native viewport

`Test-Houdini.ps1` runs headless LOP/proxy checks and an isolated GUI verification. The synthetic viewport suite captures ten renderer states and six compositor images.

It covers Lumen, 64-sample Path Tracer, Y-up/meters normalization, pan/orbit/dolly, transform edits, accumulation reset, native assets on Xform/Mesh prims, parent transforms, resizing, matching color/depth frames, delegate re-creation, owner-thread checks, stale/short/unsupported depth calls, and engine lifetime.

Real scene depth uses a per-ViewFamily extension and GPU readback. Color and depth share frame ID and generation. Solaris receives linear axial depth in stage units. Constant placeholder depth is not used.

Selection state can persist without resetting accumulation, but the standard selection outline is not implemented. Verification records that limitation explicitly.

## Coordinates and proxies

- `tests/scene_space_test.py`: Y/Z source axes, meters/centimeters, animated intermediate transforms, reset stacks, camera optics, negative/nonuniform parent scale, and native dimension preservation.
- `tests/houdini_cook.py`: real LOP cooking, canonical output metadata, generated proxies, settings edits, source-layer preservation, repeated preparation, reference removal, purpose restoration, and reactivation.
- `tests/asset_controls_test.py`: shared USD proxy behavior.

Headless fixtures create temporary runtime configurations and do not require a private catalog.

## COP textures

```powershell
.\scripts\Test-HoudiniOpTextures.ps1
```

Headless tests cover resolution, unchanged-cache reuse, immutable replacement, source preservation, cleanup, node deletion/re-creation, time dependence, basic MaterialX fallback, legacy COPs, raw values, output-port selection, and invalid inputs.

The GUI suite captures seven renderer states and five native viewport images. It checks red/blue images in both modes, automatic COP edits without a forced LOP cook, frame-dependent changes, and a basic MaterialX Image-to-Standard Surface connection. Applicable Path Tracer updates reach 64 samples.

This validates base color rather than arbitrary graphs, dynamic normal maps, COP UDIM collections, or alpha-to-opacity wiring.

## MaterialX and virtual textures

Owned fixtures cover Standard Surface, OpenPBR, metal, coat, anisotropy, transmission, subsurface, textures, and inline USD graphs. Full native validation uses the cooked Substrate registry in usdview and Solaris.

`tests/houdini_embedded_materials.py` creates its own temporary HDA and embedded image. It checks byte-preserving extraction, immutable updates, resolver-independent signatures, reference rebasing, different internal/composed paths, session recreation, binding removal, instance expansion, and unchanged sources. No vendor asset is included in this test.

The optional Solaris `materials` test kind consumes a caller-selected USD and cooked library. It checks native material/texture statistics, Lumen and 64-sample Path Tracer images, real compositor captures, ABI/thread/session validation, subset rebinding/removal/restoration, and normal engine shutdown. Input assets and evidence remain local and ignored.

The `references` test kind references a caller-selected prepared asset downstream of `hdUE_settings` with instanceability enabled. It checks remapped native MaterialX bindings and textures, parent-transform changes in real-time and 64-sample Path Tracer output, and changed depth pixels to distinguish geometry movement from sampling noise. Owned headless HDA tests also cover final-output expansion, unchanged upstream instancing/source bytes, live transform recooks, appending edits to generated output without cycles, and adopting/cleaning a saved preparation node.

These checks establish binding and converged finite images; they do not assert pixel-identical subsurface scattering between Lumen and Path Tracer.

`Test-MaterialX.ps1` checks public parameter edits, material changes, image comparisons, invalid updates, source preservation, renderer re-creation, and shutdown. Graph validation rejects cycles, unknown nodes, incompatible connections/types, missing images, nonfinite values, and unsupported volume/displacement outputs.

Synthetic normal/UDIM tests cover raw normals, strength, texture replacement, tile coordinates, malformed/missing tiles, dependency hashes, and material parameter compatibility.

`Test-Residency.ps1` keeps Path Tracer at zero samples until all required pages and mappings are ready. It checks capacity/timeout failures, lock release, recovery, edits during preparation, mode changes, and delegate re-creation. The generated large fixture requires 10,922 pages/mappings; insufficient capacity must fail without accumulating low-resolution images.

## Reference-safe sample

`scripts/generate_solaris_materialx_sample.py` creates `samples/materialx/showcase_solaris.usda`. It normalizes the scene, moves all roots below one default prim, repairs relationships/connections, and preserves relative asset paths.

The generator verifies 21 mesh bindings and twelve materials after reference composition. Native Solaris captures confirmed colored Preview Surface fallbacks in Lumen and 64-sample Path Tracer mode. The generator repairs reference encapsulation; full native MaterialX rendering uses the separately prepared/cooked registry described above.

## Karma comparisons (husk)

The README gallery comes from husk renders of synthetic scenes with `--renderer HdUEHoudiniRendererPlugin` (Path Tracer, 128 samples) and `--renderer BRAY_HdKarma`, 480x270, on Houdini 22.0.368. The scene generator and comparison scripts are kept out of the repository; the measured results were:

- Lens shift: the luminance-weighted image centroid is (238.5, 211.3) for hdUE and (238.9, 211.7) for Karma. Before the sign fix, hdUE shifted the other way.
- Orthographic: centroid (240.0, 129.8) against (240.0, 130.5).
- Depth of field (f/0.8, focus 9 m, USD lens units): same in-focus plane and blur growth as Karma; the realtime mode uses UE's post-process approximation instead.
- Exposure 1: the beauty doubles (mean 0.203 to 0.405) and matches Karma's framing.
- Light calibration with a bound, fully rough Preview Surface: mean 0.05755 (hdUE) against 0.05759 (Karma). Glossier materials (roughness 0.6 to 0.9) come out 1.3 to 1.4x brighter in hdUE.
- Motion blur: transform blur matches Karma's extent; point and instance `velocities` blur in hdUE, which Karma left sharp with default settings.
- `primId` / `instanceId`: five prims and per-instance indices on a scene with curves, points, and a PointInstancer whose prototypes are a cube, points, and a curve.
- Volumes: a SOP VDB fog and a native Houdini volume, saved by the USD Render ROP (`.vdb` and `.bgeo.sc`), render in place within about 13% of Karma's brightness in realtime. The same SOP volumes, unsaved (`op:` paths), render in the Solaris viewport.

## Post-processing and husk throughput

- Post-processing (husk, realtime and Path Tracer): with `hdUE:tonemapper` off, `ColorSaturation` leaves the image unchanged; with it on, saturation 0 gives a grey image (zero chroma), bloom and vignette apply, `AutoExposureBias` 3 saturates the image, and Lumen and Path Tracer agree (mean 0.255 against 0.259). A post-process material loads as a blendable, and an unknown property logs a warning without failing the frame. The Render Settings LOP authors the matching `hdUE:*` attributes.
- Throughput (RTX 4090, 1080p, simple scene, 16 warmup frames, one husk call for 24 frames): 0.3 s per Lumen frame and about 3 s of UE startup, against 5.3 s per frame before husk-owned ticking and readback throttling. Output images were unchanged by those changes (identical means). A 480x270, 64-sample Path Tracer frame went from 25 s to 6 s including startup.
- Deformation blur: time-sampled `points` blur over the same extent as Karma; per-point `velocities` on a mesh blur in hdUE and were left sharp by Karma with default settings.

## Publication checks

`tests/test_repository_scope.py` checks publication candidates for machine-specific absolute paths, non-English documentation, local/private directories, external project descriptors, and symlinks escaping the repository. Installation paths and runtime profiles must not be embedded in tracked defaults or sample metadata.

The publication cleanup passed the three scope checks, PowerShell parsing, Python parsing, local-setting precedence tests, headless LOP/proxy/coordinate/COP tests, the ten-state Solaris viewport regression, and the seven-state COP viewport regression. A same-shell launch/exit/COP sequence also confirmed that previously unset environment variables remain unset after the launcher returns.

```powershell
uv run --python 3.11 tests/test_repository_scope.py
.\tests\local_paths.ps1
```
