# Wet surfaces

**Wet Surface** coats a material with a thin film of water, per prim, without editing the material: a clear layer with the reflectance of water (IOR 1.33) over the original surface. **Amount** and a painted mask primvar place it; **Roughness** sets how sharp its reflections are.

It works on MaterialX Standard Surface and OpenPBR materials cooked with a wet profile. For a water film on any graph, or other layers, build it in the [Substrate Builder](substrate-builder.md#water-film-over-a-material).

## Before you start

The bound material must be in a [Cooked MaterialX Library](reference/render-settings/rendering.md#unreal-materialxlibrary) imported with the wet profile:

```powershell
.\scripts\Import-MaterialX.ps1 -Source @('looks.mtlx') -WetProfile -OutputDirectory out/materialx-wet
.\scripts\Stage-Runtime.ps1
```

Then set **Cooked MaterialX Library** (hdUE Render Settings > Rendering > Materials) to `out/materialx-wet/library.json` and restart Houdini on the staged runtime.

A material qualifies when it is:

- a Standard Surface or OpenPBR surface;
- opaque;
- without an active `subsurface`, `transmission` or `sheen` (OpenPBR `subsurface_weight`, `transmission_weight`, `fuzz_weight`): each must be 0 and not connected.

Its normal maps, coat, metalness and UDIM textures stay as they are under the water.

## Using it in Solaris

1. Add a **Render Geometry Settings** LOP after the material assignment.
2. Set **Primitives** to the meshes, their GeomSubsets, or a parent prim.
3. On the **Unreal Engine** tab, under **Wet Surface**, set each control to **Set or Create** and turn **Wet Surface** on.
4. Set **Amount**, **Roughness** and, for a painted mask, **Mask Primvar**.

From Houdini's Python shell, `hdue_surface.create()` adds this LOP for the prims selected in the Scene Graph Tree, with **Wet Surface** on.

Amount, roughness and mask edits are live: no compile. The Path Tracer restarts its accumulation.

## Wet Surface

**Wet Surface** · `unreal:surface:wet:enabled` · default off

Turns the coating on for the prim, its GeomSubsets and the meshes below it. Set it off on a child to keep that child dry.

## Amount

**Amount** · `unreal:surface:wet:amount` · default 1 · 0 to 1

The fraction of the surface the water covers, multiplied by the mask. It is coverage, not water depth: 0.5 is a surface half covered, not a thinner film. 0 renders the original material.

## Roughness

**Roughness** · `unreal:surface:wet:roughness` · default 0.08 · 0 to 1

Roughness of the water's surface. 0.08 is a still film; raise it for drizzled or dirty water.

## Mask Primvar

**Mask Primvar** · `unreal:surface:wet:maskPrimvar` · default empty

The base name of a float primvar on the mesh that scales the coverage, for example `wetness` for `primvars:wetness`. Empty covers the whole surface.

- Paint the primvar in SOPs, as a point, vertex, primitive or detail attribute. Values are clamped to 0..1 and read as linear data.
- Mask edits update the mesh's mask only, without re-sending its geometry.
- A named primvar that is missing, has the wrong count or holds NaN is an error naming the prim.

To see the mask, turn on **Preview Primvar mask** on the hdUE settings subnet: the wet meshes show their coverage in grey levels.

## In USD { #usd-authoring-contract }

The four attributes can sit on a Mesh, a material-binding GeomSubset or any ancestor prim. Each mesh uses the nearest authored value, from the subset up through its ancestors. Two meshes sharing one material keep their own wet settings.

```usda
over "World"
{
    over "Asset"
    {
        custom bool unreal:surface:wet:enabled = true
        custom float unreal:surface:wet:amount = 0.8
        custom float unreal:surface:wet:roughness = 0.08
        custom token unreal:surface:wet:maskPrimvar = "wetness"

        over "Panel"
        {
            float[] primvars:wetness = [0, 0, 1, 1] (
                interpolation = "vertex"
            )
        }
    }
}
```

## Recipes

| Look | Amount | Roughness | Mask Primvar |
| --- | --- | --- | --- |
| Rain-soaked surface | 1 | 0.08 | empty |
| Puddles and dry patches | 1 | 0.05 | a painted `wetness`, 0 dry, 1 wet |
| Damp, half-dried surface | 0.5 | 0.2 | empty |

The water does not darken the material under it. For a soaked look, darken and smooth the base material too (lower its base colour and roughness).

## Limits

- One water layer per material, over the whole original surface.
- Opaque Standard Surface and OpenPBR materials only: no glass, subsurface, sheen, cutouts, hair or native UE asset materials.
- Masks are float primvars only; image masks need the [Substrate Builder](substrate-builder.md).
- Meshes with a wet surface are not displaced.
- The GPU may store the mask at 8 bits per value (steps of 1/255).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| An error "wet profile unavailable" naming the prim | The bound material is not in the Cooked MaterialX Library, or was imported without `-WetProfile` | Import it with `-WetProfile` and select that library |
| "Wet v1 does not support an active subsurface input" (or transmission, sheen) | The material uses a lobe the wet profile does not cover | Set that input to 0, or use a Substrate Builder graph |
| "missing float wet mask" | **Mask Primvar** names a primvar the mesh does not have, or not a float | Fix the name (no `primvars:` prefix) or the attribute type |
| "use the mask base name without 'primvars:'" | The name includes the namespace | Write `wetness`, not `primvars:wetness` |
| The wet surface looks like the dry one | Amount 0, or the base already has a glossy coat with the same roughness | Check Amount; raise the base's roughness to compare |

Implementation details are in [Wet surface internals](developer/internals/wet-surfaces.md).
