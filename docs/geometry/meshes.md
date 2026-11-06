# Geometry

hdUE renders the geometry of your USD stage as UE geometry: meshes, curves and hair, points, instancers and implicit shapes. This page says what each prim type becomes in UE and which USD attributes it reads.

| USD prim | Renders in UE as |
| --- | --- |
| Mesh | a static mesh, Nanite where it can be ([Nanite](nanite.md)) |
| Mesh with `catmullClark` or `loop` scheme | a refined mesh at the [subdivision level](subdivision.md) |
| Cube, Sphere, Cone, Cylinder, Capsule, Plane | a mesh |
| BasisCurves | tubes, or UE hair strands |
| NurbsCurves, NurbsPatch | the evaluated curve or surface |
| Points | one sphere per point |
| PointInstancer, instanceable references | instanced static meshes |
| Volume | a UE Heterogeneous Volume ([Volumes](../environment/volumes.md)) |
| HermiteCurves | not rendered; a warning names the prim |

## Meshes

Meshes keep their points, face-varying UVs, normals and colours. A mesh hdUE cannot convert (broken indices, or more than about 715 million vertices) is skipped with a warning naming it in the UE log; the rest of the scene renders.

**Normals.** Authored normals are kept (`primvars:normals` wins over `normals`). Missing normals are generated smooth, so faces that share a point share its normal. For hard edges, author face-varying normals or split the points. Subdivision meshes ignore authored normals, as USD specifies.

**Materials on faces.** A GeomSubset of family `materialBind` binds a material to some faces. Faces in no subset keep the mesh's material. Overlapping or invalid subsets are rejected.

**Holes, orientation, concave polygons.** `holeIndices`, `orientation = "leftHanded"` and concave n-gons render as authored.

## Double-sided meshes

**Cull Backface** (`unreal:cullBackface`) · Default On

A mesh with `doubleSided = false` (USD's default) renders its front faces only, as UE's USD importer imports it. A mesh with `doubleSided = true` renders both sides, each lit as a front face. To render every mesh on both sides, as Karma does, turn **Cull Backface** off (hdUE Render Settings > Rendering tab, **Show: Advanced**). See [Cull Backface](../reference/render-settings/rendering.md#unreal-cullbackface).

!!! tip
    A plane, a leaf or a single wall that vanishes from one side is single-sided. Author `doubleSided = true` on the mesh, or turn off **Cull Backface**.

Karma's per-object `karma:object:cull_backface` is not read: `doubleSided` is the per-mesh switch.

## Implicit shapes

Cube, Sphere, Cone, Cylinder, Capsule and Plane prims, including the `Cylinder_1` and `Capsule_1` forms with separate top and bottom radii, are converted to meshes by Hydra. Hydra's sphere is a coarse Catmull-Clark cage, so it looks faceted at **Subdivision Level** 0: set it to 2 or more for round spheres.

## Curves

BasisCurves render as six-sided tubes, with flat caps on open curves.

- Linear curves follow their points. Cubic Bezier, B-spline and Catmull-Rom curves follow their basis. Periodic curves close.
- `widths` sets the diameter, per curve or per point. Unauthored, the width is 0.01.
- `displayColor` per point or per curve colours them when no material is bound.

## Hair

Curves render as UE hair strands when you turn on **Hair Strands** (`primvars:unreal:groom`, Render Geometry Settings LOP > Unreal Engine tab) on the curves or an ancestor.

hdUE builds a UE groom with UE's own groom builder from the curves' points and per-point `widths`: one group, guides from every strand, no simulation, with UE's default hair material. It renders in Realtime and in the Path Tracer.

Not yet translated: instanced grooms, hair colour, root UVs and per-strand attributes.

## Points

Points render as one sphere per point, sized by `widths` (the diameter; 0.1 when unauthored). Per-point `displayColor` colours them.

## Instancing

PointInstancers, nested PointInstancers and instanceable references (USD native instancing) render as UE instanced static meshes: each prototype is built once and drawn once per instance.

| PointInstancer attribute | Read |
| --- | --- |
| `protoIndices`, `positions`, `orientations`, `orientationsf`, `scales` | yes |
| `invisibleIds`, `inactiveIds` | yes: those instances are not drawn |
| `velocities`, `accelerations`, `angularVelocities` | yes, for motion blur |
| instance-rate primvars, such as `displayColor` | yes: each instance gets its colour |

Points, curves and volumes under an instancer are drawn once per instance too.

UE instances cannot shear. An instance whose transform shears (a rotated prototype under a non-uniform scale) renders as a separate mesh with its points transformed; the others stay UE instances. A prototype that is a cooked UE mesh (**Native Asset**) cannot be transformed that way, so its sheared instances render without shear, with a warning.

## Primvars

**UVs.** A mesh's UVs come from the primvar its material's texture reads (the `varname` of a UsdPrimvarReader, such as `st`, or `map1` from a 3ds Max export), else from `st`. A mesh carries up to four UV sets, one per primvar its material reads.

**Display colour.** Geometry with no material renders with a Preview Surface (roughness 0.5) whose base colour is `displayColor`, as UE's USD importer draws unbound prims. A prim with no `displayColor` renders white, as in UE's importer. A `displayColor` that varies (per face, point, vertex, curve or instance) is applied as vertex or instance colours. Mesh vertex colours are stored in 8 bits, so very dark colours band slightly.

Not translated: varying `displayOpacity`, and colours on meshes that also carry a wet surface mask.

## Visibility and purpose

- `visibility = "invisible"` hides a prim and its children.
- **Purpose.** Final renders include the purposes listed in the render settings prim's `includedPurposes`, by default `default` and `render`. Proxy and guide geometry does not render.
- `renderVisibility` and `guideVisibility` (UsdGeomVisibilityAPI) are followed.
- A `model:drawMode` of `bounds` or `cards` replaces the model with its box or cards, as UE's USD importer does.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| A wall or plane disappears from one side | The mesh is single-sided | Set `doubleSided`, or turn off **Cull Backface** |
| A prim is missing; the UE log names it | The mesh has broken indices or is too large | Fix or split the mesh |
| Hard edges look smooth | No authored normals: hdUE generates smooth ones | Author face-varying normals, or split the points |
| Unbound geometry is white, not grey as in Karma | No `displayColor`: hdUE uses white, as UE's importer does | Author `displayColor`, or bind a material |
| Proxy geometry renders, or render geometry is missing | The render settings prim's `includedPurposes` | Set `includedPurposes` to `default` and `render` |
| Curves render as tubes, not hair | **Hair Strands** is off | Turn it on on the curves or an ancestor |

## Unreal Engine and Karma

hdUE follows UE's USD importer: single-sided meshes are culled, unbound prims are white with their display colour, and draw modes apply. Karma renders both sides of every mesh by default and draws unbound prims grey. Turn off **Cull Backface** to match Karma's double-sided default.
