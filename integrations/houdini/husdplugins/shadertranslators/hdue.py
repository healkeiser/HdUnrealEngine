"""hdUE Substrate shader translator, chosen by the hdUE render mask of the Substrate VOPs.

A Material Library LOP hands it an hdUE Substrate Builder, which it authors whole as the typed USD graph of
scripts/substrate_graph.py (hdue:substrate:* shaders, outputs:hdue:surface and a Preview Surface fallback):
the builder's nodes are not renderer shaders but a graph hdUE prepares into a UE Substrate material.
"""

import re

import hdue_substrate
import hou
from husd.previewshadertranslator import PreviewShaderTranslator
from husd.shadertranslator import ShaderTranslator
from pxr import UsdShade


def _builder(node):
    while node and not hdue_substrate.is_builder(node):
        node = node.parent()
    return node


class SubstrateTranslator(ShaderTranslator):
    def matchesRenderMask(self, render_mask):
        return bool(re.search(r"\bhdUE\b", render_mask))

    def renderContextName(self, shader_node, shader_node_output_name):
        return "hdue"

    def createMaterialShader(
        self,
        usd_stage,
        usd_material_path,
        usd_time_code,
        shader_node,
        shader_type,
        shader_node_output_name,
    ):
        builder = _builder(shader_node)
        if not builder:
            raise hou.OperationFailed(
                "hdUE Substrate nodes translate inside an hdUE Substrate Builder: "
                + shader_node.path()
            )
        hdue_substrate.translate(
            usd_stage, str(usd_material_path), builder, replace=False
        )

    def updateShaderParameters(
        self,
        usd_stage,
        usd_shader_path,
        usd_time_code,
        shader_node,
        shader_node_parameter_names,
    ):
        prim = usd_stage.GetPrimAtPath(usd_shader_path)
        while prim and not prim.IsA(UsdShade.Material):
            prim = prim.GetParent()
        if prim and _builder(shader_node):
            hdue_substrate.translate(
                usd_stage,
                str(prim.GetPath()),
                _builder(shader_node),
                replace=False,
            )


class SubstratePreviewTranslator(PreviewShaderTranslator):
    """The universal Preview Surface is part of the authored graph already."""

    def matchesRenderContext(self, render_context):
        return render_context == "hdue"

    def createMaterialPreviewShader(self, *args):
        pass

    def deleteMaterialPreviewShader(self, *args):
        pass

    def updateMaterialPreviewShaderParameters(self, *args):
        pass


translator = SubstrateTranslator()


def usdShaderTranslator():
    return translator


preview_translator = SubstratePreviewTranslator()


def usdPreviewShaderTranslator():
    return preview_translator
