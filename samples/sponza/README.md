# Intel Sponza 2022

The README's Sponza gallery renders [Intel's Sponza 2022](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) (Frank Meinl and Anton Kaplanyan, CC BY 4.0). It is not part of this repository: download it from Intel and place the files below in `samples/sponza/intel/`, which git ignores.

```
samples/sponza/
├── lighting.usda                 sun, sky dome and render settings (tracked)
├── materials.usda                MaterialX materials on Intel's textures (tracked, generated)
└── intel/                        Intel's download (ignored)
    ├── main/                     "Main Sponza" package
    │   ├── NewSponza_Main_USD_Yup_003.usda
    │   ├── credits_license.txt
    │   ├── reference/            Intel's own renders (Render_*), to compare against
    │   └── textures/
    ├── curtains/                 "PKG A: Curtains" add-on
    │   ├── NewSponza_Curtains_USD_YUp.usda
    │   ├── credits_license.txt
    │   ├── reference/            Intel's own renders (Render_*), to compare against
    │   └── textures/
    └── cypress_tree/             "PKG C: Cypress Tree" add-on
        ├── NewSponza_CypressTree_USDA_YUp.usda
        ├── credits_license.txt
        ├── reference/
        └── textures/
```

Only the Y-up USD files and the textures they reference are needed (about 3.2 GB); the 3ds Max, FBX and glTF files in Intel's packages are not used. Each package's `Render_*` images go in its `reference/` folder. Then render with:

```
hython scripts/render_gallery.py --set sponza --dev
```

## MaterialX materials

Intel exports UsdPreviewSurface materials only. `materials.usda` adds a MaterialX `ND_standard_surface` network to every one of them (35, in the main scene and both add-ons) as `outputs:mtlx:surface` on the same Material prims, so the bindings are unchanged and hdUE and Karma, which both prefer the `mtlx` render context, render it. Its texture paths are relative to `samples/sponza/intel`. Regenerate it from Intel's USD with:

```
hython scripts/generate_sponza_materials.py [--intel DIR]
```

Every network reads Intel's maps: base colour (`srgb_texture`), roughness, normal (`ND_image_vector3` → `ND_normalmap_float`, raw), and opacity where Intel has one. The exporter's placeholders are replaced by values per surface class:

| Class | Materials | standard_surface |
| --- | --- | --- |
| Stone, plaster, brick, marble, roof tiles | walls, columns, floor, ornaments, trims, window frames | dielectric, `specular_IOR` 1.5 (Intel's 1.1 gives 0.2 % reflectance), `metalness` 0: their metalness maps are grime noise (at most 0.37) |
| Metal, wood | `metal_door`, `wood_door_01`, `wood_01` | Intel's metalness map (iron fittings, the metal door), IOR 1.5 |
| Fabric | `curtain_01`–`03` | `sheen` 0.8 in the base colour, `sheen_roughness` 0.4, `specular` 0.5, `thin_walled` with `subsurface` 0.3 (light through the cloth); the metalness map keeps the gold borders |
| Foliage | `LeafSpring` | `thin_walled`, `subsurface` 0.5, `specular` 0.5, roughness 0.45, cut out by the texture's alpha (`ND_image_color4` → `ND_extract_color4`; Intel's export reads the colour) |
| Bark, twigs | `Bark`, `SmallBranches` | roughness 0.8 (Intel's 0.5 / 1.0 placeholders) |
| Glass | `glass`, `lamp_glass_01` | `thin_walled`, `transmission` 0.95 (clear) and 0.4 (blue lantern glass, `transmission_color` its colour), roughness 0 and 0.39 |
| Others | `light_bulb`, `dirt_decal` | emission 1; the decal's mask as opacity |

hdUE approximates transmission as see-through coverage (opacity 1 − transmission, Intel's own values) and cuts textured opacity at 0.3333, as UE's Interchange imports MaterialX (UMaterial's default clip value); Karma refracts and blends opacity as fractional coverage. Intel's own UsdPreviewSurface leaves cut at 0.1. Lumen reads the cutout from mipmaps, where the thin sprigs average below the clip, so the distant cypress and its fallen leaves thin in Lumen while the Path Tracer, which samples full-resolution texels, keeps them: the per-engine difference a scene adjusts for (a lower clip on the leaves' alpha), not something hdUE tunes. See [MaterialX surfaces](../../docs/rendering.md#materialx-surfaces).
