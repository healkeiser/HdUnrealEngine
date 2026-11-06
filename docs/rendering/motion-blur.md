# Motion blur

With the render camera's `shutter:open` before `shutter:close` and something moving, hdUE averages `unreal:motionSamples` sub-frames spread over the shutter. Moving means:

`unreal:motionBlur` off and on, a ball crossing the box during a half-frame shutter (Realtime).

<div class="hdue-figure" data-figure="motion-blur"></div>

- transform samples on meshes, curves, points and their ancestors;
- PointInstancer transform samples, and per-instance `velocities` and `accelerations` (a nested instancer's add its parent instance's); without `velocities` and `angularVelocities`, time-sampled `positions`, `orientations`, `scales` or instance matrices, interpolated linearly between the first and last sample around the frame, as `UsdGeomPointInstancer::ComputeInstanceTransformsAtTime` does ([instancer/sampled_positions](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/conformance/instancer/sampled_positions.usda));
- per-point `velocities` on Points;
- deforming meshes: time-sampled `points` with a constant point count, else per-point `velocities` (Houdini's `v`);
- camera transform samples.

`velocities` are in units per second, converted with `houdini:fps` when husk provides it, else `unreal:framesPerSecond` (24). Matrices are interpolated linearly between samples, so fast rotations shorten slightly within a frame. Deforming meshes are uploaded again for each sub-frame, which costs time on dense meshes. `angularVelocities` turn each instance about its position, and `motion:blurScale` scales a prim's motion (its sub-frame times). Time-sampled instance placements become a velocity and an angular velocity per instance, so a path through more than two samples within the shutter is followed as one straight segment, and animated scales do not blur. Lumen keeps its history between sub-frames, so the realtime warmup and the Path Tracer samples are divided between sub-frames and the total stays about the same. Too few sub-frames show discrete copies of fast objects.
