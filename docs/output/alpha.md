# Alpha

The beauty carries straight alpha: 1 where geometry covers the pixel, 0 where the camera sees no geometry, fractional on anti-aliased edges and see-through surfaces. hdUE turns on UE's `r.PostProcessing.PropagateAlpha` at startup and converts UE's inverted coverage to straight alpha. A dome light hidden from camera rays (`unreal:visibleBackground = 0`) leaves alpha 0 behind it.
