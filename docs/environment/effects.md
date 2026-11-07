# Game effects (Niagara)

hdUE renders UE's Niagara effects: fire, sparks, smoke, lightning, explosions, the effects games use. You make or buy them in UE's editor, cook them into hdUE's runtime, and place them on a prim. hdUE simulates each one to the frame you render, as Sequencer plays a Niagara track, so a husk sequence and the Solaris viewport show the effect at the same moment of its life.

Three effects from Epic's free Niagara Examples Pack at frame 48 (2 s), in Realtime:

<div class="hdue-figure" data-figure="effects-render-mode"></div>

## Bringing effects into hdUE

hdUE renders cooked UE assets, as it does cooked meshes and Blueprints (see [cooked native assets](../materials.md#cooked-native-assets)). The **hdUE Unreal Asset** LOP cooks them for you from a UE project:

1. Put the effects in a UE 5.8 project. For a Fab pack, use **Add to project** in the Epic Games Launcher; a pack made for an earlier UE version goes into a 5.8 project when you pick the closest version in **Select Version**.
2. Add an **hdUE Unreal Asset** LOP (TAB menu > hdUE > Scene). Set **UE Project** to the project's `.uproject` and pick the effect in **Asset**: the menu lists the project's levels and the assets named with UE's prefixes (`NS_` for Niagara Systems, `SM_`, `SK_`, `BP_`, `SVT_`).
3. Press **Import**. UE's editor cooks the effect and what it uses in the background (about a minute for one effect) and adds it to the running hdUE runtime: no restart. **Imported** turns on, and the prim renders the effect.

Your project is not changed: its content is copied into hdUE's UE project (`unreal/HdUEBackend/Content`) to cook. Import needs UE's editor (`editor_root` in `.local/paths.json`), as hdUE's Substrate materials do.

For a whole project at once, `scripts/Prepare-ProjectAssets.ps1` cooks everything into a new runtime (point Houdini at the `runtime.json` it writes). It stops on assets that fail to compile; demo levels and editor tools that come with a pack often do: delete them from `unreal/HdUEBackend/Content` (never from your project) and run it again with `-SkipCopy`.

## Placing an effect

=== "Houdini"

    The **hdUE Unreal Asset** LOP places what it imports: set **Translate**, **Rotate** and **Scale**, and **Effect Start Frame**, the frame the effect starts at. Its prim is a small box hdUE replaces with the effect.

    For a prim of your own, use a **Render Geometry Settings** LOP: on its **Unreal Engine** tab, set **Native Asset** to the effect (the menu lists the runtime's Niagara Systems with its other cooked assets) and **Effect Start Frame**.

=== "USD"

    ```usda
    def Mesh "fire"
    {
        string primvars:unreal:asset = "/Game/NiagaraExamples/FX_Misc/NS_Fire.NS_Fire" (interpolation = "constant") # (1)!
        float primvars:unreal:effect:startFrame = 1 (interpolation = "constant") # (2)!
        point3f[] points = [(-0.1, 0, -0.1), (-0.1, 0, 0.1), (-0.1, 0.2, -0.1), (-0.1, 0.2, 0.1), (0.1, 0, -0.1), (0.1, 0, 0.1), (0.1, 0.2, -0.1), (0.1, 0.2, 0.1)]
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 2, 3, 1, 4, 5, 7, 6, 0, 1, 5, 4, 2, 6, 7, 3, 0, 4, 6, 2, 1, 3, 7, 5]
        double3 xformOp:translate = (0, 0, 0.6)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    ```

    1. The Niagara System's UE object path in the runtime project. The mesh is a placeholder: the effect renders in its place.
    2. The frame the effect starts at. Without it, it starts at frame 0.

The effect sits at the prim's pivot, in UE's own size: an effect authored in UE is the same size in a metre stage and in a centimetre stage.

## Time

The effect's age is the frame rendered minus **Effect Start Frame**, in seconds at the scene's frame rate. hdUE drives it as Sequencer does (`MovieSceneNiagaraSystemTrackTemplate.cpp`): the component runs alone (solo), its age is set from the scene's time, and it simulates in steps of one frame.

The same effect at four ages:

<div class="hdue-figure" data-figure="effects-age"></div>

- Before its start frame the effect has not started; at the start frame it has just begun.
- Going to a later frame simulates forward from where it is. Going back restarts it and simulates up to that frame again, so scrubbing backwards over a long shot is slower than playing forwards.
- The frame waits for the simulation: a render of frame 200 of an effect that starts at 1 first simulates 200 frames.
- Effects move by whole frames: motion blur's sub-frames see the frame's state, so an effect is sharp where the rest of the frame is blurred, as with Sequencer's frame-locked playback.
- A **Velocity** AOV makes hdUE render the frame before each frame first, which restarts the effects: each frame then simulates them from their start, slower on long shots.

**Repeatability.** Niagara picks new random numbers each time an effect starts, unless the effect asks for the same ones: in UE's editor, turn on **Determinism** in the Niagara System's properties (and on its emitters). Without it, rendering the same frame twice gives different flames, as it does in UE's Movie Render Queue.

## Render modes

| | Realtime | Path Tracer |
| --- | --- | --- |
| Effect renders | yes | yes |
| Lights the effect carries (Niagara light renderer) light the scene | yes | no |

Measured on the scene above (`samples/docs/effects.usda`): in the Path Tracer the fire renders, but no longer lights the floor or the box.

## Effects that need a game

Some effects in game packs only start when gameplay tells them to: impacts, footsteps and pickups that wait for an event, effects that read a character's skeleton, or effects fed by Niagara Data Channels. Placed on a prim, they render nothing or only their first burst. Looping effects (fire, smoke, electricity, ambient sparks) and effects that start on their own work as they do in the editor.

Effects that burst once at their start (an explosion, a firework) are over a second or two later: set **Effect Start Frame** a few frames before the frame you want to see.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The effect is not in the Native Asset menu | The runtime does not have it yet | Import it with an **hdUE Unreal Asset** LOP |
| Nothing renders at the start frame | At age 0 the effect has emitted nothing yet | Render a later frame, or set **Effect Start Frame** earlier |
| A frame looks different each time it renders | The effect is not deterministic | Turn on **Determinism** on the Niagara System in UE's editor |
| The effect renders nothing at any frame | It waits for a game event or data channel | Use a looping effect, or one that starts on its own |
| `Prepare-ProjectAssets.ps1` or **Import** stops on Blueprint or Control Rig errors | A pack's demo level, characters or editor tools need plugins hdUE's project leaves out | Delete those assets from `unreal/HdUEBackend/Content` and run it again with `-SkipCopy` |
