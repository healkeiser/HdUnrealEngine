# Intel Sponza 2022

The README's Sponza gallery renders [Intel's Sponza 2022](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) (Frank Meinl and Anton Kaplanyan, CC BY 4.0). It is not part of this repository: download it from Intel and place the files below in `samples/sponza/intel/`, which git ignores.

```
samples/sponza/
├── lighting.usda                 sun, sky dome and render settings (tracked)
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
