"""Generate the README banner from hdUE renders of Intel's Sponza.

The render bleeds full frame under the title, set in fxhoudinimotion's banner type.

1. Render samples/sponza/intel/banner/banner.hip (/root/PhysCamera004, frames 1-96, 2560x1440) and convert
   each EXR to an sRGB PNG (the Un-tone-mapped view), 001.png..096.png.
2. Render the title layer (banner.html, 2x, transparent):
       chrome --headless=new --hide-scrollbars --virtual-time-budget=5000 --default-background-color=00000000 \\
           --force-device-scale-factor=2 --window-size=1280,640 --screenshot=banner_title.png banner.html
3. uv run --with numpy --with pillow python banner.py FRAMES_DIR OUT_DIR
   writes banner.png (frame 48, 2x) and the 1280x640 frames into OUT_DIR.
4. Forward then backward, an 8 s loop: OUT_DIR's frames 1..96 then 95..2 as loop/001.png.., then
       ffmpeg -framerate 24 -i loop/%03d.png -vf fps=16 -c:v libwebp -quality 40 -compression_level 6 -loop 0 banner.webp
   (16 fps, quality 40: under the 5 MB GitHub's image proxy, camo, defaults to)
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

W, H = 2560, 1280  # 2x the 1280x640 layout
FRAMES, STILL = 96, 48


def compose(frame: Path, text: Image.Image) -> Image.Image:
    src = Image.open(frame).convert("RGB")
    top = (src.height - H) // 2
    # husk's frame as UE shows it (sRGB, Un-tone-mapped): Houdini's viewport, darker in the shadows, crushes linear
    # values under ~0.004 in its OCIO display (docs/rendering.md, "Viewport and husk").
    photo = np.asarray(src.crop((0, top, W, top + H)))
    out = photo.astype(float)
    # A soft shadow under the title keeps it legible over the render.
    shade = (
        np.asarray(
            text.getchannel("A").filter(ImageFilter.GaussianBlur(12)), float
        )[..., None]
        / 255
    )
    out = out * (1 - 0.8 * np.clip(shade * 2.5, 0, 1))
    img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
    img.paste(text, (0, 0), text)
    return img


def _frame(args):
    frames, out, i = args
    text = Image.open(Path(__file__).with_name("banner_title.png")).convert(
        "RGBA"
    )
    img = compose(frames / f"{i:03d}.png", text)
    if i == STILL:
        img.save(Path(__file__).with_name("banner.png"))
    img.resize((W // 2, H // 2), Image.LANCZOS).save(out / f"{i:03d}.png")


if __name__ == "__main__":
    from multiprocessing import Pool

    frames, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    with Pool() as pool:
        pool.map(_frame, [(frames, out, i) for i in range(1, FRAMES + 1)])
