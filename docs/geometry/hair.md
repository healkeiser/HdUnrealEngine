# Hair

hdUE renders curves as UE hair strands: the strands UE's groom system draws, with UE's hair shading, in Realtime and in the Path Tracer. Without it, curves render as thin tubes.

<div class="hdue-figure" data-figure="geometry-hair-strands"></div>

12,000 strands on a 25 cm head, under one rect light: **Hair Strands** off, the curves as tubes; on, UE hair strands. Drag to compare.

## Rendering curves as hair

=== "Houdini"

    1. Bring the curves into Solaris: a groom from the **Hair Generate** SOP, or any curves, through a **SOP Import** LOP. Give them a width: Houdini's `width` (or `pscale`) point attribute becomes USD's `widths`. Real hair is 0.05 to 0.1 mm thick; widths of 0.2 to 0.6 mm read well at a few metres.
    2. Add a **Render Geometry Settings** LOP and select the curves, or a prim above them.
    3. On its **Unreal Engine** tab, turn on **Hair Strands**.

=== "USD"

    ```usda
    def BasisCurves "hair"
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [16, 16, ...]
        point3f[] points = [...]
        float[] widths = [...] (interpolation = "vertex") # (1)!
        bool primvars:unreal:groom = 1 (interpolation = "constant") # (2)!
    }
    ```

    1. Per point, tapering from root to tip; one constant width works too.
    2. On the curves or an ancestor: a primvar is inherited.

hdUE builds the UE groom asset UE's importer would make from a groom file, with UE's own groom builder, from the curves' evaluated points and `widths`: one group, guides from every strand, no simulation. Linear and cubic curves (Bezier, B-spline, Catmull-Rom) are both evaluated to points first.

## Colour and materials

<div class="hdue-figure" data-figure="geometry-hair-colour"></div>

The same groom with UE's default hair material, a `groom_color` per strand, and a bound Substrate Builder **Substrate Hair** material. Drag to compare.

The strands take their look from, in this order:

1. **A bound UE hair material**: a [Substrate Builder](../substrate-builder.md) material whose graph has a **Substrate Hair** node, prepared as any Substrate Builder material. MaterialX's `chiang_hair_bsdf` goes through UE's MaterialX importer the same way.
2. **The groom's own colour and roughness**: the `groom_color` and `groom_roughness` primvars, which hdUE's hair material reads through UE's **Hair Attributes** node, as a UE hair material does. Without `groom_color` and without a bound material, `displayColor` gives the colour, as it does for unbound meshes. In Houdini, name the attributes `groom_color` and `groom_roughness` (or set `Cd`), per point or per curve.
3. **A bound Preview Surface or Standard Surface**: its base colour and roughness, as constants (textures are not read).
4. Otherwise, **UE's default hair material**.

## Groom attributes

Every `groom_*` primvar on the curves reaches the UE groom, under the names UE's groom importers use, so a UE hair material reads it with **Hair Attributes**:

| Primvar | Type | UE |
| --- | --- | --- |
| `groom_color` | `color3f`, per point or per curve | Hair Attributes > Base Color |
| `groom_roughness` | `float`, per point or per curve | Hair Attributes > Roughness |
| `groom_ao` | `float`, per point or per curve | Hair Attributes > AO |
| `groom_root_uv` | `float2`, per curve | Hair Attributes > Root UV |
| `groom_clumpid` | `int`, per curve | Hair Attributes > Clump ID |
| `groom_id` | `int`, per curve | the strand's ID |

As in UE's USD importer, `uniform` and `constant` primvars are per curve and `vertex` or `varying` ones per point. UE reads colour, roughness and AO per point only: hdUE spreads a per-curve value over its curve's points.

## Instancing

Curves under a PointInstancer render as one groom built once and drawn once per instance, as instanced meshes are. A shear in an instance's transform is dropped (UE's components hold none).

## Realtime and Path Tracer

<div class="hdue-figure" data-figure="geometry-hair-render-mode"></div>

The same groom in Realtime (Lumen) and in UE's Path Tracer, which traces the strands' geometry. Drag to compare.

In Realtime, UE draws hair with its own strand rasterizer and hair shading model; depth of field can use the hair's own depth with **Use Hair Depth** (`unreal:postProcess:DepthOfFieldUseHairDepth`, hdUE Render Settings > Lens > Depth of Field).

## Not yet translated

- Simulation: UE's hair physics does not run; the strands render where the curves are.
- Textures on a bound Preview Surface or Standard Surface: its constant colour and roughness are used.
- MaterialX `chiang_hair_bsdf` renders black: UE's own translation of it (`MX_ChiangHairBSDF`) draws the strands black, with or without absorption. Use a Substrate Hair material instead.

For hair cards (textured polygon strips), use a mesh with the [Substrate Builder](../substrate-builder.md)'s **Substrate Hair** node.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Curves render as tubes, not hair | **Hair Strands** is off | Turn it on on the curves or an ancestor |
| The hair looks like straw or a solid sheet | No `widths` (hdUE then uses 0.01 scene units: 1 cm strands in a metre scene), or widths in the wrong unit | Author `widths` in scene units: 0.0002 to 0.0006 in a metre scene |
| The hair stays UE's default colour | No `groom_color`, `displayColor` or bound material, or the bound material is not a hair material | Add `groom_color`, or bind a Substrate Hair material |
| A bound material renders as UE's default hair | The material has no Substrate Hair node, so UE cannot draw strands with it (a warning names it) | Use a Substrate Hair material, or let `groom_color` colour the strands |
| MaterialX hair is black | UE's translation of `chiang_hair_bsdf` | Use a Substrate Hair material |
