# Hydra delegate conformance audit

Branch `feature/package` at `e8fe1f4`. Read-only review of `src/hydra/*`, `src/host/usd_render.cpp`, `include/hdue/*.h` and `resources/plugInfo.json.in`, checked against the OpenUSD Hydra API, the UsdGeom/UsdRender/UsdShade schemas, and public reference delegates. Every `path:line` below was read in this revision. Claims I could not check in code or docs are marked **unverified**.

## Scope & sources

- HdRenderDelegate header (Stop/Restart/Pause, settings namespaces, render contexts): https://github.com/PixarAnimationStudios/OpenUSD/blob/release/pxr/imaging/hd/renderDelegate.h
- HdRenderPass (`_Execute` and render tags): https://openusd.org/dev/api/class_hd_render_pass.html
- HdRenderPassAovBinding (lifetime of the buffers): https://github.com/PixarAnimationStudios/OpenUSD/blob/release/pxr/imaging/hd/aov.h
- HdAovTokens, HdInstancerTokens, render tags, HdRenderSettingsTokens: https://github.com/PixarAnimationStudios/OpenUSD/blob/release/pxr/imaging/hd/tokens.h
- HdRenderIndex::SyncAll / GetDrawItems (render-tag filtering): https://github.com/PixarAnimationStudios/OpenUSD/blob/release/pxr/imaging/hd/renderIndex.cpp
- HdMeshUtil (orientation and holes): https://github.com/PixarAnimationStudios/OpenUSD/blob/release/pxr/imaging/hd/meshUtil.cpp
- CameraUtilFraming: https://openusd.org/dev/api/class_camera_util_framing.html
- UsdGeomMesh / UsdGeomGprim: https://openusd.org/dev/api/class_usd_geom_mesh.html
- UsdGeomBasisCurves: https://openusd.org/dev/api/class_usd_geom_basis_curves.html
- UsdRenderVar: https://openusd.org/dev/api/class_usd_render_var.html
- UsdRenderSettingsBase: https://openusd.org/dev/api/class_usd_render_settings_base.html
- HdEmbree renderer and mesh: https://github.com/PixarAnimationStudios/OpenUSD/blob/release/pxr/imaging/plugin/hdEmbree/renderer.cpp, https://github.com/PixarAnimationStudios/OpenUSD/blob/release/pxr/imaging/plugin/hdEmbree/mesh.cpp
- HdArnold render pass: https://github.com/Autodesk/arnold-usd/blob/master/libs/render_delegate/render_pass.cpp
- SideFX HDK, Hydra delegates in Houdini (`UsdRenderers.json`, `houdini:*` settings, render stats): https://www.sidefx.com/docs/hdk/_h_d_k__u_s_d_hydra.html

## Summary

| ID | Severity | Finding | Effort |
|---|---|---|---|
| HYD-01 | Critical | In Houdini's UI, the Execute posted to the UE thread (and its pump) dereferences Hydra objects that Houdini's thread can sync or destroy at the same time: `HdCamera*`, `HdRenderBuffer*`, `HdRenderIndex::GetBprim`, `Mesh` members and the scene delegate | M |
| HYD-02 | High | Render tags and the render pass collection are ignored. Every rprim ever synced is drawn whatever its purpose, and prims never leave when the requested tags change | M |
| HYD-03 | High | A `color3f` beauty (UsdRenderVar's **fallback** `dataType`), or any AOV whose channel count differs from what hdUE produces, is dropped without a message: the image stays black or zero | S |
| HYD-04 | High | Subdivision ignores the subdiv tags (creases, corners, `interpolateBoundary`, `faceVaryingLinearInterpolation`, `triangleSubdivisionRule`) and discards the face-varying topology | M |
| HYD-05 | Medium | `HdRenderPassState` framing (data window, display window, pixel aspect) is ignored. Crop regions, overscan and non-square pixels render wrong | M |
| HYD-06 | Medium | Every `lpe:` render var receives the full beauty. `primvar` vars and other unknown vars are never written, yet the pass still reports converged | S |
| HYD-07 | Medium | Mesh, Points, Curves and Volume Sync ignore dirty bits. A transform-only change redoes triangulation, subdivision, welding and normals, then re-uploads the vertices baked into world space. Volumes re-read their VDB on any dirty bit | M |
| HYD-08 | Medium | ExtComputation primvars are never evaluated. On the scene-delegate (Hydra 1) path, UsdSkel-skinned meshes would render in their rest pose | S |
| HYD-09 | Medium | Primvar readers in bound materials (`UsdPrimvarReader_*` other than float2, `ND_geompropvalue_*`) are not translated. Vertex colours are read only for unbound meshes | M |
| HYD-10 | Medium | BasisCurves: `uniform` and `varying` widths and colours are indexed as per-vertex, `pinned` wrap is ignored, and `normals` (ribbons) are ignored | S |
| HYD-11 | Low | The `depth` AOV holds linear distance in the Houdini build, husk included, though HdAovTokens defines it as clip-space depth. `cameraDepth` is not offered, and the standalone build exposes no depth descriptor | S |
| HYD-12 | Low | Motion blur: `disableMotionBlur`/`instantaneousShutter` are not read, time-sampled instancer primvars do not blur, curves have no deformation blur, and accelerations/angular velocities are ignored | M |
| HYD-13 | Low | Sync error paths return before clearing the dirty bits, so a broken prim is synced again (and logs again) every frame | S |
| HYD-14 | Low | `doubleSided` is ignored: every generated base material is two-sided | S |
| HYD-15 | Low | Camera: far clipping and `clippingPlanes` are ignored | S |
| HYD-16 | Low | Render settings: descriptors list 6 of about 25 keys, the standard `HdRenderSettingsTokens` are not honoured, and there is no `GetRenderSettingsNamespaces`, render-settings Bprim, Stop/Restart or AOV `clearValue` | M |
| HYD-17 | Low | Mesh edge cases: authored normals are used on a subdivision mesh at level 0, overlapping materialBind subsets drop the whole mesh, stale instancer primvars are never pruned, and a change to a material's UV primvar does not re-sync its meshes | S |
| HYD-18 | Low | Lights: no light or shadow linking, light filters, mesh lights, portal lights or `pluginLight` | L |

---

## HYD-01 Concurrent access to Hydra objects from the UE thread (Houdini UI)

**What we do**

- In continuous mode (Houdini's UI), `_Execute` stores a `PassSnapshot`, posts `executeLatest` to the UE thread and returns (`src/hydra/delegate.cpp:1023-1035`).
  - It also installs a pump that calls `executeLatest()` after every UE tick while the pass is not converged (`delegate.cpp:1033`). The pump is removed only in `~Pass` (`delegate.cpp:974-976`).
- The snapshot copies the AOV bindings, and with them the raw `HdRenderBuffer*` pointers. It also keeps a raw `const HdCamera*` (`delegate.cpp:986-992, 1000`).
- On the UE thread, `executeOnce` does the following:
  - Dereferences that camera: `GetFStop`, `GetClippingRange`, `GetShutterOpen/Close`, `GetId` (`delegate.cpp:1211-1221, 1350, 1406`).
  - Looks up buffers through `GetRenderIndex()->GetBprim(...)` (`delegate.cpp:1094, 1152`).
  - `dynamic_cast`s the bound buffer pointers (`delegate.cpp:1108-1118`), and keeps them across calls in `aovs_` and `gpuBuffers_` (`delegate.cpp:845, 857`).
- `executeOnce` calls `HdUERefreshWetMasks` (`delegate.cpp:1198`). That function calls `index->GetRprim`, `GetSceneDelegateForRprim`, `delegate->Get(...)`, and reads `Mesh::emitted_` and `maskTopology_` (`prims.cpp:428-456, 877-888`). `Mesh::Sync` writes those same members on Houdini's threads (`prims.cpp:278, 425`).
- `IsConverged()` runs on Houdini's thread. It reads `aovs_`, `aovsDone_`, `denoisePending_` and the `revision` counters without a lock (`delegate.cpp:977-983`), while the UE thread reassigns `aovs_` (`delegate.cpp:1142`).
- `DestroyBprim` is a plain `delete` (`delegate.cpp:1634`). `HdUERenderBuffer` has no hand-off with the UE thread.

**What the reference says**

- aov.h, `HdRenderPassAovBinding::renderBuffer`: "hydra never takes ownership of the renderBuffer, but assumes it will be alive until the end of the renderpass, or whenever the buffer is marked converged, whichever is later." Nothing guarantees the buffer outlives that.
- HdRenderIndex is not thread-safe for lookups that run while prims are being inserted or removed (**unverified** as a documented statement; the containers in `renderIndex.cpp` take no locks).

**Impact**

- **Render buffers.** A viewport resize, a change of AOV or display channel, or a camera switch makes Houdini destroy and recreate render buffer Bprims. For example, HdxTaskController rebuilds them when the size or AOVs change. During that window the pump can still run with the old `latestPass_` and `dynamic_cast` a freed `HdRenderBuffer`.
- **Camera.** Deleting or switching the camera LOP while a Path Tracer render is still converging leaves `latestPass_->camera` dangling. The pump keeps calling it about every 2 ms.
- **Wait for UE.** The UE thread is often busy for seconds during uploads (`delegate.cpp:1015-1016`), which widens every window above.
- **Result.** Each case is a use-after-free: an intermittent crash of Houdini with no clear repro. The `GetBprim` and `GetRprim` lookups race with HdRenderIndex insertions in the same way.

**Recommendation**

- **Snapshot.** Copy every camera value into `PassSnapshot` on the calling thread (clip range, shutter, id) and drop the `HdCamera*`. Resolve the buffer pointers on the calling thread too.
- **Buffer lifetime.** Hold buffers through a ref-counted handle, or have `HdUERenderBuffer::_Deallocate` and its destructor post to the UE thread and wait until no Execute references them.
- **Pump.** Reset `latestPass_` and stop the pump in `CreateBprim` and `DestroyBprim`.
- **Wet masks.** Move `HdUERefreshWetMasks` out of `executeOnce` into `CommitResources` or `_Execute`, which run on the Hydra thread.
- **Shared fields.** Make `aovsDone_` and `denoisePending_` atomics, and publish an "AOVs pending" flag instead of reading `aovs_` from `IsConverged`.

## HYD-02 Render tags (purpose) and the collection are ignored

**What we do**

- `executeOnce(const std::shared_ptr<const PassSnapshot>&, const TfTokenVector&)` discards the render tags (`delegate.cpp:1060`).
- `CreateRenderPass` stores the collection only in the base class (`delegate.cpp:1600-1601`).
- No rprim calls `GetRenderTag()` (grep: no hit in `src/`), and `HdUEState::meshes` is uploaded whole.

**What the reference says**

- HdRenderPass: "`_Execute` the buckets corresponding to renderTags; renderTags.empty() implies execute everything."
- `renderIndex.cpp`: `SyncAll` builds its dirty list from the tasks' render tags (`_rprimDirtyList.UpdateRenderTagsAndReprSelectors(taskRenderTags, …)`). Filtering by tag at draw time is the delegate's job: Storm does it in `GetDrawItems`.
- HdArnold `_Execute`: `if (_renderDelegate->SetRenderTags(renderTags))`, then `AiNodeSetDisabled(node, !_renderDelegate->IsVisibleRenderTag(...))`.
- HDK: Houdini drives the purposes through `defaultpurposes` and the viewport's display-purpose toggles.

**Impact**

- A model with `purpose=proxy` and `purpose=render` children renders both, overlapping, as soon as both tags have been requested once. Typical cases: the viewport shows proxy, then switches to render; or a husk run with `includedPurposes` that differ from those of the viewport session.
- Turning off "guide" display in the Solaris viewport does not hide guides that were already synced.
- The prims stay until they are deleted from the stage.

**Recommendation**

- Store `GetRenderTag()` on `HdUEMeshData` and `HdUEVolumeData` during Sync.
- In `_Execute`, compare `tags` with the last set and send the visibility as `visible && tagIncluded`. This needs only the existing `visible` flag; no re-upload of geometry.

## HYD-03 Beauty and AOV channel counts must match exactly, or the frame is silently dropped

**What we do**

- **Allocate** accepts `Float32Vec4`, `Float16Vec4`, `Float32Vec3`, `Float32` and `Int32` (`render_buffer.cpp:10-12`). So a `color3f` beauty allocates as `Float32Vec3`.
- **Beauty.** The beauty is always RGBA: `pixels` is sized from `frame.required_bytes` and processed 4 floats per pixel (`delegate.cpp:1238, 1281`).
- **publish** returns without a message whenever `pixels.size() != width*height*channels` (`render_buffer.cpp:50`).
- **Other AOVs.** `publishAov` produces 3 channels for albedo and normal and 1 channel for the scalar AOVs, whatever format the buffer was allocated with (`delegate.cpp:953-968`).

**What the reference says**

UsdRenderVar: "`uniform token dataType = "color3f"`", with fallback value `color3f`. A RenderVar that does not author `dataType` (for example a hand-written `RenderVar "Ci"` or a third-party product) is therefore 3-channel.

**Impact**

- **Beauty.** husk with a `color3f` beauty (`C`, `Ci` or `beauty`, all mapped to color by `HdUEBeauty`, `delegate.cpp:191-193`) writes a black image, with no error. Pass convergence does not depend on the buffer, so husk finishes normally.
- **Other AOVs.** The same silent zeros appear for:
  - `albedo` or `normal` authored as `color4f`/`float4`;
  - `roughness` authored as `color3f`.

**Recommendation**

- Convert in `publish`: copy `min(srcChannels, dstChannels)` and fill the rest (alpha = 1).
- Pass the source channel count explicitly.
- Turn the silent `return` into a `TF_WARN` (once per buffer).

## HYD-04 Subdivision drops creases, corners, boundary rules and face-varying topology

**What we do**

- **Subdiv tags.** `HdUESubdivision::Create` builds the refiner from `coarse.GetPxOsdMeshTopology()` (`subdivision.h:26`). The topology comes from `delegate->GetMeshTopology(GetId())` (`prims.cpp:270`). `GetSubdivTags` is never called (grep: no hit in `src/`).
- **Face-varying topology.** The face-varying channel is the identity, one value per face corner (`subdivision.h:23-26`), because primvars are flattened first (`prims.cpp:72-84`). Every face is therefore its own face-varying island.

**What the reference says**

- HdEmbree mesh.cpp calls `_topology.SetSubdivTags(sceneDelegate->GetSubdivTags(id));` separately from `GetMeshTopology`. The tags do not travel with the topology.
- UsdGeomMesh: creases are "successive pairs that identify edges to be creased", and corners are "points for which a corresponding sharpness value is specified". `faceVaryingLinearInterpolation` "specifies how faceVarying primvar elements are interpolated".

**Impact**

- **Creases and corners.** Hard-surface assets that rely on crease or corner sharpness come out rounded. This affects Houdini SOP `creaseweight` exports and UsdSkel assets from DCCs.
- **Boundary rule.** `interpolateBoundary` falls back to OpenSubdiv's default instead of the USD default, `edgeAndCorner`. The resulting boundary shape is **unverified**.
- **UVs.** Face-varying UVs refine as disconnected per-face islands. Interior UVs are not smoothed across shared vertices: the authored `cornersPlus1` and `none` modes are both ignored, which shifts textures relative to Karma and Storm.

**Recommendation**

- Call `topology.SetSubdivTags(delegate->GetSubdivTags(GetId()))` before `HdUESubdivision::Create`.
- Build the face-varying channel from the authored primvar indices: `GetIndexedPrimvar` already returns them at `prims.cpp:74`. Refine the indexed values, then flatten.

## HYD-05 Framing, data window and pixel aspect ratio are ignored

**What we do**

- The pass reads only `GetWorldToViewMatrix()` and `GetProjectionMatrix()` (`delegate.cpp:1000, 1209-1210`). It renders that projection over the whole render buffer (`delegate.cpp:1343-1362`).
- `GetFraming()`, `dataWindow` and pixel aspect are never read (grep: no hit).

**What the reference says**

- CameraUtilFraming::ApplyToProjectionMatrix: "To obtain a correct result, a rasterizer needs to use the resulting projection matrix and set the viewport to the data window."
- HdEmbree computes NDC relative to the data window: `2.0f * ((x + jitter[0] - minX) / w) - 1.0f`.
- HdArnold maps `_framing.dataWindow` to `region_min_x` and the related region values.
- UsdRenderSettingsBase `dataWindowNDC`: "the axis-aligned rectangular region … within which the renderer should produce data".

**Impact**

When the data window is not the whole buffer, the projection, built for a viewport equal to the data window, is stretched over the full buffer. Affected cases:

- a render region or crop (`dataWindowNDC`) in husk;
- overscan;
- Houdini's region render.

The image comes out wrong, and `pixelAspectRatio != 1` squashes it. The plain viewport and full-frame husk renders are unaffected.

**Recommendation**

- Read `HdRenderPassState::GetFraming()`. When it is valid, either:
  - offset and scale the projection so that UE renders the display window and the delegate crops the data window; or
  - pass a sub-rect to the backend.
- Carry the pixel aspect ratio into `projection_scale`.

## HYD-06 LPE and primvar render vars produce wrong or missing data without an error

**What we do**

- `if(sourceType==TfToken("lpe")) name=HdAovTokens->color;` (`delegate.cpp:1105`): every LPE var becomes a second beauty.
- **`primvar` vars.** `sourceType=="primvar"` keeps `aovName`. `HdUEAovId` then returns 0 (`delegate.cpp:195-202`), so the buffer is never published.
- **Unknown names.** These are ignored in the same way. The error at `delegate.cpp:1151-1158` fires only when *no* bound AOV is renderable.
- **Convergence.** `IsConverged` ignores these buffers (`delegate.cpp:981-982`).

**What the reference says**

- UsdRenderVar `sourceType`:
  - `lpe`: "Specifies a Light Path Expression … as the source for this RenderVar";
  - `primvar`: "This source represents the name of a primvar".
- HdAovTokens `lpe:` asks for "a partial shading signal".
- HdArnold maps `lpe` vars to `light_path_expressions`, and `primvar` vars to user-data AOV writers. It marks the outputs it does not support as converged deliberately, with a comment.

**Impact**

- A husk product with `diffuse`, `specular` and `emission` LPE vars writes three copies of the beauty. Compositing them doubles or triples the light, and nothing in the logs says so.
- `primvars:st` and similar outputs are written as clear values.

**Recommendation**

- Short term: emit `TF_WARN` once per unsupported var and publish the clear value (or `NaN`) with `converged=1`, as HdArnold does.
- Map `lpe:C.*` (exactly the beauty) to color and reject other expressions.
- Longer term: support `primvar` vars through the existing ID/GBuffer AOV path.

## HYD-07 Dirty bits are ignored, causing full recompute and re-upload

**What we do**

- **Mesh.** `Mesh::Sync` rebuilds everything on every call: transform, topology, subdivision, triangulation, welding, normals, UVs, masks and motion samples (`prims.cpp:240-386`). It then sets `*bits = Clean` (`prims.cpp:426`). No `IsTopologyDirty`, `IsTransformDirty` or `IsPrimvarDirty` test exists.
- **World-space bake.** Non-instanced generic meshes are submitted with the transform baked into the vertices: `bake = data.transform` (`delegate.cpp:497-507`).
  - `meshChanged` includes `transform` (`prims.cpp:410`), so a transform-only change re-sends the whole vertex buffer.
- **Points, Curves and Volume.** These follow the same pattern (`prims.cpp:539-566, 613-698, 722-765`). `Volume::Sync` re-opens and densifies the VDB grid (up to 512³) on any dirty bit, a visibility toggle included (`prims.cpp:729-750`).
- **Material lookups.** `meshUvPrimvar` calls `GetMaterialResource` (a full network conversion) for the mesh and for each subset on every Sync (`prims.cpp:47-51`, `materials.cpp:315-323`).

**What the reference says**

HdEmbree mesh.cpp gates its work behind `IsTopologyDirty`, `IsTransformDirty` and `IsPrimvarDirty(…, points)`. It keeps instance and prototype transforms separate from the geometry.

**Impact**

- An animated transform on a heavy subdivided asset re-runs OpenSubdiv at level 2, plus normals and welding, and uploads full vertex buffers every frame. This happens in husk sequences and while scrubbing in the viewport.
- Material-binding edits redo the topology work as well.
- This is the dominant per-frame cost for rigid animation (**unverified** by profiling here, though `HDUE_TRACE_TIMING` already reports mesh Sync time at `prims.cpp:225-230`).

**Recommendation**

- Cache the topology products (triangles, parameters, subdivision, weld map) on the `Mesh` and recompute them only on `DirtyTopology`, `DirtySubdivTags` or a dirty `points`/`normals`/UV primvar.
- Send generic meshes in local space with `desc.transform`, as native assets already are (`delegate.cpp:550`). A transform change then goes through `set_transforms`.
- In `Volume`, gate the file read on `DirtyVolumeField`.

## HYD-08 ExtComputation primvars are not evaluated

**What we do**

- `GetSupportedSprimTypes` has no `extComputation` (`delegate.cpp:1589-1591`).
- Rprims read points only through `delegate->Get(points)` (`prims.cpp:269, 542, 620`). `GetExtComputationPrimvarDescriptors` is never called.

**What the reference says**

HdEmbree mesh.cpp evaluates them on the CPU: `compPrimvars = sceneDelegate->GetExtComputationPrimvarDescriptors` and `HdExtComputationUtils::GetComputedPrimvarValues(…)`.

**Impact**

- With UsdImagingDelegate (the Hydra 1 path), UsdSkel skinning is published as an ExtComputation, so skinned characters render in their bind pose.
- Whether Houdini 22's Solaris uses the scene-index UsdSkel resolution, which publishes plain points instead, is **unverified**. usdview with the scene delegate is affected.

**Recommendation**

Add HdEmbree's `HdExtComputationUtils::GetComputedPrimvarValues` block for `points` and `normals` in Mesh, Points and Curves. This is about 20 lines.

## HYD-09 Primvar readers in materials are not translated

**What we do**

- `SurfaceReader::trace` handles UsdUVTexture and image nodes, constant, convert, extract, normalmap and multiply. Every other node warns and uses the shader's constant (`materials.cpp:126-156`).
  - So `UsdPrimvarReader_float3` (for example `varname=displayColor`) and `ND_geompropvalue_color3` drive nothing.
- Vertex and face-varying `displayColor` is extracted only when the mesh has no bound material: `if (mesh.material.empty() && …)` (`prims.cpp:284-287`).
- **UV set.** The UV primvar name comes from the material network at mesh Sync (`prims.cpp:47-51`). If the material is edited to read another UV set, the mesh is not re-synced, because no material-to-rprim dependency exists.

**What the reference says**

UsdShade's `UsdPrimvarReader_*` is a standard UsdPreviewSurface node; see https://openusd.org/release/spec_usdpreviewsurface.html#primvar-reader (**unverified** fetch; the node set is well known).

**Impact**

- Houdini's default pattern (SOP `Cd` exported as `displayColor`, plus a preview or MaterialX material reading it) renders with the material's constant, typically 0.18 grey.
- Karma and Storm show per-point colour.

**Recommendation**

- In `trace`, map a primvar reader of `displayColor` (or any vec3 primvar) to the existing vertex-colour path. That means `HDUE_MATERIAL_VERTEX_COLOR` plus extracting that primvar for bound meshes.
- Add a material-to-rprim dependency, or re-mark the affected rprims dirty when a material's UV primvar changes.

## HYD-10 BasisCurves: interpolation, pinned wrap and normals

**What we do**

- **Widths.** `HdUEWidth(widths, vertex, …)` indexes by vertex and falls back to `widths[0]` (`prims.cpp:515-518, 635`).
  - With `uniform` widths (one per curve), curve *k* gets `widths[v]` for local vertices `v < curveCount`: the widths of other curves.
  - `varying` widths (one per segment end) are misindexed in the same way.
- **Colours** handle only per-vertex and per-curve counts (`prims.cpp:636-637`), so `varying` colours fall back to white.
- **Wrap** is checked only for `periodic` (`prims.cpp:624`). For `pinned` b-spline or Catmull-Rom curves, the phantom end points are never added.
- **Normals** are never read, so ribbons render as tubes.

**What the reference says**

UsdGeomBasisCurves:

- varying data has "one value per segment, plus one extra for nonperiodic curves", and "uniform applies once per curve";
- pinned: "These phantom points are injected to ensure that the interpolated curve begins at P[0] and ends at P[n-1]";
- "Curves with both widths and normals are imaged as ribbons oriented in the direction of the interpolated normal vectors."

**Impact**

- Hair and grass exported with per-curve widths get random widths.
- Pinned b-spline curves are short at both ends: Houdini's default b-spline export is pinned (**unverified** for every exporter).
- Ribbon-based foliage cards render as tubes.

**Recommendation**

- Resolve the interpolation with the existing `interpolation(delegate, id, name)` helper (`prims.cpp:52-56`), then index by curve, segment or vertex accordingly.
- Inject phantom points for `pinned`.
- Use `normals`, when present, to orient a flat two-triangle strip.

## HYD-11 Depth AOV convention

**What we do**

- **Houdini build.** Depth is the linear distance in scene units, with `FLT_MAX` for background (`delegate.cpp:1259-1260`). The descriptor is defined only in this build (`delegate.cpp:1670-1672`), and `UsdRenderers.json` declares `"depthstyle": "linear"` (`integrations/houdini/UsdRenderers.json:14`).
- **Standalone build.** The code projects to NDC [0,1] (`depth.h:10-17`), but `GetDefaultAovDescriptor(depth)` returns an invalid descriptor there, so usdview never binds depth.
- `cameraDepth` is not accepted.

**What the reference says**

- HdAovTokens (tokens.h): "`depth` represents the clip-space depth of the final fragment"; "`cameraDepth` represents the camera-space depth".
- HdEmbree writes `(hitPos[2] + 1) / 2` for `depth` and `tfar` for `cameraDepth`.
- HDK: `depthstyle` "linear (distance to camera)" is a viewport-only contract.

**Impact**

- **Viewport.** Correct in Houdini, because the depth style is declared.
- **husk.** A `depth` render var receives linear depth, and the convention depends on the build flag rather than on the host.
- **usdview.** No depth compositing with the standalone build, so Hdx overlays (selection, guides) are not depth-tested against the image.

**Recommendation**

- Select the convention at runtime: linear when `houdini:viewport` is set, clip space otherwise.
- Accept `cameraDepth` as linear.
- Return the depth descriptor in both builds.

## HYD-12 Motion blur gaps

**What we do**

- `disableMotionBlur` and `instantaneousShutter` are never read (grep: no hit). Blur is gated only by `hdUE:motionBlur` and the camera shutter (`delegate.cpp:1405-1409`). Whether husk zeroes the shutter of the HdCamera itself is **unverified**.
- **Instancer.** Motion comes from `SampleInstancerTransform` and the instance `velocities` only (`prims.cpp:166-182`). Time-sampled `instanceTranslations`/`Rotations` without velocities do not blur, and nested instancers drop their velocities (`prims.cpp:169`).
- **Curves** have no `SamplePrimvar(points)` and no velocities. **Points** have no sampled positions; they use velocities only (`prims.cpp:553-559`).
- `accelerations`, `angularVelocities`, `motion:nonlinearSampleCount` and `motion:blurScale` are ignored.

**What the reference says**

UsdRenderSettingsBase `disableMotionBlur`: "Disable all motion blur by setting the shutter interval of the targeted camera to [0,0]". `instantaneousShutter` is "Deprecated - use disableMotionBlur instead."

**Impact**

- Products that set `disableMotionBlur` still render with blur, and pay 16 sub-frames per frame (`hdUE:motionSamples` defaults to 16, `delegate.cpp:1405`).
- Particle-instanced debris, cached as time samples, renders sharp.

**Recommendation**

- Treat `disableMotionBlur` and `instantaneousShutter` as forcing `subframes_ = 1`.
- Sample `HdInstancerTokens` primvars with `SamplePrimvar` in `Instancer`.

## HYD-13 Error paths leave prims dirty

**What we do**

`Mesh::Sync` returns early on weld, normal or subset errors before `*bits = HdChangeTracker::Clean` (`prims.cpp:315, 322, 386` and `426`). `Curves::Sync` does the same on an invalid index (`prims.cpp:633`).

**What the reference says**

HdRenderIndex reads the bits after Sync (`_tracker.GetRprimDirtyBits`) and keeps uncleaned prims in the dirty list (renderIndex.cpp).

**Impact**

- One malformed mesh (bad indices, or overlapping subsets: see HYD-17) is fully re-synced on every `SyncAll`.
- A new `TF_RUNTIME_ERROR` is emitted each time, which floods the Houdini console.

**Recommendation**

Clear the bits on every exit path. Use an RAII guard, or `*bits = Clean` before the `return`.

## HYD-14 `doubleSided` is ignored

**What we do**

- No rprim calls `IsDoubleSided` or `GetCullStyle` (grep).
- The generated UE base materials are `TwoSided = true` (`unreal/HdUEBackend/Source/HdUEContent/Private/HdUEPrepareCommandlet.cpp:304, 392, 522`).

**What the reference says**

- UsdGeomGprim `doubleSided`: renderers "derive significant optimizations by considering these surfaces to have only a single outward side".
- HdEmbree culls back faces with `cull = !isFrontFace && !mesh->_doubleSided`.

**Impact**

- Single-sided cards and interiors seen from outside render their back faces. This matches Karma's default, so it is low.
- Lumen and the Path Tracer always pay the two-sided cost.

**Recommendation**

Carry `doubleSided` in `HdueMeshDesc` flags, and choose one- or two-sided material variants in the backend.

## HYD-15 Far clip and clipping planes

**What we do**

- In perspective, only `GetClippingRange().GetMin()` is sent (`delegate.cpp:1350`). Orthographic cameras derive both near and far from the projection (`delegate.cpp:1356-1357`).
- `GetClipPlanes()` is never read.

**What the reference says**

HdCamera exposes `GetClippingRange()` and `GetClipPlanes()` (https://openusd.org/dev/api/class_hd_camera.html, **unverified** fetch).

**Impact**

Scenes that rely on the far plane to hide background geometry, or on clipping planes for cutaways, render differently from Storm and Karma.

**Recommendation**

Send the far clip, and apply clip planes as a material clip or a UE `ClipPlane`.

## HYD-16 Render settings API surface

**What we do**

- **Descriptors.** `GetRenderSettingDescriptors` lists `renderMode`, `samplesPerPixel`, `warmupFrames`, `lumenEnabled`, `viewerLightIntensity` and `domeResolution` (`delegate.cpp:1675-1680`).
  - The code reads about 20 more keys: `motionBlur`, `motionSamples`, `subdivisionLevel`, `tonemapper`, `denoiser`, `debugView`, `postProcess:*`, `cvar:*`, `sceneLightsEnabled` and others (`delegate.cpp:1744-1770`).
- **Namespace.** The `hdUE:` prefix is stripped before storing (`delegate.cpp:1490-1491, 1694`). As a result, `HdRenderDelegate::GetRenderSetting("hdUE:x")` returns empty.
- **Standard keys.** `HdRenderSettingsTokens` (`convergedSamplesPerPixel`, `enableSceneLights`, `enableSceneMaterials`, `enableShadows`, `enableInteractive`) are not honoured.
- **API overrides.**
  - Not overridden: `GetRenderSettingsNamespaces`, `IsStopSupported`/`Stop`/`Restart`.
  - Not supported: the `renderSettings` Bprim (`delegate.cpp:1592-1597`).
  - `HdRenderPassAovBinding::clearValue` is ignored.
- `houdini:interactive` (the HDK's tumbling, scrubbing and playing hint) is not used.

**What the reference says**

- renderDelegate.h, `GetRenderSettingsNamespaces`: "Returns a list of namespace prefixes for render settings attributes relevant to a render delegate … The default is an empty list, which will gather all custom (non-schema) attributes."
- `IsStopSupported`: "Advertise whether this delegate supports stopping and restarting of background render threads."
- aov.h `clearValue`: "The clear value to apply to the bound render buffer, before rendering."

**Impact**

- usdview's settings panel and any host UI built from the descriptors expose only 6 of the controls.
- usdview's "Enable Scene Lights/Materials" toggles do nothing.
- With the Hydra 2 render-settings prim flow, all custom attributes of other delegates are gathered.
- Hosts that call `Stop()` before editing (for example usdview's stop button) get no effect.

**Recommendation**

- List every key in the descriptors.
- Return `{"hdUE"}` from `GetRenderSettingsNamespaces`.
- Map the standard tokens onto the existing keys.
- Implement `Stop` and `Restart` on top of the existing `paused` flag.

## HYD-17 Mesh edge cases

**What we do**

- **Normals at level 0.** With `hdUE:subdivisionLevel 0`, `Create` returns null (`subdivision.h:21-22`), so authored normals are used on a catmullClark mesh (`prims.cpp:280-281`).
- **Overlapping subsets.** Overlapping or out-of-range `materialBind` subsets throw (`geom_subsets.h:43-46`), and the whole mesh is skipped (`prims.cpp:385-386`).
- **Instancer primvars.** `Instancer::primvars_` is only ever assigned, never erased (`prims.cpp:162-164`). A removed `scales` keeps its old values.

**What the reference says**

- UsdGeomMesh: "Normals should not be authored on a subdivision mesh, since subdivision algorithms define their own normals."
- The materialBind family is `nonOverlapping`, but Storm still draws such meshes (**unverified**).

**Impact**

- Faceted shading on subdivision meshes at level 0.
- A whole asset disappears because of one bad subset.
- Stale instance scales remain after an edit.

**Recommendation**

- Ignore authored normals whenever the scheme is not `none`.
- On overlap, assign the first subset and warn.
- Rebuild `primvars_` from the current descriptor set.

## HYD-18 Lights

**What we do**

- **Supported types.** Sprims cover distant, sphere, rect, dome, disk and cylinder lights only (`delegate.cpp:1589-1591`).
- **Parameters read.** Light Sync reads color, intensity, exposure, color temperature, normalize, shadow enable, `treatAsPoint`, diffuse and specular, angle, cone shaping, IES, and dome texture and format (`prims.cpp:778-824`).
- **Not read.** `lightLink`, `shadowLink`, light filters, `shaping:focus` and `shadow:color` (grep: no hit).

**What the reference says**

HdPrimTypeTokens includes `meshLight`, `pluginLight`, `light` and `lightFilter`. UsdLuxLightAPI defines `collection:lightLink` and `collection:shadowLink` (https://openusd.org/dev/api/class_usd_lux_light_a_p_i.html, **unverified** fetch).

**Impact**

Light-linked lookdev scenes from Karma light every object with every light. Mesh lights (UsdLuxMeshLightAPI) are not lights at all.

**Recommendation**

- Read `lightLink` (HdLightTokens) and map it to UE lighting channels (three channels: a hard ceiling to document).
- Treat `shaping:focus` and `shadow:color` as parameters.

---

## What is already right

- **Instancing.**
  - Instance composition matches HdEmbree: instancer × translate × rotate × scale × instanceTransform, then the prototype transform first and nested instancers parent-major (`prims.cpp:196-222, 256`). The tokens come from `HdInstancerTokens`, so the `hydra:instance*` names resolve.
  - `_UpdateInstancer` and `HdInstancer::_SyncInstancerAndParents` are used from every rprim Sync (`prims.cpp:251-253, 509`), which is the thread-safe pattern.
- **Mesh primvars.**
  - Indexed primvars are expanded through `GetIndexedPrimvar` (`prims.cpp:72-84`).
  - Face-varying and uniform primvars are triangulated through `HdMeshUtil`, whose `ComputeTriangleIndices` honours `orientation` (`flip = orientation != rightHanded`) and skips hole faces (meshUtil.cpp).
  - Smooth normals are generated only when none, or zero-length ones, are authored (`prims.cpp:319-329`).
- **GeomSubsets.** Only `TypeFaceSet` subsets with a material split the mesh, and unbound faces keep the mesh's material (`geom_subsets.h:37-47`).
- **Implicit prims** are converted with `HdsiImplicitSurfaceSceneIndex` to meshes, registered per renderer as HdEmbree does (`implicit_surfaces.cpp`, `plugInfo.json.in:9-12`).
- **Camera projection.** The projection from `HdRenderPassState` is used as-is, so the window policy, aperture offsets (lens shift) and orthographic cameras follow Hydra's conformance (`delegate.cpp:1351-1362`). DOF uses HdCamera's world-unit `fStop`, `focusDistance` and `focalLength`, and the exposure uses `GetLinearExposureScale` (`delegate.cpp:1212, 1363-1365`).
- **Motion sampling.** It uses the official `SampleTransform`, `SampleInstancerTransform` and `SamplePrimvar` APIs over a ±1-frame window, with sub-frames spread over `[shutterOpen, shutterClose]` (`prims.cpp:120-124, 178-182, 336`; `delegate.cpp:1220-1222`). Velocities are divided by `houdini:fps`, which the HDK documents as the stage's `timeCodesPerSecond` (`prims.cpp:126-132`).
- **AOVs and render buffers.**
  - World normals are converted back from UE to USD world space and normalized (`delegate.cpp:960-966`).
  - `primId` and `instanceId` are `Int32` with -1 as the clear value (`delegate.cpp:1666-1667`).
  - Render buffers keep an immutable pinned snapshot while mapped, and publish bottom-up rows (`render_buffer.cpp:28-35, 50-66`).
- **Materials.** Material render contexts prefer `mtlx`, then the universal context (`delegate.h:273`). `HdMaterialNetworkMap` is converted with `HdConvertToHdMaterialNetwork2` (`materials.cpp:45-50`).
- **Delegate API.** Pause and Resume are supported and wired to `houdini:render_pause` (`delegate.cpp:1651-1660, 1698`). `GetRenderStats` provides `percentDone`, `totalClockTime` and `rendererStage` from a cache refreshed on the UE thread, so Houdini's UI thread never blocks on UE (`delegate.cpp:1778-1797`).

## Gaps vs reference delegates

- **Render tags.** HdArnold disables nodes whose purpose is not in the render tags. hdUE has nothing equivalent (HYD-02).
- **Framing and data window.** HdEmbree and HdArnold render the data window and support crop regions. hdUE has no equivalent (HYD-05).
- **LPE and primvar AOVs.** HdArnold handles both kinds of render var. hdUE duplicates the beauty for LPEs and leaves primvar vars empty (HYD-06).
- **Unsupported AOVs.** HdArnold marks them converged explicitly. hdUE leaves them unpublished and silent (HYD-06).
- **Dirty-bit gating and local-space geometry.** HdEmbree gates its work on dirty bits and keeps geometry in local space with transforms on top. hdUE does neither (HYD-07).
- **ExtComputations.** HdEmbree evaluates them on the CPU. hdUE does not (HYD-08).
- **Subdivision tags.** HdEmbree and Storm both read them. hdUE does not (HYD-04).
- **`doubleSided` and cull style.** HdEmbree culls back faces. hdUE does not (HYD-14).
- **`cameraDepth` AOV.** HdEmbree writes it. hdUE does not accept it (HYD-11).
- **Missing AOVs.** `elementId`, `Peye`, `Neye` and `primvars:*` (HdEmbree, HdArnold) are not produced. Hydra pick tasks that need `elementId` (face selection) have no data.
- **Lights.** Light linking, light filters and mesh lights (hdPrman, HdArnold) are missing (HYD-18).
- **Render settings.**
  - `GetRenderSettingsNamespaces` (hdPrman returns `ri`, HdArnold `arnold`) is not overridden.
  - There is no render-settings Bprim (hdPrman).
  - Stop/Restart are not supported (HdEmbree, HdArnold).
- **Volumes.** These cover the Houdini build only, and only a density float grid (`prims.cpp:708-770`). There is no volume material, no other fields (temperature, emission) and no field transform, and the standalone build advertises no volume support (`delegate.cpp:1582-1586`).
- **Clipping.** Clip planes and far clip are missing; Storm supports them (HYD-15).
