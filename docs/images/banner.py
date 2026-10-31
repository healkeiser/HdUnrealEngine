"""Generate the README banner from hdUE renders of Intel's Sponza.

The render bleeds full frame under a halftone dot lattice on its left side and the title: the type and the
lattice of fxhoudinimotion's banner.

1. Render samples/sponza/intel/banner/banner.hip (/root/PhysCamera004, frames 1-96, 2560x1440) and convert
   each EXR to an sRGB PNG, 001.png..096.png.
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
from PIL import Image, ImageDraw, ImageFilter

W, H = 2560, 1280  # 2x the 1280x640 layout
BG = np.array([0x12, 0x11, 0x10], float)
DOT = np.array([0xF1, 0xEB, 0xE3], float)
STEP = 40  # lattice pitch (20 px at 1x, as fxhoudinimotion)
SS = 3  # dot supersampling
FRAMES, STILL = 96, 48
LUT = np.load(Path(__file__).with_name("banner_lut.npy"))


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def compose(frame: Path, text: Image.Image) -> Image.Image:
    src = Image.open(frame).convert("RGB")
    top = (src.height - H) // 2
    photo = np.asarray(src.crop((0, top, W, top + H)))
    # banner_lut.npy: per-channel curves (histogram matching) from husk's EXR to the same frame in Houdini's
    # viewport, whose image is darker and more contrasted than husk's for the same settings.
    photo = np.stack([LUT[c][photo[..., c]] for c in range(3)], -1)
    x = np.linspace(0, 1, W)[None, :, None]
    # The render bleeds full frame; the dot lattice covers its left side.
    fade = smooth(0.06, 0.46, x)
    out = photo.astype(float)
    # A soft shadow under the title keeps it legible over the render.
    shade = np.asarray(text.getchannel("A").filter(ImageFilter.GaussianBlur(12)), float)[..., None] / 255
    out = out * (1 - 0.8 * np.clip(shade * 2.5, 0, 1))
    # Halftone lattice over the render's left side: cream dots on its dark areas, dark dots on its bright ones, so the
    # lattice reads everywhere without hiding the image.
    lum = (photo @ [0.2126, 0.7152, 0.0722]) / 255
    knock = np.asarray(text.getchannel("A").filter(ImageFilter.GaussianBlur(26)), float) / 255
    layers = {"light": Image.new("L", (W * SS, H * SS)), "dark": Image.new("L", (W * SS, H * SS))}
    draws = {k: ImageDraw.Draw(v) for k, v in layers.items()}
    for cy in range(STEP // 2, H, STEP):
        for cx in range(STEP // 2, W, STEP):
            w = 1 - fade[0, cx, 0]
            if w <= 0.02:
                continue
            l = lum[max(0, cy - 6):cy + 6, max(0, cx - 6):cx + 6].mean()
            r = 2.2 + 3.2 * w
            a = 0.42 * w * (1 - 0.9 * min(1.0, knock[cy, cx] * 3))
            if a < 0.01:
                continue
            draws["light" if l < 0.4 else "dark"].ellipse(
                [(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=int(255 * a)
            )
    for name, color in (("light", DOT), ("dark", BG)):
        alpha = np.asarray(layers[name].resize((W, H), Image.LANCZOS), float)[..., None] / 255
        out = out + (color - out) * alpha
    img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
    img.paste(text, (0, 0), text)
    return img


def _frame(args):
    frames, out, i = args
    text = Image.open(Path(__file__).with_name("banner_title.png")).convert("RGBA")
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
