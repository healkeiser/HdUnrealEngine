# Alpha

The beauty's alpha channel is the coverage of each pixel: how much of it the camera sees covered by geometry. You use it to composite the render over a plate or another layer.

## What alpha holds

| Pixel | Alpha |
|---|---|
| Opaque geometry | 1 |
| Nothing but empty space | 0 |
| Anti-aliased edge, depth-of-field blur, motion blur | Between 0 and 1: the covered fraction |
| Translucent surface (glass, a translucent material) over empty space | Between 0 and 1 |
| Opaque object seen through glass | 1 |
| Cutout (masked opacity) | 1 where kept, 0 where cut |
| Visible dome light background (Realtime) | 0 |

Alpha works in Realtime and Path Tracer modes. DLSS upscales it too (**DLSS Alpha Upscaling**, on by default). hdUE turns on UE's alpha output (`r.PostProcessing.PropagateAlpha`) for every render; there is nothing to set.

## Premultiplication

Where nothing is behind the geometry, the colour is the geometry's colour times its coverage: an edge pixel half covered by a white object is (0.5, 0.5, 0.5) with alpha 0.5. That is premultiplied (associated) alpha, what EXR and Houdini's compositing expect. Composite with a plain Over; do not premultiply again.

A visible background breaks this: a dome light drawn behind the geometry fills the colour where alpha is 0. For a layer you composite, hide the background.

## How to render a layer for comp

1. Select the dome light. Turn off **Visible to Camera** (`unreal:visibleBackground` = 0). It still lights the scene and appears in reflections.
2. Write the beauty as `color4f` (RGBA): hdUE Standard Render Vars' **Beauty** does.
3. In comp, put the layer Over the plate.

Effects that spread light (bloom, lens flares) brighten the colour beyond the alpha edge: turn them off for layers, and add them in comp.

## Holdouts and mattes

hdUE has no holdout (matte) objects. UsdRenderPass `matte` and `cameraVisibility` collections are not applied; `prune` collections are. To cut an object out of a layer, render it into a separate layer, or use [Cryptomatte](aovs.md#cryptomatte) or `primId` as a matte in comp.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Alpha 0 behind the object, but the environment shows in the colour | A visible dome fills the colour; alpha counts geometry only | Hide the background for comp layers (above). |
| Dark fringe around the object in comp | The render was premultiplied again, or unpremultiplied colour was assumed | Composite with Over without an extra Premultiply. |
| Glow from bloom outside the alpha | Bloom and lens flares are image effects, not geometry | Turn them off for layers. |
| A matte collection has no effect | hdUE does not apply matte collections | Use Cryptomatte or a separate layer. |

## Unreal Engine and Karma

- hdUE uses UE's own alpha output, the project setting **Alpha Output** (`r.PostProcessing.PropagateAlpha`). UE stores 1 minus the coverage; hdUE outputs the coverage itself.
- Karma also writes coverage alpha, and supports holdout mattes, which hdUE does not.
