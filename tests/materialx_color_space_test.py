"""How a prepared MaterialX graph's images are decoded (export_usd_materialx.record): as the render-time translation
decodes the slot each drives (materials.cpp, Scene.cpp TextureSRGB) and converts colour images
(color_management.cpp HdUELinearRec709Texture). Needs PyOpenColorIO and OpenImageIO (Test.ps1 pins both).
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy
import OpenImageIO as oiio
import PyOpenColorIO as ocio

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import export_usd_materialx
from export_usd_materialx import record, write
from pxr import Sdf, Usd, UsdShade

COLOR = Sdf.ValueTypeNames.Color3f
FLOAT = Sdf.ValueTypeNames.Float
# OCIO's ACES config: scene_linear is ACEScg, so untagged float colour images convert.
ACES = "ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4"
PIXEL = (0.5, 0.25, 0.125)


class ColorSpaces(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = Path(folder.name)
        # The conversions' cache (the delegate's hdue_ocio folder) inside the test's folder.
        patch = mock.patch.object(tempfile, "tempdir", str(self.folder))
        patch.start()
        self.addCleanup(patch.stop)
        self.addCleanup(export_usd_materialx._color_management.cache_clear)

    def environment(self, **values):
        patch = mock.patch.dict(os.environ, values)
        patch.start()
        self.addCleanup(patch.stop)
        export_usd_materialx._color_management.cache_clear()

    def image(self, name):
        file = self.folder / name
        pixels = numpy.array([[PIXEL]], dtype=numpy.float32)
        out = oiio.ImageBuf(
            oiio.ImageSpec(
                1, 1, 3, oiio.HALF if file.suffix == ".exr" else oiio.UINT8
            )
        )
        out.set_pixels(out.roi, pixels)
        self.assertTrue(out.write(str(file)), out.geterror())
        return file

    def material(self, images):
        """A standard_surface whose inputs read images: (surface input, file name, colour space, extract channel or
        None, image type).
        """
        stage = Usd.Stage.CreateInMemory()
        material = UsdShade.Material.Define(stage, "/M")
        surface = UsdShade.Shader.Define(stage, "/M/S")
        surface.SetShaderId("ND_standard_surface_surfaceshader")
        material.CreateSurfaceOutput("mtlx").ConnectToSource(
            surface.CreateOutput("surface", Sdf.ValueTypeNames.Token)
        )
        for i, (target, name, space, channel, kind) in enumerate(images):
            image = UsdShade.Shader.Define(stage, f"/M/I{i}")
            image.SetShaderId(f"ND_image_{kind}")
            file = image.CreateInput("file", Sdf.ValueTypeNames.Asset)
            file.Set(Sdf.AssetPath(str(self.image(name))))
            if space:
                file.GetAttr().SetColorSpace(space)
            out = image.CreateOutput(
                "out", COLOR if kind == "color3" else FLOAT
            )
            if channel is not None:
                extract = UsdShade.Shader.Define(stage, f"/M/E{i}")
                extract.SetShaderId("ND_extract_color3")
                extract.CreateInput("in", COLOR).ConnectToSource(out)
                extract.CreateInput("index", Sdf.ValueTypeNames.Int).Set(
                    channel
                )
                out = extract.CreateOutput("out", FLOAT)
            elif kind == "float" and target.endswith("color"):
                convert = UsdShade.Shader.Define(stage, f"/M/C{i}")
                convert.SetShaderId("ND_convert_float_color3")
                convert.CreateInput("in", FLOAT).ConnectToSource(out)
                out = convert.CreateOutput("out", COLOR)
            surface.CreateInput(target, out.GetTypeName()).ConnectToSource(out)
        return record(material)

    @staticmethod
    def texture(item, i):
        return (
            item["textures"][f"/M/I{i}.inputs:file"][0],
            item["spaces"][f"/M/I{i}.inputs:file"],
        )

    def test_untagged_8_bit_images_decode_by_the_input_they_reach(self):
        self.environment(HDUE_COLOR_MANAGEMENT="0")
        item = self.material(
            [
                # A colour image split into roughness and metalness: data, as their slots are.
                ("specular_roughness", "packed.png", "", 0, "color3"),
                ("metalness", "packed2.png", "", 1, "color3"),
                # A float image made a colour: the base colour slot decodes sRGB.
                ("base_color", "grey.png", "", None, "float"),
                ("emission_color", "emit.png", "auto", None, "color3"),
            ]
        )
        self.assertEqual(
            [self.texture(item, i)[1] for i in range(4)],
            ["", "", "srgb_texture", "srgb_texture"],
        )
        with tempfile.TemporaryDirectory() as folder:
            text = Path(
                write([item], Path(folder) / "out", "Live")["source"]
            ).read_text(encoding="utf-8")
        self.assertEqual(text.count('colorspace="srgb_texture"'), 2)

    def test_untagged_float_colour_images_are_linear(self):
        self.environment(HDUE_COLOR_MANAGEMENT="0")
        item = self.material([("base_color", "base.exr", "", None, "color3")])
        file, space = self.texture(item, 0)
        self.assertEqual((Path(file).name, space), ("base.exr", ""))

    def converted(self, file):
        pixels = oiio.ImageBuf(file).get_pixels(oiio.FLOAT)[0, 0, :3]
        return [float(v) for v in pixels]

    def expected(self, space, encoded=False):
        config = ocio.GetCurrentConfig()
        processor = ocio.Config.GetProcessorToBuiltinColorSpace(
            config, space, "Linear Rec.709 (sRGB)"
        ).getDefaultCPUProcessor()
        # The 8-bit image holds PIXEL rounded to 1/255.
        pixel = [round(v * 255) / 255 for v in PIXEL] if encoded else PIXEL
        return processor.applyRGB(list(pixel))

    def test_colour_images_convert_from_their_space_as_at_render_time(self):
        self.environment(OCIO=ACES, HDUE_COLOR_MANAGEMENT="")
        item = self.material(
            [
                # Untagged float colour: from scene_linear (ACEScg here).
                ("emission_color", "emit.exr", "", None, "color3"),
                # A named space on a colour input: converted.
                ("specular_color", "spec.png", "acescg", None, "color3"),
                # The same space on a data input: as is, read linear.
                ("coat_roughness", "rough.png", "acescg", None, "float"),
                # sRGB-encoded and raw names, and untagged 8-bit colour: decoded, not converted.
                ("sheen_color", "sheen.png", "srgb_texture", None, "color3"),
                ("subsurface_color", "sss.png", "raw", None, "color3"),
                ("base_color", "base.png", "", None, "color3"),
            ]
        )
        cache = self.folder / "hdue_ocio"
        file, space = self.texture(item, 0)
        self.assertEqual((Path(file).parent, space), (cache, ""))
        for got, want in zip(
            self.converted(file), self.expected("ACEScg"), strict=True
        ):
            self.assertAlmostEqual(got, want, delta=2e-3)
        file, space = self.texture(item, 1)
        self.assertEqual((Path(file).parent, space), (cache, ""))
        for got, want in zip(
            self.converted(file),
            self.expected("ACEScg", encoded=True),
            strict=True,
        ):
            self.assertAlmostEqual(got, want, delta=2e-3)
        self.assertEqual(
            [
                (Path(f).name, s)
                for f, s in (self.texture(item, i) for i in range(2, 6))
            ],
            [
                ("rough.png", "acescg"),
                ("sheen.png", "srgb_texture"),
                ("sss.png", "raw"),
                ("base.png", "srgb_texture"),
            ],
        )
        # The dependencies are the authored files, not the conversions.
        self.assertNotIn(str(cache), " ".join(item["dependencies"]))
        with tempfile.TemporaryDirectory() as folder:
            text = Path(
                write([item], Path(folder) / "out", "Live")["source"]
            ).read_text(encoding="utf-8")
        self.assertIn(Path(self.texture(item, 0)[0]).as_posix(), text)
        # A second preparation reuses the conversion.
        converted = Path(self.texture(item, 0)[0])
        stamp = converted.stat().st_mtime_ns
        self.assertEqual(
            export_usd_materialx._linear_rec709(
                str(self.folder / "emit.exr"), None, ""
            ),
            str(converted),
        )
        self.assertEqual(converted.stat().st_mtime_ns, stamp)

    def test_an_empty_switch_is_unset_and_on_as_in_the_delegate(self):
        # TfGetenvBool: unset or empty is the default (on); 0 is off.
        for value, on in (
            ("", True),
            ("1", True),
            ("0", False),
            ("off", False),
        ):
            self.environment(OCIO=ACES, HDUE_COLOR_MANAGEMENT=value)
            self.assertEqual(
                export_usd_materialx._color_management() is not None, on, value
            )

    def test_an_image_that_cannot_be_converted_is_refused_by_name(self):
        self.environment(OCIO=ACES, HDUE_COLOR_MANAGEMENT="1")

        def unreadable(name):
            (self.folder / name).write_bytes(b"not an image")
            return self.folder / name

        self.image = unreadable
        with self.assertRaisesRegex(ValueError, "spec.png"):
            self.material(
                [("specular_color", "spec.png", "acescg", None, "color3")]
            )


if __name__ == "__main__":
    unittest.main()
