# Writing the user guide

The user guide is written for the artist or TD rendering with hdUE in Houdini, not for hdUE's developers. Its model is Autodesk's Arnold User Guide: short reference pages built around images, and tutorials named after the result they produce. Implementation details (how hdUE drives UE, why a code path exists) go to the Developer pages.

## Two kinds of pages

**Reference pages** describe one feature: a light, the camera, a material model, a render setting group.

1. Title, then one or two sentences saying what the feature is, then a hero image.
2. **Using it in Solaris**: the LOP or parameter that sets it, as numbered steps when there is more than one.
3. **Parameters**: one `##` section per parameter, in the order the UI shows them. Each section:
    - the parameter's label in bold, its `unreal:*` attribute or USD property, and its default;
    - one to three sentences: what it does, its unit, what raising it costs;
    - a figure when the effect is visible: a slider for two values, a scrubber for more, the default marked;
    - a tip or a note only when there is a symptom, a cause and a fix to give.
4. **Recipes** ("How to get…"): exact values for an effect, ready to type in.
5. **Troubleshooting**: symptom, cause, fix. One line each where possible.
6. **Unreal Engine and Karma**: where hdUE follows UE and where the result differs from Karma, briefly.

**Tutorials** produce one result: "Lighting a room", "Removing noise", "Glass and liquids".

1. The final image first, then what the reader will build and roughly how long it takes.
2. The starting scene: a file in `samples/` the reader opens.
3. `##` sections, one per stage (Lights, Materials, Render settings, Final render). Each holds short imperative steps with exact values, each followed by one sentence of why.
4. A render after each stage that changes the image.
5. **Final render settings**, as a short list.

## Images

- Every comparison is rendered by `scripts/render_docs_images.py` from a scene in `samples/`, so a rebuild refreshes it. No hand-made screenshots of renders.
- One small scene per kind of effect, chosen so the effect reads: the shader ball for materials, the chrome ball for light shapes, the beam in fog for scattering, the row of spheres for focus, the room for global illumination, the Cornell box for render settings. Keep the camera and lighting fixed across a page.
- Captions are the value only ("0.5"), with "(default)" after the default. Add the render time when the setting costs time.
- Two values: before/after slider. Three to six: scrubber. Pick values that span the useful range, not the full numeric range.
- Houdini UI screenshots only where the step cannot be said in words (a network to wire, a menu to find), and cropped to the panel.

## Writing

- Second person, present tense, imperative for steps: "Set **Exposure** to 2."
- Sentences under about 20 words. One idea per sentence.
- Name parameters as the UI does, in bold, with the attribute in code the first time: **Fog Density** (`unreal:heightFog:FogDensity`).
- Numbers, not adjectives: "doubles the render time", "1 stop brighter", not "slower", "brighter".
- Say what UE does, citing the UE setting it maps to. Do not explain hdUE's internals on a user page.
- Every claim must be true of the current build. When a behaviour is not verified, leave it out or say so.
- No marketing words. No "simply", "just", "easily".
