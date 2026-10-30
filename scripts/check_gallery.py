"""Fails when a gallery scene's hdUE brightness against Karma CPU leaves its band, or an image it links is missing.

Reads the tables render_gallery.py writes into docs/gallery.md, so CI checks what is committed and render_gallery.py
checks a new render before it is committed. A black scene (brightness 0.00) can no longer ship.

    python scripts/check_gallery.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# (Lumen, Path Tracer) brightness bands relative to Karma CPU. Lumen's GI is approximate and darker in a closed box.
DEFAULT = ((0.65, 1.15), (0.9, 1.1))
# Known, documented differences. Light linking: UE's lighting channels, which Lumen's GI ignores. Main view: Sponza
# without the denoiser, sun and sky fitted to Intel's renders.
BANDS = {
    "Glossy materials": ((0.65, 1.15), (1.1, 1.3)),
    "Volumes (OpenVDB)": ((0.65, 1.15), (1.2, 1.4)),
    "Light linking": ((1.4, 1.7), (0.9, 1.1)),
    "Main view": ((0.65, 1.15), (0.8, 1.0)),
}


def rows(text):
    """(title, Lumen ratio, Path Tracer ratio) per summary row; 'n/a' rows (display-referred images) are skipped."""
    for line in text.splitlines():
        match = re.match(
            r"\| ([^*|][^|]*?) \|.*\| (\d+\.\d+) / (\d+\.\d+) \|$", line
        )
        if match:
            yield match[1], float(match[2]), float(match[3])


def check(page=ROOT / "docs" / "gallery.md"):
    text = page.read_text(encoding="utf-8")
    failures = []
    found = list(rows(text))
    if not found:
        failures.append(f"{page}: no brightness table")
    for title, *ratios in found:
        for mode, ratio, (low, high) in zip(
            ("Lumen", "Path Tracer"),
            ratios,
            BANDS.get(title, DEFAULT),
            strict=True,
        ):
            if not low <= ratio <= high:
                failures.append(
                    f"{title}: {mode} brightness {ratio:.2f}x Karma CPU, outside {low}-{high}"
                )
    for image in re.findall(
        r"\]\((images/[^)]+\.(?:png|svg))\)", text
    ) + re.findall(r'src(?:set)?="(images/[^"]+)"', text):
        if not (page.parent / image).exists():
            failures.append(f"{page.name} links a missing image: {image}")
    return len(found), failures


def main():
    count, failures = check()
    print(
        "\n".join(failures)
        or f"Gallery: {count} scenes within their brightness bands."
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
