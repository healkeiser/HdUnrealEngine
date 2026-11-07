"""Writes samples/conformance/materialx/lowered_nodes.usda: one emissive quad per MaterialX node UE's importer cannot
translate as is (materialx_lowering.py rewrites them), each drawing a value computed by hand, so run_conformance.py
checks the rewritten graphs render what MaterialX defines.

    python scripts/generate_materialx_conformance.py
"""

import json
import math
from pathlib import Path

OUT = (
    Path(__file__).resolve().parents[1]
    / "samples/conformance/materialx/lowered_nodes.usda"
)


def color(*values):
    return "(" + ", ".join(f"{v:g}" for v in values) + ")"


# (name, doc, shaders {name: (info:id, {input: "type value" | "type.connect = <name.outputs:out>"})}, output shader,
# expected rgb). The output shader's colour goes to an emissive Standard Surface.
CASES = [
    (
        "MatrixTransform",
        "transformmatrix of creatematrix's columns (0,1,0), (1,0,0), (0,0,1): M * (0.2, 0.6, 0.9)",
        {
            "m": (
                "ND_creatematrix_vector3_matrix33",
                {
                    "in1": "float3 (0, 1, 0)",
                    "in2": "float3 (1, 0, 0)",
                    "in3": "float3 (0, 0, 1)",
                },
            ),
            "t": (
                "ND_transformmatrix_vector3",
                {"in": "float3 (0.2, 0.6, 0.9)", "mat": "matrix3d <m>"},
            ),
            "c": ("ND_convert_vector3_color3", {"in": "float3 <t>"}),
        },
        "c",
        (0.6, 0.2, 0.9),
    ),
    (
        "MatrixInverse",
        "inverse of diag(2, 4, 5) applied to (0.4, 0.8, 1)",
        {
            "inv": (
                "ND_invertmatrix_matrix33",
                {"in": "matrix3d ( (2, 0, 0), (0, 4, 0), (0, 0, 5) )"},
            ),
            "t": (
                "ND_transformmatrix_vector3",
                {"in": "float3 (0.4, 0.8, 1)", "mat": "matrix3d <inv>"},
            ),
            "c": ("ND_convert_vector3_color3", {"in": "float3 <t>"}),
        },
        "c",
        (0.2, 0.2, 0.2),
    ),
    (
        "MatrixDeterminant",
        "determinant of diag(0.5, 0.6, 1) times transpose of itself: 0.09",
        {
            "tr": (
                "ND_transpose_matrix33",
                {"in": "matrix3d ( (0.5, 0, 0), (0, 0.6, 0), (0, 0, 1) )"},
            ),
            "mul": (
                "ND_multiply_matrix33",
                {
                    "in1": "matrix3d <tr>",
                    "in2": "matrix3d ( (0.5, 0, 0), (0, 0.6, 0), (0, 0, 1) )",
                },
            ),
            "d": ("ND_determinant_matrix33", {"in": "matrix3d <mul>"}),
            "c": ("ND_convert_float_color3", {"in": "float <d>"}),
        },
        "c",
        (0.09, 0.09, 0.09),
    ),
    (
        "Integers",
        "(2 + 3) as a float, times 0.1",
        {
            "add": ("ND_add_integer", {"in1": "int 2", "in2": "int 3"}),
            "f": ("ND_convert_integer_float", {"in": "int <add>"}),
            "m": (
                "ND_multiply_float",
                {"in1": "float <f>", "in2": "float 0.1"},
            ),
            "c": ("ND_convert_float_color3", {"in": "float <m>"}),
        },
        "c",
        (0.5, 0.5, 0.5),
    ),
    (
        "Booleans",
        "(true and false, true or false, not false) as floats",
        {
            "a": ("ND_logical_and", {"in1": "bool 1", "in2": "bool 0"}),
            "o": ("ND_logical_or", {"in1": "bool 1", "in2": "bool 0"}),
            "n": ("ND_logical_not", {"in": "bool 0"}),
            "fa": ("ND_convert_boolean_float", {"in": "bool <a>"}),
            "fo": ("ND_convert_boolean_float", {"in": "bool <o>"}),
            "fn": ("ND_convert_boolean_float", {"in": "bool <n>"}),
            "c": (
                "ND_combine3_color3",
                {"in1": "float <fa>", "in2": "float <fo>", "in3": "float <fn>"},
            ),
        },
        "c",
        (0.0, 1.0, 1.0),
    ),
    (
        "Swizzle",
        "Houdini's MaterialX 1.38 swizzle bgr of (0.1, 0.5, 0.9)",
        {
            "s": (
                "ND_swizzle_color3_color3",
                {"in": "color3f (0.1, 0.5, 0.9)", "channels": 'string "bgr"'},
            )
        },
        "s",
        (0.9, 0.5, 0.1),
    ),
    (
        "CurveAdjust",
        "curveadjust of 0.25 through the knots (0, 0), (0.5, 0.8), (1, 1): linear, 0.4",
        {
            "k": (
                "ND_curveadjust_float",
                {
                    "in": "float 0.25",
                    "knots": "float2[] [(0, 0), (0.5, 0.8), (1, 1)]",
                },
            ),
            "c": ("ND_convert_float_color3", {"in": "float <k>"}),
        },
        "c",
        (0.4, 0.4, 0.4),
    ),
    (
        "Atan2",
        "Houdini's MaterialX 1.38 atan2(1, 1) = pi / 4",
        {
            "a": ("ND_atan2_float_1_38", {"in1": "float 1", "in2": "float 1"}),
            "c": ("ND_convert_float_color3", {"in": "float <a>"}),
        },
        "c",
        (math.pi / 4,) * 3,
    ),
    (
        "Frame",
        "frame / 10 at frame 1 (24 fps)",
        {
            "f": ("ND_frame_float", {}),
            "m": (
                "ND_multiply_float",
                {"in1": "float <f>", "in2": "float 0.1"},
            ),
            "c": ("ND_convert_float_color3", {"in": "float <m>"}),
        },
        "c",
        (0.1, 0.1, 0.1),
    ),
    (
        "AmbientOcclusion",
        "ambientocclusion: 1 (UE occludes the lighting itself)",
        {
            "a": ("ND_ambientocclusion_float", {}),
            "m": (
                "ND_multiply_float",
                {"in1": "float <a>", "in2": "float 0.7"},
            ),
            "c": ("ND_convert_float_color3", {"in": "float <m>"}),
        },
        "c",
        (0.7, 0.7, 0.7),
    ),
    (
        "Flake3D",
        "flake3d presence, flakes of 5 cm at coverage 0.5 (hdUE's import runs MaterialX's flake code): the mean of "
        "MaterialX's mx_flake over the region, 0.25 to 0.28 as the plane's orientation varies",
        {
            "f": (
                "ND_flake3d",
                {"size": "float 0.05", "coverage": "float 0.5"},
                {"presence": "float"},
            ),
            "c": ("ND_convert_float_color3", {"in": "float <f.presence>"}),
        },
        "c",
        (0.27, 0.27, 0.27),
    ),
]


def value_line(name, spec):
    kind, _, value = spec.partition(" ")
    if value.startswith("<"):
        node, _, output = value.strip("<>").partition(".")
        return f"        {kind} inputs:{name}.connect = </Looks/{{material}}/{node}.outputs:{output or 'out'}>"
    return f"        {kind} inputs:{name} = {value}"


def material(name, shaders, output):
    lines = [
        f'    def Material "{name}"',
        "    {",
        f"        token outputs:mtlx:surface.connect = </Looks/{name}/surface.outputs:out>",
    ]
    for shader, (identity, inputs, *outputs) in shaders.items():
        lines += [
            f'        def Shader "{shader}"',
            "        {",
            f'            uniform token info:id = "{identity}"',
        ]
        for key, spec in inputs.items():
            lines.append("    " + value_line(key, spec).format(material=name))
        out_type = (
            "color3f"
            if identity.endswith(("color3", "_color3_color3"))
            or identity.startswith("ND_combine3_color3")
            else "float"
        )
        declared = outputs[0] if outputs else {"out": out_type}
        lines += [
            f"            {kind} outputs:{name}"
            for name, kind in declared.items()
        ]
        lines.append("        }")
    lines += [
        '        def Shader "surface"',
        "        {",
        '            uniform token info:id = "ND_standard_surface_surfaceshader"',
        "            float inputs:base = 0",
        "            float inputs:specular = 0",
        "            float inputs:emission = 1",
        f"            color3f inputs:emission_color.connect = </Looks/{name}/{output}.outputs:out>",
        "            token outputs:out",
        "        }",
        "    }",
    ]
    return lines


def main():
    # No "not translated" log check: these nodes are UE's import's to render, and hdUE warns while the import runs.
    checks = []
    looks, quads = [], []
    for index, (name, doc, shaders, output, expected) in enumerate(CASES):
        column, row = index % 4, index // 4
        x, y = -3.75 + 2.5 * column, 3.75 - 2.5 * row
        looks += [f"    # {doc}"] + material(name, shaders, output)
        quads += [
            f'def Mesh "{name}Quad" (prepend apiSchemas = ["MaterialBindingAPI"])',
            "{",
            "    int[] faceVertexCounts = [4]",
            "    int[] faceVertexIndices = [0, 1, 2, 3]",
            f"    point3f[] points = [({x - 1}, {y - 1}, 0), ({x + 1}, {y - 1}, 0), ({x + 1}, {y + 1}, 0), ({x - 1}, {y + 1}, 0)]",
            '    uniform token subdivisionScheme = "none"',
            f"    rel material:binding = </Looks/{name}>",
            "}",
        ]
        checks.append(
            {
                "name": f"{name}: {doc}",
                "color": {"region": [x - 0.5, y - 0.5, x + 0.5, y + 0.5]},
                "rgb": [round(v, 4) for v in expected],
                # The flake mean is statistical: its pattern depends on the plane's orientation in UE's space.
                "tol": 0.05 if name == "Flake3D" else 0.03,
            }
        )
    spec = {
        "schema": "MaterialX",
        "feature": "nodes UE's importer cannot translate as is",
        "frames": 1,
        "checks": checks,
    }
    text = "\n".join(
        [
            "#usda 1.0",
            "(",
            '    doc = """Generated by scripts/generate_materialx_conformance.py: the MaterialX nodes hdUE rewrites for UE\'s',
            'importer (scripts/materialx_lowering.py), each on an emissive quad showing a value computed by hand."""',
            "    customLayerData = {",
            '        string conformance = """'
            + json.dumps(spec, indent=1)
            + '"""',
            "    }",
            "    metersPerUnit = 1",
            '    upAxis = "Y"',
            "    startTimeCode = 1",
            "    endTimeCode = 1",
            "    timeCodesPerSecond = 24",
            '    renderSettingsPrimPath = "/Render/Settings"',
            "    subLayers = [@../common/stage.usda@]",
            ")",
            "",
            'def Scope "Looks"',
            "{",
            *looks,
            "}",
            "",
            *quads,
            "",
        ]
    )
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(OUT)


if __name__ == "__main__":
    main()
