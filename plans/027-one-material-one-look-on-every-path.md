# Plan 027: One material renders the same whether hdUE translates it at render time or UE's importer prepares it

> **Executor instructions**: This is a design-and-decision plan. Part A records
> decisions with the maintainer; Part B implements only the decided items.
> Follow the steps in order. Run every verification command and confirm the
> expected result before moving on. If anything in the "STOP conditions"
> section occurs, stop and report — do not improvise. When done, update the
> status row for this plan in `plans/README.md` — unless a reviewer
> dispatched you and told you they maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/materials.cpp src/hydra/color_management.cpp src/hydra/prims.cpp src/hydra/delegate.cpp scripts/usd_materialx_graph.py scripts/export_usd_materialx.py scripts/materialx_lowering.py unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEContent/Private/`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P3
- **Effort**: M
- **Risk**: MED (looks change in existing scenes; each change is a policy choice)
- **Depends on**: `plans/011-render-time-materialx-matches-the-spec.md`, `plans/012-texture-mips-and-cache-keys.md` (they fix the clear-cut defects in the same files first)
- **Category**: bug (design)
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE has two ways to turn a USD material into a UE material:

- **Render-time translation**: C++ in `src/hydra/materials.cpp` reads UsdPreviewSurface, `standard_surface` and OpenPBR into a fixed set of parameters on hdUE's parent materials.
- **Preparation**: graphs the render-time translation cannot express are written as MaterialX (`scripts/export_usd_materialx.py`, rewritten by `scripts/materialx_lowering.py`) and imported by UE's Interchange MaterialX importer in a background editor process.

A material can move from one path to the other when a user adds one node. Today several rules differ between the paths, or between hdUE and UE's own USD importer, so the look changes when it should not. This plan lists each difference with what UE does, what the spec says, and a recommendation. The repo's rule decides: **what Unreal does first (its USD importer, Interchange), then the USD/MaterialX spec, then Karma**. Some items change existing scenes; those need the maintainer's decision before code changes.

## Current state

Paths: `<engine_root>` is `engine_root` in `.local/paths.json`; UE plugin sources are under `<engine_root>/Engine/Plugins`. The OpenUSD spec quoted is the local OpenUSD 26.11 mirror, `C:/Users/valen/.claude/plugins/data/vfx-healkeiser/openusd-26-11/docs/openusd/spec_usdpreviewsurface.md`.

### D1. Wrap mode of `UsdUVTexture` when `wrapS`/`wrapT` are not authored

hdUE — `src/hydra/materials.cpp:262-279`: an unauthored wrap becomes `useMetadata`, which falls to `HDUE_WRAP_BLACK`; the backend then masks texels outside [0, 1] (`<Slot>Black`):

```cpp
            // ponytail: useMetadata (UsdUVTexture's default) does not read the file's wrap metadata; it falls back
            // to black, as the spec says for files without it.
            const auto wrap = [&](const char* input, const char* fallback) {
                auto mode = token(param(node, input)).GetString();
                if (mode.empty()) mode = fallback;
                return mode == "repeat" || mode == "periodic" ? HDUE_WRAP_REPEAT
                       : mode == "clamp"                      ? HDUE_WRAP_CLAMP
                       : mode == "mirror"                     ? HDUE_WRAP_MIRROR
                                                              : HDUE_WRAP_BLACK;
            };
            if (!starts("ND_tiledimage_")) {
                t.wrap[0] = wrap(usd ? "wrapS" : "uaddressmode", usd ? "useMetadata" : "periodic");
```

UE's USD importer — `<engine_root>/Engine/Plugins/Runtime/USDCore/Source/USDUtilities/Private/USDShadeConversion.cpp:584-607`: unauthored is `TA_Wrap` (repeat); authored `repeat` → wrap, `mirror` → mirror, anything else (`clamp`, `black`, `useMetadata`) → clamp ("We also consider the "black" wrap mode as clamp as that is the closest we can get").

Spec — `spec_usdpreviewsurface.md:382-397`: default `useMetadata`; with no metadata in the file, fall back to `black`.

Recommendation: follow UE (rule order). Unauthored → repeat. `useMetadata` and `black` → the decision below. **Needs the maintainer's decision**: hdUE can render true black (spec) where UE clamps. Options: (a) match UE fully (black → clamp), (b) unauthored → repeat as UE, keep authored `black` as true black (closer to the spec, which UE cannot do). Recommended: (b) — it matches UE for the common case (unauthored) and keeps a spec feature hdUE implements.

### D2. Colour space of MaterialX image files

Render time — `src/hydra/materials.cpp:255-261` and `src/hydra/color_management.cpp:135-142`: an untagged image gets `HDUE_TEXTURE_ENCODING_AUTO` (sRGB for colour slots, linear for data); any name containing "srgb" without "lin" decodes as sRGB:

```cpp
bool HdUESRGBColorSpace(const std::string& space) {
    const auto lower = TfStringToLower(space);
    return lower.find("srgb") != std::string::npos && lower.find("lin") == std::string::npos;
}
```

Prepared path — `scripts/export_usd_materialx.py:242` sets the document colour space to `lin_rec709`, and `:362-378` copies an authored image colour space through unchanged (`port.setColorSpace(str(colorspace))`, `raw` for normal maps).

UE's Interchange MaterialX importer — `<engine_root>/Engine/Plugins/Interchange/Runtime/Source/Import/Public/MaterialX/MaterialXUtils/MaterialXSurfaceShaderAbstract.h:474-476`:

```cpp
					const FString ColorSpace = GetColorSpace(InputFile);
					const bool bIsSRGB = ColorSpace == TEXT("srgb_texture");
					TextureNode->SetCustomSRGB(bIsSRGB);
```

An untagged image inherits the document's `lin_rec709` and is imported linear; `sRGB - Texture` (an ACES config name) is also linear there. So an untagged 8-bit albedo is sRGB at render time and linear (washed out) once prepared.

Recommendation (no decision needed; it makes the prepared path match render time and UE's own sRGB naming): in `export_usd_materialx.write`, normalise each image file input's colour space before writing: untagged on a colour input → `srgb_texture`; any name `HdUESRGBColorSpace` would call sRGB → `srgb_texture`; raw/linear names unchanged. Keep the rule in one place: a small table in `scripts/usd_materialx_graph.py` (Python), mirrored by a comment pointing at `color_management.cpp:139-142`, with a unit test listing the names both sides must agree on.

### D3. Where colours are converted from the OCIO `scene_linear` role to UE's linear Rec.709

- Render-time constants are converted: `src/hydra/materials.cpp:197-202` (`color()` calls `HdUEFromSceneLinear`).
- Constant `displayColor` is converted, varying vertex colours are not — `src/hydra/prims.cpp:178-179`:

```cpp
    material.vertexColor = !mesh.colors.empty() || !mesh.instanceColors.empty();
    material.color = material.vertexColor || color.empty() ? GfVec3f(1) : HdUEFromSceneLinear(color[0]);
```

- Prepared MaterialX colours are written raw under `lin_rec709` (`export_usd_materialx.py:242`); UE treats any other colour space as linear with a warning (`<engine_root>/Engine/Plugins/Interchange/Runtime/Source/Import/Private/MaterialX/MaterialXUtils/MaterialXBase.cpp:297-312`, "is not supported yet, falling back to linear space").
- Live native-material parameters pass vectors raw (`src/hydra/delegate.cpp:656-661`).

This matters only when `scene_linear` is not linear Rec.709 (an ACEScg config). **Needs the maintainer's decision**: (a) convert everywhere (prepared export, live parameters, varying vertex colours) with the same OCIO processor; (b) convert nowhere and document that hdUE reads colours as linear Rec.709. Recommended: (a), because render-time constants already convert and Houdini's default OCIO config decides what artists see; but first check which config Houdini 22 uses by default (Step A1).

### D4. Graphs the render-time path accepts but cannot express

`scripts/usd_materialx_graph.py:199-262` (`RUNTIME_SURFACES`, `RUNTIME_NODES`, `needs_import`) sends a graph to preparation only when a node is outside the list. Cases the list accepts but `materials.cpp` mistranslates silently:

- One UV transform per material, from the first textured input — `materials.cpp:191-193`:

```cpp
        // ponytail: one UV transform per material, from the first textured input; per-texture transforms if assets mix
        // them.
        if (!uvSet_) uvSet_ = uvTransform(*source.image, data_.uvTransform);
```

- `place2d` inputs are read as constants only (`materials.cpp:347-351`: `value(param(place, ...))`); a connected `offset` or `rotate` falls to its default.
- `ND_texcoord_*` is in `RUNTIME_NODES`, but `uvPrimvar` (`materials.cpp:121-127`) knows only primvar readers, so a texcoord with `index != 0` reads UV set 0.
- `readMaterialX` (`materials.cpp:503-560`) reads a fixed list of inputs; any other authored input (`diffuse_roughness`, `coat_IOR`, `coat_normal`, `specular_anisotropy`, `thin_film_*`, ...) is dropped without a warning.
- Related: `materialx_lowering.lower_primvars` (`scripts/materialx_lowering.py:1065-1082`) writes `texcoord` indices for every UV primvar, while the render-time side warns and falls back to set 0 past 4 (`materials.cpp:358-363`).

Recommendation (no decision needed): make `needs_import` return True for these cases (more than one distinct UV transform, a connected `place2d` input, a texcoord with `index` other than 0, an authored surface input outside the list `materials.cpp` reads), and make `readMaterialX` warn once for any authored input it does not read. UE's importer handles all of them.

### D5. Textures UE cannot decode (`.rat`, other formats)

`unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp:1112`: `if (!Image.RawData.Num() && !FImageUtils::LoadImage(*File, Image)) return;` — the slot then draws its constant, with only a `LogTemp` line. Nothing in `src`, `unreal` or `scripts` handles `.rat` (Houdini's native format). Recommendation (no decision needed): the delegate warns (`warnOnce`, which reaches husk and the Houdini status line) when a texture's extension is not one UE's ImageWrapper decodes (PNG, JPEG, EXR, HDR, TGA, BMP, TIFF, DDS — confirm the list in `<engine_root>/Engine/Source/Runtime/ImageWrapper/`). Converting `.rat` through OIIO (as `HdUELinearRec709Texture` already reads images) is a follow-up, not part of this plan.

### D6. Smaller deviations

- **Translucent materials drop subsurface, sheen and coat silently** — `Scene.cpp:2117-2124`: `Layered = Transmissive || (!Translucent && (Desc.subsurface > 0.f || Desc.sheen > 0.f || Coat))`. Recommendation: warn once in the delegate when a see-through material authors them (UE's Interchange makes such materials translucent too; the warning is the missing part).
- **`opacityMode`** — the spec (`spec_usdpreviewsurface.md:138-153`) defines `transparent` (default) and `presence`. UE's USD importer reads it: `USDShadeConversion.cpp:2363-2366` maps `transparent` → 0, `presence` → 1 into an `OpacityMode` material parameter. hdUE's `readPreviewSurface` (`materials.cpp:378-394`) does not read it. Recommendation: read it and pass it the way UE's importer does; check how UE's preview-surface reference material uses `OpacityMode` before choosing the hdUE parent parameter. **Needs a decision only if** hdUE's parent materials have no equivalent (then it is an ABI and content change; report back).
- **`thin_surface` back side** — `scripts/materialx_lowering.py:558-573` rewrites `thin_surface` to `surface` and drops `back_bsdf`/`back_edf` without adding a note to `notes`, unlike other approximations in that file. Recommendation: append a note so the user sees the approximation.
- **Flake units** — `unreal/HdUEBackend/Source/HdUEContent/Private/HdUEImportMaterialXCommandlet.cpp:184-190` hard-codes centimetres to metres (`ConstB = 0.01f`) for `flake3d`'s object position, ignoring the stage's `metersPerUnit`. Recommendation: investigate only; flake cells are statistical, so record it in the docs as a known difference unless a user reports it.

### D7. Version numbers that must move together

- `IMPORT_VERSION = 4` (`scripts/usd_materialx_graph.py:291-294`) is documented as tracking only "hdUE's corrections after Interchange (HdUEImportMaterialXCommandlet.cpp)", but `materialx_lowering.py` rewrites also change what UE imports. Recommendation: extend the comment to name `materialx_lowering.py` and `export_usd_materialx.py`.
- `COMPILER_VERSION = 4` (`scripts/substrate_graph.py:348`) must equal `HdUESubstrateCompilerVersion = 4` (`unreal/HdUEBackend/Source/HdUEContent/Private/IdSwitch.h:59-60`). A mismatch fails loudly at prepare time, but a cheap unit test catches it before.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | "Suite unit passed." |
| Delegate (Houdini closed) | `.\scripts\Build-Houdini.ps1` | exit 0 |
| Backend | `.\scripts\Build-Backend.ps1`; `.\scripts\Update-HoudiniRuntime.ps1 -SetDefault` | exit 0 |
| Conformance | `& "<houdini_root>\bin\hython.exe" scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only materialx/* preview_surface/* texture/* shade/*` | exit 0 |
| MaterialX coverage | `.\scripts\Test-MaterialXCoverage.ps1` | exit 0 |
| Python format/lint | `uvx black@24.10.0 scripts tests integrations`; `uvx ruff@0.7.4 check scripts tests integrations` | no changes needed / "All checks passed!" |

## Scope

**In scope**: `src/hydra/materials.cpp`, `src/hydra/color_management.cpp`, `src/hydra/prims.cpp` (D3 only), `src/hydra/delegate.cpp` (D3 live parameters only), `scripts/usd_materialx_graph.py`, `scripts/export_usd_materialx.py`, `scripts/materialx_lowering.py` (D6 note only), `tests/` (new unit tests), `samples/conformance/` (new scenes), `docs/materials/*.md` (record decisions), `plans/027-one-material-one-look-on-every-path.md` (record decisions in Part A).

**Out of scope**: the C ABI (`include/hdue/bridge.h`) — if a decision needs an ABI change, STOP; UE content assets (`unreal/HdUEBackend/Content`); UE engine source; `scripts/materialx_library/hdue_defs.mtlx` (generated).

## Git workflow

- Branch or worktree the operator gave; if none, `main` (AGENTS.md).
- One commit per decided item (D1, D2, ...), plain English subject (e.g. `UsdUVTexture without a wrap repeats, as UE's USD importer imports it`), body with the source cited and the verification, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not push unless told.

## Steps

### Part A — decisions

### Step A1: Record the facts the decisions need

1. Find Houdini 22's default OCIO config: `& "<houdini_root>\bin\hython.exe" -c "import hou, os; print(os.environ.get('OCIO')); print(hou.Color.ocio_activeDisplays())"`. Record the config and its `scene_linear` role (also printed by the hdUE delegate at startup with `HDUE_TRACE_TIMING=1`, or read `color_management.cpp` `management()`).
2. Confirm UE's preview-surface reference material uses `OpacityMode`: `git grep`-equivalent over `<engine_root>/Engine/Plugins/Importers/USDImporter` and `<engine_root>/Engine/Plugins/Runtime/USDCore` for `OpacityMode`, and record what it changes.
3. List ImageWrapper's decodable formats from `<engine_root>/Engine/Source/Runtime/ImageWrapper/Public/IImageWrapper.h` (`EImageFormat`).

**Verify**: the three facts are written into this plan file under a new heading "## Decisions" (the only file edit in this step).

### Step A2: Ask the maintainer about D1, D3 and (if needed) D6 opacityMode

Ask one question at a time, each with the options above and the recommendation. Record each answer under "## Decisions" with the date.

**Verify**: "## Decisions" has an answer for D1 and D3 (and D6 opacityMode if Step A1 found no parent parameter). STOP if the maintainer is not reachable: implement only the items marked "no decision needed" (D2, D4, D5, D6 warnings and note, D7).

### Part B — implementation (one commit per item)

### Step B1 (D7): Version comments and the compiler-version test

- Extend the `IMPORT_VERSION` comment in `scripts/usd_materialx_graph.py:291-293` to: bump it when `HdUEImportMaterialXCommandlet.cpp`'s corrections, `materialx_lowering.py`'s rewrites or `export_usd_materialx.py`'s output change.
- Add `tests/material_versions_test.py` (unittest, no Houdini): read `unreal/HdUEBackend/Source/HdUEContent/Private/IdSwitch.h`, regex `HdUESubstrateCompilerVersion = (\d+)`, import `substrate_graph` from `scripts/` and assert equality. Model it on an existing unit test that adds `scripts/` to `sys.path` (look at `tests/substrate_graph_test.py`'s imports).
- Add it to the unit list in `scripts/Test.ps1` (the `-Suite unit` list near line 13; follow its format).

**Verify**: `.\scripts\Test.ps1 -Suite unit` → "Suite unit passed." and the new test runs (its name appears in the output).

### Step B2 (D4): needs_import sends what the render-time path cannot express

In `scripts/usd_materialx_graph.py` `needs_import`, return True when: two image nodes reach different `place2d`/`UsdTransform2d`/`tiledimage` transforms; a `place2d` input is connected (`"node" in node["inputs"][name]`); an `ND_texcoord_*` node has an `index` other than 0; the surface node authors an input outside a new `RUNTIME_SURFACE_INPUTS` tuple copied from `materials.cpp`'s `standardSurface`/`openPbr` tables (keep a comment "keep in step with materials.cpp"). In `materials.cpp` `readMaterialX`, push a warning for each authored surface input not in the table (`shader.parameters` and `shader.inputConnections` keys).

Add unit tests to an existing graph test (`tests/materialx_live_test.py` builds graphs; follow it) for each new True case and one unchanged False case.

**Verify**: unit suite passes; `run_conformance.py --only materialx/*` → exit 0.

### Step B3 (D2): one colour-space rule for image files

Implement the normalisation in `export_usd_materialx.write` (see D2), with a shared name table in `usd_materialx_graph.py` and unit tests for `""`, `"auto"`, `"srgb_texture"`, `"sRGB - Texture"`, `"srgb_tx"`, `"lin_srgb"`, `"raw"`, `"lin_rec709"` on colour and normal inputs. Bump `IMPORT_VERSION` (the import output changes).

**Verify**: unit suite passes; `.\scripts\Test-MaterialXCoverage.ps1` → exit 0.

### Step B4 (D5, D6 warnings and note)

- D5: in `materials.cpp` where a texture file is accepted (`SurfaceReader::trace`, after `t.file` is set), warn once when the extension is not in the list from Step A1.
- D6: warn once when a material is see-through (opacity < 1 or an opacity texture, no threshold) and authors subsurface, sheen or coat; append a note in `materialx_lowering.py`'s `thin_surface` branch.

**Verify**: build; a husk render of a small test layer with a `.rat` texture prints the warning once (`husk ... 2>&1 | Select-String "hdUE"`); conformance `materialx/*`, `preview_surface/*` → exit 0.

### Step B5 (D1, only as decided)

Change `wrap()`'s fallback for `UsdUVTexture` from `useMetadata` to `repeat`, and map `useMetadata` (and `black` if option (a)) as decided. Update the comment to cite `USDShadeConversion.cpp:584-607`. Add a conformance scene `samples/conformance/texture/wrap_default.usda`: a `UsdTransform2d` with scale (2, 1) and no wrap authored on `red_blue.png`; check that the quad's right half shows the texture again (a hue check is fine: red against black or blue).

**Verify**: build; `run_conformance.py --only texture/* preview_surface/*` → exit 0.

### Step B6 (D3, only as decided)

If option (a): convert colour-typed values in `export_usd_materialx.write` with the same rule as `HdUEFromSceneLinear` (Python cannot call the C++ helper; use `PyOpenColorIO` from Houdini's Python only if available, else STOP and report), convert varying vertex colours in `prims.cpp`, and convert vector live parameters in `delegate.cpp` `materialParameter` only when the parameter is declared a colour (if that information is not available, STOP and report). If option (b): document in `docs/rendering/color-management.md` that colours are read as linear Rec.709 and remove the conversion from `color()` and `displayColorMaterial`.

**Verify**: build; with the Step A1 config, a scene with the same colour as a constant, a varying vertex colour and a prepared material renders the same three values (write a small scene in `samples/conformance/shade/` with three quads and `ratio` checks, `min` 0.95 `max` 1.05).

### Step B7: Record the decisions in the user docs

Update `docs/materials/preview-surface.md` (wrap default, opacityMode) and `docs/materials/materialx-surfaces.md` (colour spaces, what goes to preparation) with the decided behaviour and the sources cited. Plain, short sentences, matching the surrounding prose.

**Verify**: `uvx zensical build --clean --strict` if zensical is installed locally (`uv tool install zensical==0.1.0`) → exit 0; otherwise `git diff --stat docs/` shows only those two files.

## Test plan

- Unit (no Houdini): `tests/material_versions_test.py` (D7); new `needs_import` cases (D4); colour-space name table (D2). Pattern: `tests/materialx_live_test.py`.
- Conformance: `texture/wrap_default.usda` (D1), the D3 colour scene if (a), plus all existing `materialx/*`, `preview_surface/*`, `texture/*`, `shade/*` scenes in both modes.
- Manual: the D5 warning in a husk log.

## Done criteria

- [ ] "## Decisions" in this file records D1 and D3 (and D6 opacityMode if needed), each with a date.
- [ ] `.\scripts\Test.ps1 -Suite unit` passes and lists the new tests.
- [ ] `run_conformance.py --only materialx/* preview_surface/* texture/* shade/*` exits 0 in both modes.
- [ ] `.\scripts\Test-MaterialXCoverage.ps1` exits 0.
- [ ] `git grep -n "IMPORT_VERSION" scripts/usd_materialx_graph.py` shows the extended comment.
- [ ] `git status` lists only in-scope paths.
- [ ] `plans/README.md` status row updated.

## STOP conditions

Stop and report back (do not improvise) if:

- The maintainer has not decided D1 or D3 (implement only the "no decision needed" items).
- A decision needs a change to `include/hdue/bridge.h` (an ABI bump) or to UE content assets.
- UE's source at the cited lines differs from the excerpts (another engine build).
- Making `needs_import` stricter sends a currently render-time conformance scene to preparation in husk (husk logs `material preparation`); report which scene and why.
- Any existing conformance scene fails after an item; do not adjust thresholds to pass.

## Maintenance notes

- `RUNTIME_NODES`/`RUNTIME_SURFACE_INPUTS` in `usd_materialx_graph.py` and the tables in `materials.cpp` must change together; a reviewer should check both in any PR touching either.
- Every change to what the prepared path imports must bump `IMPORT_VERSION`, or published live materials keep serving the old result until they age out.
- Deferred: `.rat` conversion through OIIO; per-texture UV transforms at render time (sending such graphs to preparation is the cheaper fix this plan takes); flake units (D6).
