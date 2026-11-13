# Plan 005: Integer menus in hdUE's render settings author the value they show

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- scripts/generate_render_settings.py integrations/houdini/soho/parameters/ integrations/houdini/otls/hdue_rendersettings.hda integrations/houdini/otls/hdue_postprocessvolume.hda src/hydra/delegate.cpp samples/conformance/render_settings/`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: MED
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

In Houdini, an integer parameter with a menu stores the **position** of the chosen item (0, 1, 2, …) unless the menu is marked "use token as value". hdUE generates six integer menus whose item values are not 0, 1, 2, …, and none of them is marked. So the value the user picks is not the value hdUE receives:

| Setting (USD attribute) | User picks | hdUE receives today |
|---|---|---|
| Environment Cube Resolution (`unreal:domeResolution`) | 256 | 3 |
| Anti-Aliasing Method (`unreal:cvar:r:AntiAliasingMethod`) | TSR (4) | 3 (MSAA) |
| DLSS Preset (`unreal:cvar:r:NGX:DLSS:Preset`) | Preset K (11) | 3 |
| Ray Reconstruction Preset (`unreal:cvar:r:NGX:DLSSRR:Preset`) | Preset F (6) | 3 |
| DLSS Alpha Upscaling (`unreal:cvar:r:NGX:DLSS:EnableAlphaUpscaling`) | Off (0) | 1 (On) |
| MegaLights Samples per Pixel (`unreal:cvar:r:MegaLights:NumSamplesPerPixel`) | 2 | 1 |

The worst case is the dome resolution: the backend rejects any value that is not a power of two from 32 to 2048, and that rejection fails the **whole light batch**, so every light in the scene disappears. Defaults are not affected (the hdUE Render Settings LOP only authors a value that differs from its default), so the bug appears the moment a user changes one of these menus. After this plan, every generated integer menu authors the number it shows, and a scene that already stored a bad dome resolution keeps its lights.

## Current state

Files and their roles:

- `scripts/generate_render_settings.py` — generates the Houdini dialog-script (`.ds`) parameter files, the schema, the reference docs and `src/hydra/light_properties.h` from the UE source. Writes each parameter in `ds_parm` (lines 2112–2209).
- `integrations/houdini/soho/parameters/HdUnrealEngine_Global.ds`, `HdUnrealEngine_Viewport.ds` — generated output (only the `/// BEGIN generated …` / `/// END generated` blocks are generated). Never edit by hand.
- `scripts/build_render_settings_hda.py` — builds the hdUE LOP HDAs (`integrations/houdini/otls/hdue_rendersettings.hda` and others) by copying the parameter templates Houdini loads from the `.ds` files (`build()` at line 268 reads `settings.parmTemplateGroup().findFolder("Unreal Engine")` from a stock Render Settings LOP). Fixing the `.ds` and rebuilding is enough; no builder change is needed.
- `src/hydra/delegate.cpp` — reads `domeResolution` (line 1247) and passes it to every light.
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Lighting.cpp` — validates it (lines 158–162). Out of scope; read only.

How an integer menu is written today, `scripts/generate_render_settings.py:2178-2182`:

```python
    if setting["menu"]:
        items = " ".join(
            f"{ds_string(str(v))} {ds_string(l)}" for v, l in setting["menu"]
        )
        lines.append(f"    menu {{ {items} }}")
```

`ds_parm` maps both `"int"` and `"intmenu"` kinds to `type int` (lines 2116–2126). Integer console-variable menus become kind `"intmenu"` in `console_settings` (line 2032–2033):

```python
        elif options.get("menu"):
            kind, value = "intmenu", [int(float(default))]
```

and `domeResolution` is declared as `"intmenu"` directly (lines 983–995):

```python
    hdue(
        "domeResolution",
        "Environment Cube Resolution",
        "Lighting|Dome and Viewer Lights",
        "intmenu",
        # UE's USkyLightComponent::CubemapResolution (SkyLightComponent.cpp).
        128,
        "Face size of the cubemap dome lights are baked into, for lighting, reflections and the background.",
        menu=[(v, str(v)) for v in (32, 64, 128, 256, 512, 1024, 2048)],
```

The generated result, `HdUnrealEngine_Global.ds:3385-3390`:

```
                name    "xn__unrealdomeResolution_n8a"
                label   "Environment Cube Resolution"
                type    int
                size    1
                default { 128 }
                menu { "32" "32" "64" "64" "128" "128" "256" "256" "512" "512" "1024" "1024" "2048" "2048" }
```

The dialog-script keyword Houdini 22 writes for "Use Menu Item Token As Value" (checked in hython 22.0.368 with `hou.IntParmTemplate(..., menu_use_token=True)` inside a `hou.ParmTemplateGroup` and `group.asDialogScript()`):

```
        menu usetokenvalue {
            "32"    "32"
            "128"   "128"
        }
```

A plain `menu {` means "store the index". Houdini's doc (`hom/hou/IntParmTemplate`): `menuUseToken()` — "Return whether the parameter is set to the token value instead of the index."

The six affected menus (verified in hython: the hdUE Render Settings HDA has 21 integer menus, 6 of them have tokens that are not `0..n-1`, all 6 have `menuUseToken() == False`):

- `xn__unrealdomeResolution_n8a` — tokens 32…2048
- `xn__unrealcvarrAntiAliasingMethod_bobeb` — 0, 1, 2, 4
- `xn__unrealcvarrNGXDLSSPreset_5fbebde` — 0, 6, 10, 11, 12, 13
- `xn__unrealcvarrNGXDLSSRRPreset_fjbebdg` — 0, 4, 5, 6
- `xn__unrealcvarrNGXDLSSEnableAlphaUpscaling_y2bebde` — -1, 0, 1
- `xn__unrealcvarrMegaLightsNumSamplesPerPixel_l4bebk` — 1, 2, 4

The other 15 integer menus (`sg.*` scalability 0–4, and menus such as `(0, "PCF"), (1, "PCSS")`) have tokens equal to their positions, so marking them `usetokenvalue` changes nothing for them. Mark every `intmenu` the same way; it is simpler and protects future menus.

The delegate today, `src/hydra/delegate.cpp:1247`:

```cpp
    const auto domeResolution = uint32_t(state.setting<int>("domeResolution", 128));
```

The backend's check, `Lighting.cpp:158-162` (any light with a bad value fails the batch):

```cpp
        if (D.struct_size != sizeof(D) || D.type > HDUE_CYLINDER_LIGHT || D.visible > 1 || (D.flags & ~31u) ||
            (D.lighting_channels & ~uint32(HDUE_LINK_CHANNELS)) || !Text(D.id) || D.cube_resolution < 32 ||
            D.cube_resolution > 2048 || !FMath::IsPowerOfTwo(D.cube_resolution) || D.property_count > 1024 ||
            (D.property_count && (!D.property_names || !D.property_values))) {
            Error = TEXT("Unsupported light descriptor or environment resolution.");
```

Existing pattern for a once-only warning of a bad render setting, `delegate.cpp` `HdUEWarnRenderingColorSpace` (around line 3537): a `static std::once_flag` and `TF_WARN("hdUE: …")`. husk shows hdUE warnings as `hdUE warning: <message>` on stderr (`batchWarning`, line 185). Match that pattern.

Conformance scenes with a log check, to copy: `samples/conformance/render_settings/rendering_color_space.usda` (a `"log": "hdUE warning:.*renderingColorSpace"` check plus a coverage check). Every scene sublayers `samples/conformance/common/stage.usda`, which has one distant light, so a failed light batch is visible.

Compatibility note: `.hip` files saved before this fix that changed one of these menus stored the **index** (for example 3 for "256"). After the fix the same stored number is read as a token. For the five console variables the stored index becomes some other valid-looking or invalid value; for `domeResolution` it is always invalid (3 is not a power of two ≥ 32). This plan guards `domeResolution` in the delegate (warn once, use UE's 128); the console variables are passed to UE as they are, as today.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Regenerate `.ds`, schema, docs | `python scripts/generate_render_settings.py` (engine root comes from `.local/paths.json`, key `engine_root`) | exit 0; prints setting counts; no `skipped` line new compared to before |
| Rebuild HDAs | `python scripts/build_render_settings_hda.py` (relaunches itself in hython) | exit 0; prints `<hda>: N parameters` per asset |
| Regenerate node docs | `<houdini_root>\bin\hython.exe scripts\generate_node_docs.py` (`houdini_root` from `.local/paths.json`) | exit 0 |
| Build delegate + native tests | `.\scripts\Build-Houdini.ps1` (Houdini must be closed) | exit 0; ctest passes |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | ends with no thrown error |
| Conformance (targeted) | `<houdini_root>\bin\hython.exe scripts\run_conformance.py --runtime-config (Resolve-Path .local\runtime-houdini.json) --only render_settings/*` | "0 failing runs" |
| Python format/lint | `uvx black@24.10.0 --check scripts tests integrations` ; `uvx ruff@0.7.4 check scripts tests integrations` | exit 0 |
| C++ format | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp` | exit 0 |

## Suggested executor toolkit

- If available, the `vfx:houdini-22` skill for dialog-script and `hou.ParmTemplate` questions.

## Scope

**In scope** (the only files you should modify):
- `scripts/generate_render_settings.py`
- Generated outputs it rewrites: `integrations/houdini/soho/parameters/HdUnrealEngine_Global.ds`, `HdUnrealEngine_Viewport.ds`, `HdUnrealEngine_Light.ds`, `resources/generatedSchema.usda`, `src/hydra/light_properties.h`, `docs/reference/**` (only through the generator)
- HDAs rebuilt by `scripts/build_render_settings_hda.py` under `integrations/houdini/otls/` (only through the builder)
- `docs/reference/nodes/**` and `zensical.toml` (only through `generate_node_docs.py`)
- `src/hydra/delegate.cpp` (the `domeResolution` guard)
- `samples/conformance/render_settings/dome_resolution_invalid.usda` (create)

**Out of scope** (do NOT touch, even though they look related):
- Any `.ds` or `.hda` edited by hand — they are generated (AGENTS.md).
- `unreal/HdUEBackend/**` — the backend's validation is correct; the delegate must send valid values.
- `scripts/build_render_settings_hda.py` source — it copies templates from the `.ds`; no change needed. If Step 3 shows the rebuilt HDA still has `menuUseToken() == False`, STOP.
- The other default-value drift (`autoViewerLight`, 64 samples) — plan 013.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, on `main` (AGENTS.md: `main` is the only branch).
- One commit per logical step. Subjects are plain English sentences in the repo's style, for example `Integer render setting menus author the value they show`. The body says why and how it was verified, and ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless the operator told you to.

## Steps

### Step 1: Write `menu usetokenvalue` for integer menus in the generator

In `ds_parm` (`scripts/generate_render_settings.py`, around line 2178), write `menu usetokenvalue { … }` when `kind == "intmenu"`, and keep `menu { … }` for string menus (`kind == "menu"`, which are `type string` and store the token already). Add a one-line comment saying why (an int menu stores its position unless the token is used).

Target shape:

```python
    if setting["menu"]:
        items = " ".join(
            f"{ds_string(str(v))} {ds_string(l)}" for v, l in setting["menu"]
        )
        # An int menu stores the item's position unless its token is the value (Houdini's "Use Token as Value").
        keyword = "menu usetokenvalue" if kind == "intmenu" else "menu"
        lines.append(f"    {keyword} {{ {items} }}")
```

**Verify**: `python -c "import sys; sys.path.insert(0, 'scripts'); import generate_render_settings as g; s=[x for x in g.HDUE if x['attribute']=='unreal:domeResolution'][0]; print([l for l in g.ds_parm(s, '', False) if 'menu' in l])"` → one line containing `menu usetokenvalue {`.

### Step 2: Regenerate the dialog scripts

Run `python scripts/generate_render_settings.py`.

**Verify**:
- `git diff --stat` → changes in the three `HdUnrealEngine_*.ds` files (and possibly docs/schema only if their content depends on it; they should not).
- `grep -c "menu usetokenvalue" integrations/houdini/soho/parameters/HdUnrealEngine_Global.ds` → 21 (every integer menu; if the number differs, count `type    int` parms that have a `menu` line and make sure the two counts match).
- `grep -n "menu usetokenvalue" integrations/houdini/soho/parameters/HdUnrealEngine_Viewport.ds` → the integer menus of the Engine Quality block (at least the `r.AntiAliasingMethod` one).
- `git diff integrations/houdini/soho/parameters/ | grep '^[-+]' | grep -v 'menu' | grep -v '^[-+][-+]'` → no output (only menu lines changed).

### Step 3: Rebuild the HDAs and check the menus in Houdini

Run `python scripts/build_render_settings_hda.py`, then `hython scripts/generate_node_docs.py`.

Then run this read-only check (save it in a temp folder outside the repo, run it with hython; it saves nothing):

```python
import hou
hda = r"<repo>/integrations/houdini/otls/hdue_rendersettings.hda"
hou.hda.installFile(hda)
for d in hou.hda.definitionsInFile(hda):
    def walk(ts):
        for t in ts:
            if t.type() == hou.parmTemplateType.Folder:
                yield from walk(t.parmTemplates())
            else:
                yield t
    ints = [t for t in walk(d.parmTemplateGroup().entries())
            if t.type() == hou.parmTemplateType.Int and t.menuItems()]
    bad = [t.name() for t in ints
           if not t.menuUseToken()
           and t.menuItems() != tuple(str(i) for i in range(len(t.menuItems())))]
    print(d.nodeTypeName(), "int menus:", len(ints), "index-stored with non-index tokens:", len(bad), bad)
```

**Verify**: output line for `hdue::hduerendersettings::1.0` → `int menus: 21 index-stored with non-index tokens: 0 []`. Also check one value: add to the script `n = hou.node("/stage").createNode(d.nodeTypeName()); p = n.parm("xn__unrealdomeResolution_n8a"); p.set(256); print(p.eval())` → `256` (with `usetokenvalue`, `set` and `eval` use the token).

### Step 4: Guard `domeResolution` in the delegate

In `prepareLights` (`src/hydra/delegate.cpp`, line 1247), accept only a power of two from 32 to 2048 (the backend's rule). For any other value, warn once and use UE's default 128, so old scenes keep their lights. Follow the `HdUEWarnRenderingColorSpace` pattern (static `std::once_flag`, `TF_WARN("hdUE: …")`). The message must name the setting and say why it is probably wrong, for example: `hdUE: domeResolution %d is not a power of two from 32 to 2048 (scenes saved before hdUE 0.5.1 stored the menu position); using 128`. Use the next version number the repo will ship if you can tell it; otherwise say "older scenes".

Target shape:

```cpp
    int requested = state.setting<int>("domeResolution", 128);
    // UE bakes a power-of-two cube from 32 to 2048 (Lighting.cpp rejects the whole light batch otherwise); older
    // hdUE Render Settings LOPs authored the menu position instead of the value.
    if (requested < 32 || requested > 2048 || (requested & (requested - 1))) {
        static std::once_flag warned;
        std::call_once(warned, [&] { TF_WARN("hdUE: domeResolution %d ...; using 128", requested); });
        requested = 128;
    }
    const auto domeResolution = uint32_t(requested);
```

**Verify**: `.\scripts\Build-Houdini.ps1` → exit 0, ctest all pass. `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp` → exit 0.

### Step 5: Add a conformance scene for an invalid dome resolution

Create `samples/conformance/render_settings/dome_resolution_invalid.usda`, modelled on `rendering_color_space.usda` (same stage metadata, `subLayers = [@../common/stage.usda@]`, a `Cube "Box"` of size 1.5). It authors `int unreal:domeResolution = 3` on `over "Render" { over "Settings" { … } }`. Checks:
1. `"log": "hdUE warning:.*domeResolution"`, `"present": true`.
2. A `"value"` check on the box region `[-0.5, -0.5, 0.5, 0.5]` with the default `"luma"` stat and a `"min"`: run the scene once **with `unreal:domeResolution = 128`** to read the box's luma from the report, then set `"min"` to half of it. This proves the key light renders.

**Verify**:
- Before Step 4's build (or by temporarily reverting it), the scene fails (no lights: the luma check fails or the frame errors). Skip this if rebuilding twice is too slow; the warning check alone proves the guard runs.
- `hython scripts\run_conformance.py --runtime-config (Resolve-Path .local\runtime-houdini.json) --only render_settings/dome_resolution_invalid` → "0 failing runs".

### Step 6: Run the targeted checks

**Verify**:
- `hython scripts\run_conformance.py --runtime-config (Resolve-Path .local\runtime-houdini.json) --only render_settings/*` → "0 failing runs".
- `hython scripts\run_conformance.py --runtime-config (Resolve-Path .local\runtime-houdini.json) --only lux/*` → "0 failing runs" (dome lights use the cube resolution).
- `.\scripts\Test.ps1 -Suite unit` → no error.
- `uvx black@24.10.0 --check scripts tests integrations` and `uvx ruff@0.7.4 check scripts tests integrations` → exit 0.

## Test plan

- New conformance scene `samples/conformance/render_settings/dome_resolution_invalid.usda` (Step 5): the guard warns and lights still render. Pattern: `samples/conformance/render_settings/rendering_color_space.usda`.
- The hython check of Step 3 is the regression check for the menu fix. It needs Houdini, so it is not added to CI; if plan 010 (test tiers) has landed, add it to the Houdini suite there, otherwise leave it as the documented manual check in "Maintenance notes".

## Done criteria

- [ ] `grep -c "menu usetokenvalue" integrations/houdini/soho/parameters/HdUnrealEngine_Global.ds` equals the number of integer menus (21 at planning time)
- [ ] The Step 3 hython check prints `index-stored with non-index tokens: 0` for `hdue::hduerendersettings::1.0`, and setting the dome menu to 256 evaluates to 256
- [ ] `.\scripts\Build-Houdini.ps1` exits 0 (ctest passes)
- [ ] `run_conformance.py --only render_settings/*` and `--only lux/*` report 0 failing runs, including `dome_resolution_invalid`
- [ ] `.\scripts\Test.ps1 -Suite unit` exits without error; Black and Ruff checks pass
- [ ] `git status` shows only in-scope files changed
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- The excerpts above do not match the live code (drift since `6fe3e12`).
- After Step 3 the rebuilt HDA still reports `menuUseToken() == False` for any of the six menus: then the builder does not keep the `.ds` flag, and `scripts/build_render_settings_hda.py` must change, which this plan does not cover.
- Regenerating (Step 2) changes lines other than menu lines in the `.ds` files, or changes `resources/generatedSchema.usda` / `docs/reference/**` beyond what Step 1 explains. The generator output must otherwise be identical; a different output means the engine source or the generator drifted.
- The generator prints a new `skipped` line or exits with an error.
- `run_conformance.py` reports a failure outside `dome_resolution_invalid` that also fails on `6fe3e12`: report it, don't fix it here.

## Maintenance notes

- Any new integer setting with a menu must use kind `"intmenu"` so it gets `usetokenvalue`. A future `int` setting that gets a `menu` but kind `"int"` would fall back to index storage; a reviewer should check the kind.
- `src/hydra/delegate.cpp`'s `domeResolution` guard duplicates the backend's rule (`Lighting.cpp`). If the backend ever accepts other sizes, update both.
- The five console-variable menus have no guard: a scene saved before this fix that changed one of them now sends its old index as the value (for example `r.AntiAliasingMethod = 3`, MSAA). That is what such a scene already sent, so nothing gets worse; mention it in the release notes so users re-pick those menus.
- Reviewers: check the generated diff is menu-only, and that the hython check was run on the rebuilt HDA, not the old one.
