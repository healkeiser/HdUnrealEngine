"""Draws the README comparison charts from the gallery results (out/gallery/results.json and out/gallery/sponza/).

    uv run --with matplotlib python scripts/gallery_charts.py

Writes docs/images/gallery/chart-times-{light,dark}.svg (render time per scene, log scale) and
chart-accuracy-{light,dark}.svg (mean brightness relative to Karma CPU). Colour encodes the engine (hdUE blue,
Karma orange) and marker shape the mode, so no two series rely on colour alone.
"""

import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("svg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FixedLocator, NullLocator

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out" / "gallery"
IMAGES = ROOT / "docs" / "images" / "gallery"

THEMES = {  # GitHub's page colours (the charts are transparent; surface only rings the markers) and engine hues
    "light": dict(
        surface="#ffffff",
        primary="#1f2328",
        secondary="#59636e",
        muted="#59636e",
        grid="#e3e7eb",
        axis="#d1d9e0",
        hdue="#2a78d6",
        karma="#eb6834",
    ),
    "dark": dict(
        surface="#0d1117",
        primary="#f0f6fc",
        secondary="#9198a1",
        muted="#9198a1",
        grid="#21262d",
        axis="#3d444d",
        hdue="#3987e5",
        karma="#d95926",
    ),
}
SERIES = {  # renderer key: label, engine, marker
    "lumen": ("hdUE Lumen", "hdue", "o"),
    "pathtraced": ("hdUE Path Tracer", "hdue", "D"),
    "xpu": ("Karma XPU", "karma", "^"),
    "karma": ("Karma CPU", "karma", "s"),
}
SIZE = {
    "o": 8,
    "D": 7.5,
    "^": 9.5,
    "s": 7.5,
}  # optically equal areas (a triangle reads smaller at the same size)
ORDER = [
    "cornell",
    "glossy",
    "depth_of_field",
    "motion_blur",
    "subdivision",
    "instances",
    "volume",
    "ies",
    "light_linking",
    "clearcoat",
    "render_region",
    "lens_shift",
    "orthographic",
    "exposure",
    "post_processing",
]


def rows():
    """(label, renders) per scene, Cornell first, then Sponza."""
    out = []
    cornell = OUT / "results.json"
    if cornell.exists():
        results = json.loads(cornell.read_text())
        out += [
            (results[n]["title"], results[n]["renders"])
            for n in ORDER
            if n in results
        ]
    sponza = OUT / "sponza" / "results.json"
    if sponza.exists():
        out += [
            (f"Sponza: {entry['title'].lower()}", entry["renders"])
            for entry in json.loads(sponza.read_text()).values()
        ]
    return out


def style(ax, t):
    ax.set_facecolor("none")
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(t["axis"])
    ax.tick_params(axis="x", colors=t["muted"], labelsize=9, length=0)
    ax.tick_params(axis="y", colors=t["secondary"], labelsize=10, length=0)
    ax.grid(axis="x", color=t["grid"], linewidth=0.8)
    ax.set_axisbelow(True)


def legend(fig, t, keys):
    handles = [
        Line2D(
            [],
            [],
            linestyle="none",
            marker=SERIES[k][2],
            markersize=SIZE[SERIES[k][2]],
            markerfacecolor=t[SERIES[k][1]],
            markeredgecolor=t["surface"],
            markeredgewidth=1.5,
            label=SERIES[k][0],
        )
        for k in keys
    ]
    fig.legend(
        handles=handles,
        loc="upper left",
        ncol=len(keys),
        frameon=False,
        fontsize=10,
        labelcolor=t["primary"],
        bbox_to_anchor=(0.005, fig.legend_top),
        handletextpad=0.3,
        columnspacing=1.4,
    )


def dots(ax, t, data, keys, value):
    for y, (_, renders) in enumerate(data):
        values = [value(renders, k) for k in keys if k in renders]
        if (
            len(values) > 1
        ):  # hairline span from fastest to slowest, the gap a reader compares
            ax.plot(
                [min(values), max(values)],
                [y, y],
                color=t["axis"],
                linewidth=1,
                zorder=1,
            )
        for k in keys:
            if k in renders:
                ax.plot(
                    value(renders, k),
                    y,
                    linestyle="none",
                    marker=SERIES[k][2],
                    markersize=SIZE[SERIES[k][2]],
                    markerfacecolor=t[SERIES[k][1]],
                    markeredgecolor=t["surface"],
                    markeredgewidth=2,
                    zorder=3,
                )


HEADER = 1.25  # inches above the plot: title, subtitle, legend
FOOTER = 0.42  # inches below the axis labels: how the scenes were rendered


def setup_lines():
    """Resolution, samples and machine of the renders, as scripts/render_gallery.py records them."""
    path = OUT / "setup.json"
    if not path.exists():
        return []
    setup = json.loads(path.read_text(encoding="utf-8"))
    names = {"cornell": "Cornell box", "sponza": "Sponza"}
    scenes = " · ".join(
        f"{names[k]}: {v['resolution']}, {v['samples']} spp (path tracers)"
        for k, v in setup["sets"].items()
    )
    return [
        scenes,
        f"Up to 32 bounces · hdUE Lumen: 32 warmup frames, then 1 per frame · {setup['machine']}",
    ]


def figure(t, data, title, subtitle):
    height = 0.42 * len(data) + HEADER + 0.5 + FOOTER
    fig, ax = plt.subplots(figsize=(9, height), dpi=100)
    fig.patch.set_alpha(0)
    fig.subplots_adjust(
        left=0.27,
        right=0.97,
        top=1 - HEADER / height,
        bottom=(0.5 + FOOTER) / height,
    )
    at = lambda inches: 1 - inches / height
    fig.text(
        0.01,
        at(0.12),
        title,
        color=t["primary"],
        fontsize=13,
        fontweight="bold",
        va="top",
    )
    fig.text(
        0.01, at(0.42), subtitle, color=t["secondary"], fontsize=9.5, va="top"
    )
    fig.legend_top = at(0.68)
    for n, line in enumerate(setup_lines()):
        fig.text(
            0.01,
            (FOOTER - 0.14 - 0.17 * n) / height,
            line,
            color=t["secondary"],
            fontsize=8.5,
            va="baseline",
        )
    style(ax, t)
    ax.set_yticks(range(len(data)), [label for label, _ in data])
    ax.set_ylim(len(data) - 0.5, -0.5)
    return fig, ax


def times(theme, data):
    t = THEMES[theme]
    keys = [k for k in SERIES if any(k in r for _, r in data)]
    fig, ax = figure(
        t,
        data,
        "Render time per frame",
        "Log scale. Same scene, resolution, samples and bounces; startup excluded. Lower is faster. "
        "Right: times faster than Karma XPU (Lumen / Path Tracer).",
    )
    seconds = lambda r, k: max(r[k]["seconds"], 0.005)
    dots(ax, t, data, keys, seconds)
    ax.set_xscale("log")
    values = [seconds(r, k) for _, r in data for k in keys if k in r]
    shortest, longest = min(values), max(values)
    ticks = [
        v
        for v in (0.01, 0.03, 0.1, 0.3, 1, 3, 10, 30, 100, 300, 1000, 3000)
        if shortest / 1.2 <= v * 3 and v <= longest * 1.2
    ]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_xticks(ticks, [f"{v * 1000:g} ms" if v < 1 else f"{v:g} s" for v in ticks])
    # How many times faster hdUE is than the GPU Karma, at the end of each row.
    reference = "xpu" if any("xpu" in r for _, r in data) else "karma"
    for y, (_, r) in enumerate(data):
        if reference not in r:
            continue
        speed = [
            f"{seconds(r, reference) / seconds(r, k):,.0f}×"
            for k in ("lumen", "pathtraced")
            if k in r
        ]
        ax.text(
            max(seconds(r, k) for k in keys if k in r) * 1.5,
            y,
            " / ".join(speed),
            color=t["secondary"],
            fontsize=9,
            va="center",
        )
    ax.set_xlim(shortest / 1.6, longest * 14)
    legend(fig, t, keys)
    save(fig, f"chart-times-{theme}", t)
    plt.close(fig)


def accuracy(theme, data):
    t = THEMES[theme]
    data = [
        (label, r)
        for label, r in data
        if "karma" in r and label != "Unreal post-processing"
    ]
    keys = [
        k
        for k in ("lumen", "pathtraced", "xpu")
        if any(k in r for _, r in data)
    ]
    fig, ax = figure(
        t,
        data,
        "Brightness relative to Karma CPU",
        "Mean luminance ratio; 1.0 = same as Karma CPU. Post-processing is tone-mapped and left out.",
    )
    ratio = lambda r, k: r[k]["luminance"] / r["karma"]["luminance"]
    ax.axvline(
        1.0, color=t["muted"], linewidth=1, linestyle=(0, (3, 3)), zorder=2
    )
    ax.text(
        1.0,
        -0.62,
        "Karma CPU",
        color=t["muted"],
        fontsize=9,
        ha="center",
        va="bottom",
    )
    dots(ax, t, data, keys, ratio)
    ticks = [0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5]
    ax.set_xticks(ticks, [f"{v:g}x" for v in ticks])
    ax.set_xlim(-0.05, 1.55)
    legend(fig, t, keys)
    save(fig, f"chart-accuracy-{theme}", t)
    plt.close(fig)


PREVIEW = (
    Path(sys.argv[sys.argv.index("--preview") + 1])
    if "--preview" in sys.argv
    else None
)


def save(fig, name, t):
    fig.savefig(IMAGES / f"{name}.svg", transparent=True)
    if PREVIEW:  # PNG copies for checking the layout
        fig.savefig(PREVIEW / f"{name}.png", facecolor=t["surface"], dpi=110)


def main():
    plt.rcParams.update(
        {"font.family": ["Segoe UI", "DejaVu Sans"], "svg.fonttype": "none"}
    )
    data = rows()
    IMAGES.mkdir(parents=True, exist_ok=True)
    for theme in THEMES:
        times(theme, data)
        accuracy(theme, data)
    print("charts:", len(data), "scenes")


if __name__ == "__main__":
    main()
