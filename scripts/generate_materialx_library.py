# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = ["MaterialX==1.39.4"]
# ///
"""Writes scripts/materialx_library/hdue_defs.mtlx: the MaterialX 1.39.5 nodes Houdini 22 ships that UE 5.8's MaterialX
(1.39.4) lacks, for hdUE's export to UE (export_usd_materialx.definitions):

- extract of a matrix33/44 (a column), lowered by materialx_lowering.py;
- flake2d and flake3d, as node graphs of standard nodes porting MaterialX's mx_flake.glsl (the 3x3x3 cells around the
  point, each holding a randomly rotated flake cube; the highest-priority flake covering the point gives its normal,
  sampled from GGX with the given roughness). MaterialX's cell noise takes a fourth coordinate for its second and third
  random streams; here they are the cell offset by fixed vectors, so the flakes match MaterialX's statistically, not
  cell for cell (UE's cell noise is not MaterialX's either).

    uv run scripts/generate_materialx_library.py
"""

import itertools
import math
from pathlib import Path

import MaterialX as mx
from local_paths import local_path

OUT = Path(__file__).resolve().parent / "materialx_library" / "hdue_defs.mtlx"
# MaterialX's flake density fit (mx_flake_density_to_probability).
ABCD = (-26.19771808, 26.39663835, 85.53857017, -102.35069432)
EF = (-101.42634862, 118.45082288)
DIAMETER = 1.5 / math.sqrt(3.0)
# Stand-ins for mx_cell_noise(vec4(cell, w)): the cell offset per stream.
STREAMS = {
    "probability": (0.0, 0.0, 0.0),
    "priority": (173.1, 91.7, 37.3),
    "rx": (11.3, 47.9, 83.1),
    "ry": (59.7, 17.3, 29.9),
    "rz": (97.1, 71.3, 5.9),
    "nx": (31.7, 3.1, 67.3),
    "ny": (7.9, 89.3, 43.1),
    "nz": (61.3, 23.9, 13.7),
}


class Builder:
    def __init__(self, graph):
        self.g = graph
        self.count = itertools.count()

    def node(self, category, type_, **inputs):
        node = self.g.addNode(category, f"n{next(self.count)}", type_)
        for name, value in inputs.items():
            if isinstance(value, mx.Node):
                port = node.addInput(name, value.getType())
                port.setConnectedNode(value)
            elif isinstance(value, tuple) and isinstance(value[0], mx.Node):
                port = node.addInput(name, value[1])
                port.setConnectedNode(value[0])
                port.setOutputString(value[2])
            elif isinstance(value, str) and value.startswith("@"):
                port = node.addInput(name, inputs.get("_type", type_))
                port.setInterfaceName(value[1:])
            else:
                kind = "float" if isinstance(value, (int, float)) else "vector3"
                port = node.addInput(name, kind)
                port.setValueString(
                    ", ".join(f"{v:.9g}" for v in value)
                    if isinstance(value, tuple)
                    else f"{value:.9g}"
                )
        return node

    def f(self, category, *args):
        names = ("in1", "in2") if len(args) == 2 else ("in",)
        return self.node(
            category, "float", **dict(zip(names, args, strict=False))
        )

    def v(self, category, *args, type_="vector3"):
        names = ("in1", "in2") if len(args) == 2 else ("in",)
        return self.node(
            category, type_, **dict(zip(names, args, strict=False))
        )

    def interface(self, name, type_):
        node = self.g.addNode("dot", f"in_{name}", type_)
        node.addInput("in", type_).setInterfaceName(name)
        return node

    def component(self, vec, index):
        node = self.node("extract", "float", **{"in": vec})
        node.addInput("index", "integer").setValueString(str(index))
        return node

    def pick(self, condition_a, condition_b, if_ge, otherwise, type_="float"):
        """If condition_a >= condition_b: if_ge else otherwise (MaterialX 1.39: value1 >= value2 ? in1 : in2)."""
        return self.node(
            "ifgreatereq",
            type_,
            value1=condition_a,
            value2=condition_b,
            in1=if_ge,
            in2=otherwise,
        )


def flake_graph(doc, name, two_d):
    nodedef = doc.getNodeDef(f"ND_{name}")
    graph = doc.addNodeGraph(f"NG_{name}")
    graph.setNodeDefString(nodedef.getName())
    b = Builder(graph)
    size = b.interface("size", "float")
    roughness = b.interface("roughness", "float")
    coverage = b.interface("coverage", "float")
    normal = b.interface("normal", "vector3")
    tangent = b.interface("tangent", "vector3")
    bitangent = b.interface("bitangent", "vector3")
    if two_d:
        uv = b.interface("texcoord", "vector2")
        position = b.node(
            "combine3",
            "vector3",
            in1=b.component(uv, 0),
            in2=b.component(uv, 1),
            in3=0.0,
        )
    else:
        position = b.interface("position", "vector3")
    # probability = (a x^2 + b x) / (c x^3 + d x^2 + e x + f), x = clamp(coverage, 0, 1)
    x = b.node("clamp", "float", **{"in": coverage, "low": 0.0, "high": 1.0})
    xx = b.f("multiply", x, x)
    numerator = b.f(
        "add", b.f("multiply", xx, ABCD[0]), b.f("multiply", x, ABCD[1])
    )
    denominator = b.f(
        "add",
        b.f(
            "add",
            b.f("multiply", b.f("multiply", xx, x), ABCD[2]),
            b.f("multiply", xx, ABCD[3]),
        ),
        b.f("add", b.f("multiply", x, EF[0]), EF[1]),
    )
    probability = b.f("divide", numerator, denominator)
    p = b.node(
        "divide",
        "vector3",
        in1=position,
        in2=b.node("convert", "vector3", **{"in": size}),
    )
    base = b.v("floor", p)

    def noise(cell, stream):
        offset = STREAMS[stream]
        point = b.v("add", cell, offset) if any(offset) else cell
        return b.node("cellnoise3d", "float", position=point)

    # The highest priority wins, the later cell on a tie (as the GLSL loop). Each cell's priority and index are packed
    # into one float, floor(priority * 65536) * 32 + index, so one chain of max nodes selects it: a running best that
    # several nodes read made the graph's paths double per cell (2^27 for UE's importer, which walks paths).
    packed = None
    for index, (i, j, k) in enumerate(itertools.product((-1, 0, 1), repeat=3)):
        cell = b.v("add", base, (float(i), float(j), float(k)))
        pp = b.v("subtract", b.v("subtract", p, cell), (0.5, 0.5, 0.5))
        inside = b.pick(
            DIAMETER * DIAMETER * 3.0,
            b.node("dotproduct", "float", in1=pp, in2=pp),
            1.0,
            0.0,
        )
        # A cell exists when its noise is at most the probability.
        exists = b.pick(probability, noise(cell, "probability"), 1.0, 0.0)
        # The flake's random rotation (Arvo's fast random rotation, mx_rotate_flake) applied to pp.
        ix, iy, iz = noise(cell, "rx"), noise(cell, "ry"), noise(cell, "rz")
        theta = b.f("multiply", ix, 2.0 * math.pi)
        phi = b.f("multiply", iy, 2.0 * math.pi)
        z = b.f("multiply", iz, 2.0)
        r = b.f("sqrt", z)
        vx = b.f("multiply", b.f("sin", phi), r)
        vy = b.f("multiply", b.f("cos", phi), r)
        vz = b.f("sqrt", b.f("subtract", 2.0, z))
        st, ct = b.f("sin", theta), b.f("cos", theta)
        sx = b.f("subtract", b.f("multiply", vx, ct), b.f("multiply", vy, st))
        sy = b.f("add", b.f("multiply", vx, st), b.f("multiply", vy, ct))
        # mat3 m columns: (vx*sx - ct, vx*sy - st, vx*vz), (vy*sx + st, vy*sy - ct, vy*vz), (vz*sx, vz*sy, 1 - z)
        columns = [
            (
                b.f("subtract", b.f("multiply", vx, sx), ct),
                b.f("subtract", b.f("multiply", vx, sy), st),
                b.f("multiply", vx, vz),
            ),
            (
                b.f("add", b.f("multiply", vy, sx), st),
                b.f("subtract", b.f("multiply", vy, sy), ct),
                b.f("multiply", vy, vz),
            ),
            (
                b.f("multiply", vz, sx),
                b.f("multiply", vz, sy),
                b.f("subtract", 1.0, z),
            ),
        ]
        px, py, pz = (b.component(pp, c) for c in range(3))
        inbox = None
        for row in range(3):  # (m * pp)[row] = sum_j column_j[row] * pp_j
            value = b.f(
                "add",
                b.f(
                    "add",
                    b.f("multiply", columns[0][row], px),
                    b.f("multiply", columns[1][row], py),
                ),
                b.f("multiply", columns[2][row], pz),
            )
            within = b.pick(DIAMETER, b.f("absval", value), 1.0, 0.0)
            inbox = within if inbox is None else b.f("multiply", inbox, within)
        valid = b.f("multiply", b.f("multiply", inside, exists), inbox)
        priority = b.f("multiply", valid, noise(cell, "priority"))
        key = b.f(
            "add",
            b.f(
                "multiply",
                b.f("floor", b.f("multiply", priority, 65536.0)),
                32.0,
            ),
            float(index),
        )
        packed = key if packed is None else b.f("max", packed, key)
    winner = b.f("modulo", packed, 32.0)
    best = b.f("divide", b.f("floor", b.f("divide", packed, 32.0)), 65536.0)
    # The winning cell: base + (index % 3, index / 3 % 3, index / 9) - 1, the loop's order (i outer, k inner).
    oz = b.f("subtract", b.f("modulo", winner, 3.0), 1.0)
    oy = b.f(
        "subtract",
        b.f("modulo", b.f("floor", b.f("divide", winner, 3.0)), 3.0),
        1.0,
    )
    ox = b.f("subtract", b.f("floor", b.f("divide", winner, 9.0)), 1.0)
    best_cell = b.v(
        "add", base, b.node("combine3", "vector3", in1=ox, in2=oy, in3=oz)
    )
    xi0, xi1, rand = (noise(best_cell, s) for s in ("nx", "ny", "nz"))
    phi = b.f("multiply", xi0, 2.0 * math.pi)
    tan_theta = b.f(
        "divide",
        b.f(
            "multiply", b.f("multiply", roughness, roughness), b.f("sqrt", xi1)
        ),
        b.f("sqrt", b.f("subtract", 1.0, xi1)),
    )
    sin_theta = b.f(
        "divide",
        tan_theta,
        b.f("sqrt", b.f("add", 1.0, b.f("multiply", tan_theta, tan_theta))),
    )
    cos_theta = b.f(
        "sqrt", b.f("subtract", 1.0, b.f("multiply", sin_theta, sin_theta))
    )
    flake = b.v(
        "add",
        b.v(
            "add",
            b.v(
                "multiply", tangent, b.f("multiply", b.f("cos", phi), sin_theta)
            ),
            b.v(
                "multiply",
                bitangent,
                b.f("multiply", b.f("sin", phi), sin_theta),
            ),
        ),
        b.v("multiply", normal, cos_theta),
    )
    flake = b.v("normalize", flake)
    # No flake (priority 0): id 0, rand 0, presence 0, the surface's normal.
    has = lambda value, default, type_="float": b.node(
        "ifgreater", type_, value1=best, value2=0.0, in1=value, in2=default
    )  # noqa: E731
    outputs = {
        "id": b.node(
            "floor",
            "integer",
            **{"in": has(b.f("multiply", rand, 16777215.0), 0.0)},
        ),
        "rand": has(rand, 0.0),
        "presence": has(best, 0.0),
        "flakenormal": has(flake, normal, "vector3"),
    }
    for out_name, node in outputs.items():
        out = graph.addOutput(out_name, nodedef.getOutput(out_name).getType())
        out.setConnectedNode(node)


def main():
    houdini = mx.createDocument()
    root = local_path("houdini_root") / "houdini/materialx"
    mx.loadLibraries(["libraries"], mx.FileSearchPath(str(root)), houdini)
    doc = mx.createDocument()
    for name in (
        "ND_extract_matrix33",
        "ND_extract_matrix44",
        "ND_flake2d",
        "ND_flake3d",
    ):
        source = houdini.getNodeDef(name)
        target = doc.addNodeDef(name, "", source.getNodeString())
        target.copyContentFrom(source)
    flake_graph(doc, "flake2d", True)
    flake_graph(doc, "flake3d", False)
    check = (
        mx.createDocument()
    )  # validated against UE's MaterialX (1.39.4) libraries
    standard = mx.createDocument()
    mx.loadLibraries(
        mx.getDefaultDataLibraryFolders(),
        mx.getDefaultDataSearchPath(),
        standard,
    )
    check.importLibrary(standard)
    check.importLibrary(doc)
    valid, message = check.validate()
    if not valid:
        raise SystemExit(message)
    OUT.parent.mkdir(exist_ok=True)
    header = "<!-- Generated by scripts/generate_materialx_library.py. Do not edit. -->\n"
    # The copied definitions keep Houdini's file as their source, which MaterialX would write as an include.
    for element in doc.traverseTree():
        element.removeAttribute("xi:include")
        element.setSourceUri("")
    text = mx.writeToXmlString(doc)
    OUT.write_text(text.replace("?>\n", "?>\n" + header, 1), encoding="utf-8")
    print(f"{OUT}: {sum(len(g.getNodes()) for g in doc.getNodeGraphs())} nodes")


if __name__ == "__main__":
    main()
