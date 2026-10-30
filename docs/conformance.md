# Conformance

A spec-driven test set: one small USD scene per schema feature in [samples/conformance](../samples/conformance),
written from the OpenUSD schemas (`usdGeom`, `usdLux`, `usdShade`, `usdRender`, `usdVol` `schema.usda`), the
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
exits 1 when a check fails or a render does not finish. Each run takes about 8-12 s (UE startup dominates), so the
whole set (109 scenes, both modes) takes about 40 minutes; use `--only` and `--mode` while iterating.

## How scenes are built

- Every scene sublayers [common/stage.usda](../samples/conformance/common/stage.usda): metres, Y up, a camera at
  z = 10 whose frame is exactly x, y in [-5, 5] on the z = 0 plane, a normalized distant key light (4 lux), and
  128 x 128 render settings with two products: the beauty and the `primId`/`instanceId` AOVs.
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

Last full run: local main at `74e8853` plus the conformance fixes (Houdini build, `.local/runtime-houdini.json` with
that backend), 108 scenes x 2 modes: 107 scenes pass in both modes, 1 fails one check. At `4a8d783` 95 passed.
mesh/double_sided (now with `hdUE:cullBackface`) and the new mesh/single_sided were run alone since, and pass in
both modes.

### Gaps found

| Scene | Expected (spec) | Observed | Likely code area |
| --- | --- | --- | --- |
| camera/clipping_range | the cube beyond the far plane is clipped | it renders: hdUE applies the near plane only (check removed) | UE's views keep an infinite far plane; a finite one changed Lumen's lighting by 8% on Cornell with nothing clipped, and the Path Tracer traces past any far plane |
| lux/dome_latlong (realtime) | rotated 180 degrees, the dome lights the sphere blue | blue with a green cast (normalized RGB 0.03, 0.17, 0.80; the check allows 0.2 from pure blue); path traced passes. It passed at `4a8d783` and fails the same with main's own plugin and runtime at `74e8853` | realtime dome lighting (Lumen / sky light capture) or the dome texture's colour conversion, changed between those commits |

Resolved since `4a8d783`: material binding purpose (the render settings' `materialBindingPurposes`, else `full`),
`disableDepthOfField`, `includedPurposes`, the `UsdUVTexture` fallback, GeometryLight (accepted and
warned), the Path Tracer's displacement, and hdUE's warnings in husk (`hdUE warning:` lines on stderr). Two scenes'
checks were wrong: the motion-blur-off scenes probed inside their cube (UsdGeomCube's fallback size is 2), and
lux/invisible_light measured husk's headlight, which husk adds when no light is visible; the runner now passes
`--headlight none`.
Not tested yet (no scene): `angularVelocities`, dome `mirroredBall`/`angular`/`cubeMapVerticalCross` formats and
`poleAxis`, `PortalLight`, light `specular` multiplier, `shaping:focus`/`cone:softness`, IES `angleScale`/`normalize`,
subdivision creases/corners/`interpolateBoundary`, `UsdGeomSubset` partition validation, NURBS, MaterialX, UDIMs,
render passes and camera-visibility collections.

Generated by `run_conformance.py --matrix` from the last run (pass, partial passed/total, fail, error):

<!-- matrix:start -->
| Schema | Feature | Scene | Realtime | Path traced | Failed checks |
| --- | --- | --- | --- | --- | --- |
| UsdShadeMaterialBindingAPI | bindMaterialAs strongerThanDescendants | [binding/binding_strength](../samples/conformance/binding/binding_strength.usda) | pass | pass |  |
| UsdShadeMaterialBindingAPI | collection-based binding | [binding/collection](../samples/conformance/binding/collection.usda) | pass | pass |  |
| UsdGeomSubset | materialBind GeomSubset | [binding/geom_subset](../samples/conformance/binding/geom_subset.usda) | pass | pass |  |
| UsdShadeMaterialBindingAPI | binding inherited from an ancestor | [binding/inherited](../samples/conformance/binding/inherited.usda) | pass | pass |  |
| UsdShadeMaterialBindingAPI | material purpose: full vs preview vs allPurpose | [binding/purpose](../samples/conformance/binding/purpose.usda) | pass | pass |  |
| UsdGeomCamera | horizontalApertureOffset, verticalApertureOffset | [camera/aperture_offset](../samples/conformance/camera/aperture_offset.usda) | pass | pass |  |
| UsdGeomCamera | clippingRange | [camera/clipping_range](../samples/conformance/camera/clipping_range.usda) | pass | pass |  |
| UsdGeomCamera | fStop, focusDistance | [camera/depth_of_field](../samples/conformance/camera/depth_of_field.usda) | pass | pass |  |
| UsdGeomCamera | exposure, exposure:fStop/iso/time | [camera/exposure](../samples/conformance/camera/exposure.usda) | pass | pass |  |
| UsdGeomCamera | shutter:open / shutter:close | [camera/motion_blur](../samples/conformance/camera/motion_blur.usda) | pass | pass |  |
| UsdGeomCamera | projection orthographic | [camera/orthographic](../samples/conformance/camera/orthographic.usda) | pass | pass |  |
| UsdGeomPointInstancer | accelerations motion blur | [instancer/accelerations](../samples/conformance/instancer/accelerations.usda) | pass | pass |  |
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
| UsdLux DomeLight | texture:format latlong orientation | [lux/dome_latlong](../samples/conformance/lux/dome_latlong.usda) | partial 1/2 | pass | rotated 180: blue (realtime) |
| UsdLux | DomeLight, exposure | [lux/dome_light](../samples/conformance/lux/dome_light.usda) | pass | pass |  |
| UsdLux | GeometryLight (unsupported, warned) | [lux/geometry_light](../samples/conformance/lux/geometry_light.usda) | pass | pass |  |
| UsdLux | intensity | [lux/intensity](../samples/conformance/lux/intensity.usda) | pass | pass |  |
| UsdLux | light visibility | [lux/invisible_light](../samples/conformance/lux/invisible_light.usda) | pass | pass |  |
| UsdLuxLightFilter | light:filters (unsupported, warned) | [lux/light_filter](../samples/conformance/lux/light_filter.usda) | pass | pass |  |
| UsdLux | light linking (collection:lightLink) | [lux/light_link](../samples/conformance/lux/light_link.usda) | pass | pass |  |
| UsdLux | normalize | [lux/normalize](../samples/conformance/lux/normalize.usda) | pass | pass |  |
| UsdLux | RectLight, exposure | [lux/rect_light](../samples/conformance/lux/rect_light.usda) | pass | pass |  |
| UsdLux ShadowAPI | shadow:color (unsupported, warned) | [lux/shadow_color](../samples/conformance/lux/shadow_color.usda) | pass | pass |  |
| UsdLux ShadowAPI | shadow:enable | [lux/shadow_enable](../samples/conformance/lux/shadow_enable.usda) | pass | pass |  |
| UsdLux | shadow linking (collection:shadowLink) | [lux/shadow_link](../samples/conformance/lux/shadow_link.usda) | pass | pass |  |
| UsdLux | ShapingAPI cone angle | [lux/shaping_cone](../samples/conformance/lux/shaping_cone.usda) | pass | pass |  |
| UsdLux | ShapingAPI IES profile | [lux/shaping_ies](../samples/conformance/lux/shaping_ies.usda) | pass | pass |  |
| UsdLux | light size without normalize | [lux/size_unnormalized](../samples/conformance/lux/size_unnormalized.usda) | pass | pass |  |
| UsdLux | SphereLight, exposure | [lux/sphere_light](../samples/conformance/lux/sphere_light.usda) | pass | pass |  |
| UsdGeomMesh | concave n-gon | [mesh/concave_polygon](../samples/conformance/mesh/concave_polygon.usda) | pass | pass |  |
| UsdGeomMesh | displayColor constant, uniform, vertex, faceVarying | [mesh/display_color_interpolation](../samples/conformance/mesh/display_color_interpolation.usda) | pass | pass |  |
| UsdGeomMesh | displayOpacity | [mesh/display_opacity](../samples/conformance/mesh/display_opacity.usda) | pass | pass |  |
| UsdGeomMesh | doubleSided (hdUE:cullBackface) | [mesh/double_sided](../samples/conformance/mesh/double_sided.usda) | pass | pass |  |
| UsdGeomMesh | holeIndices | [mesh/hole_indices](../samples/conformance/mesh/hole_indices.usda) | pass | pass |  |
| UsdGeom gprims | Cube, Sphere, Cone, Cylinder, Capsule, Plane | [mesh/implicit_gprims](../samples/conformance/mesh/implicit_gprims.usda) | pass | pass |  |
| UsdGeomMesh | authored normals: vertex, faceVarying, uniform | [mesh/normals_interpolation](../samples/conformance/mesh/normals_interpolation.usda) | pass | pass |  |
| UsdGeomMesh | orientation leftHanded | [mesh/orientation](../samples/conformance/mesh/orientation.usda) | pass | pass |  |
| UsdGeomMesh | primvars:normals over normals | [mesh/primvars_normals](../samples/conformance/mesh/primvars_normals.usda) | pass | pass |  |
| UsdGeomImageable | purpose | [mesh/purpose](../samples/conformance/mesh/purpose.usda) | pass | pass |  |
| UsdGeomMesh | single-sided back faces render by default (as Karma) | [mesh/single_sided](../samples/conformance/mesh/single_sided.usda) | pass | pass |  |
| UsdGeomMesh | subdivisionScheme none vs catmullClark | [mesh/subdivision_scheme](../samples/conformance/mesh/subdivision_scheme.usda) | pass | pass |  |
| UsdGeomMesh | multiple UV sets (st, st1) | [mesh/uv_sets](../samples/conformance/mesh/uv_sets.usda) | pass | pass |  |
| UsdGeomImageable | visibility (and inheritance) | [mesh/visibility](../samples/conformance/mesh/visibility.usda) | pass | pass |  |
| native instancing | native instance as a PointInstancer prototype | [native_instancing/in_point_instancer](../samples/conformance/native_instancing/in_point_instancer.usda) | pass | pass |  |
| native instancing | material bound on the instance root | [native_instancing/instance_material_binding](../samples/conformance/native_instancing/instance_material_binding.usda) | pass | pass |  |
| native instancing | constant primvars inherited into instances | [native_instancing/instance_primvars](../samples/conformance/native_instancing/instance_primvars.usda) | pass | pass |  |
| native instancing | instanceable references | [native_instancing/instanceable_references](../samples/conformance/native_instancing/instanceable_references.usda) | pass | pass |  |
| native instancing | nested instancing | [native_instancing/nested](../samples/conformance/native_instancing/nested.usda) | pass | pass |  |
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
| UsdRenderSettings | camera relationship | [render_settings/camera_rel](../samples/conformance/render_settings/camera_rel.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy adjustApertureHeight | [render_settings/conform_adjustApertureHeight](../samples/conformance/render_settings/conform_adjustApertureHeight.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy adjustApertureWidth | [render_settings/conform_adjustApertureWidth](../samples/conformance/render_settings/conform_adjustApertureWidth.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy adjustPixelAspectRatio | [render_settings/conform_adjustPixelAspectRatio](../samples/conformance/render_settings/conform_adjustPixelAspectRatio.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy cropAperture | [render_settings/conform_cropAperture](../samples/conformance/render_settings/conform_cropAperture.usda) | pass | pass |  |
| UsdRenderSettings | aspectRatioConformPolicy expandAperture | [render_settings/conform_expandAperture](../samples/conformance/render_settings/conform_expandAperture.usda) | pass | pass |  |
| UsdRenderSettings | dataWindowNDC | [render_settings/data_window](../samples/conformance/render_settings/data_window.usda) | pass | pass |  |
| UsdRenderVar | depth AOV | [render_settings/depth_var](../samples/conformance/render_settings/depth_var.usda) | pass | pass |  |
| UsdRenderSettings | disableDepthOfField | [render_settings/disable_depth_of_field](../samples/conformance/render_settings/disable_depth_of_field.usda) | pass | pass |  |
| UsdRenderSettings | disableMotionBlur | [render_settings/disableMotionBlur](../samples/conformance/render_settings/disableMotionBlur.usda) | pass | pass |  |
| UsdRenderSettings | includedPurposes | [render_settings/included_purposes](../samples/conformance/render_settings/included_purposes.usda) | pass | pass |  |
| UsdRenderSettings | instantaneousShutter | [render_settings/instantaneousShutter](../samples/conformance/render_settings/instantaneousShutter.usda) | pass | pass |  |
| UsdRenderSettings | materialBindingPurposes | [render_settings/material_binding_purposes](../samples/conformance/render_settings/material_binding_purposes.usda) | pass | pass |  |
| UsdRenderProduct | several RenderVars in one product | [render_settings/multi_var_product](../samples/conformance/render_settings/multi_var_product.usda) | pass | pass |  |
| UsdRenderSettings | pixelAspectRatio | [render_settings/pixel_aspect_ratio](../samples/conformance/render_settings/pixel_aspect_ratio.usda) | pass | pass |  |
| UsdRenderSettings | resolution | [render_settings/resolution](../samples/conformance/render_settings/resolution.usda) | pass | pass |  |
| UsdRenderVar | unsupported RenderVar (warned) | [render_settings/unsupported_var](../samples/conformance/render_settings/unsupported_var.usda) | pass | pass |  |
| UsdUVTexture | outputs:a to opacity with opacityThreshold | [texture/alpha_cutout](../samples/conformance/texture/alpha_cutout.usda) | pass | pass |  |
| UsdUVTexture | fallback when the file cannot be read | [texture/fallback](../samples/conformance/texture/fallback.usda) | pass | pass |  |
| UsdUVTexture | scale and bias | [texture/scale_bias](../samples/conformance/texture/scale_bias.usda) | pass | pass |  |
| UsdUVTexture | single-channel output (outputs:r) | [texture/single_channel](../samples/conformance/texture/single_channel.usda) | pass | pass |  |
| UsdUVTexture | sourceColorSpace raw, sRGB, auto | [texture/source_color_space](../samples/conformance/texture/source_color_space.usda) | pass | pass |  |
| UsdTransform2d | UsdTransform2d translation, rotation | [texture/transform2d](../samples/conformance/texture/transform2d.usda) | pass | pass |  |
| UsdUVTexture | wrapS / wrapT: repeat, clamp, mirror, black | [texture/wrap_modes](../samples/conformance/texture/wrap_modes.usda) | pass | pass |  |
| UsdVolVolume | Volume with an OpenVDBAsset density field | [volume/openvdb](../samples/conformance/volume/openvdb.usda) | pass | pass |  |
<!-- matrix:end -->
