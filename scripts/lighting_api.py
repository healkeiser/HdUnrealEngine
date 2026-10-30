"""Caller-owned ctypes mirror of the lighting diagnostics ABI (include/hdue/bridge.h)."""

import ctypes as C

from hdue_runtime import U32, U64, Result


class Status(C.Structure):
    _fields_ = [
        (n, U32)
        for n in (
            "struct_size",
            "light_count",
            "dome_count",
            "environment_pending",
            "mega_lights_project",
            "mega_lights_allowed",
            "mega_lights_active",
            "cube_resolution",
        )
    ] + [
        ("frame_id", U64),
        ("generation", U64),
        ("environment_max_radiance", C.c_float),
        ("reserved", U32),
    ]


class Lighting:
    def __init__(self, runtime):
        if C.sizeof(Status) != 56:
            raise RuntimeError("Invalid lighting ABI layout")
        self.runtime = runtime
        self.get_status = runtime.bind(
            C.CFUNCTYPE(Result, U64, C.POINTER(Status)),
            runtime.api.get_lighting_status,
        )

    def status(self, session):
        status = Status()
        status.struct_size = C.sizeof(status)
        self.runtime.check(
            self.get_status(session, C.byref(status)), "lighting status"
        )
        return {n: getattr(status, n) for n, _ in status._fields_}
