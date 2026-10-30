"""husk's verbose callback for hdUE (husk.verbose_callback in UsdRenderers.json), as karma_stats.py is for Karma.

husk runs it with the verbosity code, the mode (start, active, snapshot, end, info) and its stats as JSON. It prints
the progress at -V3 and above, the frame time at the end of a frame (-V2), and the render stats annotation (-V5).
"""

import json
import sys


def main(argv):
    verbosity, mode, stats = int(argv[1][0]), argv[2], json.loads(argv[3])
    render = stats.get("render_stats", {})
    progress = render.get("renderProgressAnnotation", "").replace("\n", ", ")
    if mode == "start" and verbosity > 1:
        print(
            f">>> Render {stats.get('product', '')} frame {stats.get('frame', '')}"
        )
    elif mode in ("active", "snapshot") and verbosity > 2:
        line = f"{render.get('percentDone', 0):5.1f}% {progress}"
        # husk calls every husk.verbose_interval: print changes only.
        if line != globals().get("_last"):
            globals()["_last"] = line
            print(line)
    elif mode == "end" and verbosity > 1:
        print(f"{progress}; {render.get('requestToFrameMs', 0) / 1000:.2f} s")
        if verbosity > 4 and render.get("renderStatsAnnotation"):
            print(render["renderStatsAnnotation"])
    sys.stdout.flush()


if __name__ == "__main__":
    main(sys.argv)
