# Displacement

Displacement moves a surface along its normals by a height from the material, so bumps become real geometry: they cast shadows and change the silhouette. hdUE dices and displaces the mesh before UE renders it, so Lumen, the Path Tracer and Nanite all see the same surface.

A flat plate displaced by a 128 x 128 bump texture, at three heights (Path Tracer).

<div class="hdue-figure" data-figure="displacement-height"></div>

## Using displacement in Solaris

1. Connect a height to the material's displacement:
    - UsdPreviewSurface: its **Displacement** input (`inputs:displacement`), a distance in the mesh's units.
    - MaterialX: an `ND_displacement_float` node on the material's displacement output. The height is its `displacement` times its `scale`.
2. For a texture, read it with a UsdUVTexture (or MaterialX image) through the mesh's UVs. Its **Scale** sets the height: the sample's texture at scale 0.6 raises the white bumps 0.6 m.
3. Render. There is no setting to turn on: every mesh whose material displaces is displaced.

## How the mesh is diced

A flat mesh has too few points to show a texture's detail. Before displacing, hdUE splits every triangle into 4, as many times as needed:

- It dices until the mesh has about one triangle per two texels of the displacement texture.
- It stops at about one million triangles per mesh.
- A constant displacement (no texture) moves the points without dicing.

The sample's plate is one quad (2 triangles). Its 128 x 128 texture has 16,384 texels, so the plate is diced 6 times, into 8,192 triangles.

After displacing, normals are recomputed from the displaced surface. A UV seam stays closed where the texture is the same on both sides. Hard edges (split normals) open into cracks, because each side moves along its own normal.

The dicing is uniform over the mesh, not adaptive to the camera: a mesh far from the camera is diced as finely as one near it. A subdivision mesh is first refined to its [Subdivision Level](subdivision.md), then diced.

## Recipes

**How to get sharp displacement detail**

1. Use a displacement texture at the resolution the detail needs: a 1024 x 1024 map dices to about 500,000 triangles; 2048 x 2048 and larger reach the one-million-triangle limit.
2. Split a large displaced surface into several meshes: the limit is per mesh.
3. Keep the texture's colour space **raw**, so the heights are linear.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| No displacement; the log warns "cannot displace with ..." | The texture cannot be read, or the mesh lacks the UVs it reads | Fix the path; add the UV primvar the texture reads |
| Displacement is soft or stair-stepped | A small texture limits dicing, or the mesh reached the one-million-triangle limit | Use a larger texture; split the mesh |
| Cracks open along edges | Hard edges (split normals) move apart | Use smooth normals across the edge, or a subdivision mesh |
| A MaterialX vector displacement does nothing | Only float displacement (`ND_displacement_float`) is translated; a warning names it | Convert to a float height |
| A wet surface is not displaced | Meshes with wet surface effects keep their points | Remove the wet surface from that mesh |

## Unreal Engine and Karma

UE's own displacement, Nanite tessellation and World Position Offset, is not used: UE 5.8's ray-traced geometry ignores both, and the Path Tracer renders that geometry, so it would not see them. Dicing the mesh gives every mode the same surface. Karma dices adaptively, by the size of the surface on screen; hdUE dices uniformly by the texture's resolution.
