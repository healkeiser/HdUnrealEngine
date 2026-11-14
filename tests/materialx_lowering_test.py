"""materialx_lowering.py: one small document per pass, checked after the rewrite UE's MaterialX importer reads."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import MaterialX as mx
from export_usd_materialx import definitions
from materialx_lowering import (
    Graph,
    lower,
    lower_flakes,
    lower_primvars,
    merge_displacements,
    upgrade_legacy,
)

LIBRARY, _ = definitions()


def document():
    """A document over the libraries export_usd_materialx.write gives lower()."""
    doc = mx.createDocument()
    doc.importLibrary(LIBRARY)
    return doc


def output(doc, name, type_, node):
    result = doc.addOutput(name, type_)
    result.setConnectedNode(node)
    return result


def source(doc, name):
    """The node the document's output name reads."""
    return doc.getOutput(name).getConnectedNode()


def values(node, key="value"):
    return [float(v) for v in node.getInput(key).getValueString().split(",")]


class Lowering(unittest.TestCase):
    def test_integers_become_floats(self):
        doc = document()
        total = doc.addNode("add", "total", "integer")
        total.addInput("in1", "integer").setValueString("2")
        total.addInput("in2", "integer").setValueString("3")
        convert = doc.addNode("convert", "to_float", "float")
        convert.addInput("in", "integer").setConnectedNode(total)
        output(doc, "result", "float", convert)
        lower(doc, LIBRARY)
        self.assertFalse(
            [n for n in doc.getNodes() if n.getType() in ("integer", "boolean")]
        )
        # The add is retyped to float, not folded into a constant; the convert goes.
        node = source(doc, "result")
        self.assertEqual(node.getCategory(), "add")
        self.assertEqual(sum(values(node, k)[0] for k in ("in1", "in2")), 5.0)

    def test_matrices_fold(self):
        doc = document()
        inverse = doc.addNode("invertmatrix", "inverse", "matrix33")
        inverse.addInput("in", "matrix33").setValueString("2,0,0, 0,4,0, 0,0,5")
        transform = doc.addNode("transformmatrix", "transform", "vector3")
        transform.addInput("in", "vector3").setValueString("0.4, 0.8, 1")
        transform.addInput("mat", "matrix33").setConnectedNode(inverse)
        output(doc, "result", "vector3", transform)
        determinant = doc.addNode("determinant", "determinant", "float")
        determinant.addInput("in", "matrix33").setValueString(
            "0.3,0,0, 0,0.3,0, 0,0,1"
        )
        output(doc, "det", "float", determinant)
        lower(doc, LIBRARY)
        self.assertFalse(
            [n for n in doc.getNodes() if n.getType().startswith("matrix")]
        )
        for got, want in zip(
            values(source(doc, "result")), (0.2, 0.2, 0.2), strict=True
        ):
            self.assertAlmostEqual(got, want, places=6)
        self.assertAlmostEqual(values(source(doc, "det"))[0], 0.09, places=6)

    def test_divide_by_zero_folds_to_zero(self):
        # Scalars.div folds x / 0 to 0, where a GPU gives inf: the inverse of a singular matrix is all zeros. A change
        # to this must be deliberate (and this test updated with it).
        doc = document()
        inverse = doc.addNode("invertmatrix", "inverse", "matrix33")
        inverse.addInput("in", "matrix33").setValueString("0,0,0, 0,0,0, 0,0,0")
        transform = doc.addNode("transformmatrix", "transform", "vector3")
        transform.addInput("in", "vector3").setValueString("1, 1, 1")
        transform.addInput("mat", "matrix33").setConnectedNode(inverse)
        output(doc, "result", "vector3", transform)
        lower(doc, LIBRARY)
        self.assertEqual(values(source(doc, "result")), [0.0, 0.0, 0.0])

    def test_displacement_mix_merges(self):
        doc = document()
        sides = []
        for name, value, scale in (("fg", "2", "0.5"), ("bg", "4", "1")):
            node = doc.addNode("displacement", name, "displacementshader")
            node.setNodeDefString("ND_displacement_float")
            node.addInput("displacement", "float").setValueString(value)
            node.addInput("scale", "float").setValueString(scale)
            sides.append(node)
        mix = doc.addNode("mix", "mix", "displacementshader")
        mix.addInput("fg", "displacementshader").setConnectedNode(sides[0])
        mix.addInput("bg", "displacementshader").setConnectedNode(sides[1])
        mix.addInput("mix", "float").setValueString("0.25")
        output(doc, "result", "displacementshader", mix)
        merge_displacements(Graph(doc, LIBRARY))
        merged = source(doc, "result")
        self.assertEqual(merged.getCategory(), "displacement")
        # 2 * 0.5 * 0.25 + 4 * 1 * 0.75
        self.assertAlmostEqual(
            values(merged, "displacement")[0], 3.25, places=6
        )
        self.assertEqual(values(merged, "scale"), [1.0])
        self.assertIsNone(doc.getNode("mix"))

    def test_legacy_swizzle_upgrades(self):
        doc = document()
        swizzle = doc.addNode("swizzle", "swizzle", "color3")
        swizzle.addInput("in", "color3").setValueString("0.1, 0.2, 0.3")
        swizzle.addInput("channels", "string").setValueString("bgr")
        output(doc, "result", "color3", swizzle)
        upgrade_legacy(doc)
        self.assertFalse(
            [n for n in doc.getNodes() if n.getCategory() == "swizzle"]
        )
        for got, want in zip(
            values(source(doc, "result")), (0.3, 0.2, 0.1), strict=True
        ):
            self.assertAlmostEqual(got, want, places=6)

    def test_primvar_reader_becomes_texcoord(self):
        doc = document()
        reader = doc.addNode("geompropvalue", "reader", "vector2")
        reader.addInput("geomprop", "string").setValueString("st")
        output(doc, "result", "vector2", reader)
        exported = {"/M/reader": reader}
        nodes = {
            "/M/reader": {
                "id": "ND_geompropvalue_vector2",
                "inputs": {"geomprop": {"value": "st"}},
            }
        }
        notes = []
        lower_primvars(doc, exported, nodes, notes)
        texcoord = source(doc, "result")
        self.assertEqual(texcoord.getCategory(), "texcoord")
        self.assertEqual(texcoord.getInput("index").getValueString(), "0")
        self.assertEqual(notes, [])

    def test_flake_becomes_markers(self):
        doc = document()
        flake = doc.addNode("flake3d", "flake", "multioutput")
        for name, type_ in (("presence", "float"), ("flakenormal", "vector3")):
            flake.addOutput(name, type_)
        reader = doc.addNode("multiply", "reader", "float")
        reader.addInput("in1", "float").setConnectedNode(flake)
        reader.getInput("in1").setOutputString("presence")
        reader.addInput("in2", "float").setValueString("1")
        output(doc, "result", "float", reader)
        lower_flakes(
            doc,
            {"/M/flake": flake},
            {"/M/flake": {"id": "ND_flake3d", "inputs": {}}},
        )
        self.assertIsNone(doc.getNode("flake"))
        marker = reader.getInput("in1").getConnectedNode()
        # Mode 1 (flake3d) and the defaults size 0.01, roughness 0.1, coverage 0.5, in millionths.
        self.assertEqual(
            marker.getName(), "flake_hdueflake_1_10000_100000_500000_presence"
        )
        self.assertEqual(marker.getCategory(), "constant")


if __name__ == "__main__":
    unittest.main()
