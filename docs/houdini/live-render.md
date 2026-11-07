# Live Render and Copernicus

The Live Render LOP renders the stage in the background while you edit it, and sends the image to the Render Gallery. With hdUE, it renders with Unreal Engine; a File COP can then bring that image into Copernicus for compositing.

## Rendering with Live Render

1. Add a **Live Render** LOP after your render settings.
2. Set **Render Delegate** to **HdUnrealEngine**.
3. Set **Render Settings** to your render settings prim, or leave it empty to use the stage's default.
4. Click **Start Render**.

The render reacts to upstream LOP edits, as in the viewport. Click **Stop Render** to end it.

!!! warning "One hdUE render per session"
    A Houdini session renders one hdUE image at a time. Before you start a Live Render with hdUE, switch the Scene Viewer to another renderer, and the other way round.

## In Copernicus

1. Add a **File** COP.
2. Set **Source** to **Live Render**.
3. Set **Live Render LOP** to your Live Render node.

The File COP can start and stop the render itself. See SideFX's [Live rendering in Solaris](https://www.sidefx.com/docs/houdini22.0/solaris/support/liverender.html#copernicus).

!!! note "What the tests check"
    hdUE's tests check that a Live Render converges with its AOVs. They do not check the image that reaches the File COP: check it in your scene.

## Render settings

Live Render reads the same `unreal:*` settings as the viewport and husk. See [Solaris viewport](viewport.md#render-settings) and the [Render settings reference](../reference/render-settings/index.md).
