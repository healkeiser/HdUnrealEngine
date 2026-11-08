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

hdUE builds the UE groom asset UE's importer would make from a groom file, with UE's own groom builder, from the curves' evaluated points and `widths`: one group, guides from every strand, no simulation. Linear and cubic curves (Bezier, B-spline, Catmull-Rom) are both evaluated to points first. The hair uses UE's default hair material.

## Realtime and Path Tracer

<div class="hdue-figure" data-figure="geometry-hair-render-mode"></div>

The same groom in Realtime (Lumen) and in UE's Path Tracer, which traces the strands' geometry. Drag to compare.

In Realtime, UE draws hair with its own strand rasterizer and hair shading model; depth of field can use the hair's own depth with **Use Hair Depth** (`unreal:postProcess:DepthOfFieldUseHairDepth`, hdUE Render Settings > Lens > Depth of Field).

## Not yet translated

- Hair colour: every groom uses UE's default hair material. `displayColor` and bound materials are ignored.
- Root UVs and per-strand attributes (clumps, random values, ...).
- Instanced grooms (curves under a PointInstancer).
- Simulation: UE's hair physics does not run; the strands render where the curves are.

For hair cards (textured polygon strips), use a mesh with the [Substrate Builder](../substrate-builder.md)'s **Substrate Hair** node.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Curves render as tubes, not hair | **Hair Strands** is off | Turn it on on the curves or an ancestor |
| The hair looks like straw or a solid sheet | No `widths` (hdUE then uses 0.01 scene units: 1 cm strands in a metre scene), or widths in the wrong unit | Author `widths` in scene units: 0.0002 to 0.0006 in a metre scene |
| The hair has the wrong colour | Hair colour is not translated yet | No fix yet: UE's default hair material is used |
