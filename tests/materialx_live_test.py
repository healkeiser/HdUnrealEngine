"""MaterialX graphs hdUE cannot translate at render time: which ones need UE's importer, how the library leaves the rest
to the render-time translation and binds live-prepared ones, and the document the worker writes for UE.
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from materialx_controls import MaterialXLibrary, OfflineView
from pxr import Sdf, Usd, UsdShade
from usd_materialx_graph import (
    graph_data,
    graph_signature,
    needs_import,
    request_key,
    structure_key,
)

COLOR = Sdf.ValueTypeNames.Color3f
FLOAT = Sdf.ValueTypeNames.Float
FLOAT2 = Sdf.ValueTypeNames.Float2


def shader(stage, path, identity):
    node = UsdShade.Shader.Define(stage, path)
    node.SetShaderId(identity)
    return node


def material(procedural=True, path="/Looks/M"):
    """A standard_surface whose base colour is a constant, or a mix of red and blue driven by place2d'd UVs through
    a multi-output separate2 (no render-time translation for either node).
    """
    stage = Usd.Stage.CreateInMemory()
    result = UsdShade.Material.Define(stage, path)
    surface = shader(
        stage, path + "/Surface", "ND_standard_surface_surfaceshader"
    )
    result.CreateSurfaceOutput("mtlx").ConnectToSource(
        surface.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    )
    base = surface.CreateInput("base_color", COLOR)
    if not procedural:
        base.Set((0.5, 0.5, 0.5))
        return stage, result
    uv = shader(stage, path + "/UV", "ND_texcoord_vector2")
    place = shader(stage, path + "/Place", "ND_place2d_vector2")
    place.CreateInput("texcoord", FLOAT2).ConnectToSource(
        uv.CreateOutput("out", FLOAT2)
    )
    place.CreateInput("rotate", FLOAT).Set(30.0)
    split = shader(stage, path + "/Split", "ND_separate2_vector2")
    split.CreateInput("in", FLOAT2).ConnectToSource(
        place.CreateOutput("out", FLOAT2)
    )
    mix = shader(stage, path + "/Mix", "ND_mix_color3")
    mix.CreateInput("fg", COLOR).Set((1.0, 0.0, 0.0))
    mix.CreateInput("bg", COLOR).Set((0.0, 0.0, 1.0))
    mix.CreateInput("mix", FLOAT).ConnectToSource(
        split.CreateOutput("outx", FLOAT)
    )
    base.ConnectToSource(mix.CreateOutput("out", COLOR))
    return stage, result


def entry(graph, mutable):
    """A live-prepared library entry for graph, as live_materials.finish writes it."""
    return dict(
        kind="materialx",
        key=request_key(graph),
        structure=structure_key(graph),
        asset="/Game/HdUE/Live/P/V1/MX/M.M",
        model="standard_surface",
        usd_material=graph["material"],
        usd_bindings=[
            dict(
                attribute=attribute,
                parameter=attribute.rsplit(":", 1)[1],
                type="vector",
                default=[0, 0, 0, 1],
            )
            for attribute in mutable
        ],
        usd_graph_signature=graph_signature(graph, mutable),
    )


class NeedsImportTests(unittest.TestCase):
    def graph(self, *nodes, surface="ND_standard_surface_surfaceshader"):
        result = dict(
            terminal=dict(node="/S", output="surface"),
            nodes={"/S": dict(id=surface, inputs={})},
        )
        for i, (identity, inputs) in enumerate(nodes):
            result["nodes"][f"/N{i}"] = dict(id=identity, inputs=inputs)
        return result

    def test_render_time_translation_decides(self):
        self.assertFalse(needs_import(self.graph()))
        self.assertFalse(
            needs_import(
                self.graph(
                    ("ND_image_color3", {}),
                    ("ND_texcoord_vector2", {}),
                    ("ND_normalmap_float", {}),
                    ("ND_multiply_color3", {"in1": {"node": "/N0"}}),
                )
            )
        )
        self.assertTrue(needs_import(self.graph(("ND_mix_color3", {}))))
        self.assertTrue(needs_import(self.graph(("ND_place2d_vector2", {}))))
        self.assertTrue(
            needs_import(
                self.graph(
                    (
                        "ND_multiply_color3",
                        {"in1": {"node": "/N1"}, "in2": {"node": "/N2"}},
                    )
                )
            )
        )
        self.assertTrue(
            needs_import(self.graph(surface="ND_gltf_pbr_surfaceshader"))
        )


class LibraryTests(unittest.TestCase):
    def sync(self, stage, prepared=()):
        view = OfflineView(stage, Usd.TimeCode.Default(), object())
        library = MaterialXLibrary(None, view)
        library.set_prepared([dict(e) for e in prepared])
        self.assertTrue(library.sync(), library.preparation_error)
        return library, view.settings

    def test_translatable_graphs_are_left_to_the_render_time_translation(self):
        library, settings = self.sync(material(procedural=False)[0])
        self.assertEqual(library.missing, [])
        self.assertEqual(settings.get("nativeMaterialAssets") or {}, {})

    def test_untranslatable_graphs_are_missing_until_prepared(self):
        stage, m = material()
        library, settings = self.sync(stage)
        self.assertEqual([x.GetPath() for x in library.missing], [m.GetPath()])
        self.assertEqual(settings.get("nativeMaterialAssets") or {}, {})
        graph = graph_data(m)
        prepared = entry(graph, ["/Looks/M/Mix.inputs:fg"])
        library, settings = self.sync(stage, [prepared])
        self.assertEqual(library.missing, [])
        self.assertEqual(
            settings["nativeMaterialAssets"]["/Looks/M"], prepared["asset"]
        )
        # A value the import kept as a parameter changes it; one it baked in needs a new preparation.
        stage.GetAttributeAtPath("/Looks/M/Mix.inputs:fg").Set((0.0, 1.0, 0.0))
        library, settings = self.sync(stage, [prepared])
        self.assertEqual(library.missing, [])
        self.assertEqual(
            tuple(settings["nativeMaterialParameters"]["/Looks/M"]["fg"])[:3],
            (0.0, 1.0, 0.0),
        )
        stage.GetAttributeAtPath("/Looks/M/Place.inputs:rotate").Set(45.0)
        library, _ = self.sync(stage, [prepared])
        self.assertEqual(len(library.missing), 1)

    def test_a_prepared_graph_binds_at_another_path(self):
        first_stage, first = material()  # the stage outlives the read
        graph = graph_data(first)
        stage, _ = material(path="/Other/Looks/M")
        library, settings = self.sync(stage, [entry(graph, [])])
        self.assertEqual(library.missing, [])
        self.assertIn("/Other/Looks/M", settings["nativeMaterialAssets"])


class ExportTests(unittest.TestCase):
    def test_document_holds_standard_nodes_only(self):
        try:
            import MaterialX  # noqa: F401
        except ImportError:
            self.skipTest("MaterialX is not installed")
        from export_usd_materialx import record, write

        _stage, m = material()
        items = [record(m)]
        with tempfile.TemporaryDirectory() as folder:
            manifest = write(items, Path(folder) / "out", "Live")
            text = Path(manifest["source"]).read_text(encoding="utf-8")
        self.assertNotIn("xi:include", text)
        self.assertIn('output="outx"', text)
        self.assertIn("<place2d", text)
        self.assertEqual(
            {f["attribute"] for f in manifest["materials"][0]["fields"]},
            {
                "/Looks/M/Place.inputs:rotate",
                "/Looks/M/Mix.inputs:fg",
                "/Looks/M/Mix.inputs:bg",
            },
        )


if __name__ == "__main__":
    unittest.main()
