"""Write samples/docs/textures/lens_dirt.png, the user guide's lens dirt for the Dirt Mask figures: soft dust spots
and smudges on black, as a dirt mask texture for UE's bloom is. Run with uv run python scripts/make_lens_dirt.py.
"""

from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

OUT = (
    Path(__file__).resolve().parents[1]
    / "samples"
    / "docs"
    / "textures"
    / "lens_dirt.png"
)


def main():
    rng = np.random.default_rng(
        7
    )  # fixed: the figure must not change between runs
    width, height = 1024, 576
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    dirt = np.zeros((height, width), np.float32)
    for _ in range(260):  # dust: small soft discs of varied strength
        cx, cy, r = (
            rng.uniform(0, width),
            rng.uniform(0, height),
            rng.uniform(2, 14),
        )
        dirt += rng.uniform(0.2, 1) * np.exp(
            -((x - cx) ** 2 + (y - cy) ** 2) / (2 * r * r)
        )
    for _ in range(
        14
    ):  # smudges: wide, faint, stretched blobs, like fingerprints
        cx, cy = rng.uniform(0, width), rng.uniform(0, height)
        rx, ry, a = (
            rng.uniform(40, 160),
            rng.uniform(15, 60),
            rng.uniform(0, np.pi),
        )
        u = (x - cx) * np.cos(a) + (y - cy) * np.sin(a)
        v = -(x - cx) * np.sin(a) + (y - cy) * np.cos(a)
        dirt += rng.uniform(0.15, 0.4) * np.exp(
            -((u / rx) ** 2 + (v / ry) ** 2)
        )
    image = Image.fromarray((np.clip(dirt, 0, 1) * 255).astype(np.uint8))
    # RGB, not greyscale: UE imports a one-channel image as red only.
    image.filter(ImageFilter.GaussianBlur(1.5)).convert("RGB").save(
        OUT, optimize=True
    )
    print(OUT)


if __name__ == "__main__":
    main()
