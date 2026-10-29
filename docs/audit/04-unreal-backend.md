# Unreal Engine backend audit

## Scope & sources

**Area:** the embedded UE 5.8.4 backend compared with Epic's documentation and practice. Files covered:

- `unreal/HdUEBackend/Source/**` (excluding Intermediate/Saved/Binaries), `HdUEBackend.uproject`, `Config/*.ini`
- `include/hdue/*.h`, `src/client/*`
- `scripts/Setup-Engine.ps1`, `Build-Backend.ps1`, `Stage-Runtime.ps1`, `Apply-ResidencyPatch.ps1`, `Record-PipelineCache.ps1`, `Prepare-Content.ps1`, `Package-Houdini.ps1`, `packaging/houdini/*`
- The built release folder `out/package/hdUE-v0.2.0-houdini22.0-win64/` and the public GitHub release `v0.2.0`. `gh release view` shows `hdUE-v0.2.0-houdini22.0-win64.zip`, 954,596,768 bytes, uploaded 2026-10-29.

**How claims were checked:**

- **Code claims** were checked against the repository at `feature/package` (`e8fe1f4`).
- **Engine behaviour** was checked against the pinned engine source at `H:/hdUE/UE_5.8.4_src`. Paths written as `Engine/...` are in that tree.
- **Doc claims** cite the Epic page that was actually fetched. Items marked **(unverified)** could not be confirmed from a fetched page or from code.

**Epic sources used:**

- Unreal Engine EULA, https://www.unrealengine.com/eula/unreal. Fetched 2026-10-29. WebFetch got HTTP 403, so the page was downloaded with curl. Engine `LICENSE.md` points to the same page.
- Path Tracer, https://dev.epicgames.com/documentation/en-us/unreal-engine/path-tracer-in-unreal-engine
- Lumen Technical Details, https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
- Nanite, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- PSO Precaching, https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine
- Virtual Shadow Maps, https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine
- IES Light Profiles, https://dev.epicgames.com/documentation/en-us/unreal-engine/using-ies-light-profiles-in-unreal-engine
- Build Configurations, https://dev.epicgames.com/documentation/en-us/unreal-engine/build-configurations-reference-for-unreal-engine
- USD in UE, https://dev.epicgames.com/documentation/en-us/unreal-engine/universal-scene-description-in-unreal-engine
- Movie Render Queue / Movie Render Graph:
  - https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/MovieRenderPipelineCore/UMoviePipelineAntiAliasingSettin-
  - https://dev.epicgames.com/documentation/unreal-engine/setting-up-motion-blur
  - https://dev.epicgames.com/documentation/unreal-engine/transitioning-to-the-movie-render-graph-from-movie-render-queue-in-unreal-engine
- UE as a Library. The only Epic page found is for 4.27: https://docs.unrealengine.com/4.27/SharingAndReleasing/BuildAsALibrary
- Licensing overview: https://www.unrealengine.com/license and https://www.unrealengine.com/en-us/custom-licensing

**MRQ behaviour came from engine source.** Epic's MRQ settings pages returned empty bodies, so MRQ behaviour is cited from the engine's MRQ source, for example `Engine/Plugins/MovieScene/MovieRenderPipeline/.../MoviePipelineGameOverrideSetting.cpp`.

> **This is not legal advice.** The licensing findings (UE-01 to UE-03) are a technical reading of the public EULA text against what the package contains. Before distributing further, get a decision from Epic (custom-licensing / licensing@epicgames.com) or a lawyer.

## Summary

| ID | Severity | Finding | Effort |
| --- | --- | --- | --- |
| UE-01 | Critical | The public v0.2.0 release ships "Engine Tools": the NaniteBuilder Developer module and MeshUtilities Developer source compiled into the backend DLL. EULA §6(d) allows such a product only through Fab or a fork of Epic's GitHub UnrealEngine network. | M–L |
| UE-02 | High | The release zip has no EULA-required end-user terms (§4(c)) and no Epic notices or third-party licences (§7(a), §7(b)). A bare MIT `LICENSE` sits at the root, so the whole package appears to be MIT. | S |
| UE-03 | High | The shipped runtime is a separable UE player: a public C ABI, a `.uproject`, the Engine tree and a smoke `.exe`. This sits badly with §4's "inseparable part of the Product" and §4(c)'s ban on letting end users build on the Licensed Technology. Seat/royalty status of the developing entity is undocumented. | M |
| UE-04 | High | Motion-blur sub-frames in the Path Tracer reuse the identical random sequence: locked seed and a fresh ViewState per sub-frame. Static regions then get spp/N worth of noise, not spp. | S |
| UE-05 | High | Offline frames can count as "converged" while PSOs are still compiling. `r.PSOPrecache.ProxyCreationStrategy=2` draws those proxies with the default material, and nothing waits for precache or bundled-cache completion. | S |
| UE-06 | Medium | No equivalent of MRQ "Game Overrides" for husk: no Cinematic scalability, no texture-streaming flush, no VT tile flush, no `r.D3D12.GPUTimeout 0`. | S |
| UE-07 | Medium | UE's path-tracer denoiser is forced off on every request (`r.PathTracing.Denoiser 0`) and no denoiser plugin is enabled. The documented "UE OIDN plugin left the image unchanged" conclusion is probably caused by this. Epic's default NNE denoiser was not evaluated. | S |
| UE-08 | Medium | Lumen final-frame quality is left at game defaults: Final Gather, Scene Detail, Scene Lighting Quality and Hit Lighting are not raised for offline. Generated meshes get a fixed 12 Lumen cards. Lumen reaches 0.54–0.83× of Karma brightness in the gallery. | S |
| UE-09 | Medium | The released backend is a Development build. Epic positions Shipping (or Test) for release. | S |
| UE-10 | Medium | The engine patch adds a friend to a private VT header. The public `RequestVirtualTextureTiles` / `LoadPendingVirtualTextureTiles`, which MRQ itself uses, may cover most of the need. | M |
| UE-11 | Medium | Virtual Shadow Maps are off, so directional shadows use the legacy path even on Nanite meshes. Epic says VSMs are designed for Nanite and are on by default in new projects. | S |
| UE-12 | Medium | IES: `bUseIESBrightness=false` with UE's peak-normalized profiles is the likely cause of the 60× darker IES result. | S |
| UE-13 | Low | Dead or ineffective config: `r.UsePreExposure=False` (removed in UE5), `r.GenerateMeshDistanceFields` (runtime meshes get no SDF), Substrate 128 B/px. | S |
| UE-14 | Low | The package carries plugins and binaries hdUE does not need: MetaHuman*, LiveLink, ControlRig, Datasmith, Vulkan validation layers, `d3d12SDKLayers.dll`, Ogg/Vorbis/XAudio, editor content. Total 1.3 GB. | S |
| UE-15 | Low | AOVs are separate rasterized re-renders with no Object ID/Cryptomatte and no Path Tracer AOVs. UE ships an Object ID (Cryptomatte) pass in `MoviePipelineMaskRenderPass`. | M |
| UE-16 | Low | 8192 px cap, and no high-resolution tiling. | M |
| UE-17 | Low | Lifecycle is built on undocumented UELibrary behaviour: a game-thread-id swap at static destruction, no re-initialization, and the API is documented only for 4.27. | S |
| UE-18 | Low | Content is cooked by the launcher editor (CL 58902052) and run by a source build (CL 0). The PSO cache works only because `GameVersion` is pinned. | S |

---

## UE-01 — Release includes Engine Tools (EULA §6(d)) — Critical

**What we do**

- `Source/HdUEBackend.Target.cs:16` sets `bBuildDeveloperTools = true` with the comment "NaniteBuilder (a Developer module) builds Nanite data for the meshes hdUE creates at runtime".
- `HdUEBackend.Build.cs:10` links `NaniteBuilder`, which lives in `Engine/Source/Developer/NaniteBuilder`.
- `HdUEBackend.Build.cs:24-26` adds `MeshUtilities` private include paths and Embree.
- `MeshCards.cpp:18-19` compiles Developer source straight into the DLL: `#include "MeshRepresentationCommon.cpp"` and `#include "MeshCardRepresentationUtilities.cpp"`, both from `Engine/Source/Developer/MeshUtilities/Private`.
- `scripts/Apply-ResidencyPatch.ps1:17-27` also modifies `Engine/Source/Developer/NaniteBuilder/Private/NaniteDisplace.cpp`.
- The resulting `HdUEBackend-h22-*.dll` is inside the public GitHub release `v0.2.0` (`scripts/Package-Houdini.ps1:49-51`).

**What Epic documents** — EULA §6(d), https://www.unrealengine.com/eula/unreal:

> "If your Product includes Engine Tools, you may only Distribute it through either the Fab Marketplace or through a fork of Epic's GitHub UnrealEngine Network. 'Engine Tools' means the (i) editors and other tools included in the Engine Code; (ii) any code and modules in either the Developer or Editor folders, including in object code format, whether statically or dynamically linked; and (iii) other software that may be used to develop standalone products based on the Licensed Technology."

§5(a)(i) restates that any public Distribution of Engine Tools must go through Fab or the GitHub UnrealEngine network.

**Impact.** Clause (ii) names Developer-folder code "in object code format, whether statically or dynamically linked". Read literally, the release DLL is a Product containing Engine Tools, and a GitHub release on a public, non-UE-network repo is not one of the two permitted channels. This is the highest-exposure item in the project: Epic could ask for the release to be taken down, and the licence could be breached.

**Recommendation**

1. Take down or make private the v0.2.0 and v0.1.0 binary assets until this is resolved. Keep distributing source, which is what `docs/building.md` already describes: users build with their own engine access.
2. Remove Developer code from the runtime target. Options:
   - Build Nanite and Lumen cards only for cooked native assets.
   - Write an independent card generator. Lumen cards are a set of oriented boxes, so a simple axis-aligned 6-card generator is feasible and is what Lumen uses for skinned meshes (Lumen Technical Details: skeletal meshes "don't have offline generated cards… six cards placed around their pre-skinned bounding boxes").
   - Drop runtime Nanite for Hydra meshes. It is interactive-only anyway (`Scene.cpp:816-818`).
   - Set `bBuildDeveloperTools = false` and make the build fail if any `Developer/` module is linked.
3. If runtime Nanite or cards are essential, either distribute binaries only to UE licensees through the GitHub UnrealEngine network or Fab, or obtain a custom licence (https://www.unrealengine.com/en-us/custom-licensing).

## UE-02 — Release package lacks EULA terms and notices — High

**What we do**

- `Package-Houdini.ps1:56-57` copies `packaging/houdini/*` and the repo's MIT `LICENSE` ("Copyright (c) 2026 K240") to the package root.
- `packaging/houdini/README.txt` says nothing about Unreal Engine licensing.
- `THIRD_PARTY_NOTICES.md` is not packaged.
- The only notice file inside the runtime is `runtime/Windows/NOTICES.txt`, and it contains a single FreeType line.
- The package does ship third-party binaries:
  - `embree4.dll`, `tbb12.dll`, `tbbmalloc.dll`
  - `ispc_texcomp.dll`
  - `GFSDK_Aftermath_Lib.x64.dll`
  - Ogg/Vorbis, `xaudio2_9redist.dll`, `WinPixEventRuntime.dll`
  - `D3D12Core.dll`, `d3d12SDKLayers.dll`
  - Seven Vulkan layer DLLs

**What Epic documents** — EULA, https://www.unrealengine.com/eula/unreal:

- §4(c): end users may use the Licensed Technology only "pursuant to an end user license agreement that explicitly disclaims any representations, warranties, conditions, and liabilities related to the Licensed Technology. You may not permit your end users to incorporate any Licensed Technology into their own products".
- §7(a): "retain the copyright, trademark, and other proprietary notices"; the attribution text goes in credits.
- §7(b): third-party terms "can be found in … /Engine/Source/ThirdParty/Licenses".

**Impact**

- The package currently implies that everything in it is MIT, which is not true of the UE runtime.
- End users receive no licence restricting reuse of the Licensed Technology.
- The third-party licence texts that §7(b) incorporates are missing (Apache-2.0 for Embree and TBB, NVIDIA's Aftermath terms, Microsoft redistribution terms).

**Recommendation**

- Ship an end-user licence (`EULA.txt`) that:
  - scopes MIT to hdUE's own code;
  - carries the §4(c) disclaimer;
  - forbids extracting or reusing the UE runtime;
  - contains the §7(a) attribution.
- Generate `runtime/Windows/ThirdPartyNotices/` from `Engine/Source/ThirdParty/Licenses` for exactly the modules linked. `Manifest_NonUFSFiles_Win64.txt` lists them.
- Package `THIRD_PARTY_NOTICES.md`.
- Drop `d3d12SDKLayers.dll` and the Vulkan layers (UE-14). Whether Microsoft's Agility SDK licence permits redistributing the SDK layers is **(unverified)**; they are not needed at runtime.

## UE-03 — The runtime is a separable, reusable UE player — High

**What we do**

- The zip contains `runtime/Windows/` as a complete cooked UE project: `.uproject`, `Engine/` (743 MB), the backend DLL and `hdue_backend_smoke.exe`.
- `runtime.json` lets any host load the backend.
- `include/hdue/*.h` is a documented, versioned public C ABI to drive UE rendering from any process (`docs/architecture.md`).

**What Epic documents** — EULA, https://www.unrealengine.com/eula/unreal:

- §4: "Any Product that you Distribute that incorporates Licensed Technology must incorporate the Licensed Technology only in object code and only as an inseparable part of the Product."
- §4(c) (quoted above) bans letting end users incorporate the Licensed Technology into their own products.
- §6(d)(iii) counts as Engine Tools "other software that may be used to develop standalone products based on the Licensed Technology".

**Royalties and seats.** Free distribution falls under §4(a)(ii), products that "do not directly generate revenue", so no royalty is due. Rendered images are "Non-Engine Products" under §4(a)(i). Separately, §6(a) requires a Seat for Users developing Royalty-Free Products, unless §6(b) applies: under $1M revenue, non-commercial, or educational.

**Impact**

- A public ABI plus a standalone runtime lets third parties build their own products on the shipped UE binaries. That is the pattern §4(c) and §6(d)(iii) restrict **(interpretation; unverified with Epic)**.
- If development is done for or by a commercial entity with more than $1M group revenue, Seats are required.
- Precedents differ:
  - **Packaged UE simulators** (e.g. CARLA) ship binaries publicly. They ship a complete app with no public engine ABI and no Developer modules **(not checked against their current releases)**.
  - **Pixel Streaming** ships a packaged game.
  - **Houdini Engine for Unreal** goes the other way: a UE plugin distributed as source.
  - **Twinmotion** is Epic's own product.
  - None of these ships the engine as a host-embeddable library.

**Recommendation**

- Get written confirmation from Epic before any further binary release, describing the architecture precisely.
- Until then:
  - Treat the C ABI as internal: no stable public headers in binary releases, and an ABI check that only accepts the delegate it was built with. The exact-version check exists; make it a signed or hash pairing.
  - Remove `hdue_backend_smoke.exe` from packages.
  - Record the seat/revenue status of the developing entity in `THIRD_PARTY_NOTICES.md`.

## UE-04 — Path Tracer motion-blur sub-frames reuse the same random sequence — High

**What we do**

1. `Bridge.cpp:442` forces `r.PathTracing.FrameIndependentTemporalSeed 0` on every request.
2. Each motion-blur sub-frame is a new `RequestRender` (`src/hydra/delegate.cpp:1214-1272`). For the Path Tracer, `KeepHistory` is false, so the ViewState is destroyed and reallocated (`Bridge.cpp:417-425`) and accumulation restarts at sample 0.
3. `View->PathTracerSeedOffset` is never set.

**What the engine does**

- With the cvar at 0, `Config.LockedSamplingPattern = true` and `TemporalSeed = SampleIndex + SeedOffset` (`Engine/Source/Runtime/Renderer/Private/PathTracing.cpp:4267,4673-4685`). Its help text: "Count samples from 0 for deterministic results".
- Every sub-frame therefore draws the same sample sequence per pixel.
- MRG avoids this by giving each sample its own seed offset: `View->PathTracerSeedOffset = InCameraInfo.SamplingParams.SeedOffset` (`MovieGraphPathTracerPass.cpp:105`).
- Epic: "The most accurate results can be obtained with Movie Render Queue when Reference Motion Blur is enabled" (Path Tracer doc).

**Impact.** In anything that does not move during the shutter, N sub-frames × (spp/N) samples average N identical noise realizations. The noise therefore stays at spp/N. With the default 16 sub-frames, a 512-spp blurred frame is as noisy as a 32-spp frame everywhere except where things move. Derived from code, **not measured**. Lumen is unaffected.

**Recommendation**

- Set `View->PathTracerSeedOffset = SubframeIndex * samples_per_pixel`. This needs a sub-frame index (or seed) field in `HdueRenderRequest` or `motion_api.h`.
- Alternatively, drop the forced `FrameIndependentTemporalSeed 0` for sub-frames.
- Verify with a static-scene, moving-object render: background noise should match an unblurred render at the same total spp.

## UE-05 — Frames can converge with default materials while PSOs compile — High

**What we do**

- `Config/DefaultEngine.ini:49` sets `r.PSOPrecache.ProxyCreationStrategy=2`. In `Engine/Source/Runtime/Engine/Private/PSOPrecache.cpp:36-41` this means "2 : Use fallback material when PSOs are still compiling". The cvar is ECVF_ReadOnly.
- Convergence is purely a frame count: `Converges()` (`Bridge.cpp:91-94`) checks `FullResIndex >= warmup_frames`, 16 by default.
- The backend never queries `FShaderPipelineCache::NumPrecompilesRemaining()` or the PSO precache backlog. A grep of `Source/HdUEBackend` for PSO/Precache/ShaderPipelineCache finds nothing.

**What Epic documents** — PSO Precaching (URL above):

- Precaching does not cover everything: "Not all material shaders go through a mesh pass processor… (such as Hair, Nanite or Ray Tracing dynamic geometry updates)".
- Loading screens should check "the number of outstanding PSO precache compiles for both the bundled cache and PSO precaching".

**Impact**

- On a cold driver cache, or after a shader or driver change, a husk frame can be written after 16 warm-up frames with some meshes still drawn in `WorldGridMaterial`. Nothing flags it.
- In the viewport strategy 2 is fine. For final frames it is a correctness bug.
- It is also hard to reproduce, because the driver cache hides it on the developer machine. Epic notes: "hitches may be masked by the PSO cache built by the graphics driver".

**Recommendation.** In non-interactive mode, hold `RequestRender`, or do not count warm-up frames, until all of these are true:

- `FShaderPipelineCache::NumPrecompilesRemaining() == 0`
- the PSO precache backlog is empty (`PipelineStateCache::NumActivePrecacheRequests()`, name per 5.8 headers, **unverified**)
- `GShaderCompilingManager` reports no outstanding jobs

Report the wait as `HDUE_PENDING`. That is already the pattern used for assets and lighting (`Bridge.cpp:403-408`).

## UE-06 — No MRQ-equivalent "Game Overrides" for offline renders — Medium

**What we do**

- In offline mode the backend only raises Lumen surface-cache update rates (`Bridge.cpp:467-474`).
- Scalability stays at the project default. `docs/rendering.md` says "default Epic".
- Texture streaming is fed a view (`Bridge.cpp:513-515`), but nothing waits for it.
- Runtime Hydra textures are non-streaming (`Scene.cpp:385-395`), but native cooked assets do stream.
- VT residency is only prepared for the Path Tracer (`Bridge.cpp:434-438`).

**What Epic does.** MRQ's Game Overrides, which are on by default for renders, apply (from `MoviePipelineGameOverrideSetting.cpp`):

- lines 214-223: all `sg.*Quality=4` (Cinematic)
- lines 78-82: `r.TextureStreaming=0` / `r.Streaming.FullyLoadUsedTextures=1` / `FramesForFullUpdate=0`
- lines 91-93: `r.ForceLOD=0`, LOD biases -10
- line 110: `r.ShadowQuality=5`
- line 132: `r.D3D12.GPUTimeout 0`
- line 137: `r.Streaming.SyncStatesWhenBlocking=1`

It also flushes VT tiles before each frame with `GetRendererModule().LoadPendingVirtualTextureTiles` (`MoviePipelineRendering.cpp:682-688`). The Path Tracer doc likewise recommends MRQ "for production pipelines when producing high-quality rendered outputs".

**Impact**

- Lumen-mode husk frames render at game quality (Epic level).
- Native assets may render with low texture mips.
- Long Path Tracer dispatches at high resolution can hit the Windows TDR.

**Recommendation.** Apply an "offline profile" when not `-hdUEInteractive`, overridable through `hdUE:cvar:*`:

- Cinematic `sg.*`
- `r.TextureStreaming 0`, or `IStreamingManager::Get().StreamAllResources()` before the first counted frame
- `r.D3D12.GPUTimeout 0`
- `LoadPendingVirtualTextureTiles` in realtime mode too

Document the profile in `docs/rendering.md`.

## UE-07 — UE denoisers disabled by construction — Medium

**What we do**

- `Bridge.cpp:441` sets `r.PathTracing.Denoiser 0` on every request.
- `HdUEBackend.uproject:4` uses `DisableEnginePluginsByDefault: true` and enables no denoiser plugin.
- `docs/rendering.md:145` concludes "its Open Image Denoise plugin left the image unchanged".
- hdUE instead links Houdini's OIDN in the delegate (Houdini build only).

**What the engine and Epic say**

- In the engine, `GetPathTracingDenoiserMode` returns the cvar when it is ≥0 (`PathTracingSpatialTemporalDenoising.cpp:366-379`). Cvar 0 disables any plugin regardless of `PathTracingEnableDenoiser`.
- Path Tracer doc: "NNE Denoise is the default implementation". OIDN "runs on the CPU and is not designed for interactive denoising"; OptiX "is experimental".
- The engine has `Plugins/NNE/NNEDenoiser` (with a spatial-temporal path) next to `Experimental/OpenImageDenoise` and `OptiXDenoise`.

**Impact**

- The UE OIDN result in the docs is very likely an artefact of the cvar override, not a plugin bug.
- The standalone and usdview builds have no denoiser at all.
- UE's denoiser runs on GPU textures before readback, which avoids a CPU round trip.

**Recommendation**

- Enable `NNEDenoiser` (plus its NNE runtime).
- Map `hdUE:denoiser=ue` to `PathTracingEnableDenoiser=1` and stop forcing the cvar to 0 (set -1).
- Re-test OIDN, OptiX and NNE at 1080p and 4K, then update `docs/rendering.md:145`.
- Keep Houdini OIDN as the alternative.

## UE-08 — Lumen final-frame quality left at game defaults — Medium

**What we do**

- `Draw()` sets only the Lumen GI/reflection method and `LumenFrontLayerTranslucencyReflections` (`Bridge.cpp:556-560`), plus the offline surface-cache cvars (`Bridge.cpp:469-472`).
- Final-quality PPV properties are left at their defaults unless the user authors them: `LumenFinalGatherQuality`, `LumenSceneDetail`, `LumenSceneLightingQuality`, `LumenReflectionQuality`, `LumenRayLightingMode` (Hit Lighting) and `LumenMaxTraceDistance`. `docs/rendering.md` says "quality and hit lighting are post-process properties".
- Generated meshes get exactly 12 cards: `MeshCards.cpp:34`, "UE's default MaxLumenMeshCards".
- The gallery shows Lumen at 0.54–0.83× of Karma brightness (README tables).

**What Epic documents** — Lumen Technical Details:

- "By default, Lumen only places 12 Cards on a mesh", and you should raise Max Lumen Mesh Cards "for complex interiors and irregular shapes".
- Hit Lighting for Reflections "evaluates lighting at the ray hit for higher quality".
- "increase the Lumen Scene Detail will capture smaller meshes over farther distances".
- Lumen Scene Lighting Quality reduces splotchy artefacts.

**Impact.** Offline Lumen frames leave quality on the table, and the brightness gap with Karma partly reflects game-level settings rather than Lumen's ceiling. The 12-card limit hurts large merged Hydra meshes such as Sponza's architecture.

**Recommendation**

- In the offline profile (UE-06), default to:
  - `LumenFinalGatherQuality` ≥ 2
  - `LumenSceneLightingQuality` ≥ 2
  - `LumenSceneDetail` ≥ 2
  - `LumenRayLightingMode = Hit Lighting` when HWRT is available
- Scale the card count with mesh bounds and complexity, up to 32.
- Re-measure the gallery brightness ratios afterwards.

## UE-09 — Development configuration shipped — Medium

**What we do.** `scripts/Build-Backend.ps1:18` and `Stage-Runtime.ps1:8` (`-clientconfig=Development`) build and stage Development, and that is what the release contains.

**What Epic documents** — Build Configurations:

- Development "enables all but the most time-consuming… optimizations".
- Shipping "strips out console commands, stats, and profiling tools… the configuration for optimal performance and shipping your game".
- Test "is the Shipping configuration, but with some console commands, stats, and profiling tools enabled".

**Impact.** Development keeps `check()` asserts, extra logging and debug tooling. It is slower and fatter, and exposes debug and cheat commands to end users.

**Recommendation**

- Release with **Test**: hdUE relies on cvars and some visualize modes, which Shipping may compile out **(per-feature unverified)**.
- Keep Development for CI.
- Add a check in `Package-Houdini.ps1` that refuses to package Development binaries.

## UE-10 — Engine patch where public API may suffice — Medium

**What we do.** `Apply-ResidencyPatch.ps1` makes two engine edits:

- At lines 8-16 it inserts `friend class FHdueVirtualTextureResidency;` into the private `Renderer/Private/VT/VirtualTextureSystem.h`, so `VirtualTextureResidency.cpp` can take page locks.
- At lines 17-27 it guards `NaniteDisplace.cpp` (see UE-01).

**What the engine offers.** Public `IRendererModule::RequestVirtualTextureTiles(...)` overloads and `LoadPendingVirtualTextureTiles` (`Engine/Source/Runtime/RenderCore/Public/RendererInterface.h:370-386`). MRQ uses them to make VT resident before final frames (`MoviePipelineRendering.cpp:682-688`). No Epic doc page was fetched for these APIs.

**Impact**

- A source-engine patch makes every engine update a manual re-validation.
- It blocks using an unmodified binary engine.
- A licensee-modified private header adds to the UE-01 surface.

**Recommendation**

- Re-implement residency as request-every-frame plus `LoadPendingVirtualTextureTiles` until feedback is empty.
- Only the eviction-lock guarantee is lost. Mitigate that by sizing the VT pool (`DefaultEngine.ini:39-40` already raises it) and requesting again each Path Tracer frame.
- Measure on `samples/residency` before removing the patch.

## UE-11 — Virtual Shadow Maps disabled — Medium

**What we do.** `docs/rendering.md` (Engine quality table) says "`r.Shadow.Virtual.Enable` (off in this project)". `DefaultEngine.ini` does not enable VSM, so directional lights fall back to legacy shadow maps. MegaLights handles local lights (`Lighting.cpp:142`).

**What Epic documents** — Virtual Shadow Maps:

- VSMs "are designed to be used with Nanite".
- "Nanite geometry always renders to the Virtual Shadow Map regardless of distance".
- "New projects use Virtual Shadow Maps by default".

**Impact.** Interactive Nanite meshes and native Nanite assets get lower-resolution sun shadows, with cascade transitions, compared to UE's reference look.

**Recommendation.** Enable VSM in `DefaultEngine.ini` for realtime mode, then re-record the PSO cache (`Record-PipelineCache.ps1`). Keep the `hdUE:cvar` override for opting out.

## UE-12 — IES brightness — Medium

**What we do.** `Lighting.cpp:214` sets `SetIESTexture(Texture); bUseIESBrightness=false`. `docs/rendering.md` records hdUE as "more than 60x" darker than Karma at the beam centre.

**What Epic documents** — IES Light Profiles:

- With Use IES Intensity off the light "will use the selected light's intensity value".
- With it on, it "uses the light brightness from the profile".
- IES Intensity Scale "globally scales the profile's brightness contribution".

**Impact.** UE normalizes the profile to a peak of 1 and keeps the USD intensity, while UsdLux treats the profile as a distribution, optionally normalized (`shaping:ies:normalize`). The mismatch is consistent with the observed error.

**Recommendation**

- With `shaping:ies:normalize = 0`, use `bUseIESBrightness=true` and set `IESBrightnessScale` from USD intensity and exposure.
- With normalize on, compute the profile's mean-to-peak ratio from `IESFile` and compensate the light intensity.
- Validate against Karma on `samples/cornell/beam.ies`.

## UE-13 — Dead or ineffective configuration — Low

**What we do**

- `DefaultEngine.ini:12` sets `r.UsePreExposure=False`. That variable exists in 5.8.4 only in `Engine/Config/BaseEngine.ini:3400` under `+VersionedIntRValues`. No runtime code reads it, and pre-exposure is always on, which is why `Bridge.cpp:604-612` must manage `r.EyeAdaptation.CachedLightingPreExposure`.
- `DefaultEngine.ini:14` sets `r.GenerateMeshDistanceFields=True`, but runtime meshes never get SDFs, so software Lumen and DF shadows do nothing for Hydra geometry.
- `DefaultEngine.ini:10` sets `r.Substrate.BytesPerPixel=128`, plus 4 closures per pixel, which inflates GBuffer memory at 4K.

**What Epic documents.** Software Lumen "requires 'Generate Mesh Distance Fields'" (Lumen Technical Details), but that only applies to meshes that actually have distance fields.

**Recommendation**

- Delete `r.UsePreExposure`.
- Document that Lumen requires HWRT for Hydra meshes. `HasRayTracing()` already gates the Path Tracer; add a startup warning for Lumen without HWRT.
- Justify or reduce the Substrate budget with a measured memory number.

## UE-14 — Unneeded plugins and binaries in the package — Low

**What we do**

- `HdUEBackend.uproject:14-23` enables these for all targets: `MetaHumanSDK`, `LiveLink`, `FullBodyIK`, `ControlRigSpline`, `AnimationWarping`, `ACLPlugin`, `DatasmithContent` and `InterchangeAssets`.
- The package ships:
  - `Engine/Plugins/Animation/*`
  - Vulkan layers (`VkLayer_khronos_validation.dll`, etc.)
  - `d3d12SDKLayers.dll`
  - Ogg/Vorbis/XAudio
  - `Engine/Extras/GPUDumpViewer`
  - `Engine/Content/VREditor`, `EditorMaterials`, `EditorMeshes`
- The zip is 911 MB; the unpacked folder is 1.3 GB.

**Impact**

- Every extra module adds licence surface (for example MetaHuman terms in EULA §6(e)) and load time.
- The Vulkan layers and SDK layers are debug tools a D3D12-only runtime never loads.

**Recommendation**

- Restrict these plugins to the Editor target (`TargetAllowList`). Keep `InterchangeAssets` only if cooked Interchange assets need it at runtime.
- Exclude `Engine/Binaries/ThirdParty/Vulkan`, `D3D12/x64/d3d12SDKLayers.dll` and `Extras` in `Package-Houdini.ps1:49`.

## UE-15 — AOVs: separate re-renders, no Object ID/Cryptomatte — Low

**What we do**

- Every AOV (`bridge.h:112-117`: albedo, normal, roughness, metallic, specular, occlusion, primId, instanceId) is its own rasterized render. It gets its own ViewState, has GI and volumes off (`Bridge.cpp:493-498, 583-597`) and is never path traced.
- There are no lighting AOVs (diffuse/specular/emission, direct/indirect).
- There is no Cryptomatte.

**What the engine offers.** MRQ/MRG ship an Object ID pass with Cryptomatte output in the `MoviePipelineMaskRenderPass` plugin (`Public/Graph/MovieGraphObjectIdNode.h`, `MoviePipelineObjectIdPass.h`). That code is reusable as a reference for Cryptomatte hashing and EXR metadata. No Epic doc page was fetched for it.

**Recommendation**

- Add `CryptoObject`/`CryptoMaterial` from the existing primId pass, using Cryptomatte hashing of prim paths as MRQ does.
- Render Path Tracer-mode AOVs with the path tracer's own albedo and normal buffers, which already exist for its denoiser (`PathTracing.cpp:5255-5256`).

## UE-16 — Resolution cap, no tiling — Low

**What we do.** `CreateSession` rejects anything above 8192 on either axis (`Bridge.cpp:352-353`). There is no tiled rendering.

**What the engine offers.** MRQ supports High Resolution tiling, and `r.PathTracing.DispatchSize` already tiles Path Tracer dispatches (`PathTracing.cpp:300-305`). No Epic page was fetched for tiling.

**Recommendation.** Document the cap. Add tiling only when there is a request for print-resolution output (YAGNI).

## UE-17 — UELibrary lifecycle relies on undocumented behaviour — Low

**What we do**

- `Initialize` starts UE through `UELibrary_InitW` with `-game -windowed -d3d12 -unattended … -ResX=640 -ResY=360` (`Bridge.cpp:294-297`), plus `-RenderOffscreen` from the host (`src/hydra/delegate.cpp:264-265`).
- `Shutdown` installs a static whose destructor swaps `GGameThreadId` to the exiting thread (`Bridge.cpp:1080-1086`).
- After `UELibrary_Shutdown`, the state is STOPPED and UE cannot be restarted in the same process.

**What Epic documents.** The only UELibrary page is for 4.27: https://docs.unrealengine.com/4.27/SharingAndReleasing/BuildAsALibrary ("enabling other applications to spawn and control their own UE4 instances", Win64 only). No 5.x documentation was found.

**Impact.** Behaviour can change between engine versions without notice. The thread-id swap is fragile.

**Recommendation**

- Keep the static-destruction workaround behind an engine-version `static_assert`. One already exists for 5.8.4 (`Bridge.cpp:58-60`).
- Document that each process gets one UE lifetime.
- Add a CI smoke that runs init, render, shutdown and process exit on a non-main thread.

## UE-18 — Cook and runtime from different builds — Low

**What we do**

- Content is cooked with the launcher editor (`Prepare-Content.ps1:11-16`; `editor_root` points to `C:/Program Files/Epic Games/UE_5.8`, Build.version CL 58902052).
- The runtime is the source build (CL 0).
- `DefaultGame.ini:21` pins `GameVersion=1` so the PSO cache is not rejected.

**Impact.** The two builds are the same 5.8.4 release, so this works today. A launcher hotfix (5.8.5) would silently mismatch the cooked shader bytecode and the runtime.

**Recommendation.** Fail `Prepare-Content.ps1` unless the editor's `Build.version` Major/Minor/Patch equals the source engine's. Or cook with the source engine's own editor target, which `HdUEBackendEditor.Target.cs` already defines.

---

## What is already right

- **Avoids the second USD runtime.** UE's USD plugins are disabled (`HdUEBackend.uproject:24-26`). UE 5.8.4 bundles USD 26.03 (`Engine/Plugins/Runtime/USDCore/Source/ThirdParty/USD/include/pxr/pxr.h:16`) and Houdini 22 bundles 26.05. Loading both in one process would collide.
- **Offscreen rendering through an explicit view family.** `bDisableWorldRendering`, `GAreScreenMessagesEnabled=false` and `SetOnScreenDebug(false)` (`Bridge.cpp:315-320, 483`) are the correct way to keep UE's own viewport and warnings out of frames.
- **Asynchronous GPU readback.** `FRHIGPUTextureReadback` with a 3-slot ring and stale-generation discard (`Bridge.cpp:107-112, 631-672`). No `FlushRenderingCommands` on the hot path.
- **Deterministic exposure.** Manual exposure with forced adaptation speed for offline frames (`Bridge.cpp:553-582`), and the tone curve written as `SCS_FinalToneCurveHDR` as MRQ does (`Bridge.cpp:506`).
- **Pre-exposure window.** Lumen's cached-lighting pre-exposure window is recentred on the exposure (`Bridge.cpp:604-612`). This is a real UE gotcha; the engine help text says "Adjust it per project to cover used exposure range" (`PostProcessEyeAdaptation.cpp:204`).
- **Reference DOF** for the Path Tracer (`PathTracingEnableReferenceDOF`, `Bridge.cpp:568`). Path Tracer bounces are raised for refractive scenes (`Bridge.cpp:559`).
- **Runtime BC compression and texture creation.** ISPCTexComp compression (`Scene.cpp:258-273`). Discardable bulk data handed to the RHI, with no CPU copy kept (`Scene.cpp:385-395`).
- **Static render data built directly** instead of through `BuildFromMeshDescriptions` (`Scene.cpp:515-555`), with the measured reason documented. Nanite, card and tangent builds run on `UE::Tasks` workers, swapped one per frame (`Scene.cpp:877-884`). Nanite only draws opaque and masked materials and translucent ones are excluded (`Scene.cpp:570-571`), matching the Nanite doc ("supports materials that have their Blend Mode set to Opaque and Masked").
- **Bundled PSO cache** with stable keys, compiled on workers from startup (`DefaultEngine.ini:44`, `docs/building.md`). This matches Epic's advice that global/post passes are not covered by precaching (PSO Precaching doc).
- **UObject lifetime.** Long-lived UObjects are pinned with `TStrongObjectPtr` / `AddToRoot`, and render commands are drained before release (`Bridge.cpp:332-347`). Nothing calls `CollectGarbage` from the host thread.
- **Owner-thread enforcement** on every mutating ABI call (`CheckOwner`/`CheckReady`, `Bridge.cpp:201-208`). Frame reads elsewhere go through a mutex.
- **`static_assert` pinning the engine version** (`Bridge.cpp:58-60`), and every engine patch insertion point validated (`Apply-ResidencyPatch.ps1:5-6, 12, 22`).
- **Source repo licensing.** It contains no Epic source beyond short patch anchors; `git ls-files` shows no Intermediate, Binaries or Saved. EULA §5(a)(ii) allows snippets of up to 30 lines "in connection with supporting patches and plug-ins". `THIRD_PARTY_NOTICES.md` already quotes the §7(a) attribution correctly.

## UE features we should leverage

1. **NNE Denoiser** (`Plugins/NNE/NNEDenoiser`). Epic's default Path Tracer denoiser, GPU, with a spatial-temporal mode. Replaces the Houdini-only OIDN path for all hosts (UE-07).
2. **`View->PathTracerSeedOffset`.** The MRG mechanism for decorrelating sample sets (UE-04).
3. **MRQ Game Overrides as a recipe**: cinematic `sg.*`, texture streaming off, `r.D3D12.GPUTimeout 0`, `LoadPendingVirtualTextureTiles` (UE-06). The code is a fixed list of cvars in `MoviePipelineGameOverrideSetting.cpp:78-259`.
4. **Public VT tile APIs** in `RendererInterface.h:370-386`, instead of the friend-class engine patch (UE-10).
5. **Object ID / Cryptomatte pass** (`MoviePipelineMaskRenderPass`), as a reference for hashing and EXR metadata (UE-15).
6. **Lumen PPV quality and Hit Lighting** as offline defaults, plus per-mesh `MaxLumenMeshCards` (UE-08).
7. **Virtual Shadow Maps** for Nanite content (UE-11).
8. **`bUseIESBrightness` / `IESBrightnessScale`** for photometric IES (UE-12).
9. **Reuse from Epic's USD integration: not recommended.** `USDSchemas`/`USDStage` are Runtime modules and translate UsdPreviewSurface and MaterialX through Interchange. They cannot share the process with Houdini's USD (version and namespace clash, see above). The USD doc also requires `FORCE_ANSI_ALLOCATOR=1` for runtime loading. hdUE's Hydra-side translation is the right architecture. Epic's translators are useful only as a behavioural reference for UsdPreviewSurface→UE parameter mapping, and the cooked `/USDImporter` preview-surface materials could replace hdUE's own parents if licensing (UE-01/UE-03) is resolved.
