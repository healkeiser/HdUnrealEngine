# Houdini

hdUE adds **Unreal Engine** as a renderer to Houdini 22.0 Solaris. The viewport, husk, the USD Render ROP and Live Render all load the same renderer and read the same `unreal:*` render settings, so a farm renders what your viewport shows.

```mermaid
flowchart LR
    subgraph Houdini["fal:fa-house Houdini"]
        S("fal:fa-layers LOP network<br/>USD stage")
        V("fal:fa-monitor Solaris viewport")
        R("fal:fa-clapperboard USD Render ROP")
        H("fal:fa-square-terminal husk")
        L("fal:fa-radio Live Render LOP")
        C("fal:fa-image Copernicus")
    end
    E("fal:fa-file-image EXR files")
    S --> V
    S --> R --> H --> E
    S --> L --> C

    classDef host stroke-width:1px
    classDef hdue stroke-width:1px
    classDef unreal stroke-width:1px
    classDef output stroke-width:1px

    class Houdini,S,V,R,H,L,C host
    class E output
```

## Where to start

<div class="grid cards" markdown>

-   :lucide-monitor:{ .lg .middle } **Solaris viewport**

    ---

    Picking Unreal Engine, the Unreal Engine tab, the Unreal menu, keeping the viewport fast.

    [:lucide-arrow-right: Solaris viewport](viewport.md)

-   :lucide-terminal:{ .lg .middle } **Rendering with husk**

    ---

    Command lines, frame ranges, farms, render products, failures.

    [:lucide-arrow-right: Rendering with husk](husk.md)

-   :lucide-radio:{ .lg .middle } **Live Render and Copernicus**

    ---

    Rendering into a Live Render LOP for compositing.

    [:lucide-arrow-right: Live Render and Copernicus](live-render.md)

-   :lucide-layers:{ .lg .middle } **Substrate materials**

    ---

    Building UE Substrate materials in a VOP network.

    [:lucide-arrow-right: Substrate materials](substrate-materials.md)

-   :lucide-images:{ .lg .middle } **AOVs in Houdini**

    ---

    Render vars, hdUE Standard Render Vars, Cryptomatte.

    [:lucide-arrow-right: AOVs in Houdini](../output/aovs.md#aovs-in-houdini)

-   :lucide-image:{ .lg .middle } **COP and embedded textures**

    ---

    `op:` images and textures embedded in assets.

    [:lucide-arrow-right: COP and embedded textures](../materials/cop-textures.md)

</div>

## The hdUE LOPs

The package adds these LOPs to the TAB menu under **hdUE**:

| LOP | TAB menu | Karma counterpart |
| --- | --- | --- |
| **hdUE Render Settings** | hdUE > Rendering | Karma Render Settings |
| **hdUE Standard Render Vars** | hdUE > Rendering | Karma Standard Render Vars |
| **hdUE Render Products** | hdUE > Rendering | Karma Render Products |
| **hdUE Height Fog**, **hdUE Sky Atmosphere**, **hdUE Volumetric Cloud**, **hdUE Local Fog Volume** | hdUE > Visual Effects | |
| **hdUE Post Process Volume**, **hdUE Decal** | hdUE > Visual Effects | |
| **hdUE Substrate Material Builder** | hdUE (in `/mat` and Material Library LOPs) | Karma Material Builder |

The Visual Effects LOPs place Unreal Engine's scene components as USD prims, filed as UE's Place Actors panel files them. See [Fog](../environment/fog.md), [Sky and clouds](../environment/sky.md) and [Post-processing](../rendering/post-processing.md).
