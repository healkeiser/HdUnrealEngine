# Lighting an interior

The shot at the end of this tutorial: the Bistro at night, a warm spot on the near table, haze around the lamps, dust in the beam, the bar falling out of focus.

<div class="hdue-figure" data-figure="tut-interior-final"></div>

You will start from the restaurant lit by its own lamps, then give the image a subject with a key light, add atmosphere with volumetric fog and dust, shape it with the lens, and finish with a grade. Each stage changes one thing. It takes about 45 minutes.

## The starting scene

The scene is the interior of Amazon Lumberyard Bistro (NVIDIA ORCA, CC BY 4.0), converted to USD with MaterialX standard surfaces by `scripts/convert_bistro.py`. The download is not part of hdUE: [samples/bistro/README.md](https://github.com/healkeiser/HdUnrealEngine/blob/main/samples/bistro/README.md) lists the steps (download, then one `hython` command).

`samples/docs/bistro.usda` sublayers the converted scene and adds the shot: its lights, haze, dust, camera settings and grade. This tutorial switches them on one at a time.

1. In `/stage`, add a **Sublayer** LOP with **File** `$HDUE/samples/docs/bistro.usda`, and set its display flag.
2. Pick **Unreal Engine** in the viewport's renderer menu, and the Bistro's `Camera` in its camera menu.
3. Choose **Unreal > Create hdUE Render Settings**. Keep **Render Mode** on **Realtime (Lumen)** and check that **Unreal Tonemapper** is on.

## Practical lights

A real room is lit by its lamps: the photographer's "practicals". The conversion puts a 2700 K sphere light of 5 cm inside each of the twelve lamp shades, next to their glowing emissive glass.

<div class="hdue-figure" data-figure="tut-interior-practicals"></div>

- **Exposure** 10 on each lamp: a 5 cm bulb is a small source, so it needs a high exposure to light a room.
- **Exposure Compensation** (`unreal:postProcess:AutoExposureBias`) +1: a night interior, exposed one stop over UE's starting point.
- The shades are milk glass: they must not block their bulb. Each light's **shadow link** excludes the shades, so they cast no shadows (UE has no per-light shadow exclusion; a mesh outside every light's shadow link casts none).

Lit only by its lamps, the room is even and flat: nothing tells the eye where to look.

## A key light

Give the image a subject. A warm spot from the ceiling onto the near table makes the table the brightest place in the frame; the lamps become the fill and the back of the room falls off.

<div class="hdue-figure" data-figure="tut-interior-key"></div>

1. Add a **Light** LOP: **Type** Sphere, **Radius** 0.04, **Exposure** 12, **Color Temperature** 3000 K.
2. Turn on **Spot Light** (`ShapingAPI`): **Cone Angle** 22, **Cone Softness** 0.5. A soft edge reads as a real fixture, not a stage spot.
3. Place it 2.6 m above the near table, pointing down.

A warmer key (3000 K) than the room's lamps (2700 K) would be too orange; matching them keeps the light motivated: it looks like one more lamp in the restaurant.

## Haze and dust

Air in a busy room is never clean. A thin volumetric fog lets the lights draw their shape in the air, and a few motes of dust catch the key.

<div class="hdue-figure" data-figure="tut-interior-haze"></div>

1. On the **Fog** tab, turn on **Exponential Height Fog** with **Fog Density** 0.05 and **Fog Height Falloff** 0.02 (nearly even at every height in a room).
2. Turn on **Volumetric Fog**: lights scatter into the fog and the shades cast shadows in it.
3. On the lights, raise **Volumetric Scattering Intensity** (`unreal:light:VolumetricScatteringIntensity`) to 2: the lamps glow in the air without lighting the room more.
4. For dust, add a **Points** prim inside the key's cone: 700 points, 1.5 to 3 mm wide, with a pale MaterialX standard surface. Dust shows only where light hits it, so place it in the beam, not the whole room.

!!! tip
    Keep the fog thin. At **Fog Density** 0.1 the room turns milky and the shadows lose their depth; 0.03 to 0.05 reads as air.

## The lens

A photographer at this table would open the aperture: the near glass sharp, the bar soft behind it. The camera's `fStop` and `focusDistance` drive UE's depth of field (USD lens values are in tenths of a scene unit; `focusDistance` is in scene units).

<div class="hdue-figure" data-figure="tut-interior-lens"></div>

1. On the camera, set **F-Stop** to 2.8.
2. Set **Focus Distance** to the near wine glass: 1.37 m here. Measure it in the viewport with the camera's **Focus Distance** handle, or from the glass's bounds.

## The grade

A final touch of film: a little grain and a white balance a touch warmer than daylight.

<div class="hdue-figure" data-figure="tut-interior-grade"></div>

- **Film Grain Intensity** (`unreal:postProcess:FilmGrainIntensity`) 0.12: enough to break up the smooth gradients of the haze, not enough to notice.
- **White Temp** (`unreal:postProcess:WhiteTemp`) 6000: UE's white balance, set slightly below the 6500 K default, warms the whole image.

## Lumen and the Path Tracer

The same shot in both render modes (the figure at the top of this page). Both show the haze, the key's pool and the depth of field; the Path Tracer's bounce light and glass are the reference, Lumen renders the frame in a fraction of the time.

## Final render settings

- **Render Mode**: Realtime (Lumen), or Path Tracer for the reference
- **Unreal Tonemapper**: on, **Exposure Compensation** +1
- Lamps: 2700 K, **Exposure** 10, shades outside every shadow link
- Key: sphere spot, 3000 K, **Exposure** 12, cone 22°, softness 0.5
- **Exponential Height Fog**: on, **Fog Density** 0.05, **Fog Height Falloff** 0.02, **Volumetric Fog** on
- Camera: **F-Stop** 2.8, **Focus Distance** 1.37
- **Film Grain Intensity** 0.12, **White Temp** 6000
