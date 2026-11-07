# Cameras

hdUE renders through the USD camera (`UsdGeomCamera`) that Houdini's viewport or husk hands it. Framing, depth of field, exposure and the shutter come from the camera; UE's lens distortion comes from `unreal:lensDistortion:*` attributes on it.

## Using it in Solaris

1. Create a **Camera** LOP, or edit an existing camera with it.
2. Look through it in the viewport, or pick it in the render settings' **Camera** for husk.
3. Set depth of field and exposure on its **Sampling** tab.

The Camera LOP takes millimetres and writes USD's units: tenths of a scene unit, by the stage's metres per unit. A 50 mm lens in a metres stage is `focalLength` 0.5. hdUE reads the authored value in USD's units, as Hydra does.

## Projection

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Projection</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`projection`</dd></div><div markdown><dt>Default</dt><dd markdown="span">`perspective`</dd></div></dl>
</div>
</div>

</div>

`perspective` or `orthographic`. An orthographic camera's **Horizontal Aperture** is the width of the view in tenths of a scene unit.

## Focal length and aperture

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Focal Length</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`focalLength`</dd></div><div markdown><dt>Default</dt><dd markdown="span">50 (tenths of a scene unit)</dd></div></dl>
</div>
</div>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Horizontal Aperture</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`horizontalAperture`</dd></div><div markdown><dt>Default</dt><dd markdown="span">20.955</dd></div></dl>
</div>
</div>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Vertical Aperture</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`verticalAperture`</dd></div><div markdown><dt>Default</dt><dd markdown="span">15.2908</dd></div></dl>
</div>
</div>

</div>

Focal length over aperture sets the field of view. The aperture is conformed to the image's aspect ratio as Karma conforms it. Focal length also sets how much depth of field blurs: a longer lens at the same f-stop blurs more.

## Lens shift

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Horizontal Aperture Offset</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`horizontalApertureOffset`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Vertical Aperture Offset</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`verticalApertureOffset`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

</div>

Shifts the image window without turning the camera, in the aperture's units. Use it to keep verticals straight in architecture shots.

## Clipping

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Clipping Range</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`clippingRange`</dd></div><div markdown><dt>Default</dt><dd markdown="span">(1, 1000000)</dd></div></dl>
</div>
</div>

</div>

hdUE applies the near clip. **It does not apply the far clip**: everything beyond it still renders. UE's views keep an infinite far plane, and UE's Path Tracer traces past any far plane. To hide distant geometry, deactivate it or set its visibility instead.

## Depth of field

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">F-Stop</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`fStop`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Focus Distance</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`focusDistance`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0, scene units</dd></div></dl>
</div>
</div>

</div>

Depth of field is on when both are above 0. Lower f-stops blur more: each halving of the f-stop doubles the blur. Focus Distance is measured from the camera in scene units, not in tenths.

The toys are 1.2 m apart; the camera is focused on the nearest (2.4 m), with a 50 mm lens on a 36 mm sensor.

<div class="hdue-figure" data-figure="cameras-fstop"></div>

At f/1.4, the focus distance moves along the row: toy 0 is 2.4 m away, toy 1 is 3.6 m, toy 3 is 6 m, toy 5 is 8.4 m.

<div class="hdue-figure" data-figure="cameras-focus-distance"></div>

The Path Tracer traces a thin lens (UE's reference depth of field). Realtime uses UE's post-process depth of field, which approximates it and clamps very large blur.

<div class="hdue-figure" data-figure="cameras-dof-render-mode"></div>

To render every camera in focus, turn on **Disable Depth of Field** (`disableDepthOfField`) in the render settings. The UE side of depth of field (blade count, the post-process method) is in the [Lens settings](reference/render-settings/lens.md#unreal-postprocess-depthoffieldbladecount).

## Exposure

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Exposure</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`exposure`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0, stops</dd></div></dl>
</div>
</div>

</div>
**Exposure F-Stop**, **Exposure ISO**, **Exposure Time** · `exposure:fStop` (1), `exposure:iso` (100), `exposure:time` (1)

Each stop of **Exposure** doubles the image's brightness. hdUE combines the four attributes into one scale as USD defines it, so the exposure triplet works as on a real camera. With the **Unreal Tonemapper** on (the default), the camera's exposure adds to [Exposure Compensation](reference/render-settings/index.md#unreal-postprocess-autoexposurebias). With it off, it scales the beauty and leaves the other AOVs untouched, as Karma does.

<div class="hdue-figure" data-figure="cameras-exposure"></div>

**Exposure F-Stop** changes brightness only. Depth of field reads **F-Stop**.

## Lens distortion

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">K1, K2, K3, P1, P2</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`unreal:lensDistortion:K1` ... `P2`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Principal Point</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`unreal:lensDistortion:PrincipalPoint`</dd></div><div markdown><dt>Default</dt><dd markdown="span">(0.5, 0.5)</dd></div></dl>
</div>
</div>

</div>

UE's spherical lens model (Brown-Conrady), the values UE's Lens Distortion component and lens files take. K1 to K3 are radial, P1 and P2 tangential, all unitless. The principal point is the distortion centre, in normalized image coordinates. The Camera LOP has no parameter for them: author them with a Python LOP or in a USD layer:

```usda
over "camera"
{
    float unreal:lensDistortion:K1 = -0.2
}
```

<div class="hdue-figure" data-figure="cameras-lens-distortion"></div>

Distortion applies in both render modes and to every AOV except `depth`. Orthographic cameras are not distorted. USD's own `lensDistortion:*` attributes are not read: USD does not define the mathematics of its model.

## Shutter

<div class="hdue-params" markdown>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Shutter Open</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`shutter:open`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0</dd></div></dl>
</div>
</div>

<div class="hdue-param" markdown>
<div class="hdue-param__name" markdown="span">Shutter Close</div>
<div class="hdue-param__body" markdown>
<dl class="hdue-facts" markdown><div markdown><dt>USD attribute</dt><dd markdown="span">`shutter:close`</dd></div><div markdown><dt>Default</dt><dd markdown="span">0, frames</dd></div></dl>
</div>
</div>

</div>

Motion blur needs **Shutter Close** above **Shutter Open**. See [Motion blur](rendering/motion-blur.md).

## Image size, crops and tiles

The render settings' resolution, pixel aspect ratio, aspect ratio conform policy and data window (crops, overscan) are honoured, as in Karma. Depth and AOVs follow the same window. One image can be at most 16384 pixels on a side, UE's largest texture on D3D12. For larger images, render husk tiles (`--tile-count`), each within the limit.

## Recipes

**Shallow focus on a subject 2 m away, full-frame 50 mm look:**

1. In the Camera LOP, set **Focal Length** to 50 and **Horizontal Aperture** to 36 (millimetres).
2. Set **Focus Distance** to 2.
3. Set **F-Stop** to 1.4.
4. For final frames, set **Render Mode** to Path Traced for the thin-lens blur.

**Brighten a shot by one stop without touching the lights:** set the camera's **Exposure** to 1.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Everything is a blur, even at f/16. | The focal length is authored in millimetres in a metres stage: `focalLength` 50 is a 5 m lens. | Author the lens with the Camera LOP, which converts millimetres; or write 0.5 for 50 mm in a metres stage. |
| No depth of field. | **F-Stop** or **Focus Distance** is 0, or **Disable Depth of Field** is on. | Set both above 0; turn the render setting off. |
| Distant geometry renders past the far clip. | hdUE does not apply the far clip. | Deactivate or hide the geometry. |
| Tile seams in a tiled realtime render. | Lumen screen traces, bloom and temporal anti-aliasing see only the tile. | Render tiled images with the Path Tracer. |
| A huge image fails. | Over 16384 pixels on a side. | Render husk tiles. |

## Unreal Engine and Karma

- Framing, aperture offsets, crops and orthographic projection match Karma.
- The far clip is ignored, where Karma clips.
- Realtime depth of field is UE's post-process depth of field, not a ray-traced lens; the Path Tracer traces a thin lens like Karma.
- Lens distortion is UE's model, read from `unreal:lensDistortion:*`. Split diopters are not translated.
