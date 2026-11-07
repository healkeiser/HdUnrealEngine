# Amazon Lumberyard Bistro

The interior and the street of Amazon Lumberyard Bistro (NVIDIA Open Research Content Archive, CC BY 4.0), used by the user guide's interior figures and the [Lighting an interior](../../docs/tutorials/lighting-an-interior.md) tutorial. It is not part of this repository: download and convert it locally.

Attribution: "Amazon Lumberyard Bistro, Open Research Content Archive (ORCA)", https://developer.nvidia.com/orca/amazon-lumberyard-bistro

1. Download `Bistro_v5_2.zip` (894 MB) from https://developer.nvidia.com/orca/amazon-lumberyard-bistro and unzip it into `samples/bistro/source/` (giving `samples/bistro/source/Bistro_v5_2/`).
2. Optionally, point `.local/paths.json` at a Megascans library (`"megascans_root": "<library>/megascans"`): the conversion bakes its imperfection maps (smudges, worn varnish, oxidised steel, grunge) into the materials' roughness. It also gives the street a patchy water coat (puddles) and the buildings grime read in their second UV set. Without it, the glasses take a generated smudge map and the rest keep Bistro's own roughness. Glass with its own textures (dirty windows) takes its grime map as roughness.
3. Convert the interior and the street (about 5 minutes each):

    ```powershell
    hython scripts\convert_bistro.py
    hython scripts\convert_bistro.py --scene exterior
    ```

This writes `samples/bistro/usd/` (ignored by git):

- `bistro_interior.usdc` and `bistro_exterior.usdc`: the interior with wine glasses and the street, through Houdini's FBX import, with a MaterialX standard surface per material built from Bistro's maps: base colour and coverage, roughness and metalness (Specular's green and blue), DirectX normal maps flipped to OpenGL's convention, emission. Glass, water, ice, beer and wine transmit, with the absorption of the scene's Falcor file for the wines and beer.
- `bistro_lights.usda` and `bistro_exterior_lights.usda`: a 2700 K sphere light in each emissive lamp shade, and the scene's HDRI as a dome; the shades are outside every light's shadow link, so they do not block their bulbs.
- `textures/`: Bistro's DDS and TGA textures as PNG (UE's runtime image import reads no DDS), and the baked roughness maps.

`samples/docs/bistro_exterior.usda` (committed) makes the street a rainy night: its lamps, a low blue moon behind the street, blue-teal volumetric mist, dust and a 35 mm camera on the Vespa. `samples/docs/bistro.usda` (committed) sublayers the interior and adds its shot: a key spot over the near table, volumetric haze, dust in the beam, the camera's depth of field and a grade.
