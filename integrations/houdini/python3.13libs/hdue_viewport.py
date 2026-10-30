"""Caller-owned C diagnostics and readback for the Houdini delegate."""

import ctypes as C
from pathlib import Path

from hdue_runtime import U32, U64, Result, Task
from material_api import Parameter


class Frame(C.Structure):
    _fields_ = (
        [
            (n, U32)
            for n in (
                "struct_size",
                "width",
                "height",
                "pixel_format",
                "color_space",
                "row_stride_bytes",
            )
        ]
        + [(n, U64) for n in ("generation", "frame_id", "required_bytes")]
        + [
            ("completed_samples", U32),
            ("converged", U32),
            ("request_to_frame_ms", C.c_double),
            ("readback_ms", C.c_double),
        ]
    )


class Status(C.Structure):
    _fields_ = (
        [
            (n, U32)
            for n in (
                "struct_size",
                "active",
                "create_thread",
                "execute_thread",
            )
        ]
        + [
            (n, U64)
            for n in (
                "session",
                "generation",
                "published_frames",
                "scene_submissions",
            )
        ]
        + [(n, U32) for n in ("mode", "meshes", "materials", "lights")]
        + [("frame", Frame), ("error", C.c_char * 512)]
    )


class Material(C.Structure):
    _fields_ = [
        ("material_id", C.c_char_p),
        ("native_asset", C.c_char_p),
        ("parameter_count", U32),
        ("reserved", U32),
        ("parameters", C.POINTER(Parameter)),
    ]


class Materials(C.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("abi_version", U32),
        ("session", U64),
        ("material_count", U32),
        ("reserved", U32),
        ("materials", C.POINTER(Material)),
    ]


class Texture(C.Structure):
    _fields_ = [("uri", C.c_char_p), ("file", C.c_char_p)]


class Textures(C.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("abi_version", U32),
        ("session", U64),
        ("texture_count", U32),
        ("reserved", U32),
        ("textures", C.POINTER(Texture)),
    ]


class Wet(C.Structure):
    _fields_ = [
        (n, C.c_char_p)
        for n in ("prim_path", "variant_asset", "preview_asset", "mask_primvar")
    ] + [
        ("amount", C.c_float),
        ("roughness", C.c_float),
        ("flags", U32),
        ("reserved", U32),
    ]


class Surfaces(C.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("abi_version", U32),
        ("session", U64),
        ("binding_count", U32),
        ("mask_preview", U32),
        ("bindings", C.POINTER(Wet)),
        ("validation_error", C.c_char_p),
    ]


class Viewport:
    def __init__(self, runtime, plugin):
        if C.sizeof(Frame) != 72 or C.sizeof(Status) != 648:
            raise RuntimeError("Invalid viewport ABI layout")
        self.runtime = runtime
        self.library = C.CDLL(str(Path(plugin).resolve()), winmode=0x1100)
        self.get = self.library.hdue_get_viewport_status
        self.get.argtypes = [C.POINTER(Status)]
        self.get.restype = Result
        self.set_up_axis = self.library.hdue_set_viewport_up_axis
        self.set_up_axis.argtypes = [U32]
        self.set_up_axis.restype = Result
        self.set_materials = self.library.hdue_set_viewport_materials
        self.set_materials.argtypes = [C.POINTER(Materials)]
        self.set_materials.restype = Result
        # Owner-only backend calls (runtime.bind) run on the UE thread the delegate runs.
        runtime.run_on_ue_thread = self.library.hdue_run_on_ue_thread
        runtime.run_on_ue_thread.argtypes = [Task, C.py_object]
        runtime.run_on_ue_thread.restype = Result
        self.set_textures = self.library.hdue_set_viewport_textures
        self.set_textures.argtypes = [C.POINTER(Textures)]
        self.set_textures.restype = Result
        if C.sizeof(Texture) != 16 or C.sizeof(Textures) != 32:
            raise RuntimeError("Invalid viewport texture ABI")
        self.set_surfaces = self.library.hdue_set_viewport_surfaces
        self.set_surfaces.argtypes = [C.POINTER(Surfaces)]
        self.set_surfaces.restype = Result
        if C.sizeof(Wet) != 48 or C.sizeof(Surfaces) != 40:
            raise RuntimeError("Invalid viewport surface ABI")
        if C.sizeof(Material) != 32 or C.sizeof(Materials) != 32:
            raise RuntimeError("Invalid viewport material ABI")
        api = runtime.api
        self.poll = C.CFUNCTYPE(Result, U64, C.POINTER(Frame))(api.poll_frame)
        self.copy = C.CFUNCTYPE(Result, U64, U64, C.c_void_p, U64)(
            api.copy_frame
        )
        self.set_depth = runtime.bind(
            C.CFUNCTYPE(Result, U64, U32), api.set_depth_enabled
        )
        self.copy_depth = C.CFUNCTYPE(Result, U64, U64, C.c_void_p, U64)(
            api.copy_depth
        )

    def status(self):
        s = Status()
        s.struct_size = C.sizeof(s)
        self.runtime.check(self.get(C.byref(s)), "viewport status")
        result = {
            n: getattr(s, n)
            for n, _ in s._fields_
            if n not in ("frame", "error")
        }
        result["error"] = s.error.decode("utf-8", "replace")
        result["frame"] = {n: getattr(s.frame, n) for n, _ in Frame._fields_}
        return result

    def up_axis(self, z_up):
        """The viewed stage's up axis, which Houdini does not pass to the delegate. False until a viewport exists."""
        code = self.set_up_axis(int(z_up))
        if code == 1:
            return False
        self.runtime.check(code, "viewport up axis")
        return True

    def pixels(self, status, require_converged=True):
        if not status["frame"]["frame_id"]:
            return None
        f = Frame()
        f.struct_size = C.sizeof(f)
        code = self.poll(status["session"], C.byref(f))
        if code == 1:
            return None
        self.runtime.check(code, "poll image")
        if f.generation != status["generation"] or (
            require_converged and not f.converged
        ):
            return None
        pixels = (C.c_float * (f.required_bytes // 4))()
        code = self.copy(
            status["session"], f.frame_id, pixels, f.required_bytes
        )
        if code == HDUE_STALE_FRAME:
            return None
        self.runtime.check(code, "copy image")
        return f, pixels

    def materials(self, session, assets, overrides):
        storage = []
        bindings = (Material * len(assets))()
        for index, (path, asset) in enumerate(sorted(assets.items())):
            fields = overrides.get(path, {})
            parameters = (Parameter * len(fields))()
            storage.append(parameters)
            for p, (name, value) in enumerate(sorted(fields.items())):
                parameters[p].name = name.encode("utf-8")
                if isinstance(value, str):
                    parameters[p].type = 2
                    parameters[p].texture_asset = value.encode("utf-8")
                elif isinstance(value, (int, float)):
                    parameters[p].type = 0
                    parameters[p].value[0] = float(value)
                else:
                    parameters[p].type = 1
                    vector = list(value)
                    vector.extend([0.0] * (4 - len(vector)))
                    for component in range(4):
                        parameters[p].value[component] = vector[component]
            bindings[index] = Material(
                path.encode("utf-8"),
                asset.encode("utf-8"),
                len(fields),
                0,
                parameters,
            )
        update = Materials(
            C.sizeof(Materials), 1, session, len(assets), 0, bindings
        )
        code = self.set_materials(C.byref(update))
        if code == 1:
            return False
        self.runtime.check(code, "viewport materials")
        return True

    def textures(self, session, files):
        """Map op:/opdef: texture paths to the files the host exported for them (the complete set)."""
        data = (Texture * len(files))(
            *(
                Texture(uri.encode("utf-8"), file.encode("utf-8"))
                for uri, file in sorted(files.items())
            )
        )
        update = Textures(C.sizeof(Textures), 1, session, len(files), 0, data)
        code = self.set_textures(C.byref(update))
        if code == 1:
            return False
        self.runtime.check(code, "viewport textures")
        return True

    def surfaces(self, session, bindings, preview=False, error=""):
        data = (Wet * len(bindings))()
        for i, (path, b) in enumerate(sorted(bindings.items())):
            data[i] = Wet(
                path.encode(),
                b["asset"].encode(),
                b["previewAsset"].encode(),
                b["maskPrimvar"].encode(),
                b["amount"],
                b["roughness"],
                2 if b.get("graph") else 0,
                0,
            )
        update = Surfaces(
            C.sizeof(Surfaces),
            2,
            session,
            len(bindings),
            bool(preview),
            data,
            error.encode(),
        )
        code = self.set_surfaces(C.byref(update))
        if code == 1:
            return False
        self.runtime.check(code, "viewport surfaces")
        return True

    def depth_pixels(self, status):
        # The depth of UE's newest frame, by UE's own frame id: the viewport status tracks the frames Houdini shows,
        # which (with GPU frames) are numbered separately.
        f = Frame()
        f.struct_size = C.sizeof(f)
        code = self.poll(status["session"], C.byref(f))
        if code == 1 or not f.frame_id:
            return None
        self.runtime.check(code, "poll image")
        pixels = (C.c_float * (f.width * f.height))()
        code = self.copy_depth(
            status["session"], f.frame_id, pixels, C.sizeof(pixels)
        )
        if code in (1, HDUE_STALE_FRAME):
            return None
        self.runtime.check(code, "copy depth image")
        return pixels


HDUE_STALE_FRAME = -7
