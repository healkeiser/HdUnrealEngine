"""Rewrites the MaterialX nodes UE's MaterialX importer cannot translate into nodes it can, before hdUE hands a document
to it (export_usd_materialx.write). UE 5.8 reads MaterialX 1.39.4 and translates float, vector and colour graphs:

- integer and boolean values become floats (and, or, not, xor become arithmetic), as UE's materials compute in floats;
- matrices become their elements: every matrix node is evaluated element by element into float nodes, with MaterialX's
  GLSL conventions (column-major; creatematrix's inputs are columns; transformmatrix is M * v; multiply is in2 * in1,
  divide is inverse(in2) * in1; stdlib_genglsl_impl.mtlx);
- Houdini's MaterialX 1.38 nodes (houdini/legacy_1_38.mtlx) go through MaterialX's own 1.38 to 1.39 upgrade (swizzle,
  atan2, normalmap, thin_film_bsdf over a layer); those 1.39 dropped without a replacement become standard nodes
  here (thin_surface, curveadjust, the Disney surfaces, a thin film over nothing);
- frame becomes time * fps, generalized_schlick_edf a uniform_edf weighted by Schlick's factor, ambientocclusion 1.
- what UE's importer fails on is written the way it reads: every node names its definition and its unset inputs get
  their defaults (minus those UE has no pin for), a mix of displacement nodes becomes one displacement node, a
  Standard Surface mixed with another shader carries its base weight in base_color, unread shader nodes go;
- the export (export_usd_materialx.write) also calls lower_primvars (primvar readers as the UV sets and vertex colour
  hdUE uploads) and lower_flakes (flakes as markers hdUE's import replaces with MaterialX's flake code).

lower(doc) returns notes for the approximations it made (curveadjust's interpolation, ambient occlusion).
"""

import itertools

import MaterialX as mx

INDEX = {"r": 0, "g": 1, "b": 2, "a": 3, "x": 0, "y": 1, "z": 2, "w": 3}
SIZE = {
    "float": 1,
    "vector2": 2,
    "color3": 3,
    "vector3": 3,
    "color4": 4,
    "vector4": 4,
}


class Graph:
    """Node building and rewiring in one MaterialX graph element (the document or a node graph)."""

    def __init__(self, element, library):
        self.element = element
        self.library = library
        self.counter = itertools.count()

    def node(self, category, type_, inputs, nodedef=None):
        """A new node; inputs maps names to (type, value) or (type, node, output)."""
        name = self.element.createValidChildName(
            f"lower_{category}_{next(self.counter)}"
        )
        node = self.element.addNode(category, name, type_)
        if nodedef:
            node.setNodeDefString(nodedef)
        for key, port in inputs.items():
            self.set(node, key, port)
        return node

    @staticmethod
    def set(node, key, port):
        port_type = port[0]
        target = node.getInput(key) or node.addInput(key, port_type)
        target.setType(port_type)
        if len(port) == 2:
            target.setValueString(
                port[1] if isinstance(port[1], str) else value_string(port[1])
            )
        else:
            target.removeAttribute("value")
            target.setConnectedNode(port[1])
            if port[2] and port[2] != "out":
                target.setOutputString(port[2])

    def port(self, node, key):
        """The input key of node as (type, value) or (type, node, output), its nodedef's default when unset."""
        source = node.getInput(key)
        if source is not None and source.getConnectedNode() is not None:
            return (
                source.getType(),
                source.getConnectedNode(),
                source.getOutputString() or "out",
            )
        if source is not None and source.getValueString():
            return (source.getType(), source.getValueString())
        definition = node.getNodeDef()
        default = definition.getActiveInput(key) if definition else None
        if default is not None:
            return (default.getType(), default.getValueString())
        return None

    def replace(self, old, new, output="out", old_output=None):
        """Every input connected to old (its old_output, or any) now reads new's output."""
        for consumer in list(self.element.getNodes()) + list(
            self.element.getOutputs()
        ):
            ports = (
                consumer.getInputs() if consumer.isA(mx.Node) else [consumer]
            )
            for port in ports:
                if port.getNodeName() != old.getName():
                    continue
                if (
                    old_output
                    and (port.getOutputString() or "out") != old_output
                ):
                    continue
                port.setConnectedNode(new)
                if output and output != "out":
                    port.setOutputString(output)
                else:
                    port.removeAttribute("output")
        self.element.removeNode(old.getName())


def value_string(values):
    return (
        ", ".join(f"{v:.9g}" for v in values)
        if isinstance(values, (list, tuple))
        else f"{values:.9g}"
    )


def floats(text):
    words = text.replace(",", " ").split() if text else []
    return [
        1.0 if w == "true" else 0.0 if w == "false" else float(w) for w in words
    ]


# Scalars: Python floats (folded) or (node, output) of float type.
class Scalars:
    def __init__(self, graph):
        self.g = graph

    def port(self, s):
        return ("float", s) if isinstance(s, float) else ("float", s[0], s[1])

    def op(self, category, *args, fold):
        if all(isinstance(a, float) for a in args):
            return fold(*args)
        names = ("in1", "in2") if len(args) == 2 else ("in",)
        node = self.g.node(
            category,
            "float",
            {n: self.port(a) for n, a in zip(names, args, strict=False)},
        )
        return (node, "out")

    def add(self, a, b):
        if a == 0.0:
            return b
        if b == 0.0:
            return a
        return self.op("add", a, b, fold=lambda x, y: x + y)

    def sub(self, a, b):
        return (
            a
            if b == 0.0
            else self.op("subtract", a, b, fold=lambda x, y: x - y)
        )

    def mul(self, a, b):
        if a == 0.0 or b == 0.0:
            return 0.0
        if a == 1.0:
            return b
        if b == 1.0:
            return a
        return self.op("multiply", a, b, fold=lambda x, y: x * y)

    def div(self, a, b):
        return (
            a
            if b == 1.0
            else self.op("divide", a, b, fold=lambda x, y: x / y if y else 0.0)
        )

    def total(self, terms):
        result = 0.0
        for t in terms:
            result = self.add(result, t)
        return result

    def components(self, port, size):
        """A vector port's components as scalars."""
        if len(port) == 2:
            values = floats(port[1]) or [0.0] * size
            return [float(v) for v in values[:size]] + [0.0] * (
                size - len(values)
            )
        type_, node, output = port
        if (
            type_ not in SIZE or type_ == "float"
        ):  # a scalar (integers and booleans become floats)
            return [(node, output)] * size
        result = []
        for i in range(SIZE.get(type_) or size):
            extract = self.g.node(
                "extract", "float", {"in": port, "index": ("integer", str(i))}
            )
            result.append((extract, "out"))
        return result[:size] + [0.0] * (size - len(result))

    def vector(self, type_, scalars):
        """A port of type_ built from scalars."""
        if all(isinstance(s, float) for s in scalars):
            return (
                type_,
                value_string(scalars if len(scalars) > 1 else scalars[0]),
            )
        if type_ == "float":
            return self.port(scalars[0])
        node = self.g.node(
            f"combine{len(scalars)}",
            type_,
            {f"in{i + 1}": self.port(s) for i, s in enumerate(scalars)},
        )
        return (type_, node, "out")


# --- integers and booleans -------------------------------------------------------------------------------------------


def lower_integers(g):
    """Integer and boolean outputs (and the inputs they feed) become floats, as UE computes; a node then resolves to
    the float form of its definition, or, for and, or, not, xor, to arithmetic.
    """
    s = Scalars(g)
    # A constant integer or boolean converted: the float form of the conversion, or the constant itself.
    for node in list(g.element.getNodes()):
        port = g.port(node, "in") if node.getCategory() == "convert" else None
        if (
            port is None
            or len(port) == 3
            or port[0] not in ("integer", "boolean")
        ):
            continue
        source = node.getInput("in") or node.addInput("in", port[0])
        value = (floats(port[1]) or [0.0])[0]
        if node.getType() in ("integer", "boolean", "float"):
            g.replace(
                node,
                g.node(
                    "constant",
                    "float",
                    {"value": ("float", value_string(value))},
                ),
            )
        else:
            source.setType("float")
            source.setValueString(value_string(value))
            node.removeAttribute("nodedef")
    changed = True
    while changed:
        changed = False
        for node in list(g.element.getNodes()):
            if node.getType() not in ("integer", "boolean"):
                continue
            category = node.getCategory()
            if category in ("and", "or", "xor", "not"):
                a = s.components(
                    g.port(node, "in1" if category != "not" else "in"), 1
                )[0]
                if category == "not":
                    result = s.sub(1.0, a)
                else:
                    b = s.components(g.port(node, "in2"), 1)[0]
                    result = {
                        "and": lambda: s.mul(a, b),
                        "or": lambda: s.op("max", a, b, fold=max),
                        "xor": lambda: s.op("absval", s.sub(a, b), fold=abs),
                    }[category]()
                retype_value(g, node, result)
                changed = True
                continue
            node.setType("float")
            for port in node.getInputs():
                if port.getType() in ("integer", "boolean"):
                    if port.getConnectedNode() is None:
                        port.setValueString(
                            value_string(
                                (floats(port.getValueString()) or [0.0])[0]
                            )
                        )
                    port.setType("float")
            node.removeAttribute("nodedef")
            changed = True
        # Inputs reading a node that became float.
        for node in g.element.getNodes():
            for port in node.getInputs():
                upstream = port.getConnectedNode()
                if (
                    upstream is not None
                    and port.getType() in ("integer", "boolean")
                    and upstream.getType() == "float"
                ):
                    port.setType("float")
                    node.removeAttribute("nodedef")
    for node in list(g.element.getNodes()):
        # A conversion that became float to float (an integer's convert to float): its input.
        if (
            node.getCategory() == "convert"
            and node.getNodeDef() is None
            and node.getType() == "float"
        ):
            replace_port(g, node, g.port(node, "in"))
    for node in g.element.getNodes():
        if node.getNodeDefString() == "" and node.getNodeDef() is None:
            raise ValueError(
                f"No float form of {node.getCategory()} ({node.getName()}) for UE"
            )


def retype_value(g, node, scalar):
    if isinstance(scalar, float):
        replacement = g.node(
            "constant", "float", {"value": ("float", value_string(scalar))}
        )
        g.replace(node, replacement)
    else:
        g.replace(node, scalar[0], scalar[1])


# --- matrices --------------------------------------------------------------------------------------------------------


def matrix_size(type_):
    return 3 if type_ == "matrix33" else 4


class Matrices:
    """Matrix ports as columns of scalars (C[j][i]: column j, row i, GLSL's layout)."""

    def __init__(self, graph):
        self.g = graph
        self.s = Scalars(graph)
        self.memo = {}

    def columns(self, port, n):
        if len(port) == 2:
            values = floats(port[1]) or [
                1.0 if i % (n + 1) == 0 else 0.0 for i in range(n * n)
            ]
            return [
                [float(values[j * n + i]) for i in range(n)] for j in range(n)
            ]
        _, node, _ = port
        key = node.getName()
        if key not in self.memo:
            self.memo[key] = self.evaluate(node, n)
        return self.memo[key]

    def evaluate(self, node, n):
        g, s = self.g, self.s
        category = node.getCategory()
        mat = lambda key: self.columns(g.port(node, key), n)  # noqa: E731
        if category == "constant":
            return mat("value")
        if category == "dot":
            return mat("in")
        if category == "creatematrix":
            columns = []
            for j in range(n):
                port = g.port(node, f"in{j + 1}")
                values = s.components(port, min(n, SIZE.get(port[0], 3)))
                if (
                    len(values) < n
                ):  # vector3 inputs of a matrix44: 0, and 1 for the last column
                    values = values + [1.0 if j == n - 1 else 0.0]
                columns.append(values)
            return columns
        if category == "transpose":
            m = mat("in")
            return [[m[i][j] for i in range(n)] for j in range(n)]
        if category == "invertmatrix":
            return self.inverse(mat("in"), n)
        if category in ("add", "subtract", "multiply", "divide"):
            a = mat("in1")
            other = g.port(node, "in2")
            if other[0] == "float":  # the FA forms: element by element
                b = s.components(other, 1)[0]
                f = {
                    "add": s.add,
                    "subtract": s.sub,
                    "multiply": s.mul,
                    "divide": s.div,
                }[category]
                return [[f(a[j][i], b) for i in range(n)] for j in range(n)]
            b = self.columns(other, n)
            if category == "add":
                return [
                    [s.add(a[j][i], b[j][i]) for i in range(n)]
                    for j in range(n)
                ]
            if category == "subtract":
                return [
                    [s.sub(a[j][i], b[j][i]) for i in range(n)]
                    for j in range(n)
                ]
            left = b if category == "multiply" else self.inverse(b, n)
            return self.product(
                left, a, n
            )  # multiply: in2 * in1; divide: inverse(in2) * in1
        if category in ("switch", "ifgreater", "ifgreatereq", "ifequal"):
            inputs = [
                p.getName()
                for p in node.getNodeDef().getActiveInputs()
                if p.getType().startswith("matrix")
            ]
            columns = {key: mat(key) for key in inputs}
            result = []
            for j in range(n):
                column = []
                for i in range(n):
                    element = g.node(
                        category,
                        "float",
                        {key: s.port(columns[key][j][i]) for key in inputs},
                    )
                    for p in node.getInputs():
                        if not p.getType().startswith("matrix"):
                            port = g.port(node, p.getName())
                            g.set(element, p.getName(), port)
                    column.append((element, "out"))
                result.append(column)
            return result
        # Any other matrix source (an interface input, a primvar): identity.
        return [[1.0 if i == j else 0.0 for i in range(n)] for j in range(n)]

    def product(self, a, b, n):
        """A * b (GLSL): column j of the result is a times column j of b."""
        s = self.s
        return [
            [
                s.total(s.mul(a[k][i], b[j][k]) for k in range(n))
                for i in range(n)
            ]
            for j in range(n)
        ]

    def apply(self, m, v, n):
        """M * v for a column vector v (GLSL)."""
        s = self.s
        return [
            s.total(s.mul(m[j][i], v[j]) for j in range(n)) for i in range(n)
        ]

    def determinant(self, m, n):
        s = self.s
        if n == 2:
            return s.sub(s.mul(m[0][0], m[1][1]), s.mul(m[1][0], m[0][1]))
        total = 0.0
        for j in range(n):
            minor = [[m[c][r] for r in range(1, n)] for c in range(n) if c != j]
            term = s.mul(m[j][0], self.determinant(minor, n - 1))
            total = s.add(total, term) if j % 2 == 0 else s.sub(total, term)
        return total

    def inverse(self, m, n):
        """The adjugate over the determinant."""
        s = self.s
        det = self.determinant(m, n)
        result = [[0.0] * n for _ in range(n)]
        for j in range(n):
            for i in range(n):
                minor = [
                    [m[c][r] for r in range(n) if r != j]
                    for c in range(n)
                    if c != i
                ]
                cofactor = self.determinant(minor, n - 1)
                if (i + j) % 2:
                    cofactor = s.sub(0.0, cofactor)
                result[j][i] = s.div(
                    cofactor, det
                )  # inverse[i][j] = cofactor(j, i) / det, as columns
        return result


def lower_matrices(g):
    """Nodes reading matrices (transformmatrix, determinant, extract) are evaluated element by element; then no
    matrix node is left.
    """
    m = Matrices(g)
    s = m.s
    for node in list(g.element.getNodes()):
        category = node.getCategory()
        inputs = {
            p.getName(): p
            for p in (
                node.getNodeDef().getActiveInputs() if node.getNodeDef() else []
            )
        }
        matrix_inputs = [
            k for k, p in inputs.items() if p.getType().startswith("matrix")
        ]
        if not matrix_inputs or node.getType().startswith("matrix"):
            continue
        key = matrix_inputs[0]
        n = matrix_size(inputs[key].getType())
        columns = m.columns(g.port(node, key), n)
        if category == "transformmatrix":
            type_ = node.getType()
            size = SIZE[type_]
            v = s.components(g.port(node, "in"), size)
            if size < n:
                v = v + [1.0]
            result = m.apply(columns, v + [0.0] * (n - len(v)), n)[:size]
            port = s.vector(type_, result)
        elif category == "determinant":
            port = s.vector("float", [m.determinant(columns, n)])
        elif category == "extract":
            index = int(float(g.port(node, "index")[1]))
            port = s.vector(node.getType(), columns[index])
        else:
            raise ValueError(f"MaterialX {category} of a matrix is not lowered")
        if len(port) == 2:
            replacement = g.node("constant", port[0], {"value": port})
            g.replace(node, replacement)
        else:
            g.replace(node, port[1], port[2])
    for node in list(g.element.getNodes()):
        if node.getType().startswith("matrix"):
            g.element.removeNode(node.getName())


# --- legacy and unsupported nodes ------------------------------------------------------------------------------------


def lower_nodes(g, fps, notes):
    s = Scalars(g)
    for node in list(g.element.getNodes()):
        category = node.getCategory()
        type_ = node.getType()
        if category == "frame":
            time = g.node(
                "time", "float", {"fps": ("float", value_string(fps))}
            )
            new = g.node(
                "multiply",
                "float",
                {
                    "in1": ("float", time, "out"),
                    "in2": ("float", value_string(fps)),
                },
            )
            g.replace(node, new)
        elif category == "ambientocclusion":
            notes.append(
                "ambientocclusion: 1 (UE shades occlusion itself: Lumen, ambient occlusion)"
            )
            g.replace(
                node, g.node("constant", "float", {"value": ("float", "1")})
            )
        elif category == "curveadjust":
            notes.append("curveadjust: linear between its knots")
            replace_port(g, node, curve(g, s, node, type_))
        elif category == "generalized_schlick_edf":
            replace_port(g, node, schlick_edf(g, s, node))
        elif category == "thin_surface":
            inputs = {
                "bsdf": g.port(node, "front_bsdf"),
                "edf": g.port(node, "front_edf"),
                "opacity": g.port(node, "opacity"),
            }
            new = g.node(
                "surface",
                "surfaceshader",
                {
                    k: v
                    for k, v in inputs.items()
                    if v and (len(v) == 3 or v[1])
                },
            )
            g.replace(node, new)
        elif category in ("disney_brdf_2012", "disney_bsdf_2015"):
            replace_port(g, node, disney(g, node))
        elif category == "multiply" and type_ in (
            "surfaceshader",
            "volumeshader",
            "displacementshader",
        ):
            # A shader scaled by a weight (MaterialX 1.39.5): mixed over nothing by that weight (a colour's mean).
            weight = g.port(node, "in2")
            if weight[0] != "float":
                channels = s.components(weight, SIZE[weight[0]])
                weight = s.port(s.div(s.total(channels), float(len(channels))))
            mix = g.node(
                "mix",
                type_,
                {
                    "fg": g.port(node, "in1"),
                    "bg": (type_, nothing(g, type_), "out"),
                    "mix": weight,
                },
            )
            g.replace(node, mix)
            notes.append(
                "multiply of a shader: a mix over no shader, by the weight's mean"
            )
    for node in list(g.element.getNodes()):
        if (
            node.getCategory() == "arrayappend"
        ):  # folded into the curveadjust knots that read it
            g.element.removeNode(node.getName())
    for node in list(g.element.getNodes()):
        if (
            node.getCategory() == "thin_film_bsdf"
        ):  # a film with nothing under it reflects nothing
            g.replace(
                node,
                g.node("dielectric_bsdf", "BSDF", {"weight": ("float", "0")}),
            )


def merge_displacements(g):
    """A mix of two displacement nodes becomes one displacement node of the mixed displacement (exact: displacement is
    linear in its value). UE's importer maps only a displacement node onto the material's displacement; a mix of them
    fails the material ("Material information is invalid"). Shader multiplies are mixes by then (lower_nodes).
    """
    changed = True
    while changed:
        changed = False
        for node in list(g.element.getNodes()):
            if (
                node.getCategory() != "mix"
                or node.getType() != "displacementshader"
            ):
                continue
            sides = [g.port(node, key) for key in ("fg", "bg")]
            if any(
                len(side) != 3 or side[1].getCategory() != "displacement"
                for side in sides
            ):
                continue
            values = [g.port(side[1], "displacement") for side in sides]
            kind = values[0][0]
            if values[1][0] != kind:
                continue  # along the normal and a vector: no single displacement node holds both
            scales = [g.port(side[1], "scale") for side in sides]
            weight = g.port(node, "mix")
            if all(len(port) == 2 for port in values + scales + [weight]):
                # All values: the displacement folded (UE fails a chain of constants that folds to 0).
                w = floats(weight[1])[0]
                fg, bg = (
                    [v * floats(scale[1])[0] for v in floats(value[1])]
                    for value, scale in zip(values, scales, strict=True)
                )
                displacement = (
                    kind,
                    [
                        f * w + b_ * (1 - w)
                        for f, b_ in zip(fg, bg, strict=True)
                    ],
                )
            else:
                scaled = [
                    (
                        kind,
                        g.node("multiply", kind, {"in1": value, "in2": scale}),
                        "out",
                    )
                    for value, scale in zip(values, scales, strict=True)
                ]
                mixed = g.node(
                    "mix",
                    kind,
                    {"fg": scaled[0], "bg": scaled[1], "mix": weight},
                )
                displacement = (kind, mixed, "out")
            merged = g.node(
                "displacement",
                "displacementshader",
                {"displacement": displacement, "scale": ("float", "1")},
                nodedef=f"ND_displacement_{kind}",
            )
            g.replace(node, merged)
            g.element.removeNode(node.getName())
            changed = True


SHADER_TYPES = (
    "surfaceshader",
    "displacementshader",
    "volumeshader",
    "BSDF",
    "EDF",
    "VDF",
)


def remove_unused(g):
    """Removes shader and closure nodes nothing reads (left by the rewrites): UE's importer fails a material whose
    document holds an unread displacement node beside other materials ("Material information is invalid").
    """
    while True:
        read = {
            port.getNodeName()
            for element in g.element.getDocument().traverseTree()
            if element.isA(mx.Input) or element.isA(mx.Output)
            for port in [element]
            if port.getNodeName()
        }
        unused = [
            n
            for n in g.element.getNodes()
            if n.getType() in SHADER_TYPES and n.getName() not in read
        ]
        if not unused:
            return
        for node in unused:
            g.element.removeNode(node.getName())


def nothing(g, type_):
    """A shader that adds nothing: an unlit transparent black surface, no displacement, a volume absorbing nothing."""
    if type_ == "surfaceshader":
        return g.node(
            "surface_unlit",
            type_,
            {"emission": ("float", "0"), "opacity": ("float", "0")},
        )
    if type_ == "displacementshader":
        return g.node(
            "displacement",
            type_,
            {"displacement": ("float", "0")},
            nodedef="ND_displacement_float",
        )
    vdf = g.node(
        "absorption_vdf", "VDF", {"absorption": ("vector3", "0, 0, 0")}
    )
    return g.node("volume", type_, {"vdf": ("VDF", vdf, "out")})


# Inputs UE's importer has no pin for (its Surface, Standard Surface, BSDF and hex-tiling functions): present, they fail
# the import with "Invalid input ... for material expression node", and UE ignores their value anyway.
UE_NO_PIN = {
    ("surface", "thin_walled"),
    ("hextiledimage", "default"),
    ("hextilednormalmap", "default"),
    ("standard_surface", "transmission_scatter_anisotropy"),
    ("conductor_bsdf", "thinfilm_ior"),
    ("conductor_bsdf", "thinfilm_thickness"),
    ("dielectric_bsdf", "thinfilm_ior"),
    ("dielectric_bsdf", "thinfilm_thickness"),
    ("generalized_schlick_bsdf", "color82"),
    ("generalized_schlick_bsdf", "thinfilm_ior"),
    ("generalized_schlick_bsdf", "thinfilm_thickness"),
}


def fold_base(g):
    """A Standard Surface mixed with another shader keeps no `base` input: UE's importer then reads every input named
    base as Substrate's vertical-layering Bottom (MaterialXManager.cpp, FMaterialXManager's constructor) and fails it.
    Standard Surface only uses base times base_color (diffuse and metal colour), so the weight moves into the colour.
    """
    mixed = {
        port.getNodeName()
        for node in g.element.getNodes()
        if node.getType() == "surfaceshader"
        and node.getCategory() in ("mix", "multiply")
        for port in node.getInputs()
        if port.getNodeName()
    }
    for node in g.element.getNodes():
        if (
            node.getCategory() != "standard_surface"
            or node.getName() not in mixed
        ):
            continue
        weight, color = g.port(node, "base"), g.port(node, "base_color")
        if len(weight) == 2 and len(color) == 2:
            w = floats(weight[1])[0]
            g.set(
                node,
                "base_color",
                ("color3", [w * c for c in floats(color[1])]),
            )
        else:
            product = g.node(
                "multiply", "color3", {"in1": color, "in2": weight}
            )
            g.set(node, "base_color", ("color3", product, "out"))
        node.removeInput("base")


def drop_pinless(g, notes):
    """Removes the inputs UE has no pin for (UE_NO_PIN), noting the ones set away from their default."""
    for node in g.element.getNodes():
        for port in list(node.getInputs()):
            key = (node.getCategory(), port.getName())
            if key not in UE_NO_PIN:
                continue
            declared = node.getNodeDef() and node.getNodeDef().getActiveInput(
                key[1]
            )
            if port.getConnectedNode() is not None or (
                declared and port.getValueString() != declared.getValueString()
            ):
                notes.append(
                    f"{key[0]} {key[1]} is ignored (UE's importer has no input for it)"
                )
            node.removeInput(key[1])


def name_definitions(g):
    """Every node names its definition: UE's importer crashes on a BSDF multiply that leaves it to be inferred (as
    MaterialX's flattenSubgraphs writes Lama's insides), and imports it once named.
    """
    for node in g.element.getNodes():
        if not node.hasNodeDefString() and (definition := node.getNodeDef()):
            node.setNodeDefString(definition.getName())


def author_defaults(g):
    """Every input a node leaves at its default, written with that default: UE's importer reads some inputs without
    checking they exist (normalmap's in: ConnectNormalMapInputToOutput, MaterialXSurfaceShaderAbstract.cpp).
    """
    for node in g.element.getNodes():
        definition = node.getNodeDef()
        if definition is None:
            continue
        for declared in definition.getActiveInputs():
            name = declared.getName()
            if (
                node.getInput(name) is not None
                or declared.hasDefaultGeomPropString()
                or (node.getCategory(), name) in UE_NO_PIN
            ):
                continue
            if (
                declared.getType() in ("filename", "string")
                or not declared.getValueString()
            ):
                continue
            node.addInput(name, declared.getType()).setValueString(
                declared.getValueString()
            )


def replace_port(g, node, port):
    if port is None:
        g.element.removeNode(node.getName())
    elif len(port) == 2:
        g.replace(node, g.node("constant", port[0], {"value": port}))
    else:
        g.replace(node, port[1], port[2])


def array_values(g, port):
    """An array port's values: its value, or the concatenation an arrayappend chain of constants makes."""
    if len(port) == 2:
        return floats(port[1])
    node = port[1]
    if node.getCategory() != "arrayappend":
        raise ValueError(
            f"MaterialX array from {node.getCategory()} is not constant"
        )
    return array_values(g, g.port(node, "in1")) + array_values(
        g, g.port(node, "in2")
    )


def curve(g, s, node, type_):
    """Linear interpolation between curveadjust's knots ((x, y) pairs), per channel; flat beyond the ends."""
    knots = array_values(g, g.port(node, "knots"))
    points = sorted(zip(knots[0::2], knots[1::2], strict=False))
    source = g.port(node, "in")
    channels = s.components(source, SIZE[type_])
    if not points:
        return source
    result = []
    for x in channels:
        value = points[0][1]
        for (x0, y0), (x1, y1) in zip(points, points[1:], strict=False):
            t = s.div(s.sub(x, x0), x1 - x0) if x1 != x0 else 0.0
            t = (
                s.op("clamp", t, fold=lambda v: min(max(v, 0.0), 1.0))
                if not isinstance(t, float)
                else min(max(t, 0.0), 1.0)
            )
            segment = s.add(y0, s.mul(t, y1 - y0))
            if isinstance(x, float):
                value = segment if x >= x0 else value
            else:
                # MaterialX 1.39: value1 >= value2 ? in1 : in2
                pick = g.node(
                    "ifgreatereq",
                    "float",
                    {
                        "value1": s.port(x),
                        "value2": ("float", value_string(x0)),
                        "in1": s.port(segment),
                        "in2": s.port(value),
                    },
                )
                value = (pick, "out")
        result.append(value)
    return s.vector(type_, result)


def schlick_edf(g, s, node):
    """Base's emission weighted by mix(color0, color90, (1 - N.V)^exponent), as a uniform_edf (UE's)."""
    base = g.port(node, "base")
    color = ("color3", "1, 1, 1")
    if base and len(base) == 3 and base[1].getCategory() == "uniform_edf":
        color = g.port(base[1], "color")
    normal = g.node("normal", "vector3", {})
    view = g.node("viewdirection", "vector3", {})
    cosine = g.node(
        "dotproduct",
        "float",
        {"in1": ("vector3", normal, "out"), "in2": ("vector3", view, "out")},
    )
    facing = g.node("absval", "float", {"in": ("float", cosine, "out")})
    edge = g.node(
        "subtract",
        "float",
        {"in1": ("float", "1"), "in2": ("float", facing, "out")},
    )
    weight = g.node(
        "power",
        "float",
        {"in1": ("float", edge, "out"), "in2": g.port(node, "exponent")},
    )
    tint = g.node(
        "mix",
        "color3",
        {
            "bg": g.port(node, "color0"),
            "fg": g.port(node, "color90"),
            "mix": ("float", weight, "out"),
        },
    )
    emitted = g.node(
        "multiply", "color3", {"in1": color, "in2": ("color3", tint, "out")}
    )
    edf = g.node("uniform_edf", "EDF", {"color": ("color3", emitted, "out")})
    return ("EDF", edf, "out")


DISNEY = {  # MaterialX 1.38's Disney surfaces to Houdini's disney_principled (bxdf/disney_principled.mtlx)
    "baseColor": "baseColor",
    "metallic": "metallic",
    "roughness": "roughness",
    "anisotropic": "anisotropic",
    "specular": "specular",
    "specularTint": "specularTint",
    "sheen": "sheen",
    "sheenTint": "sheenTint",
    "clearcoat": "clearcoat",
    "clearcoatGloss": "clearcoatGloss",
    "specTrans": "specTrans",
    "ior": "ior",
    "subsurface": "subsurface",
}


def disney(g, node):
    inputs = {
        DISNEY[k]: g.port(node, k)
        for k in DISNEY
        if node.getInput(k) is not None
    }
    new = g.node("disney_principled", "surfaceshader", inputs)
    return ("surfaceshader", new, "out")


# --- flakes ----------------------------------------------------------------------------------------------------------

FLAKE_DEFAULTS = (("size", 0.01), ("roughness", 0.1), ("coverage", 0.5))
FLAKE_OUTPUTS = (
    ("flakenormal", "vector3", "0, 0, 1"),
    ("presence", "float", "0"),
    ("rand", "float", "0"),
    ("id", "integer", "0"),
)


def flake_code(two_d, values):
    """A flake's marker code: hdueflake_<mode>_<size>_<roughness>_<coverage>, mode 0 for texture coordinates (flake2d)
    and 1 for object position (flake3d), the values in millionths.
    """
    numbers = [values[k] for k, _ in FLAKE_DEFAULTS]
    return f"hdueflake_{0 if two_d else 1}_" + "_".join(
        str(int(round(v * 1e6))) for v in numbers
    )


def lower_flakes(doc, exported, nodes):
    """A flake whose inputs are values (not other nodes) becomes markers, one constant per output, named after its
    values (flake_code): hdUE's MaterialX import (HdUEImportMaterialXCommandlet ReplaceFlakeMarkers) puts MaterialX's
    own flake code in their place (unreal/HdUEBackend/Shaders/MaterialXFlake.ush). The values are baked in: a material
    whose flake values change no longer matches its prepared graph's signature and is prepared again. Other flakes
    keep the node graph of standard nodes (hdue_defs.mtlx).
    """
    g = Graph(doc, None)
    for path, node in nodes.items():
        if node["id"] not in ("ND_flake2d", "ND_flake3d"):
            continue
        inputs = node["inputs"]
        if any("node" in item for item in inputs.values()) or set(inputs) - {
            k for k, _ in FLAKE_DEFAULTS
        }:
            continue  # driven by other nodes, or its own position or frame: the node graph
        values = {
            k: float(inputs[k]["value"]) if k in inputs else default
            for k, default in FLAKE_DEFAULTS
        }
        code = flake_code(node["id"] == "ND_flake2d", values)
        old = exported[path]
        for output, type_, value in FLAKE_OUTPUTS:
            name = g.element.createValidChildName(
                f"{old.getName()}_{code}_{output}"
            )
            marker_node = g.element.addNode("constant", name, type_)
            marker_node.addInput("value", type_).setValueString(value)
            for consumer in list(g.element.getNodes()):
                for port in consumer.getInputs():
                    if (
                        port.getNodeName() == old.getName()
                        and (port.getOutputString() or "out") == output
                    ):
                        port.setNodeName(name)
                        port.removeAttribute("output")
        g.element.removeNode(old.getName())


# --- primvars --------------------------------------------------------------------------------------------------------

READERS = ("ND_geompropvalue_", "ND_UsdPrimvarReader_")


def primvar_name(node_id, inputs):
    """The primvar a reader reads (geompropvalue's geomprop, UsdPrimvarReader's varname), or None."""
    for reader in READERS:
        if node_id.startswith(reader):
            item = inputs.get(
                "geomprop" if reader == READERS[0] else "varname", {}
            )
            return str(item.get("value") or "")
    return None


def mesh_primvars(nodes):
    """The UV sets and the vertex colour hdUE uploads for a material's graph ({path: {id, inputs}}), as the delegate
    chooses them (src/hydra/materials.cpp uvPrimvar, colorPrimvar): 2D readers in path order, the first other one.
    """
    from pxr import Sdf

    uv, color = [], ""
    for path in sorted(nodes, key=Sdf.Path):
        node_id = nodes[path]["id"]
        name = primvar_name(node_id, nodes[path]["inputs"])
        if not name:
            continue
        kind = node_id.split("_")[-1]
        if kind == "vector2":
            if name not in uv:
                uv.append(name)
        elif kind not in ("string", "filename") and not color:
            color = name
    return uv, color


def lower_primvars(doc, exported, nodes, notes):
    """UE's importer reads a primvar only as UV set 0 (geompropvalue of st): a 2D reader becomes texcoord of the UV set
    hdUE uploads it to, the colour reader geomcolor (the vertex colour), any other its fallback.
    """
    uv, color = mesh_primvars(nodes)
    g = Graph(doc, None)
    for path, node in nodes.items():
        name = primvar_name(node["id"], node["inputs"])
        if name is None or node["id"].endswith(("_string", "_filename")):
            continue
        old = exported[path]
        type_ = old.getType()
        if type_ == "vector2" and name in uv:
            new = g.node(
                "texcoord",
                "vector2",
                {"index": ("integer", str(uv.index(name)))},
            )
            g.replace(old, new)
            exported[path] = new
            continue
        if name == color and type_ not in ("vector2",):
            size = {"color4": 4, "vector4": 4}.get(
                type_, 3 if type_ in ("color3", "vector3") else 1
            )
            source = g.node(
                "geomcolor", {1: "float", 3: "color3", 4: "color4"}[size], {}
            )
            port = (
                "float" if size == 1 else {3: "color3", 4: "color4"}[size],
                source,
                "out",
            )
            if type_ in ("vector3", "vector4"):
                port = (type_, g.node("convert", type_, {"in": port}), "out")
            g.replace(old, port[1], port[2])
            exported[path] = port[1]
            continue
        notes.append(
            f"primvar {name}: its fallback (UE meshes carry UV sets and one vertex colour)"
        )
        fallback = g.port(
            old, "default" if "geompropvalue" in node["id"] else "fallback"
        )
        replace_port(
            g,
            old,
            (
                fallback
                if fallback and (len(fallback) == 3 or fallback[1])
                else (type_, "0")
            ),
        )


# Houdini's MaterialX 1.38 nodes MaterialX's own 1.38 -> 1.39 upgrade converts (Document.upgradeVersion), by their
# 1.38 category: swizzle, atan2, normalmap, thin_film_bsdf layered over a BSDF.
UPGRADED = {
    "atan2_1_38": "atan2",
    "normalmap_1_38": "normalmap",
    "swizzle": "swizzle",
    "thin_film_bsdf": "thin_film_bsdf",
}


def upgrade_legacy(doc):
    """Runs MaterialX's 1.38 -> 1.39 upgrade on doc's own content (not the libraries it imports) when it holds
    Houdini's 1.38 nodes.
    """
    own = [e for e in doc.getChildren() if not e.hasSourceUri()]
    nodes = [
        n
        for e in own
        for n in (
            [e]
            if e.isA(mx.Node)
            else e.getChildren() if e.isA(mx.NodeGraph) else []
        )
    ]
    legacy = [
        n for n in nodes if n.isA(mx.Node) and n.getCategory() in UPGRADED
    ]
    if not legacy:
        return
    for node in legacy:
        # The upgrade reads the inputs a node holds: the defaults of Houdini's definition first (swizzle's channels).
        definition = node.getNodeDef()
        for declared in definition.getActiveInputs() if definition else []:
            if (
                node.getInput(declared.getName()) is None
                and declared.getValueString()
            ):
                node.addInput(
                    declared.getName(), declared.getType()
                ).setValueString(declared.getValueString())
        node.setCategory(UPGRADED[node.getCategory()])
        node.removeAttribute(
            "nodedef"
        )  # Houdini's legacy definition; the upgrade reads the 1.38 node itself
    content = mx.createDocument()
    for element in own:
        content.addChildOfCategory(
            element.getCategory(), element.getName()
        ).copyContentFrom(element)
        doc.removeChild(element.getName())
    content.setVersionString("1.38")
    content.upgradeVersion()
    for element in content.getChildren():
        doc.addChildOfCategory(
            element.getCategory(), element.getName()
        ).copyContentFrom(element)


def lower(doc, library, fps=24.0):
    """Rewrites doc's nodes for UE's MaterialX importer (see the module's docstring); library holds the standard
    definitions UE reads. Returns notes on the approximations made.
    """
    notes = []
    upgrade_legacy(doc)
    elements = [doc] + [
        ng for ng in doc.getNodeGraphs() if not ng.hasSourceUri()
    ]
    for element in elements:
        g = Graph(element, library)
        lower_nodes(g, fps, notes)
        merge_displacements(g)
        lower_integers(g)
        lower_matrices(g)
        author_defaults(g)
        fold_base(g)
        drop_pinless(g, notes)
        remove_unused(g)
        name_definitions(g)
    return sorted(set(notes))
