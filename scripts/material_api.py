"""ctypes mirror of hdUE's caller-owned material C ABI (include/hdue/bridge.h)."""

import ctypes as C

from hdue_runtime import U32, U64, Result


class RendererInfo(C.Structure):
    _fields_ = [
        (n, U32)
        for n in (
            "struct_size",
            "substrate_enabled",
            "adaptive_gbuffer",
            "project_closures_per_pixel",
            "bytes_per_pixel",
        )
    ]


class Parameter(C.Structure):
    _fields_ = [
        ("name", C.c_char_p),
        ("type", U32),
        ("reserved", U32),
        ("value", C.c_float * 4),
        ("texture_asset", C.c_char_p),
    ]


class Update(C.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("parameter_count", U32),
        ("material_id", C.c_char_p),
        ("parameters", C.POINTER(Parameter)),
    ]


class MaterialStats(C.Structure):
    _fields_ = [
        (n, U32)
        for n in (
            "struct_size",
            "substrate_enabled",
            "closure_count",
            "bytes_per_pixel",
            "scalar_parameters",
            "vector_parameters",
            "texture_parameters",
            "blend_mode",
        )
    ]


class MaterialApi:
    def __init__(self, runtime):
        self.runtime = runtime
        if tuple(
            map(C.sizeof, (RendererInfo, Parameter, Update, MaterialStats))
        ) != (20, 40, 24, 32):
            raise RuntimeError("Material C ABI layout mismatch")
        table = runtime.api
        self.get_info = runtime.bind(
            C.CFUNCTYPE(Result, C.POINTER(RendererInfo)),
            table.get_renderer_info,
        )
        self.set_parameters = runtime.bind(
            C.CFUNCTYPE(Result, U64, C.POINTER(Update)),
            table.set_material_parameters,
        )
        self.get_stats = runtime.bind(
            C.CFUNCTYPE(Result, U64, C.c_char_p, C.POINTER(MaterialStats)),
            table.get_material_stats,
        )

    def require(self, result):
        self.runtime.check(result, "Material API")

    def renderer_info(self):
        info = RendererInfo(struct_size=C.sizeof(RendererInfo))
        self.require(self.get_info(C.byref(info)))
        return {
            n: int(getattr(info, n))
            for n, _ in info._fields_
            if n != "struct_size"
        }

    def material_stats(self, session, material):
        stats = MaterialStats(struct_size=C.sizeof(MaterialStats))
        self.require(self.get_stats(session, material.encode(), C.byref(stats)))
        return {
            n: int(getattr(stats, n))
            for n, _ in stats._fields_
            if n != "struct_size"
        }

    def edit(self, session, material, fields, check=True):
        parameters = []
        for name, value in fields:
            p = Parameter(name=name.encode())
            if isinstance(value, str):
                p.type = 2
                p.texture_asset = value.encode()
            elif isinstance(value, (int, float)):
                p.type = 0
                p.value[0] = value
            else:
                p.type = 1
                for i, v in enumerate(value):
                    p.value[i] = v
                if len(value) == 3:
                    p.value[3] = 1
            parameters.append(p)
        array = (Parameter * len(parameters))(*parameters)
        update = Update(
            struct_size=C.sizeof(Update),
            parameter_count=len(parameters),
            material_id=material.encode(),
            parameters=array,
        )
        result = self.set_parameters(session, C.byref(update))
        if check:
            self.require(result)
        return result
