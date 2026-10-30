# Conformance

A spec-driven test set: one small USD scene per schema feature in [samples/conformance](../samples/conformance),
written from the OpenUSD schemas (`usdGeom`, `usdLux`, `usdShade`, `usdSkel`, `usdRender`, `usdVol`, `usdMedia`,
`usdProc` `schema.usda`), the
[UsdPreviewSurface specification](https://openusd.org/release/spec_usdpreviewsurface.html) and the Hydra AOV tokens,
not from what hdUE does. Each scene's doc string states what the spec says and what the render must show; its
`customLayerData["conformance"]` lists machine-checked expectations. Failures are gaps to fix or to document, not
noise to tune away.

## Running

```
hython scripts/run_conformance.py [--only AREA/FEATURE|PATTERN ...] [--mode realtime|pathtraced] [--matrix]
    [--houdini HFS] [--build build/houdini] [--integrations integrations/houdini] [--runtime-config FILE]
```

It renders every scene with husk (`HdUEHoudiniRendererPlugin`) in each mode, one husk at a time with a timeout,
prints a pass/fail table, writes `out/conformance/report.json` and, with `--matrix`, rewrites the matrix below. It
exits 1 when a check fails or a render does not finish. Each run takes about 5-12 s (UE startup dominates), so the
whole set (150 scenes, both modes) takes about 35 minutes; use `--only` and `--mode` while iterating. A scene's
`"husk"` list adds husk options (render_settings/render_pass renders `--pass Pass`).

## How scenes are built

- Every scene sublayers [common/stage.usda](../samples/conformance/common/stage.usda): metres, Y up, a camera at
  z = 10 whose frame is exactly x, y in [-5, 5] on the z = 0 plane, a normalized distant key light (4 lux), and
  128 x 128 render settings with two products: the beauty and the `primId`/`instanceId` AOVs.
- Small textures they read are in [textures](../samples/conformance/textures).
- Lights author `intensity` 1 and set brightness with `exposure` (except the scene that tests `intensity`).
- The runner writes a root layer per scene and mode (stage metadata, the scene as a sublayer, `hdUE:renderMode`).
- Effects that are global (exposure, light size, colour temperature, dome rotation) vary over frames, rendered by one
  husk process; everything else compares two prims side by side in one frame.

## Checks

A check measures a probe `{"frame", "region", "aov", "stat"}`; regions are `[x0, y0, x1, y1]` in the shared camera's
view units (-5..5, y up).

| Kind | Passes when |
| --- | --- |
| `value` | the stat lies in `[min, max]` (or `equals`) |
| `ratio` | stat(a) / stat(b) lies in `[min, max]` |
| `color` | the region's mean RGB, normalized to sum 1, is within `tol` of `rgb` normalized |
| `log` | the regex is found (or, with `"present": false`, absent) in husk's output |
| `metadata` | the frame's image metadata value named `metadata` matches the regex `match` |

Stats: `luma` (mean, default), `max`, `alpha`, `coverage` (alpha > 0.5), `soft` (0.05 < alpha < 0.95),
`partial` (luma between 5% and 95% of the region's maximum: filtered edges), `bbox_aspect`, `bbox_height`, `width`,
`height`, `ids` (distinct `primId` values over at least 3 pixels), `instances` (distinct `primId`/`instanceId` pairs);
a probe's `"channel": i` measures the mean of one channel instead. On a Cryptomatte layer: `crypto_ids` (distinct
rank-0 ids over at least 3 pixels), `crypto_id` (the most common rank-0 id, hex), `crypto_coverage` (mean coverage of
ranks 0 and 1), `crypto_soft` (share of pixels with partial rank-0 coverage). All checks are relative or structural;
none compares with another renderer.

## Support matrix

Last full run: `0e0ecf2` plus the new scenes (main's Houdini build and `.local/runtime-houdini.json`), 150 scenes x 2
modes: 133 scenes pass in both modes, 17 fail (33 failing runs). The 33 scenes added at `0e0ecf2` (Capsule_1 /
Cylinder_1, TetMesh, NURBS, HermiteCurves, subdivision tags, draw modes, VisibilityAPI, MotionAPI, `!resetXformStack!`,
`angularVelocities`, DomeLight_1 `poleAxis`, portal / plugin / mesh / volume lights, LightListAPI, `treatAsPoint`,
ShadowAPI distance, NodeGraph, CoordSysAPI, UDIMs, collection binding strength, UsdSkel skinning and blend shapes,
RenderPass, `renderingColorSpace`, ParticleField, SpatialAudio, GenerativeProcedural) account for 16 of the 17.

### Gaps found

| Scene | Expected (spec) | Observed | Likely code area |
| --- | --- | --- | --- |
| camera/clipping_range | the cube beyond the far plane is clipped | it renders: hdUE applies the near plane only (check removed) | UE's views keep an infinite far plane; a finite one changed Lumen's lighting by 8% on Cornell with nothing clipped, and the Path Tracer traces past any far plane |
| lux/dome_latlong (realtime) | rotated 180 degrees, the dome lights the sphere blue | blue with a green cast (normalized RGB 0.03, 0.17, 0.80; the check allows 0.2 from pure blue); path traced passes. It passed at `4a8d783` and fails the same with main's own plugin and runtime at `74e8853` | realtime dome lighting (Lumen / sky light capture) or the dome texture's colour conversion, changed between those commits |

| lux/dome_latlong (realtime) | (as above) | at `0e0ecf2` both frames fail: the +Z red frame also reads 0.80, 0.17, 0.03 | as above: a green cast in realtime dome lighting |
| geom/tet_mesh | TetMesh draws its surfaceFaceVertexIndices | nothing renders, no warning | hdUE's rprims are mesh/points/basisCurves; Hydra's `HdsiTetMeshConversionSceneIndex` (tetMesh to mesh) is not in hdUE's scene index chain (register it as `HdUE_ImplicitSurfaceSceneIndexPlugin` is) |
| geom/nurbs_patch, geom/nurbs_curves | NurbsPatch / NurbsCurves render their evaluated surface / curve | nothing renders, no warning | same: `HdsiNurbsApproximatingSceneIndex` (NURBS to mesh / basisCurves) is not registered for hdUE |
| geom/hermite_curves | HermiteCurves render the Hermite arch | nothing renders, no warning | UsdImaging's HermiteCurves adapter gives no prim hdUE accepts; at least warn |
| geom/draw_mode | `model:drawMode` bounds / cards replace the component model | the sphere renders in every mode (cards grey, no card texture) | husk's UsdImaging pipeline (draw-mode scene index / `UsdImagingDelegate` draw modes) is not applying draw modes for hdUE; check what the delegate or host must enable |
| geom/visibility_api | renderVisibility invisible hides render-purpose prims; guides need guideVisibility visible | both still render | UsdImaging passes only `visibility`; purpose visibility (`ComputeEffectiveVisibility(purpose)`) reaches no renderer here; a scene index or the delegate would have to apply it |
| render_settings/render_pass | `husk --pass Pass` renders only renderVisibility's members | the non-member renders too | the render pass prune scene index (`HdsiRenderPassPruneSceneIndex`, driven by the active render pass) is not in hdUE's chain |
| mesh/subdiv_fvar_linear | faceVaryingLinearInterpolation none smooths face-varying values | "none" and "all" render the same bilinear colours | src/hydra/subdivision.h: the face-varying displayColor is not refined through the OpenSubdiv face-varying channel (or ignores `faceVaryingLinearInterpolation`) |
| lux/volume_light | VolumeLightAPI emits; the volume renders | the volume disappears (alpha 0), nothing warned | the Volume with VolumeLightAPI no longer reaches hdUE as a volume rprim; its light sprim type is not accepted |
| lux/mesh_light | mesh lights warned (src/hydra/delegate.cpp accepts `meshLight`) | the mesh renders, unlit, no warning | no `meshLight` Sprim reaches hdUE in husk; warn from the mesh (it carries MeshLightAPI) instead |
| lux/portal_light | portals warned (README) | warned as "GeometryLight is not supported (/Portal)" | src/hydra/delegate.cpp: PortalLight arrives as the generic `light` Sprim, which is labelled GeometryLight |
| instancer/angular_velocities | `angularVelocities` blur, or a warning (README lists it as unsupported) | silently not blurred | src/hydra/delegate.cpp instancer motion: warn once when `angularVelocities` are authored |
| geom/motion_blur_scale | `motion:blurScale` 0 removes blur, or a warning | silently ignored | instancer / prim motion sampling |
| lux/shadow_distance | `shadow:distance` limits shadows, or a warning | silently ignored | src/hydra/prims.cpp light Sync (next to the `shadow:color` warning) |
| render_settings/rendering_color_space | rendering in `renderingColorSpace`, or a warning | silently ignored | render settings translation in src/hydra/delegate.cpp |
| volume/particle_field | Gaussian splats render, or a warning | nothing renders, no warning (the rest of the scene renders) | `HdsiParticleFieldConversionSceneIndex` is not registered for hdUE, and nothing warns |
Resolved since `4a8d783`: material binding purpose (the render settings' `materialBindingPurposes`, else `full`),
`disableDepthOfField`, `includedPurposes`, the `UsdUVTexture` fallback, GeometryLight (accepted and
warned), the Path Tracer's displacement, and hdUE's warnings in husk (`hdUE warning:` lines on stderr). Two scenes'
checks were wrong: the motion-blur-off scenes probed inside their cube (UsdGeomCube's fallback size is 2), and
lux/invisible_light measured husk's headlight, which husk adds when no light is visible; the runner now passes
`--headlight none`.
Passing in both modes among the new scenes: Capsule_1 / Cylinder_1, creases, corners and `interpolateBoundary`,
`triangleSubdivisionRule`, `!resetXformStack!`, DomeLight_1 `poleAxis`, PluginLight (warned), LightListAPI,
`treatAsPoint`, NodeGraph outputs (MaterialX `ND_multiply_color3` included), CoordSysAPI, UDIM patterns (warned in the
UE log: the texture is not imported and BaseColor stays untextured), collection binding strength, UsdSkel skinning and
blend shapes (with inbetweens), SpatialAudio and an unknown GenerativeProcedural (ignored).
Not in this USD (26.05, Houdini 22.0.368): DomeLight `poleAxis` (only DomeLight_1 has it); every other schema listed
above exists. Not tested yet (no scene): dome `mirroredBall`/`angular`/`cubeMapVerticalCross` formats, light
`specular` multiplier, `shaping:focus`/`cone:softness`, IES `angleScale`/`normalize`, semi-sharp creases,
`UsdGeomSubset` partition validation, NURBS trim curves, cards `cross`/`fromTexture`, `proxyVisibility`,
`motion:nonlinearSampleCount` and `velocityScale` (deprecated) effects, BlendShape `normalOffsets`, RenderPass
`cameraVisibility`/`matte`/`prune` collections, MaterialX nodes beyond image, multiply and opacity.

Generated by `run_conformance.py --matrix` from the last run (pass, partial passed/total, fail, error):

<!-- matrix:start -->
| Schema | Feature | Scene | Realtime | Path traced | Failed checks |
| --- | --- | --- | --- | --- | --- |
| UsdShadeMaterialBindingAPI | bindMaterialAs strongerThanDescendants | [binding/binding_strength](../samples/conformance/binding/binding_strength.usda) | pass | pass |  |
| UsdShadeMaterialBindingAPI | collection-based binding | [binding/collection](../samples/conformance/binding/collection.usda) | pass | pass |  |
| UsdShadeMaterialBindingAPI | collection binding bindMaterialAs strongerThanDescendants | [binding/collection_strength](../samples/conformance/binding/collection_strength.usda) | pass | pass |  |
| UsdGeomSubset | materialBind GeomSubset | [binding/geom_subset](../samples/conformance/binding/geom_subset.usda) | pass | pass |  |
| UsdShadeMaterialBindingAPI | binding inherited from an ancestor | [binding/inherited](../samples/conformance/binding/inherited.usda) | pass | pass |  |
| UsdShadeMaterialBindingAPI | material purpose: full vs preview vs allPurpose | [binding/purpose](../samples/conformance/binding/purpose.usda) | pass | pass |  |
| UsdGeomCamera | horizontalApertureOffset, verticalApertureOffset | [camera/aperture_offset](../samples/conformance/camera/aperture_offset.usda) | pass | pass |  |
| UsdGeomCamera | clippingRange | [camera/clipping_range](../samples/conformance/camera/clipping_range.usda) | pass | pass |  |
| UsdGeomCamera | fStop, focusDistance | [camera/depth_of_field](../samples/conformance/camera/depth_of_field.usda) | pass | pass |  |
| UsdGeomCamera | exposure, exposure:fStop/iso/time | [camera/exposure](../samples/conformance/camera/exposure.usda) | pass | pass |  |
| UsdGeomCamera | shutter:open / shutter:close | [camera/motion_blur](../samples/conformance/camera/motion_blur.usda) | pass | pass |  |
| UsdGeomCamera | projection orthographic | [camera/orthographic](../samples/conformance/camera/orthographic.usda) | pass | pass |  |
| UsdGeomCamera | camera under a scaled transform | [camera/scaled_parent](../samples/conformance/camera/scaled_parent.usda) | pass | pass |  |
| UsdGeomModelAPI | model:drawMode bounds, cards (model:cardGeometry, model:cardTexture*) | [geom/draw_mode](../samples/conformance/geom/draw_mode.usda) | partial 2/4 | partial 2/4 | bounds replaces the model by lines (pathtraced); bounds replaces the model by lines (realtime); the +Z card shows cardTextureZPos (pathtraced); the +Z card shows cardTextureZPos (realtime) |
| UsdGeomHermiteCurves | HermiteCurves | [geom/hermite_curves](../samples/conformance/geom/hermite_curves.usda) | partial 1/2 | partial 1/2 | curve arches through its middle (pathtraced); curve arches through its middle (realtime) |
| UsdGeomMotionAPI | motion:blurScale (unsupported, warned) | [geom/motion_blur_scale](../samples/conformance/geom/motion_blur_scale.usda) | partial 1/2 | partial 1/2 | warning names motion:blurScale (pathtraced); warning names motion:blurScale (realtime) |
| UsdGeomNurbsCurves | NurbsCurves (evaluated, not the control hull) | [geom/nurbs_curves](../samples/conformance/geom/nurbs_curves.usda) | partial 1/2 | partial 1/2 | curve passes through its peak (pathtraced); curve passes through its peak (realtime) |
| UsdGeomNurbsPatch | NurbsPatch surface (not its control hull) | [geom/nurbs_patch](../samples/conformance/geom/nurbs_patch.usda) | partial 1/3 | partial 1/3 | patch renders (pathtraced); patch renders (realtime); surface reaches its Bezier top (pathtraced); surface reaches its Bezier top (realtime) |
| UsdGeomXformable | !resetXformStack! | [geom/reset_xform_stack](../samples/conformance/geom/reset_xform_stack.usda) | pass | pass |  |
| UsdGeomTetMesh | TetMesh surface | [geom/tet_mesh](../samples/conformance/geom/tet_mesh.usda) | fail | fail | its surface renders (pathtraced); its surface renders (realtime) |
| UsdGeomVisibilityAPI | renderVisibility, guideVisibility | [geom/visibility_api](../samples/conformance/geom/visibility_api.usda) | partial 3/5 | partial 3/5 | guides are invisible by default (pathtraced); guides are invisible by default (realtime); renderVisibility invisible hides a render prim (pathtraced); renderVisibility invisible hides a render prim (realtime) |
| UsdGeomPointInstancer | accelerations motion blur | [instancer/accelerations](../samples/conformance/instancer/accelerations.usda) | pass | pass |  |
| UsdGeomPointInstancer | angularVelocities (unsupported, warned) | [instancer/angular_velocities](../samples/conformance/instancer/angular_velocities.usda) | partial 1/2 | partial 1/2 | warning names angularVelocities (pathtraced); warning names angularVelocities (realtime) |
| UsdGeomPointInstancer | instance-rate primvars (displayColor) | [instancer/display_color](../samples/conformance/instancer/display_color.usda) | pass | pass |  |
| UsdGeomPointInstancer | inactiveIds metadata | [instancer/inactive_ids](../samples/conformance/instancer/inactive_ids.usda) | pass | pass |  |
| UsdGeomPointInstancer | PointInstancer's own transform | [instancer/instancer_transform](../samples/conformance/instancer/instancer_transform.usda) | pass | pass |  |
| UsdGeomPointInstancer | invisibleIds (with ids) | [instancer/invisible_ids](../samples/conformance/instancer/invisible_ids.usda) | pass | pass |  |
| UsdGeomPointInstancer | nested PointInstancers | [instancer/nested](../samples/conformance/instancer/nested.usda) | pass | pass |  |
| UsdGeomPointInstancer | orientations (quath) | [instancer/orientations](../samples/conformance/instancer/orientations.usda) | pass | pass |  |
| UsdGeomPointInstancer | orientationsf (quatf), preferred over orientations | [instancer/orientationsf](../samples/conformance/instancer/orientationsf.usda) | pass | pass |  |
| UsdGeomPointInstancer | protoIndices, positions, prototypes | [instancer/proto_indices](../samples/conformance/instancer/proto_indices.usda) | pass | pass |  |
| UsdGeomPointInstancer | prototype root transform | [instancer/prototype_transform](../samples/conformance/instancer/prototype_transform.usda) | pass | pass |  |
| UsdGeomPointInstancer | prototypes beneath the instancer are not drawn | [instancer/prototypes_not_drawn](../samples/conformance/instancer/prototypes_not_drawn.usda) | pass | pass |  |
| UsdGeomPointInstancer | scales | [instancer/scales](../samples/conformance/instancer/scales.usda) | pass | pass |  |
| UsdGeomPointInstancer | velocities motion blur | [instancer/velocities](../samples/conformance/instancer/velocities.usda) | pass | pass |  |
| UsdLux | color | [lux/color](../samples/conformance/lux/color.usda) | pass | pass |  |
| UsdLux | enableColorTemperature, colorTemperature | [lux/color_temperature](../samples/conformance/lux/color_temperature.usda) | pass | pass |  |
| UsdLux | CylinderLight, exposure | [lux/cylinder_light](../samples/conformance/lux/cylinder_light.usda) | pass | pass |  |
| UsdLux | diffuse multiplier | [lux/diffuse_multiplier](../samples/conformance/lux/diffuse_multiplier.usda) | pass | pass |  |
| UsdLux | DiskLight, exposure | [lux/disk_light](../samples/conformance/lux/disk_light.usda) | pass | pass |  |
| UsdLux | DistantLight, exposure | [lux/distant_light](../samples/conformance/lux/distant_light.usda) | pass | pass |  |
| UsdLux DomeLight | texture:format latlong orientation | [lux/dome_latlong](../samples/conformance/lux/dome_latlong.usda) | fail | pass | lit from +Z: red (realtime); rotated 180: blue (realtime) |
| UsdLux | DomeLight, exposure | [lux/dome_light](../samples/conformance/lux/dome_light.usda) | pass | pass |  |
| UsdLux DomeLight_1 | poleAxis | [lux/dome_pole_axis](../samples/conformance/lux/dome_pole_axis.usda) | pass | pass |  |
| UsdLux | GeometryLight (unsupported, warned) | [lux/geometry_light](../samples/conformance/lux/geometry_light.usda) | pass | pass |  |
| UsdLux | intensity | [lux/intensity](../samples/conformance/lux/intensity.usda) | pass | pass |  |
| UsdLux | light visibility | [lux/invisible_light](../samples/conformance/lux/invisible_light.usda) | pass | pass |  |
| UsdLuxLightFilter | light:filters (unsupported, warned) | [lux/light_filter](../samples/conformance/lux/light_filter.usda) | pass | pass |  |
| UsdLux | light linking (collection:lightLink) | [lux/light_link](../samples/conformance/lux/light_link.usda) | pass | pass |  |
| UsdLux LightListAPI | lightList, lightList:cacheBehavior (a discovery cache) | [lux/light_list](../samples/conformance/lux/light_list.usda) | pass | pass |  |
| UsdLux MeshLightAPI | MeshLightAPI, light:materialSyncMode (unsupported, warned) | [lux/mesh_light](../samples/conformance/lux/mesh_light.usda) | partial 1/2 | partial 1/2 | warning names the mesh light (pathtraced); warning names the mesh light (realtime) |
| UsdLux | normalize | [lux/normalize](../samples/conformance/lux/normalize.usda) | pass | pass |  |
| UsdLux | PluginLight (unsupported, warned) | [lux/plugin_light](../samples/conformance/lux/plugin_light.usda) | pass | pass |  |
| UsdLux | PortalLight (unsupported, warned) | [lux/portal_light](../samples/conformance/lux/portal_light.usda) | partial 1/2 | partial 1/2 | warning names the portal light (pathtraced); warning names the portal light (realtime) |
| UsdLux | RectLight, exposure | [lux/rect_light](../samples/conformance/lux/rect_light.usda) | pass | pass |  |
| UsdLux ShadowAPI | shadow:color (unsupported, warned) | [lux/shadow_color](../samples/conformance/lux/shadow_color.usda) | pass | pass |  |
| UsdLux ShadowAPI | shadow:distance, shadow:falloff (unsupported, warned) | [lux/shadow_distance](../samples/conformance/lux/shadow_distance.usda) | partial 1/2 | partial 1/2 | warning names shadow:distance (pathtraced); warning names shadow:distance (realtime) |
| UsdLux ShadowAPI | shadow:enable | [lux/shadow_enable](../samples/conformance/lux/shadow_enable.usda) | pass | pass |  |
| UsdLux | shadow linking (collection:shadowLink) | [lux/shadow_link](../samples/conformance/lux/shadow_link.usda) | pass | pass |  |
| UsdLux | ShapingAPI cone angle | [lux/shaping_cone](../samples/conformance/lux/shaping_cone.usda) | pass | pass |  |
| UsdLux | ShapingAPI IES profile | [lux/shaping_ies](../samples/conformance/lux/shaping_ies.usda) | pass | pass |  |
| UsdLux | light size without normalize | [lux/size_unnormalized](../samples/conformance/lux/size_unnormalized.usda) | pass | pass |  |
| UsdLux | SphereLight, exposure | [lux/sphere_light](../samples/conformance/lux/sphere_light.usda) | pass | pass |  |
| UsdLux SphereLight | treatAsPoint | [lux/treat_as_point](../samples/conformance/lux/treat_as_point.usda) | pass | pass |  |
| UsdLux VolumeLightAPI | VolumeLightAPI (unsupported, warned) | [lux/volume_light](../samples/conformance/lux/volume_light.usda) | fail | fail | the volume still renders (pathtraced); the volume still renders (realtime); warning names the volume light (pathtraced); warning names the volume light (realtime) |
| MaterialX standard_surface (as UE Interchange imports it) | opacity (masked at OpacityMaskClipValue 1/3) | [materialx/opacity](../samples/conformance/materialx/opacity.usda) | pass | pass |  |
| MaterialX standard_surface (as UE Interchange imports it) | opacity cutout keeps coverage with distance (alpha-coverage mips) | [materialx/opacity_mips](../samples/conformance/materialx/opacity_mips.usda) | pass | pass |  |
| UsdGeomMesh | concave n-gon | [mesh/concave_polygon](../samples/conformance/mesh/concave_polygon.usda) | pass | pass |  |
| UsdGeomMesh | displayColor constant, uniform, vertex, faceVarying | [mesh/display_color_interpolation](../samples/conformance/mesh/display_color_interpolation.usda) | pass | pass |  |
| UsdGeomMesh | displayOpacity | [mesh/display_opacity](../samples/conformance/mesh/display_opacity.usda) | pass | pass |  |
| UsdGeomMesh | doubleSided (hdUE:cullBackface) | [mesh/double_sided](../samples/conformance/mesh/double_sided.usda) | pass | pass |  |
| UsdGeomMesh | holeIndices | [mesh/hole_indices](../samples/conformance/mesh/hole_indices.usda) | pass | pass |  |
| UsdGeom gprims | Cube, Sphere, Cone, Cylinder, Capsule, Plane | [mesh/implicit_gprims](../samples/conformance/mesh/implicit_gprims.usda) | pass | pass |  |
| UsdGeom gprims | Cylinder_1, Capsule_1 radiusTop, radiusBottom | [mesh/implicit_gprims_1](../samples/conformance/mesh/implicit_gprims_1.usda) | pass | pass |  |
| UsdGeomMesh | authored normals: vertex, faceVarying, uniform | [mesh/normals_interpolation](../samples/conformance/mesh/normals_interpolation.usda) | pass | pass |  |
| UsdGeomMesh | orientation leftHanded | [mesh/orientation](../samples/conformance/mesh/orientation.usda) | pass | pass |  |
| UsdGeomMesh | primvars:normals over normals | [mesh/primvars_normals](../samples/conformance/mesh/primvars_normals.usda) | pass | pass |  |
| UsdGeomImageable | purpose | [mesh/purpose](../samples/conformance/mesh/purpose.usda) | pass | pass |  |
| UsdGeomMesh | hdUE:cullBackface = 0 renders single-sided back faces (as Karma) | [mesh/single_sided](../samples/conformance/mesh/single_sided.usda) | pass | pass |  |
| UsdGeomMesh | interpolateBoundary, cornerIndices, cornerSharpnesses | [mesh/subdiv_boundary_corners](../samples/conformance/mesh/subdiv_boundary_corners.usda) | pass | pass |  |
| UsdGeomMesh | creaseIndices, creaseLengths, creaseSharpnesses | [mesh/subdiv_creases](../samples/conformance/mesh/subdiv_creases.usda) | pass | pass |  |
| UsdGeomMesh | faceVaryingLinearInterpolation all vs none | [mesh/subdiv_fvar_linear](../samples/conformance/mesh/subdiv_fvar_linear.usda) | partial 1/2 | partial 1/2 | none darkens the bright corner (pathtraced); none darkens the bright corner (realtime) |
| UsdGeomMesh | subdivisionScheme none vs catmullClark | [mesh/subdivision_scheme](../samples/conformance/mesh/subdivision_scheme.usda) | pass | pass |  |
| UsdGeomMesh | triangleSubdivisionRule catmullClark vs smooth | [mesh/triangle_subdivision_rule](../samples/conformance/mesh/triangle_subdivision_rule.usda) | pass | pass |  |
| UsdGeomMesh | multiple UV sets (st, st1) | [mesh/uv_sets](../samples/conformance/mesh/uv_sets.usda) | pass | pass |  |
| UsdGeomImageable | visibility (and inheritance) | [mesh/visibility](../samples/conformance/mesh/visibility.usda) | pass | pass |  |
| native instancing | native instance as a PointInstancer prototype | [native_instancing/in_point_instancer](../samples/conformance/native_instancing/in_point_instancer.usda) | pass | pass |  |
| native instancing | material bound on the instance root | [native_instancing/instance_material_binding](../samples/conformance/native_instancing/instance_material_binding.usda) | pass | pass |  |
| native instancing | constant primvars inherited into instances | [native_instancing/instance_primvars](../samples/conformance/native_instancing/instance_primvars.usda) | pass | pass |  |
| native instancing | instanceable references | [native_instancing/instanceable_references](../samples/conformance/native_instancing/instanceable_references.usda) | pass | pass |  |
| native instancing | nested instancing | [native_instancing/nested](../samples/conformance/native_instancing/nested.usda) | pass | pass |  |
| UsdProcGenerativeProcedural | GenerativeProcedural with an unknown procedural (ignored) | [other/generative_procedural](../samples/conformance/other/generative_procedural.usda) | pass | pass |  |
| UsdMediaSpatialAudio | SpatialAudio (ignored) | [other/spatial_audio](../samples/conformance/other/spatial_audio.usda) | pass | pass |  |
| UsdGeomBasisCurves | cubic bezier, bspline, catmullRom | [points_curves/cubic_bases](../samples/conformance/points_curves/cubic_bases.usda) | pass | pass |  |
| UsdGeomBasisCurves | uniform displayColor per curve | [points_curves/curve_display_color](../samples/conformance/points_curves/curve_display_color.usda) | pass | pass |  |
| UsdGeomBasisCurves | varying widths | [points_curves/curve_widths](../samples/conformance/points_curves/curve_widths.usda) | pass | pass |  |
| UsdGeomBasisCurves | linear curves, widths | [points_curves/linear_curves](../samples/conformance/points_curves/linear_curves.usda) | pass | pass |  |
| UsdGeomBasisCurves | wrap periodic | [points_curves/periodic](../samples/conformance/points_curves/periodic.usda) | pass | pass |  |
| UsdGeomPoints | per-point displayColor | [points_curves/points_display_color](../samples/conformance/points_curves/points_display_color.usda) | pass | pass |  |
| UsdGeomPoints | points, widths | [points_curves/points_widths](../samples/conformance/points_curves/points_widths.usda) | pass | pass |  |
| UsdPreviewSurface | clearcoat, clearcoatRoughness | [preview_surface/clearcoat](../samples/conformance/preview_surface/clearcoat.usda) | pass | pass |  |
| UsdPreviewSurface | diffuseColor | [preview_surface/diffuse_color](../samples/conformance/preview_surface/diffuse_color.usda) | pass | pass |  |
| UsdPreviewSurface | displacement | [preview_surface/displacement](../samples/conformance/preview_surface/displacement.usda) | pass | pass |  |
| UsdPreviewSurface | emissiveColor | [preview_surface/emissive_color](../samples/conformance/preview_surface/emissive_color.usda) | pass | pass |  |
| UsdPreviewSurface | ior | [preview_surface/ior](../samples/conformance/preview_surface/ior.usda) | pass | pass |  |
| UsdPreviewSurface | metallic | [preview_surface/metallic](../samples/conformance/preview_surface/metallic.usda) | pass | pass |  |
| UsdPreviewSurface | normal (tangent-space map) | [preview_surface/normal_map](../samples/conformance/preview_surface/normal_map.usda) | pass | pass |  |
| UsdPreviewSurface | occlusion | [preview_surface/occlusion](../samples/conformance/preview_surface/occlusion.usda) | pass | pass |  |
| UsdPreviewSurface | opacity (presence) | [preview_surface/opacity](../samples/conformance/preview_surface/opacity.usda) | pass | pass |  |
| UsdPreviewSurface | opacityThreshold (cutout) | [preview_surface/opacity_threshold](../samples/conformance/preview_surface/opacity_threshold.usda) | pass | pass |  |
| UsdPreviewSurface | roughness | [preview_surface/roughness](../samples/conformance/preview_surface/roughness.usda) | pass | pass |  |
| UsdPreviewSurface | useSpecularWorkflow, specularColor | [preview_surface/specular_workflow](../samples/conformance/preview_surface/specular_workflow.usda) | pass | pass |  |
| UsdRenderVar | albedo and normal render vars | [render_settings/albedo_normal](../samples/conformance/render_settings/albedo_normal.usda) | pass | pass |  |
| UsdRenderVar | buffer visualization render vars | [render_settings/buffer_aovs](../samples/conformance/render_settings/buffer_aovs.usda) | pass | pass |  |
| UsdRenderSettings | camera relationship | [render_settings/camera_rel](../samples/conformance/render_settings/camera_rel.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy adjustApertureHeight | [render_settings/conform_adjustApertureHeight](../samples/conformance/render_settings/conform_adjustApertureHeight.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy adjustApertureWidth | [render_settings/conform_adjustApertureWidth](../samples/conformance/render_settings/conform_adjustApertureWidth.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy adjustPixelAspectRatio | [render_settings/conform_adjustPixelAspectRatio](../samples/conformance/render_settings/conform_adjustPixelAspectRatio.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy cropAperture | [render_settings/conform_cropAperture](../samples/conformance/render_settings/conform_cropAperture.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy expandAperture | [render_settings/conform_expandAperture](../samples/conformance/render_settings/conform_expandAperture.usda) | pass | pass |  |
| UsdRenderVar | Cryptomatte render vars | [render_settings/cryptomatte](../samples/conformance/render_settings/cryptomatte.usda) | pass | pass |  |
| UsdRenderSettings | dataWindowNDC | [render_settings/data_window](../samples/conformance/render_settings/data_window.usda) | pass | pass |  |
| UsdRenderVar | depth AOV | [render_settings/depth_var](../samples/conformance/render_settings/depth_var.usda) | pass | pass |  |
| UsdRenderSettings | disableDepthOfField | [render_settings/disable_depth_of_field](../samples/conformance/render_settings/disable_depth_of_field.usda) | pass | pass |  |
| UsdRenderSettings | disableMotionBlur | [render_settings/disableMotionBlur](../samples/conformance/render_settings/disableMotionBlur.usda) | pass | pass |  |
| hdUE render settings | hdUE:postProcess:DepthOfFieldEnabled | [render_settings/dof_enabled_off](../samples/conformance/render_settings/dof_enabled_off.usda) | pass | pass |  |
| UsdRenderSettings | includedPurposes | [render_settings/included_purposes](../samples/conformance/render_settings/included_purposes.usda) | pass | pass |  |
| UsdRenderSettings | instantaneousShutter | [render_settings/instantaneousShutter](../samples/conformance/render_settings/instantaneousShutter.usda) | pass | pass |  |
| UsdRenderVar | lighting component render vars | [render_settings/lighting_aovs](../samples/conformance/render_settings/lighting_aovs.usda) | pass | pass |  |
| UsdRenderSettings | materialBindingPurposes | [render_settings/material_binding_purposes](../samples/conformance/render_settings/material_binding_purposes.usda) | pass | pass |  |
| UsdRenderProduct | several RenderVars in one product | [render_settings/multi_var_product](../samples/conformance/render_settings/multi_var_product.usda) | pass | pass |  |
| UsdRenderSettings | pixelAspectRatio | [render_settings/pixel_aspect_ratio](../samples/conformance/render_settings/pixel_aspect_ratio.usda) | pass | pass |  |
| hdUE post-process | texture setting from an image file | [render_settings/postprocess_texture_file](../samples/conformance/render_settings/postprocess_texture_file.usda) | pass | pass |  |
| UsdRenderPass | collection:renderVisibility, renderSource | [render_settings/render_pass](../samples/conformance/render_settings/render_pass.usda) | partial 1/2 | partial 1/2 | non-member does not render (pathtraced); non-member does not render (realtime) |
| UsdRenderSettings | renderingColorSpace (unsupported, warned) | [render_settings/rendering_color_space](../samples/conformance/render_settings/rendering_color_space.usda) | partial 1/2 | partial 1/2 | warning names renderingColorSpace (pathtraced); warning names renderingColorSpace (realtime) |
| UsdRenderSettings | resolution | [render_settings/resolution](../samples/conformance/render_settings/resolution.usda) | pass | pass |  |
| UsdRenderVar | unsupported RenderVar (warned) | [render_settings/unsupported_var](../samples/conformance/render_settings/unsupported_var.usda) | pass | pass |  |
| UsdShadeCoordSysAPI | coordSys:<name>:binding (no visible effect) | [shade/coord_sys](../samples/conformance/shade/coord_sys.usda) | pass | pass |  |
| UsdShadeNodeGraph | NodeGraph outputs (Preview Surface and MaterialX) | [shade/node_graph](../samples/conformance/shade/node_graph.usda) | pass | pass |  |
| UsdSkelBlendShape | BlendShape offsets, inbetweens | [skel/blend_shape](../samples/conformance/skel/blend_shape.usda) | pass | pass |  |
| UsdSkel | SkelRoot, Skeleton, SkelAnimation skinning | [skel/skinning](../samples/conformance/skel/skinning.usda) | pass | pass |  |
| UsdUVTexture | outputs:a to opacity with opacityThreshold | [texture/alpha_cutout](../samples/conformance/texture/alpha_cutout.usda) | pass | pass |  |
| UsdUVTexture | fallback when the file cannot be read | [texture/fallback](../samples/conformance/texture/fallback.usda) | pass | pass |  |
| UsdUVTexture | scale and bias | [texture/scale_bias](../samples/conformance/texture/scale_bias.usda) | pass | pass |  |
| UsdUVTexture | single-channel output (outputs:r) | [texture/single_channel](../samples/conformance/texture/single_channel.usda) | pass | pass |  |
| UsdUVTexture | sourceColorSpace raw, sRGB, auto | [texture/source_color_space](../samples/conformance/texture/source_color_space.usda) | pass | pass |  |
| UsdTransform2d | UsdTransform2d translation, rotation | [texture/transform2d](../samples/conformance/texture/transform2d.usda) | pass | pass |  |
| UsdUVTexture | <UDIM> file pattern (unsupported at runtime, warned) | [texture/udim](../samples/conformance/texture/udim.usda) | pass | pass |  |
| UsdUVTexture | wrapS / wrapT: repeat, clamp, mirror, black | [texture/wrap_modes](../samples/conformance/texture/wrap_modes.usda) | pass | pass |  |
| UsdVolVolume | Volume with an OpenVDBAsset density field | [volume/openvdb](../samples/conformance/volume/openvdb.usda) | pass | pass |  |
| UsdVolParticleField3DGaussianSplat | ParticleField3DGaussianSplat (unsupported, warned) | [volume/particle_field](../samples/conformance/volume/particle_field.usda) | partial 1/2 | partial 1/2 | warning names the particle field (pathtraced); warning names the particle field (realtime) |
<!-- matrix:end -->
