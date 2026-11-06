# COP and embedded textures

A texture input can read a Copernicus (COP) image live from the Houdini session, or an image embedded in a digital asset, as Karma does. Edit the COP network and the material updates in the viewport.

## Using it in Solaris

1. Build the image in a Copernicus network.
2. In a **USD UV Texture** or **MaterialX Image** node, set **File** to the COP node's path with the `op:` prefix: `op:/obj/geo1/copnet1/OUT`.
3. To read another output of the node, add its index: `op:/obj/geo1/copnet1/OUT[1]`. The default is output 0.

The same paths work in light textures (a dome light's or a rect light's **Texture**).

- Edits to the COP network, including time-dependent ones, reach the viewport within a tick. The Path Tracer restarts its accumulation when an image changes.
- An image the session has not exported yet renders untextured for a moment.
- Images are exported as 32-bit float with no colour conversion. Colour inputs then go through the OCIO `scene_linear` space as any EXR does ([Colour management](../rendering/color-management.md)); inputs with **Source Color Space** `raw` stay as they are.
- Legacy COP colour planes need integer frames.

## Embedded images

`opdef:` paths, images stored inside a digital asset, render as files: Houdini extracts each image once, without changing the asset or the USD.

## Rendering with husk

husk has no Houdini session to cook `op:` paths in. Save the stage with a **USD Render ROP** or **USD ROP**: as for Karma, they write each `op:` image next to the USD (a tiled, mip-mapped EXR) and rewrite its path, so husk renders ordinary files.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| husk renders a COP texture untextured, with a warning naming the `op:` path | The stage was saved without the ROP, so it still holds `op:` paths | Render through the USD Render ROP, or write the COP image to a file and point the texture at it |
| A COP texture shows in the viewport but a UDIM collection from COPs does not | COP UDIM collections are not supported | Write the tiles to files with a `<UDIM>` pattern |

COP images cannot replace textures inside a [cooked UE asset](../materials.md#cooked-native-assets).
