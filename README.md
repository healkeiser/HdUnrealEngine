# hdUE

hdUE is a Hydra render delegate that renders OpenUSD with **Unreal Engine 5.8** inside the host process: the Houdini Solaris viewport, husk and the USD Render ROP, usdview, or a standalone host. It renders with Lumen (realtime) or UE's Path Tracer.

It is a Windows x64 proof of concept. Everything below was checked against Karma CPU with husk; the images are those renders, not mock-ups.

## Gallery

Left: hdUE (UE Path Tracer). Right: Karma CPU. Same USD file, same husk command, only `--renderer` changes.

| | |
| --- | --- |
| **Motion blur** (transforms, camera, `velocities`) | ![Motion blur](docs/images/mblur.png) |
| **Depth of field** (`fStop`, `focusDistance`) | ![Depth of field](docs/images/dof.png) |
| **Lens shift** (aperture offsets) | ![Lens shift](docs/images/shift.png) |
| **Orthographic camera** | ![Orthographic](docs/images/ortho.png) |
| **Camera exposure** | ![Exposure](docs/images/exposure.png) |
| **Volumes from SOPs** (VDB and native Houdini volumes) | ![Volumes](docs/images/volumes.png) |
| **Curves, points, instances** with their `primId` / `instanceId` AOVs | ![Shapes](docs/images/shapes.png) |

In the motion-blur image, hdUE blurs the points and instances from their `velocities`; Karma, with its default render settings, left them sharp. With glossy Preview Surfaces, hdUE comes out about 1.3x brighter than Karma; with fully rough ones they match to 0.1%. The difference is in the specular models, not light calibration. Without a bound material, each renderer shades `displayColor` with its own default, so compare with bound materials.

## What it supports

| Area | Supported | Not yet |
| --- | --- | --- |
| Geometry | Meshes and GeomSubsets, implicit prims (Cube, Sphere, ...), Points, BasisCurves, PointInstancers (nested), `displayColor` per face/vertex/point/curve/instance | Subdivision, displacement, varying `displayOpacity` |
| Volumes | OpenVDB files, unsaved SOP volumes (`op:` paths, VDB and Houdini volumes), `.bgeo.sc` written by the USD Render ROP; `density` field | Temperature/emission, volume materials, frustum grids, volume shadows in realtime |
| Materials | UsdPreviewSurface with every texture input and cutouts, prepared MaterialX (Standard Surface, OpenPBR) and Substrate, COP textures | Arbitrary MaterialX at runtime |
| Lights | Distant, sphere, rect, disk, cylinder, dome (HDRI), UsdLux units, spot cones | Light linking, IES, portals |
| Camera | Perspective, orthographic, lens shift, depth of field, exposure, motion blur (transforms, camera, point and instance `velocities`) | Deformation blur, lens distortion |
| Output | Beauty, depth, albedo, normal, roughness, metallic, specular, occlusion, `primId`, `instanceId`; husk, USD Render ROP, Render Gallery, multi-frame | LPE AOVs, cryptomatte, deep output |
| Houdini | Viewport renderer, **Unreal Engine** tab on Render Settings LOPs, selection highlight, installable package | Houdini versions other than 22.0 |

## Install (Houdini 22.0, Windows)

Unzip `hdUE-<version>-houdini22.0-win64.zip` somewhere writable and run `Install.cmd`. Then pick **Unreal Engine (hdUE)** in the Solaris viewport's renderer menu, or render with:

```powershell
husk --renderer HdUEHoudiniRendererPlugin -f 1 -n 10 -o render.$F4.exr scene.usd
```

The target machine needs Houdini 22.0, a DirectX 12 GPU with hardware ray tracing, and nothing else (no Unreal, Visual Studio or OpenUSD). See [Installing](docs/houdini.md#installing-on-another-machine) for studio-wide installs.

Renderer settings live on any Render Settings LOP, on its **Unreal Engine** tab: render mode (Lumen or Path Tracer), samples, motion blur and motion samples, Lumen, environment resolution. They are `hdUE:*` attributes on the UsdRenderSettings prim, so files rendered on a farm carry them.

## Documentation

- [Houdini Solaris, husk and AOVs](docs/houdini.md)
- [Rendering reference](docs/rendering.md): geometry, cameras, motion blur, lights, volumes, opacity
- [Materials](docs/materials.md): MaterialX, Substrate, COP textures, cooked native assets
- [Building from source](docs/building.md): requirements, configuration, validation, usdview
- [Architecture and ABI](docs/architecture.md)
- [Verification coverage](VERIFICATION.md)

## License

Original hdUE code, documentation, and project-generated synthetic samples are available under the [MIT License](LICENSE).

Unreal Engine, Houdini, third-party libraries, and separately supplied content retain their respective licenses. The MIT license does not grant rights to vendor SDKs, engine code, binaries, or third-party assets. Building, using, or distributing those components requires compliance with their applicable terms. See [Third-party notices](THIRD_PARTY_NOTICES.md).
