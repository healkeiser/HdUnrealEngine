"""Ambient Cubemap repro for UE 5.8 (no hdUE): a sphere and a floor lit only by the cubemap.

Run in a blank project (see README.md):
    UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript -script=<this file>
        -AllowCommandletRendering -unattended -nosplash -nop4 -stdout ACINT=10 [CONTROL=1]
Writes <project>/Saved/ac.hdr. ACINT is the cubemap intensity; CONTROL=1 adds a point light.
"""

import time

import unreal

command_line = unreal.SystemLibrary.get_command_line()
intensity = (
    float(command_line.split("ACINT=")[1].split()[0])
    if "ACINT=" in command_line
    else 1.0
)

unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).new_level(
    "/Temp/AC%d" % int(time.time())
)
world = unreal.EditorLevelLibrary.get_editor_world()
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

material = unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial")
for mesh, location, scale in (
    ("/Engine/BasicShapes/Sphere", (0, 0, 60), 1.2),
    ("/Engine/BasicShapes/Plane", (0, 0, 0), 6),
):
    actor = actors.spawn_actor_from_class(
        unreal.StaticMeshActor, unreal.Vector(*location)
    )
    actor.static_mesh_component.set_static_mesh(unreal.load_asset(mesh))
    actor.static_mesh_component.set_material(0, material)
    actor.set_actor_scale3d(unreal.Vector(scale, scale, scale))

if "CONTROL=1" in command_line:
    light = actors.spawn_actor_from_class(
        unreal.PointLight, unreal.Vector(0, 0, 300)
    )
    light.point_light_component.set_intensity(50000)

target = unreal.RenderingLibrary.create_render_target2d(
    world, 256, 256, unreal.TextureRenderTargetFormat.RTF_RGBA16F
)
capture_actor = actors.spawn_actor_from_class(
    unreal.SceneCapture2D,
    unreal.Vector(-400, 0, 200),
    unreal.Rotator(0, -20, 0),
)
capture = capture_actor.capture_component2d
capture.texture_target = target
capture.capture_source = unreal.SceneCaptureSource.SCS_SCENE_COLOR_HDR
capture.capture_every_frame = False

settings = capture.post_process_settings
settings.override_ambient_cubemap_intensity = True
settings.ambient_cubemap_intensity = intensity
settings.ambient_cubemap = unreal.load_asset(
    "/Engine/MapTemplates/Sky/SunsetAmbientCubemap"
)
capture.post_process_settings = settings
capture.post_process_blend_weight = 1.0

for _ in range(3):
    capture.capture_scene()
unreal.RenderingLibrary.export_render_target(
    world, target, unreal.Paths.project_saved_dir(), "ac.hdr"
)
unreal.log(
    "ACREPRO wrote %sac.hdr, cubemap intensity %g"
    % (unreal.Paths.project_saved_dir(), intensity)
)
