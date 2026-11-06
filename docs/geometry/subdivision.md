# Subdivision surfaces

Meshes whose `subdivisionScheme` is `catmullClark` (USD's default) or `loop` are refined uniformly with OpenSubdiv to `unreal:subdivisionLevel` levels (0 by default, as UE's USD Stage Actor: `SubdivisionLevel`, `USDStageActor.cpp`; each level has 4x the faces), with their creases, corners and holes. Vertex, varying, face-varying and uniform primvars (`st`, `displayColor`, `velocities`, time-sampled `points`) follow the refined surface, and GeomSubsets keep their faces. Authored normals are ignored, as USD specifies for subdivision surfaces; normals come from the refined surface. `bilinear` and `none` render the mesh as authored.

`unreal:subdivisionLevel` on a Catmull-Clark cube (Realtime).

<div class="hdue-figure" data-figure="subdivision"></div>

A prim, or an ancestor, can set its own level with the constant primvar `primvars:unreal:subdivisionLevel` (**Subdivision Level** on the Unreal Engine tab of Render Geometry Settings LOPs), which replaces the render setting for its meshes; UE's USD Stage Actor has only the global level. Each level multiplies the face count by four. Level 3 matches Karma's surfaces closely on a cube; level 2 still shows facets on close silhouettes, and the default 0 shows the control cage (implicit spheres, which Hydra meshes coarsely, look faceted). There is no adaptive or screen-space refinement; a displaced subdivision mesh is diced further (see [Displacement](displacement.md)). Wet-surface masks are not translated on subdivided meshes.
