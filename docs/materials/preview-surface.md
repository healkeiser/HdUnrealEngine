# Preview Surface inputs

Every `UsdPreviewSurface` input is translated, as a constant or a `UsdUVTexture` (with `scale`, `bias`, a single-channel output and the first `UsdTransform2d`):

| Input | In UE |
| --- | --- |
| `diffuseColor`, `emissiveColor` | Base colour and emission |
| `metallic`, `roughness` | Metalness workflow |
| `ior` | Dielectric reflectance, F0 = ((ior - 1) / (ior + 1))² |
| `useSpecularWorkflow`, `specularColor` | Specular workflow: diffuse and F0 straight from `diffuseColor` and `specularColor` |
| `clearcoat`, `clearcoatRoughness` | A white coat layered over the base (the layered Standard Surface parent, coat IOR 1.5), only on materials with `clearcoat` above 0. With `useSpecularWorkflow` or partial opacity it stays a second specular lobe on the base (Substrate haziness) |
| `opacity`, `opacityThreshold` | See-through (translucent) or cutout (masked). `opacityMode` is not read: partial opacity is presence (it scales specular and emission too), so thin glass loses part of its Fresnel reflections (rims at 0.8 × Karma, which keeps them in both modes) |
| `normal` | Tangent-space normal map (USD's +Y up) |
| `occlusion` | Ambient occlusion |
| `displacement` | Moves the mesh's vertices along the normal (see [Displacement](../geometry/displacement.md)) |

`UsdUVTexture` `wrapS`/`wrapT` (MaterialX `uaddressmode`/`vaddressmode`) are honoured: `repeat`, `clamp`, `mirror`, and `black`, which clamps and reads the input's `bias` outside [0, 1]. `useMetadata`, the default, does not read the file's wrap metadata and falls back to `black`, as the spec says for files without it (Karma does the same). `sourceColorSpace` (or the file's `colorSpace` metadata) decides whether 8-bit texels are sRGB-decoded: `raw` and linear spaces read linear, `sRGB` decodes, `auto` decodes colour inputs only.
