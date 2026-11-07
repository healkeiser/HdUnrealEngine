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
- One small scene per kind of effect, chosen so the effect reads: SideFX's lookdev shader ball (`docs/lookdev.usda`) for materials, SideFX's Veach planks (`docs/veach.usda`) for light size against roughness, the chrome ball and box (`docs/lights.usda`) for light types and shadows, the beam in fog (`docs/fog.usda`) for scattering, the row of rubber toys (`docs/row.usda`) for focus, the Bistro (`docs/bistro.usda`, a local download) for interiors and global illumination, the Cornell box for render settings. Keep the camera and lighting fixed across a page.
- Captions are the value only ("0.5"), with "(default)" after the default. Add the render time when the setting costs time.
- Two values: before/after slider. Three to six: the picture with a strip of thumbnails (tap one, or swipe the picture). Pick values that span the useful range, not the full numeric range.
- A figure spec that shows one setting names it: `"settings": ["unreal:postProcess:BloomIntensity"]`. `generate_render_settings.py` and `generate_node_docs.py` then show the figure under that setting in the reference and on the node pages. The reference's own figures (`docs/figures/ref-*.json`) come from one table in `scripts/generate_reference_figures.py`: add a row there, run it, render the new figure, then rerun both generators.
- Scenes built in Houdini are written by scripts, so they can be rebuilt: `scripts/build_docs_box.py` (the light study's bevelled box) and `scripts/build_docs_volumes.py` (the mist volumes).
- Houdini UI screenshots only where the step cannot be said in words (a network to wire, a menu to find), and cropped to the panel. They live in `docs/images/houdini/`, taken in a `Launch-Houdini.ps1 -InputUsd samples/cornell/cornell.usda` session with the window at 1920 × 1080, Unreal Engine picked in the Scene Viewer, and each pane grabbed from the screen (a floating pane for a wider parameter editor); crop out personal shelves and menus.

## Parameters and formulas

- A page's own parameters sit in the parameter grid the node pages use: the name beside a facts strip (USD attribute, Default, Range, badges such as "Realtime only"). Copy a block from `docs/lights.md`: `<div class="hdue-params" markdown>`, one `hdue-param` per parameter, each value in `<dd markdown="span">` so code and links work. The reference and node pages write the same markup from `generate_render_settings.py` and `generate_node_docs.py`.
- A formula is math, not prose with ×: `$\text{Intensity} \times 2^{\text{Exposure}}$` inline, or `$$ … $$` on lines of its own for one the sentence leads to. MathJax typesets it (Zensical's math setup, `pymdownx.arithmatex`).

## Diagrams

Every Mermaid diagram looks the same: `docs/assets/hdue-mermaid.js` draws it in the site's colours, and `docs/architecture.md` is the model.

- Rounded nodes, `A("…")`, with a Lucide icon before the text: `"fal:fa-monitor Solaris viewport"` (`fal` is Lucide here, the prefix Mermaid reads in a label). Decisions stay diamonds, `{"…?"}`, without an icon.
- Colour comes from four classes, never from a `style` or a colour in a `classDef`: `host` for Houdini and USD, `hdue` for the delegate, `unreal` for Unreal Engine, `output` for images and files. Declare them with `classDef host stroke-width:1px` (and the same for the others), then `class S,V host`.
- Group by process or side in a `subgraph` with the same class and an icon in its title.
- The site's gallery and README are rendered by scripts; a diagram is the one figure written by hand.

## Houdini node reference

`docs/reference/nodes/` is generated: `hython scripts/generate_node_docs.py` turns each digital asset's help card (its Help section, written by `build_render_settings_hda.py` and `build_substrate_hda.py`, and shown by Houdini on F1) into a page like SideFX's node help, and writes its navigation into `zensical.toml`. Run it after rebuilding an asset; to change a node's page, change its builder's help text, never the generated Markdown.

## Writing

- Second person, present tense, imperative for steps: "Set **Exposure** to 2."
- Sentences under about 20 words. One idea per sentence.
- Name parameters as the UI does, in bold, with the attribute in code the first time: **Fog Density** (`unreal:heightFog:FogDensity`).
- Numbers, not adjectives: "doubles the render time", "1 stop brighter", not "slower", "brighter".
- Say what UE does, citing the UE setting it maps to. Do not explain hdUE's internals on a user page.
- Every claim must be true of the current build. When a behaviour is not verified, leave it out or say so.
- No marketing words. No "simply", "just", "easily".
