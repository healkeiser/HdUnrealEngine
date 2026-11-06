# Subdivision surfaces

A subdivision mesh is a coarse control cage that renders as a smooth surface. hdUE refines Catmull-Clark and Loop meshes with OpenSubdiv to the level you set, keeping their creases, corners and holes.

A Catmull-Clark cube at levels 0 to 3 (Realtime).

<div class="hdue-figure" data-figure="subdivision"></div>

## Using subdivision in Solaris

1. Make sure the mesh's **Subdivision Scheme** (`subdivisionScheme`) is `catmullClark` (USD's default) or `loop`. `bilinear` and `none` render as authored.
2. On **hdUE Render Settings** (Rendering tab, **Show: Advanced**), set **Subdivision Level**. The viewport's Display Options have it too, on their Unreal Engine tab.
3. To give some meshes their own level, add a **Render Geometry Settings** LOP on them or an ancestor and set **Subdivision Level** on its Unreal Engine tab.

Outside Houdini:

```usda
def RenderSettings "Settings"
{
    int unreal:subdivisionLevel = 3
}

def Mesh "hero"
{
    int primvars:unreal:subdivisionLevel = 4
}
```

## Subdivision Level

**Subdivision Level** (`unreal:subdivisionLevel`) · Default 0 · Range 0 to 5

How many times each subdivision mesh is refined. Each level multiplies the face count by 4: a 6-face cube has 384 faces at level 3. At 0, the default, meshes render as their control cage. See [Subdivision Level](../reference/render-settings/rendering.md#unreal-subdivisionlevel).

| Level | Faces per cage face | Looks like |
| --- | --- | --- |
| 0 (default) | 1 | the control cage |
| 1 | 4 | rounded but faceted |
| 2 | 16 | facets visible on close silhouettes |
| 3 | 64 | close to Karma's smooth surface on a cube |

Implicit spheres (a USD `Sphere` prim) are meshed coarsely, so they look faceted at level 0.

## Per-prim Subdivision Level

**Subdivision Level** (`primvars:unreal:subdivisionLevel`, Render Geometry Settings LOP > Unreal Engine tab) · Default: the render setting

Replaces the render setting for the meshes of this prim. It is a constant primvar, so it can sit on an ancestor and reach every mesh below it. Use it to refine a hero mesh more than the background, or to keep a dense mesh at 0.

## What follows the refined surface

- Creases, corners and holes (`creaseIndices`, `cornerIndices`, `holeIndices`), and the boundary and face-varying interpolation rules.
- Every primvar: UVs (`st`), `displayColor`, `velocities` and time-sampled `points`, of any interpolation.
- GeomSubsets: a material bound to some faces stays on their refined faces.

Authored normals are ignored, as the USD specification says for subdivision surfaces: normals come from the refined surface. Refinement is uniform; there is no adaptive or screen-space refinement. A displaced subdivision mesh is refined first, then diced further for its displacement: see [Displacement](displacement.md).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| A smooth model renders blocky | **Subdivision Level** is 0, UE's default | Set it to 2 or 3 |
| Hard edges are smoothed away | Normals authored for hard edges are ignored on subdivision meshes | Author creases, or set the mesh's scheme to `none` |
| Memory and load time grow fast | Each level has 4 times the faces of the one before | Raise the level only on the meshes that need it, with the per-prim primvar |
| A wet surface mask is missing on a subdivided mesh | Wet surface masks are not translated on subdivided meshes | Set the mesh's level to 0 |

## Unreal Engine and Karma

The default level 0 is UE's: UE's USD Stage Actor's **Subdivision Level** defaults to 0 and is the only, global, level. The per-prim primvar is hdUE's own. Karma refines subdivision surfaces by default, so a scene made for Karma needs about level 3 to match.
