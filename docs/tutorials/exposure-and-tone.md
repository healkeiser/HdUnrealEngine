# Exposure and tone

The room with UE's tonemapper off (the scene-linear image, sRGB-encoded) and on.

<div class="hdue-figure" data-figure="tut-exposure-tonemapper"></div>

You will choose between a scene-linear image for compositing and UE's tone-mapped image, set exposure, match the look of UE's editor, and deliver EXRs for comp. It takes about 20 minutes.

## The starting scene

Use the room, `$HDUE/samples/docs/room.usda`: a sunlit interior with a wide range of brightness, from the sun patch on the floor to the shaded corners.

1. In `/stage`, add a **Sublayer** LOP with **File** `$HDUE/samples/docs/room.usda` and set its display flag.
2. Render with **Unreal Engine** through `camera`, and choose **Unreal > Create hdUE Render Settings**.

Set **Unreal Tonemapper** off on the LOP to start from the scene-linear image.

## Scene-linear or tone-mapped

**Unreal Tonemapper** (`unreal:tonemapper`, default off) decides what the beauty holds.

- **Off**: the beauty is scene-linear HDR, like Karma's. Houdini's OCIO view does the display, and values above 1 are kept for comp.
- **On**: UE applies its film curve, bloom, colour grading, vignette and grain, as its editor viewport and Movie Render Queue do. The beauty is UE's tone-mapped image, display-linear Rec.709.

1. Set **Unreal Tonemapper** to on.
2. Keep the viewport on Houdini's default view (sRGB display, **Un-tone-mapped**).
    That view only encodes for the monitor, so it shows what UE shows. **Raw** shows the image too dark; an ACES or AgX view would tone map it a second time.

UE's film curve keeps middle grey (0.18) where it is. It darkens shadows (0.03 becomes 0.011) and rolls highlights off toward white (1 becomes 0.72, 4 becomes 0.94). A scene lit with most of its content far below middle grey therefore looks darker with the tonemapper; raise the exposure rather than the lights.

Bloom, colour grading, vignette, grain, chromatic aberration and lens flares only exist inside the tonemapper: with it off, they do nothing. See [Post-processing](../rendering/post-processing.md).

## Exposure compensation

**Exposure Compensation** (`unreal:postProcess:AutoExposureBias`, default 0, above the tabs) shifts the exposure in stops: +1 doubles the brightness, -1 halves it.

The room at -1, 0, +1 and +2.

<div class="hdue-figure" data-figure="tut-exposure-bias"></div>

1. Raise **Exposure Compensation** one stop at a time until the shaded half of the room reads, then check that the sun patch keeps its detail.
    The film curve rolls highlights off, so a stop more brightens the mid-tones more than the sun patch.

It applies with the tonemapper on. With the tonemapper off, change exposure on the camera instead (below) or in comp.

## Camera exposure

The camera's **Exposure** (`exposure`, in stops, on the Camera LOP) works with either setting:

- tonemapper off: it scales the beauty, as in Karma; AOVs are left unscaled;
- tonemapper on: it is added to **Exposure Compensation**.

1. On a **Camera** LOP in **Edit** mode for `/scene/camera`, set **Exposure** to 1.
    With the tonemapper on, this is the same image as **Exposure Compensation** 1. Keep exposure on the camera when the shot also renders in Karma, so both renderers read the same value.

## Matching UE's editor

hdUE starts from UE's settings with one exception: exposure is manual (**Metering Mode** Manual, compensation 0), so frames are deterministic and middle grey stays 0.18. A new UE project uses auto exposure: **Auto Exposure Histogram**, UE's default metering mode ([Auto Exposure](https://dev.epicgames.com/documentation/unreal-engine/auto-exposure-in-unreal-engine)), with a project exposure bias of 1 (`r.DefaultFeature.AutoExposure.Bias`, UE source `SceneView.cpp`).

Manual exposure (hdUE's default), then UE's default auto exposure.

<div class="hdue-figure" data-figure="tut-exposure-auto"></div>

To see the image UE's editor viewport shows for the same scene:

1. Turn **Unreal Tonemapper** on.
2. On the **Lens** tab, set **Metering Mode** (`unreal:postProcess:AutoExposureMethod`) to **Auto Exposure Histogram**.
3. Set **Exposure Compensation** to 1.

Auto exposure measures every frame on its own, so a sequence's exposure can shift from frame to frame, and two renders of the same frame can differ by several percent. In the viewport it reaches its target on every frame unless you set **Speed Up** or **Speed Down** (`unreal:postProcess:AutoExposureSpeedUp`, `AutoExposureSpeedDown`); husk always uses the target. Use manual exposure for final frames. See [Lens reference](../reference/render-settings/lens.md#unreal-postprocess-autoexposuremethod).

## Delivering for comp

Comp wants scene-linear EXRs. You have two ways to get them.

**Tonemapper off.** The beauty is scene-linear in the OCIO config's `scene_linear` role, ACEScg in an ACES pipeline. Grade and tone map in comp. See [Colour management](../rendering/color-management.md).

**Tonemapper on, tone curve off.** When the shot needs UE's bloom or grading in the render, turn on **Disable Tone Curve** (`unreal:disableToneCurve`, default off). This is Movie Render Queue's Disable Tone Curve: grading, bloom, vignette and grain still apply, without the film curve, so the beauty stays scene-linear.

The room with the tonemapper on, with **Disable Tone Curve** off and on.

<div class="hdue-figure" data-figure="tut-exposure-tonecurve"></div>

1. Keep **Unreal Tonemapper** on and set **Disable Tone Curve** to on.
2. View the result through your OCIO view (ACES, AgX or Un-tone-mapped), as you would a Karma render.

When the OCIO config's default view is a display transform of its own (ACES, AgX), hdUE turns the tone curve off by itself, as UE does when OpenColorIO does the display. Post-processing applies to the beauty only: AOVs stay raw for comp.

## Final render settings

- For comp: **Unreal Tonemapper** off, exposure on the camera.
- For comp with UE's bloom or grading: **Unreal Tonemapper** on, **Disable Tone Curve** on.
- For UE's look in the frame: **Unreal Tonemapper** on, **Metering Mode** Manual, **Exposure Compensation** set by eye.
