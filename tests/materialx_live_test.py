"""MaterialX graphs hdUE cannot translate at render time: which ones need UE's importer, how the library leaves the rest
to the render-time translation and binds live-prepared ones, and the document the worker writes for UE.
"""

import hashlib
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from materialx_controls import MaterialXLibrary, OfflineView
from materialx_textures import file_identity
from pxr import Sdf, Usd, UsdShade
from usd_materialx_graph import (
    COLOR_SURFACE_INPUTS,
    RUNTIME_SURFACE_INPUTS,
    graph_data,
    graph_signature,
    image_color_space,
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
    a multi-output separate2 (no render-time translation for separate2).
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
        # mix is an affine of the one input a node drives (constant fg / bg / mix otherwise).
        self.assertFalse(
            needs_import(self.graph(("ND_mix_color3", {"fg": {"node": "/N1"}})))
        )
        self.assertTrue(needs_import(self.graph(("ND_mix_color3", {}))))
        self.assertTrue(
            needs_import(
                self.graph(
                    (
                        "ND_mix_color3",
                        {"fg": {"node": "/N1"}, "bg": {"node": "/N2"}},
                    )
                )
            )
        )
        self.assertTrue(needs_import(self.graph(("ND_mix_bsdf", {}))))
        self.assertFalse(needs_import(self.graph(("ND_place2d_vector2", {}))))
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

    def test_what_the_render_time_translation_would_drop(self):
        place = ("ND_place2d_vector2", {"scale": {"value": [2.0, 2.0]}})
        tiled = ("ND_tiledimage_color3", {"uvtiling": {"value": [3.0, 3.0]}})
        image = ("ND_image_color3", {"texcoord": {"node": "/N0"}})
        plain = ("ND_image_float", {})
        # One image through a place2d: one UV transform, translated.
        self.assertFalse(needs_import(self.graph(place, image)))
        # Two images on two transforms (or one transformed, one not): render time has one per material.
        self.assertTrue(needs_import(self.graph(place, image, plain)))
        self.assertTrue(needs_import(self.graph(place, image, tiled)))
        # Two images on the same transform stay translated.
        self.assertFalse(needs_import(self.graph(place, image, image)))
        # A place2d input driven by a node: render time reads only constants.
        self.assertTrue(
            needs_import(
                self.graph(
                    ("ND_place2d_vector2", {"offset": {"node": "/N1"}}),
                    ("ND_constant_vector2", {}),
                )
            )
        )

    def test_surface_inputs_the_translation_does_not_read(self):
        for surface, unread in (
            ("ND_standard_surface_surfaceshader", "coat_IOR"),
            ("ND_open_pbr_surface_surfaceshader", "specular_anisotropy"),
        ):
            graph = self.graph(surface=surface)
            graph["nodes"]["/S"]["inputs"] = {
                "base_color": {"value": [1, 0, 0]}
            }
            self.assertFalse(needs_import(graph), surface)
            graph["nodes"]["/S"]["inputs"][unread] = {"value": 1.33}
            self.assertTrue(needs_import(graph), surface)
            # Declared without a value: nothing to read.
            graph["nodes"]["/S"]["inputs"][unread] = {"value": None}
            self.assertFalse(needs_import(graph), surface)
        # Authored at the nodedef's default (USD's float precision): nothing changes, nothing to prepare.
        for surface, name, value in (
            ("ND_standard_surface_surfaceshader", "coat_IOR", 1.5),
            ("ND_standard_surface_surfaceshader", "specular_anisotropy", 0.0),
            (
                "ND_standard_surface_surfaceshader",
                "transmission_scatter",
                [0, 0, 0],
            ),
            (
                "ND_open_pbr_surface_surfaceshader",
                "coat_ior",
                1.600000023841858,
            ),
        ):
            graph = self.graph(surface=surface)
            graph["nodes"]["/S"]["inputs"] = {name: {"value": value}}
            self.assertFalse(needs_import(graph), name)
            graph["nodes"]["/S"]["inputs"] = {name: {"node": "/N0"}}
            self.assertTrue(needs_import(graph), name)
        # Preview Surface reads its own inputs (readPreviewSurface).
        graph = self.graph(surface="ND_UsdPreviewSurface_surfaceshader")
        graph["nodes"]["/S"]["inputs"] = {"opacityMode": {"value": "presence"}}
        self.assertFalse(needs_import(graph))

    def test_surface_inputs_match_materials_cpp(self):
        source = (
            Path(__file__).resolve().parents[1] / "src/hydra/materials.cpp"
        ).read_text(encoding="utf-8")
        for surface, table in (
            ("ND_standard_surface_surfaceshader", "standardSurface{"),
            ("ND_open_pbr_surface_surfaceshader", "openPbr{"),
        ):
            body = source[source.index("MaterialXSurface " + table) :]
            body = body[: body.index("};")]
            names = set(re.findall(r'"(\w+)"', body))
            self.assertEqual(
                names, set(RUNTIME_SURFACE_INPUTS[surface]), surface
            )

    def test_colour_inputs_match_materials_cpp(self):
        # The inputs readMaterialX and readPreviewSurface read through SurfaceReader::color.
        source = (
            Path(__file__).resolve().parents[1] / "src/hydra/materials.cpp"
        ).read_text(encoding="utf-8")

        def body(start, end):
            text = source[source.index(start) :]
            return text[: text.index(end)]

        fields = [
            name.strip()
            for group in re.findall(
                r"Input<\w+>\s+([\w,\s]+);",
                body("struct MaterialXSurface {", "};"),
            )
            for name in group.split(",")
        ] + ["normal"]
        colors = re.findall(
            r"\bcolor\(s\.(\w+),", body("void readMaterialX(", "\n}\n")
        )
        for surface, table in (
            ("ND_standard_surface_surfaceshader", "standardSurface{"),
            ("ND_open_pbr_surface_surfaceshader", "openPbr{"),
        ):
            names = dict(
                zip(
                    fields,
                    re.findall(
                        r'"(\w+)"', body("MaterialXSurface " + table, "};")
                    ),
                    strict=True,
                )
            )
            self.assertEqual(
                set(COLOR_SURFACE_INPUTS[surface]),
                {names[f] for f in colors},
                surface,
            )
        self.assertEqual(
            set(COLOR_SURFACE_INPUTS["ND_UsdPreviewSurface_surfaceshader"]),
            set(
                re.findall(
                    r'r\.color\("(\w+)"',
                    body("void readPreviewSurface(", "\n}\n"),
                )
            ),
        )


class ImageColorSpaceTests(unittest.TestCase):
    def test_names_decode_as_at_render_time(self):
        # (authored, colour image, data image): what UE's importer is given. sRGB-encoded names (HdUESRGBColorSpace)
        # and untagged colour images are srgb_texture, the one name it decodes as sRGB; the rest stays linear.
        for space, color, data in (
            ("", "srgb_texture", ""),
            ("auto", "srgb_texture", ""),
            ("srgb_texture", "srgb_texture", "srgb_texture"),
            ("sRGB - Texture", "srgb_texture", "srgb_texture"),
            ("srgb_tx", "srgb_texture", "srgb_texture"),
            ("lin_srgb", "lin_srgb", "lin_srgb"),
            ("raw", "raw", "raw"),
            ("lin_rec709", "lin_rec709", "lin_rec709"),
        ):
            self.assertEqual(image_color_space(space, "color"), color, space)
            self.assertEqual(image_color_space(space, "data"), data, space)
        # Float images (EXR, HDR) are linear at render time whatever slot they drive.
        for space in ("", "auto"):
            self.assertEqual(
                image_color_space(space, "color", floating=True), ""
            )

    def test_normal_maps_are_raw(self):
        for space in ("", "auto", "raw", "lin_rec709", "lin_srgb"):
            self.assertEqual(image_color_space(space, "normal"), "raw", space)
        for space in ("srgb_texture", "sRGB - Texture", "srgb_tx"):
            with self.assertRaises(ValueError):
                image_color_space(space, "normal")


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

    def test_colours_are_written_as_the_host_converted_them(self):
        try:
            import MaterialX  # noqa: F401
        except ImportError:
            self.skipTest("MaterialX is not installed")
        from export_usd_materialx import record, write

        _stage, m = material()
        items = [record(m)]
        # The host's OCIO conversion from scene_linear (record), which the worker's Python cannot make.
        items[0]["colors"] = {"/Looks/M/Mix.inputs:fg": [0.25, 0.5, 0.75]}
        with tempfile.TemporaryDirectory() as folder:
            manifest = write(items, Path(folder) / "out", "Live")
            text = Path(manifest["source"]).read_text(encoding="utf-8")
        self.assertIn('value="0.25, 0.5, 0.75"', text)
        self.assertIn('value="0.0, 0.0, 1.0"', text)  # bg: as authored

    def test_unknown_terminal_nodedef_has_the_intended_error(self):
        try:
            import MaterialX  # noqa: F401
        except ImportError:
            self.skipTest("MaterialX is not installed")
        from export_usd_materialx import record, write

        _stage, m = material()
        items = [record(m)]
        graph = items[0]["graph"]
        terminal = next(iter(graph["nodes"]))
        graph["terminal"] = {"node": terminal}
        graph["nodes"][terminal]["id"] = "ND_does_not_exist"
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(
                ValueError, "Unknown MaterialX nodedef"
            ):
                write(items, Path(folder) / "out", "Live")


class LibraryDependencies(unittest.TestCase):
    def load(self, record, change=False):
        with tempfile.TemporaryDirectory() as folder:
            tex = Path(folder) / "tex.png"
            tex.write_bytes(b"pixels")
            library = Path(folder) / "library.json"
            library.write_text(
                json.dumps(
                    dict(
                        version=1,
                        materials=[],
                        dependencies={str(tex): record(tex)},
                    )
                )
            )
            if change:
                tex.write_bytes(b"other pixels")
            view = OfflineView(
                Usd.Stage.CreateInMemory(), Usd.TimeCode.Default(), object()
            )
            MaterialXLibrary(str(library), view)

    def test_stat_dependency_loads(self):
        self.load(file_identity)

    def test_changed_stat_dependency_is_refused(self):
        with self.assertRaisesRegex(ValueError, "MaterialX source changed"):
            self.load(file_identity, change=True)

    def test_sha256_dependency_still_loads(self):
        self.load(lambda p: hashlib.sha256(p.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
