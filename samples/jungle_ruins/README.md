# Intel Jungle Ruins

[Intel's Jungle Ruins](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html) (Cristiano Siqueira and Charlene Teets, CC BY 4.0). It is not part of this repository: download it from Intel and place the files below in `samples/jungle_ruins/intel/`, which git ignores.

```
samples/jungle_ruins/
├── scene.usda                    render settings over Intel's Karma layer (tracked)
└── intel/                        Intel's download, without its Blender folder (ignored)
    ├── USD/                      JungleRuins_Karma.usda, cameras/, elements/, its sky HDRI
    ├── textures/                 referenced by USD/elements as ../../../textures
    ├── reference/                Intel's own renders (Render_*), to compare against
    ├── credits_license.txt
    └── Jungle Ruins_1.0.1b_HelpDoc.pdf
```

`scene.usda` sublayers Intel's `USD/JungleRuins_Karma.usda` unchanged, its sky dome included, and adds a 960 x 540 render setup shared by hdUE and Karma (path traced, 256 samples, camera `/root/Camera/Camera`). Render it with husk or open it in Solaris:

```
husk --renderer HdUnrealEngine -f 1 -o jungle_ruins.exr samples/jungle_ruins/scene.usda
```

Poly Haven HDRIs for scenes without their own lighting go in `samples/hdris/` (also ignored).
