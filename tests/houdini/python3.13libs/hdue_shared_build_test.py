"""Shared background builds in Houdini's viewport (Scene.cpp, PendingStatic): identical meshes run one card and Nanite
build per key, which TickConversions swaps into every mesh of the key.

Running builds: while they run, the test removes some copies of a key, then every copy of another, rebinds a copy to a
translucent material (a new key), and re-adds a copy of each emptied key.
Queued builds: with r.HdUE.MeshBuildMaxMemoryUsage at 1 GB (UE's Editor.AsyncAssetCompilationMaxMemoryUsage), one mesh
whose build reserves more than that runs alone, and the builds of keys added after it wait for memory while the test
makes the same edits to their copies.

In-place material edits: the copies of a key bound to a material whose opacity is edited in place re-upload (another
Nanite choice) when it turns translucent and again when it turns opaque, and not when only its colour changes.

After each phase the backend's summary line must show the edits hit running (then queued) builds, every generated mesh
drawing its converted mesh and no reserved memory left; the UE log must hold no ensure or error. The test ends with
new builds running, so UE shuts down under them.
"""

import json
import os
import re
import time
from datetime import UTC, datetime
from pathlib import Path

import hdue_houdini as integration
from hdue_houdini_test import Test as ViewportTest
from surface_api import SurfaceApi

_test = None
# Key: (rings, segments) of a sphere. Four keys of ten copies, dense enough (360k triangles) that their builds outlast
# the edits.
KEYS = {"A": (300, 600), "B": (290, 600), "C": (280, 600), "D": (270, 600)}
COPIES = 10
# 2.6M vertices with normals, UVs and colours: its build's estimate (7 times its arrays) is over 1 GB.
BIG = {"Big": (2000, 1300)}
QUEUED = {"E": (260, 600), "F": (250, 600)}
QUEUED_COPIES = 6
BUILD = """import numpy as np
from pxr import Gf, Sdf, UsdGeom, UsdShade, Vt
stage = hou.pwd().editableStage()
if CAP:
    settings = stage.GetPrimAtPath("/Render/rendersettings")
    settings.CreateAttribute("unreal:cvar:r:HdUE:MeshBuildMaxMemoryUsage", Sdf.ValueTypeNames.Int).Set(CAP)
looks = {}
for name, opacity in (("Grey", 1.0), ("Glass", 0.3)):
    material = UsdShade.Material.Define(stage, "/Shared/Looks/" + name)
    shader = UsdShade.Shader.Define(stage, material.GetPath().AppendChild("Surface"))
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.6, 0.6, 0.65))
    shader.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(opacity)
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
    looks[name] = material
rng = np.random.default_rng(SEED)
for row, (key, (rings, segments)) in enumerate(KEYS.items(), ROW):
    v, u = np.meshgrid(np.linspace(0, np.pi, rings + 1), np.linspace(0, 2 * np.pi, segments, endpoint=False), indexing="ij")
    unit = np.stack([np.sin(v) * np.cos(u), np.sin(v) * np.sin(u), np.cos(v)], -1).reshape(-1, 3).astype(np.float32)
    # Per-run jitter: new content each run, so the Nanite cache (Saved/NaniteCache) cannot skip the builds.
    points = (unit * (30 + rng.uniform(0, 0.5, (len(unit), 1)))).astype(np.float32)
    st = np.stack([u / (2 * np.pi), v / np.pi], -1).reshape(-1, 2).astype(np.float32)
    r, c = np.meshgrid(np.arange(rings), np.arange(segments), indexing="ij")
    a, b = r * segments + c, r * segments + (c + 1) % segments
    indices = Vt.IntArray.FromNumpy(np.stack([a, a + segments, b + segments, b], -1).reshape(-1).astype(np.int32))
    counts = Vt.IntArray.FromNumpy(np.full(rings * segments, 4, np.int32))
    positions, normals = Vt.Vec3fArray.FromNumpy(points), Vt.Vec3fArray.FromNumpy(unit)
    st, colors = Vt.Vec2fArray.FromNumpy(st), Vt.Vec3fArray.FromNumpy(np.abs(unit))
    for copy in range(COPIES):
        mesh = UsdGeom.Mesh.Define(stage, f"/Shared/{key}{copy}")
        mesh.CreatePointsAttr(positions)
        mesh.CreateFaceVertexCountsAttr(counts)
        mesh.CreateFaceVertexIndicesAttr(indices)
        mesh.CreateNormalsAttr(normals)
        mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
        mesh.CreateSubdivisionSchemeAttr("none")
        primvars = UsdGeom.PrimvarsAPI(mesh)
        primvars.CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex).Set(st)
        primvars.CreatePrimvar("displayColor", Sdf.ValueTypeNames.Color3fArray, UsdGeom.Tokens.vertex).Set(colors)
        mesh.AddTranslateOp().Set(Gf.Vec3d((copy - 4.5) * 70, (row - 1.5) * 70, 250))
        UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(looks["Grey"])
"""
EDIT = """from pxr import UsdShade
stage = hou.pwd().editableStage()
for path in REMOVED:
    stage.GetPrimAtPath(path).SetActive(False)
glass = UsdShade.Material(stage.GetPrimAtPath("/Shared/Looks/Glass"))
for path in GLASS:
    UsdShade.MaterialBindingAPI(stage.GetPrimAtPath(path)).Bind(glass)
"""
TINT = """from pxr import Gf, Sdf, UsdShade
stage = hou.pwd().editableStage()
material = UsdShade.Material.Define(stage, "/Shared/Looks/Tint")
shader = UsdShade.Shader.Define(stage, "/Shared/Looks/Tint/Surface")
shader.CreateIdAttr("UsdPreviewSurface")
shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*COLOR))
shader.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(OPACITY)
material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
for path in PATHS:
    UsdShade.MaterialBindingAPI(stage.GetPrimAtPath(path)).Bind(material)
"""
# The displaced mesh's material (constant displacement), edited with the same colour and opacity.
DISPLACED = """
displaced = UsdShade.Material.Define(stage, "/Shared/Looks/Displaced")
shader = UsdShade.Shader.Define(stage, "/Shared/Looks/Displaced/Surface")
shader.CreateIdAttr("UsdPreviewSurface")
shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*COLOR))
shader.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(OPACITY)
shader.CreateInput("displacement", Sdf.ValueTypeNames.Float).Set(2.0)
displaced.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
displaced.CreateDisplacementOutput().ConnectToSource(shader.ConnectableAPI(), "displacement")
UsdShade.MaterialBindingAPI.Apply(stage.GetPrimAtPath("/Shared/G0")).Bind(displaced)
"""
SUMMARY = re.compile(
    r"hdUE conversions done: (\d+) builds swapped into (\d+) meshes, (\d+) dropped and (\d+) shrunk by edits while "
    r"they ran, (\d+) dropped and (\d+) shrunk while queued; (\d+) of (\d+) generated meshes unconverted; "
    r"(-?\d+) bytes reserved"
)
FIELDS = (
    "swapped_builds",
    "swapped_meshes",
    "dropped_builds",
    "shrunk_builds",
    "queued_dropped",
    "queued_shrunk",
    "unconverted",
    "generated",
    "reserved",
)


def process_start():
    """This process's start (Windows GetProcessTimes), as a time.time() value."""
    import ctypes as C
    from ctypes import wintypes as W

    times = [W.FILETIME() for _ in range(4)]
    kernel = C.windll.kernel32
    kernel.GetCurrentProcess.restype = W.HANDLE
    kernel.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
    kernel.GetProcessTimes(kernel.GetCurrentProcess(), *map(C.byref, times))
    return (
        (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
    ) / 1e7 - 11644473600


def session_log():
    """This Houdini's UE log lines. UE names its log by the first free slot (HdUEBackend.log, _2, ...) and other
    hosts write theirs alongside, so the file is the one holding conversion summaries (written by interactive hosts
    only) stamped since this process started; lines are taken by their UTC stamp, not by the files' times.
    """
    start = process_start()
    profile = json.loads(Path(os.environ["HDUE_RUNTIME_CONFIG"]).read_text())
    since = datetime.fromtimestamp(start, UTC).strftime("[%Y.%m.%d-%H.%M.%S")
    found = ""
    for log in (
        Path(profile["runtime_project"]).parent / "Saved" / "Logs"
    ).glob("*.log"):
        try:
            if log.stat().st_mtime < start:
                continue
            lines = [
                line
                for line in log.read_text(
                    encoding="utf-8", errors="replace"
                ).splitlines()
                if line[:20] >= since and line.startswith("[")
            ]
        except OSError:  # rotated away while listed
            continue
        text = "\n".join(lines)
        if "hdUE conversions done" in text and len(text) > len(found):
            found = text
    return found


def source(seed, keys=KEYS, count=COPIES, row=0, cap=0):
    return (
        f"SEED = {seed}\nKEYS = {keys!r}\nCOPIES = {count}\nROW = {row}\nCAP = {cap}\n"
        + BUILD
    )


def copies(key, numbers):
    return [f"/Shared/{key}{n}" for n in numbers]


class Test(ViewportTest):
    def run(self):
        import hdefereval

        self.deadline = time.monotonic() + 600
        start = time.time()
        viewer = integration.scene_viewer()
        for _ in range(24):
            yield
        yield from self.image("01-smoke", 0)
        # The smoke scene's own generated meshes, from the summary of their builds.
        waited = time.monotonic() + 60
        while not SUMMARY.findall(session_log()):
            if time.monotonic() > waited:
                raise AssertionError(
                    "No conversion summary in this session's UE log"
                )
            yield from self.idle(1)
        before = int(SUMMARY.findall(session_log())[-1][7])
        base = self.api.status()["meshes"]
        settings = viewer.currentNode()
        build = settings.parent().createNode(
            "pythonscript", "hdUE_shared_builds"
        )
        build.setInput(0, settings)
        build.parm("python").set(source(int(start)))
        edit = settings.parent().createNode(
            "pythonscript", "hdUE_shared_build_edits"
        )
        edit.setInput(0, build)
        self.steps_seen = []
        self.summaries = {}

        def step(label, node, meshes, removed=None, glass=(), code=None):
            submissions = self.api.status()["scene_submissions"]
            if code is not None:
                node.parm("python").set(code)
            if removed is not None:
                node.parm("python").set(
                    f"REMOVED = {removed!r}\nGLASS = {list(glass)!r}\n" + EDIT
                )
            node.setDisplayFlag(True)
            node.setCurrent(True, clear_all_selected=True)
            viewer.curViewport().draw()
            self.update()  # Houdini syncs a converged renderer only on interaction (hdue_houdini_test.update)
            began = time.monotonic()
            yield from self.uploaded(meshes, submissions)
            self.steps_seen.append(
                dict(
                    label=label, meshes=meshes, seconds=time.monotonic() - began
                )
            )
            integration._report(f"Shared builds: {label} ({meshes} meshes)")

        def summary(phase, generated):
            # Only a summary written once the phase's last edit is built counts its meshes: earlier ones had others.
            while True:
                lines = [
                    line
                    for line in SUMMARY.findall(session_log())
                    if int(line[7]) == generated
                ]
                if lines:
                    break
                yield from self.idle(1)
            values = dict(zip(FIELDS, map(int, lines[-1]), strict=True))
            self.summaries[phase] = values
            integration._report(f"Shared builds summary ({phase}): {values!r}")
            if values["unconverted"] or values["reserved"]:
                raise AssertionError(
                    "Meshes left unconverted or memory reserved: "
                    + repr(values)
                )
            return values

        yield from step("added", edit, base + len(KEYS) * COPIES, [], [])
        hdefereval.execute_deferred(viewer.curViewport().frameAll)
        # While the builds run: four copies of A and every copy of B go, a copy of C turns translucent (another key).
        removed = copies("A", range(4)) + copies("B", range(COPIES))
        yield from step(
            "removed",
            edit,
            base + len(KEYS) * COPIES - len(removed),
            removed,
            ["/Shared/C0"],
        )
        # The first copy of A again (its key's build may still run) and of B (its build was dropped).
        removed = copies("A", range(1, 4)) + copies("B", range(1, COPIES))
        kept = len(KEYS) * COPIES - len(removed)
        yield from step("re-added", edit, base + kept, removed, ["/Shared/C0"])
        running = yield from summary("running", before + kept)
        if not running["dropped_builds"] or not running["shrunk_builds"]:
            raise AssertionError(
                "The edits did not hit running builds: " + repr(running)
            )
        yield from self.image("02-converted", 0, self.frames[-1]["generation"])

        # Queued: the large mesh's build runs alone under the 1 GB cap, so the keys added after it wait for memory.
        big = settings.parent().createNode("pythonscript", "hdUE_shared_big")
        big.setInput(0, edit)
        big.parm("python").set(source(int(start), BIG, 1, -1, cap=1))
        yield from step("big", big, base + kept + 1)
        queued = settings.parent().createNode(
            "pythonscript", "hdUE_shared_queued"
        )
        queued.setInput(0, big)
        queued.parm("python").set(source(int(start), QUEUED, QUEUED_COPIES, 4))
        queued_edit = settings.parent().createNode(
            "pythonscript", "hdUE_shared_queued_edits"
        )
        queued_edit.setInput(0, queued)
        total = base + kept + 1 + len(QUEUED) * QUEUED_COPIES
        yield from step("queued", queued_edit, total, [], [])
        removed = copies("E", range(1, 4)) + copies("F", range(QUEUED_COPIES))
        yield from step(
            "queued removed",
            queued_edit,
            total - len(removed),
            removed,
            ["/Shared/E0"],
        )
        removed = copies("E", range(1, 4)) + copies(
            "F", range(1, QUEUED_COPIES)
        )
        yield from step(
            "queued re-added",
            queued_edit,
            total - len(removed),
            removed,
            ["/Shared/E0"],
        )
        waited = yield from summary(
            "queued",
            before + kept + 1 + len(QUEUED) * QUEUED_COPIES - len(removed),
        )
        if (
            waited["queued_dropped"] <= running["queued_dropped"]
            or waited["queued_shrunk"] <= running["queued_shrunk"]
        ):
            raise AssertionError(
                "The edits did not hit queued builds: " + repr(waited)
            )
        yield from self.image(
            "03-queued-converted", 0, self.frames[-1]["generation"]
        )

        # A material edited in place: the copies of D drawn with it re-upload when its blend mode changes (Nanite for
        # opaque, a full mesh for translucent), both ways, and not when only its colour does. So does G0, a displaced
        # mesh, whose geometry the delegate keeps after its upload.
        surface = SurfaceApi(self.host.runtime)
        session = self.api.status()["session"]
        displaced = settings.parent().createNode(
            "pythonscript", "hdUE_shared_displaced"
        )
        displaced.setInput(0, queued_edit)
        tinted = "COLOR = (0.5, 0.6, 0.9)\nOPACITY = 1.0\n"
        code = source(int(start), {"G": (40, 80)}, 1, 6) + tinted + DISPLACED
        yield from step(
            "displaced", displaced, self.api.status()["meshes"] + 1, code=code
        )
        meshes = self.api.status()["meshes"]
        tint = settings.parent().createNode("pythonscript", "hdUE_shared_tint")
        tint.setInput(0, displaced)
        copies_d = copies("D", range(COPIES))

        def uploads():
            return surface.status(session)["geometry_uploads"]

        def tint_step(label, color, opacity, expected):
            code = (
                f"PATHS = {copies_d!r}\nCOLOR = {color!r}\nOPACITY = {opacity}\n"
                + TINT
                + DISPLACED
            )
            yield from step(label, tint, meshes, code=code)
            # A re-upload pulls the geometry from Hydra again: it comes with a later submission.
            end = time.monotonic() + 10
            while uploads() < expected and time.monotonic() < end:
                yield from self.idle(1)
                self.update()  # the re-pull needs a Sync, which a converged viewport runs on interaction only
            yield from self.idle(2)
            if uploads() != expected:
                raise AssertionError(
                    f"{label}: {uploads()} geometry uploads, expected {expected}"
                )

        uploaded = uploads()
        # Rebinding to another opaque material and a colour edit keep the meshes as they are.
        yield from tint_step("tint bound", (0.5, 0.6, 0.9), 1.0, uploaded)
        yield from tint_step("tint colour", (0.9, 0.6, 0.5), 1.0, uploaded)
        yield from tint_step(
            "tint translucent", (0.9, 0.6, 0.5), 0.3, uploaded + COPIES + 1
        )
        yield from self.image(
            "04-tint-translucent", 0, self.frames[-1]["generation"]
        )
        yield from tint_step(
            "tint opaque", (0.9, 0.6, 0.5), 1.0, uploaded + 2 * (COPIES + 1)
        )
        yield from self.image(
            "05-tint-opaque", 0, self.frames[-1]["generation"]
        )
        # Converged frames wait for the builds (Bridge.cpp): the newest summary covers the re-uploaded meshes.
        values = dict(
            zip(
                FIELDS,
                map(int, SUMMARY.findall(session_log())[-1]),
                strict=True,
            )
        )
        self.summaries["material"] = values
        integration._report(f"Shared builds summary (material): {values!r}")
        if values["unconverted"] or values["reserved"]:
            raise AssertionError(
                "Meshes left unconverted or memory reserved: " + repr(values)
            )

        problems = [
            line
            for line in session_log().splitlines()
            if "Ensure condition failed" in line
            or "LogStaticMesh: Warning"
            in line  # a material its mesh cannot draw (BindMaterials)
            or re.search(r"\bLog\w+: Error:", line)
        ]
        if problems:
            raise AssertionError("UE log errors:\n" + "\n".join(problems[:20]))
        # New content for the first keys, and Houdini closes half a second into their builds: UE shut down under
        # running builds crashed in a readback copy pass until the scene waited for them.
        generation = self.api.status()["generation"]
        build.parm("python").set(source(int(start) + 1))
        viewer.curViewport().draw()
        self.update()
        while self.api.status()["generation"] == generation:
            yield
        yield from self.idle(0.5)

    def uploaded(self, meshes, submissions):
        """Until the delegate holds that many meshes and has submitted them, then half a second: UE ticks
        continuously, and its next tick after the upload launches their background builds (TickConversions). Frames
        are no signal: interactive frames do not converge or read back while builds are pending.
        """
        nudged = time.monotonic()
        while True:
            status = self.api.status()
            if (
                status["meshes"] == meshes
                and status["scene_submissions"] > submissions
            ):
                break
            # A converged viewport syncs on interaction only: an edit whose LOP cook ends after the step's update()
            # (the large mesh takes seconds) waited for one that never came.
            if time.monotonic() - nudged > 2:
                nudged = time.monotonic()
                self.update()
            yield
        yield from self.idle(0.5)

    def idle(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            yield

    def finish(self):
        # verified.json: what the wrapper checks, from the base test's record and this one's.
        if self.verified is not None:
            self.verified.update(
                shared_builds=getattr(self, "summaries", {}).get("queued"),
                summaries=getattr(self, "summaries", {}),
                steps=getattr(self, "steps_seen", []),
            )
        super().finish()


def start(host):
    global _test
    _test = Test(host)
