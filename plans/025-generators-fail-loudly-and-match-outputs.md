# Plan 025: The generators fail loudly on what they cannot read, and reproduce their committed outputs

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- scripts/generate_render_settings.py scripts/build_render_settings_hda.py scripts/build_substrate_hda.py scripts/generate_substrate_textures.py scripts/generate_substrate_comparison.py scripts/generate_materialx_conformance.py scripts/gallery_charts.py resources/plugInfo.json.in tests/abi_layout.c .github/workflows/ci.yml scripts/Test.ps1`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition. Plans 009 (honest conformance
> checks) and 013 (setting defaults header) edit some of the same files;
> their changes are expected — re-read those files before editing.

## Status

- **Priority**: P3
- **Effort**: M
- **Risk**: LOW
- **Depends on**: plans/013-generate-hdue-setting-defaults-once.md (its header is one of the outputs the CI check covers); run after plans/009-conformance-and-gallery-checks-can-fail.md, which edits `scripts/generate_materialx_conformance.py`
- **Category**: tech-debt
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

hdUE's UI, schema and docs are generated from the UE source by `scripts/generate_render_settings.py`, and its HDAs by two builders. Many of their parsing and editing steps succeed silently when they do not find what they expect: a C++ default the regex cannot read becomes 0 or the first number in the expression, a `re.sub` that matches nothing leaves an HDA without its output tab, a plugin console variable keeps a hand-typed default nobody checks. After an engine update this ships wrong defaults with no error, against the project rule that every setting defaults to UE's own value. Some sample generators have also drifted from the files they produce (two still write lights at intensity 12, against the rule "intensity 1, brightness in exposure", while the committed samples were fixed by hand). After this plan, those paths stop with a message naming the property, and CI proves the deterministic generators reproduce their committed outputs. The goal statement for every step: **regenerating and diffing shows no unintended change.**

## Current state

### Render settings generator (`scripts/generate_render_settings.py`)

Run as `python scripts/generate_render_settings.py` (engine root from `.local/paths.json` key `engine_root`). It writes the `HdUnrealEngine_*.ds` generated blocks, `resources/generatedSchema.usda`, `docs/reference/**`, and `src/hydra/light_properties.h` (and, after plan 013, `src/hydra/render_setting_defaults.h`).

**(a) Deprecated enumerators are dropped, but the settings that only show for them are kept, always hidden.** `parse_enums` (lines 1156–1188) skips items containing `UE_DEPRECATED`:

```python
                if (
                    not match
                    or "UE_DEPRECATED" in item
                    or "Hidden" in item
                    or match.group(1).endswith("MAX")
                ):
                    continue
```

UE 5.8.4 `Engine/Source/Runtime/Engine/Classes/Engine/Scene.h:116`:
`RayTraced_Deprecated UE_DEPRECATED(5.6, "Use RayTraced instead") = 1 UMETA(DisplayName = "Legacy Ray Tracing (Deprecated)", …)`, and properties at Scene.h:2463, 2467, 2471 (and others nearby) carry `TranslucencyType = "RayTraced_Deprecated"`. `parse_properties` turns that into a hide condition (lines 1845–1869):

```python
                hide=(
                    [
                        f'{houdini_name("unreal:postProcess:TranslucencyType")} != "ETranslucencyType::{translucency}"'
                    ]
                    if translucency
                    else []
                ),
```

The generated menu has no such entry (`HdUnrealEngine_Global.ds:5281`: `menu { "ETranslucencyType::Raster" "Raster" "ETranslucencyType::RayTraced" "Ray Traced" }`), so the hide condition at `.ds` lines 5301, 5314, 5329, 5342, 5357, 5370 is always true. `build_render_settings_hda.py` clears hide-when for the Post Process Volume HDA, so there those settings show but cannot apply. Also, `"Hidden" in item` drops any enumerator whose text merely contains "Hidden" (for example in a tooltip), not only `UMETA(Hidden)`.

**(b) Silent default parsing.** `default_value` (lines 1211–1265):

```python
    if expression is None:
        return (
            [enum[0][0]]
            if kind == "menu"
            else {
                "float": [0.0] * size,
                "int": [0],
                "toggle": [False],
                "string": [""],
            }[kind]
        )
...
    if kind == "toggle":
        return [expression.strip() not in ("0", "false")]
...
        numbers = [
            float(n.rstrip("f"))
            for n in re.findall(
                r"(?<![\w.])-?(?:\d+\.?\d*|\.\d+)f?", expression
            )
        ]
```

So `X = Raw * (1.0f / Raw.B)` reads 1.0, `1e-3f` reads 1, and any non-literal boolean (a variable, `!GIsEditor`) reads True. The override tables exist exactly for these cases: `HDUE_DEFAULTS` (line 222), `FOG_DEFAULTS` (250), `SECOND_FOG_DEFAULTS` (259), `SKY_DEFAULTS` (266, comment: "defaults their constructors compute"), `LIGHT_DEFAULTS` (1416). Its one caller is `parse_properties` (line 1809), which has the property `name`.

`parse_defaults` (lines 1191–1208) keeps the **last** assignment of a name in the constructor body; its comment notes the auto-exposure case relies on that ("the else branch: r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange is 0").

`parse_enums`: the regex allows `namespace X {` only directly before `enum` (a comment in between loses the namespace), and `enums[owner] = entries` lets two enums of the same name overwrite each other.

`parse_component` (line 1308) cuts the class body at its first `"\npublic:"` (the `end` default), so properties declared after a later `public:` would be lost silently.

`parse_cvars` (lines 1912–1962) replaces `Scalability::DefaultQualityLevel` with a literal `"3"` (it matches UE 5.8.4 `Engine/Source/Runtime/Engine/Public/Scalability.h:21`: `inline const int32 DefaultQualityLevel = 3;`), and searches only `Runtime/Renderer|Engine|RenderCore`:

```python
    for directory in ("Renderer", "Engine", "RenderCore"):
        for folder, _, files in os.walk(engine_source / "Runtime" / directory):
```

**(c) Plugin console variables are never checked.** `console_settings` (lines 2003–2077) falls back to the hand-typed `type`/`default` in `CVARS` when no declaration is found:

```python
        type_name, default, help_text, read_only = declared.get(
            name,
            (
                options.get("type"),
                options.get("default"),
                options.get("help"),
                False,
            ),
        )
```

That is always the case for the DLSS (lines 405–498) and NNE/OptiX denoiser (around 653–713) entries, which live in engine or project plugins (the DLSS plugin is a local download under `unreal/HdUEBackend/Plugins/`, not tracked by git).

**(d) Multi-component settings and kinds the schema cannot write.** `write_schema` (lines 1618–1666) writes `f"    {USD_TYPES[setting['kind']]} {setting['attribute']} = {value(setting)} ("`, where `value()` returns only `ds_number(v[0])` for a `float` of size 2–4, and `USD_TYPES` (lines 1488–1495) has no `"intmenu"`. Not hit today (the schema classes have no such properties), but the next such UE property writes a wrong schema silently or fails with a bare `KeyError`.

**(e) `--defaults-json`** (lines 2749–2776) writes with the platform's line endings (`write_text` without `newline="\n"`) and its help says "Write every generated setting at its default" while it omits `HDUE` and light properties. `scripts/Test-RenderSettings.ps1` consumes it (`settings` and `declaredConsoleDefaults`).

### HDA builders

`scripts/build_render_settings_hda.py`:
- `TABLE_ROW` regex (lines 52–55) parses `src/hydra/aov_table.h`; `aov_table()` (65–72) returns whatever matched. The table has 27 rows today (`grep -c 'HdUEAovKind::' src/hydra/aov_table.h` → 27); nothing compares the count.
- The Image Output tab is inserted only before a tab labelled exactly `"Engine Quality"` (lines 533–536):
  ```python
      for tab in tabs:
          if tab.label() == "Engine Quality":
              group.append(output)
          group.append(tab)
  ```
- `save()` rewrites input labels with `re.sub` (lines 790–797) and the local fog builder inserts Sphere LOP handles with `re.sub(..., count=1)` (lines 1333–1343), neither checking a match happened.
- `property_templates` (lines 1143–1213) has no case for `kind == "menu"`/`"intmenu"` or `size > 1` floats (a menu falls into the number branch with a string default).

`scripts/build_substrate_hda.py`:
- Inserts `rendermask`/`externalshader`/`shadertype` with `re.sub(r"(\n    label\s.*\n)", …, count=1)` (lines 519–525), unchecked.
- Rewrites `INDEX__SECTION` `Subnet: true` → `false` with `index.write_text(re.sub(...))` (lines 595–598): unchecked, and written with platform line endings.
- `build()` and `help_page()` use `NODES` and `outputs`, imported only under `if __name__ == "__main__"` (lines 601–605), so importing the module and calling `build()` fails.

### Schema registration and ABI asserts

`resources/plugInfo.json.in` (lines 47–88) registers, by hand, two entries per generated schema class (`HdUE_<X>Adapter` with `"primTypeName": "Unreal<X>"`, and `HdUEUnreal<X>` with `"schemaIdentifier": "Unreal<X>"`). The classes are `SCHEMA_CLASSES` in `generate_render_settings.py` (lines 1576–1616): UnrealLocalFogVolume, UnrealPostProcessVolume, UnrealExponentialHeightFog, UnrealSkyAtmosphere, UnrealVolumetricCloud, UnrealDecal. Nothing checks that every class is registered; a missing entry gives a prim type USD does not recognise, with no error.

`tests/abi_layout.c` asserts sizes of most structs in `include/hdue/bridge.h`. Correction to the audit: `HdueDecalDesc` and `HduePostProcessVolumeDesc` are `typedef HdueLocalFogVolumeDesc` (bridge.h:504, 509), already asserted (168). Two structs have no assert: `HduePostProcessDesc` (bridge.h:447–456) and `HdueMeshTransform` (bridge.h:426–430). By x64 layout they should be 56 and 24 bytes; the build confirms.

### Sample and conformance generators

- `scripts/generate_substrate_textures.py:215-222` and `scripts/generate_substrate_comparison.py:96-103` author `("Key", …, 12, …)` with `light.CreateIntensityAttr(intensity)` and no exposure. The committed outputs were fixed by hand (commit 86ae431): `samples/substrate/textures.usda:189-190` and `samples/substrate/contrast.usda:191-193` have `float inputs:intensity = 1` and `float inputs:exposure = 3.585` (log2(12) rounded to 3 decimals). AGENTS.md: "Lights in samples: intensity 1, tune with `inputs:exposure`."
- `generate_substrate_textures.py:44` builds texture paths with `str(folder / file)`, which writes Windows backslashes into the committed `samples/substrate/textures.usda:50,65,80` (`@textures\color.png@`). USD asset paths should use `/`.
- `scripts/generate_materialx_conformance.py:223-229` declares every non-`color3` shader output as `float`:
  ```python
          out_type = (
              "color3f"
              if identity.endswith(("color3", "_color3_color3"))
              or identity.startswith("ND_combine3_color3")
              else "float"
          )
          declared = outputs[0] if outputs else {"out": out_type}
  ```
  so matrix, vector3, integer and boolean nodes in `samples/conformance/materialx/lowered_nodes.usda` declare `float outputs:out` while their consumers take `matrix3d`, `float3`, `int`, `bool`. A case can already override with an explicit outputs dict (the `Flake3D` case, around line 191: `{"presence": "float"}`).
- `scripts/generate_materialx_conformance.py`, `scripts/generate_groom_conformance.py` and `scripts/generate_reference_figures.py` use only the standard library and write with `newline="\n"`; they are deterministic and cheap. Nothing checks that their committed outputs (`samples/conformance/materialx/lowered_nodes.usda`, `samples/conformance/points_curves/groom_attributes.usda`, `docs/figures/ref-*.json`) match.
- `scripts/gallery_charts.py` (run as `uv run --with matplotlib python scripts/gallery_charts.py`, unpinned) saves SVGs with `fig.savefig(IMAGES / f"{name}.svg", transparent=True)` (line ~322): each run embeds a new `<dc:date>` and random clip ids, so the 4 committed SVGs change even with no data change.

### CI

`.github/workflows/ci.yml` job `scripts` (Windows) runs `uv run --python 3.13 python -m compileall`, a PowerShell parse, and `./scripts/Test.ps1 -Suite unit`. `.gitattributes` sets `* text=auto eol=lf`.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Render settings generator | `python scripts/generate_render_settings.py` | exit 0 |
| Defaults snapshot | `python scripts/generate_render_settings.py --defaults-json out/settings-check/defaults.json` | exit 0 |
| Render settings HDAs | `python scripts/build_render_settings_hda.py` | exit 0 |
| Substrate HDA | `python scripts/build_substrate_hda.py` | exit 0 |
| Node docs | `<houdini_root>\bin\hython.exe scripts\generate_node_docs.py` | exit 0 |
| Stdlib generators | `python scripts/generate_materialx_conformance.py`; `python scripts/generate_groom_conformance.py`; `python scripts/generate_reference_figures.py` | exit 0 |
| Substrate sample generators | see each script's docstring (they need Houdini's Python with `pxr`; `generate_substrate_comparison.py`: "Run using the configured Houdini Python executable through uv") | exit 0 |
| Native tests (ABI asserts) | `.\scripts\Build-Houdini.ps1` (Houdini closed); CI-style: `cmake -S . -B build -DHDUE_BUILD_HYDRA=OFF; cmake --build build --config Release; ctest --test-dir build -C Release` | exit 0 |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | no error |
| Settings check | `.\scripts\Test-RenderSettings.ps1 -RuntimeConfig (Resolve-Path .local\runtime-houdini.json)` | "0 skipped, 0 console defaults differ" |
| Format | `uvx black@24.10.0 --check scripts tests integrations`; `uvx ruff@0.7.4 check scripts tests integrations` | exit 0 |
| Workflow lint | actionlint 1.7.4 (CI runs it) | no findings |

## Scope

**In scope** (the only files you should modify):
- `scripts/generate_render_settings.py`, `scripts/build_render_settings_hda.py`, `scripts/build_substrate_hda.py`
- `scripts/generate_substrate_textures.py`, `scripts/generate_substrate_comparison.py`, `scripts/generate_materialx_conformance.py`, `scripts/gallery_charts.py`
- Their generated outputs, only by running them: `integrations/houdini/soho/parameters/HdUnrealEngine_*.ds`, `resources/generatedSchema.usda`, `docs/reference/**`, `integrations/houdini/otls/*.hda` (rebuilt), `samples/substrate/textures.usda`, `samples/substrate/contrast.usda`, `samples/conformance/materialx/lowered_nodes.usda`, `docs/images/gallery/chart-*.svg`
- `tests/abi_layout.c` (two asserts)
- `tests/generated_files_test.py` (create)
- `scripts/Test.ps1` (add the new test to the unit list)
- `.github/workflows/ci.yml` (only if Step 9 needs a separate step)

**Out of scope** (do NOT touch, even though they look related):
- Hand edits to any `.ds`, `.hda`, schema or generated doc.
- `resources/plugInfo.json.in` content: this plan adds a test that it lists every schema class; it does not generate the file (it is a CMake template with other hand-written entries; JSON has no comments for markers).
- The conformance check *contents* of `generate_materialx_conformance.py` (value/colour checks): plan 009 owns those. This plan changes only the declared output types.
- Splitting the 2,800-line generator into modules: worth it, but only after this plan's regenerate-and-diff check exists. Deferred.
- `convert_bistro.py` / `generate_sponza_materials.py` issues: dev-only converters with local-only outputs; deferred (see Maintenance notes).

## Git workflow

- Work on the branch or worktree the operator gave you; if none, on `main` (AGENTS.md).
- One commit per step. Plain English subjects (e.g. `The render settings generator stops on defaults it cannot read`); body with why and the regenerate-and-diff result; end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push unless told.

## Steps

### Step 1: Baseline — the generators reproduce the committed files today

Before changing anything, run:

```
python scripts/generate_render_settings.py
python scripts/generate_render_settings.py --defaults-json out/settings-check/before.json
python scripts/generate_materialx_conformance.py
python scripts/generate_groom_conformance.py
python scripts/generate_reference_figures.py
```

**Verify**: `git status --short` → empty (apart from `out/`, which is ignored). If any committed file changed, the tree had already drifted from its generator: **STOP** and report which files.

### Step 2: Drop settings that only apply to a dropped enumerator; match `UMETA(Hidden)` exactly

In `parse_enums`, replace `"Hidden" in item` with a regex for the metadata flag (`re.search(r"UMETA\s*\([^)]*\bHidden\b", item)`). In `parse_properties`, after computing `translucency = meta.get("TranslucencyType")`, skip the property (append `(name, f"only shown for the deprecated {translucency} translucency method")` to `skipped` and `continue`) when no entry of the `ETranslucencyType` enum ends with `::{translucency}`. This follows hdUE's existing choice to drop deprecated enumerators.

Run the generator, then `python scripts/build_render_settings_hda.py` and `hython scripts/generate_node_docs.py`.

**Verify**:
- The generator prints new `skipped <Name>: only shown for the deprecated RayTraced_Deprecated …` lines (one per property; count them).
- `grep -c 'RayTraced_Deprecated' integrations/houdini/soho/parameters/HdUnrealEngine_Global.ds` → 0.
- `git diff --stat` → only `.ds`, `docs/reference/**`, the rebuilt HDAs (binary) and `docs/reference/nodes/**` changed; `git diff integrations/houdini/soho/parameters | grep '^-' | grep -c 'RayTraced'` equals the number of removed hide conditions.
- No other enumerator disappeared: `python -c` diff of the menus before/after is beyond scope; instead `git diff integrations/houdini/soho/parameters | grep '^[-+] *menu'` → no output (no menu line changed).

### Step 3: Make default parsing strict

1. Give `default_value` a `name` parameter (pass `name` from `parse_properties`, line 1809). Accept only:
   - `None` (no default found — keep today's zero/first-enum value; see item 4),
   - for `toggle`: `0`, `1`, `true`, `false` (after `strip()`),
   - for numbers: an expression that is entirely one numeric literal (allow `f`, a sign and scientific notation: `^-?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?f?$`), or the comma-separated literals of a constructor such as `FVector2D(1.0f, 2.0f)` / `FLinearColor(…)`, or the names already handled (`FLinearColor::Black`, `…::White`, `FColor(…)`, `FPostProcessSettings::kDefaultDepthOfFieldBladeCount`).
   Anything else: `raise SystemExit(f"{name}: cannot read the default {expression!r}; add it to the override table")`.
2. Run the generator. For each property it now rejects, add an entry to the right override table (`HDUE_DEFAULTS` for post-process, `FOG_DEFAULTS`, `SKY_DEFAULTS`, `LIGHT_DEFAULTS`) with the value the generator produced **before** (read it from `out/settings-check/before.json` or the committed `.ds`), and a comment naming the UE source line it comes from (open the engine file and confirm the value is right; if the old value was wrong, use UE's real value and note it in the commit).
3. In `parse_defaults`, raise when the constructor body assigns the same name twice, except for names in a small allowlist with the reason (the auto-exposure `else` branch named in its comment). Run and add allowlist entries only for cases you have read in the engine source.
4. Replace the `Scalability::DefaultQualityLevel` literal with a value read from `Engine/Source/Runtime/Engine/Public/Scalability.h` (`inline const int32 DefaultQualityLevel = (\d+);`), raising if not found.
5. In `parse_enums`, raise on a second enum with the same owner name, and allow a comment between `namespace X {` and `enum` (strip comments from the source before matching).
6. In `parse_component`, raise if the class has more `UPROPERTY(` occurrences after the cut than inside it within the same class (i.e. properties after a later `public:` would be lost).

**Verify** (the important gate): `python scripts/generate_render_settings.py` → exit 0; `git diff --stat` → no change at all in `.ds`, schema, docs or headers compared to Step 2's result (the override entries reproduce the old values exactly unless you documented a correction). `python scripts/generate_render_settings.py --defaults-json out/settings-check/after.json`, then `python -c "import json; a=json.load(open('out/settings-check/before.json')); b=json.load(open('out/settings-check/after.json')); print({k for k in a['settings'] if a['settings'][k]!=b['settings'].get(k)})"` → only the properties removed in Step 2 (or `set()`), unless you documented a correction.

### Step 4: Check plugin console-variable defaults against their declarations

In `parse_cvars`, also walk `Engine/Plugins` (and, if it exists, `unreal/HdUEBackend/Plugins`, the project's local DLSS download; print one line and continue if it is missing). In `console_settings`, when a `CVARS` entry gives its own `default` and a declaration was found, compare them; if they differ and the entry does not say why (add an optional `override="<reason>"` key for intentional hdUE overrides), `raise SystemExit(f"{name}: hand-typed default {x} but UE declares {y}")`.

**Verify**: generator → exit 0 and `git diff --stat` unchanged from Step 3. If it raises, read the plugin source: if UE's value is right, fix the `CVARS` entry and regenerate (an intended change; note it in the commit), or add `override=` with the reason.

### Step 5: Make schema writing and the volume/decal HDA properties refuse what they cannot express

- `write_schema`: raise `SystemExit` naming the property for any kind not in `USD_TYPES` and for a `float` of `size > 1` (or support `size` 2–4 with `float2/3/4` and tuple values — choose the raise unless a property needs it today).
- `build_render_settings_hda.property_templates`: raise for `kind in ("menu", "intmenu")` or `size > 1` floats, naming the attribute.

**Verify**: generator and `python scripts/build_render_settings_hda.py` → exit 0 with no diff in text outputs (HDAs are binary and always rewrite; check `git diff --stat` shows only HDAs if you rebuilt them).

### Step 6: Check every text edit the HDA builders make

In `build_render_settings_hda.py` and `build_substrate_hda.py`, replace each unchecked `re.sub` that must match with `re.subn` and `if count != 1: raise SystemExit("<what> not found in <hda>")` (input labels: one per label; Sphere LOP handles: the `label` insertion; Substrate: the `label` insertion and the `Subnet: true` rewrite — that one may match several entries: require `count >= 1`). After the tab loop (lines 533–536), raise if `output` was never appended. In `aov_table()`, raise if `len(rows) != text.count("HdUEAovKind::")` (27 today).

In `build_substrate_hda.py`, move `from substrate_graph import NODES, outputs` to module level inside `build()`/`help_page()` (import at the top of each function), so `build()` works when imported. Write `INDEX__SECTION` with `newline="\n"`… **only if** `hotl -l` accepts LF (it wrote CRLF before; if unsure, leave the line endings as they are).

**Verify**: both builders → exit 0. Temporarily rename the `"Engine Quality"` label check to a wrong label (not committed) → the builder stops with your message; revert.

### Step 7: Make the sample generators follow the repo rules

- `generate_substrate_textures.py` and `generate_substrate_comparison.py`: author the key light as `intensity 1` and `exposure = round(math.log2(12), 3)` (3.585); the fill light keeps intensity 1 with no exposure. Use `light.CreateExposureAttr(...)`.
- `generate_substrate_textures.py:44`: write texture asset paths with `.as_posix()` (`(folder / file).as_posix()`).
- Re-run both generators per their docstrings.

**Verify**: `git diff samples/substrate/contrast.usda samples/substrate/textures.usda` → only the texture paths changed from `\` to `/`, plus at most the order of the light's `inputs:exposure` / `inputs:intensity` lines (the hand fix put them in different orders in the two files; USD writes its own order). The values must be `intensity = 1` and `exposure = 3.585`; if a value differs, fix the generator. If the generators cannot run here (they need Houdini's `pxr` and write a `.hip`), STOP after making the code change and report that the outputs were not regenerated.

### Step 8: Declare the real output types in the MaterialX conformance generator

In `generate_materialx_conformance.py`, derive each shader's output type from its node id instead of defaulting to `float`: `matrix33`/`matrix44` → `matrix3d`/`matrix4d`, `vector2/3/4` → `float2/3/4`, `color3`/`color4` → `color3f`/`color4f`, `integer` → `int`, `boolean` → `bool`, else `float`. Use the output type in the node definition id (the suffix after the node name, e.g. `ND_invertmatrix_matrix33` → `matrix33`); keep explicit `outputs` dicts (the Flake3D case) as overrides. Regenerate.

**Verify**: `python scripts/generate_materialx_conformance.py`; `git diff samples/conformance/materialx/lowered_nodes.usda` → only `outputs:out` type lines change. Then `hython scripts\run_conformance.py --runtime-config (Resolve-Path .local\runtime-houdini.json) --only materialx/lowered_nodes` → 0 failing runs. If it fails only after this change, STOP: the lowering pass reads the declared type and mis-handles a correct one — report it (that is a real bug to plan separately).

### Step 9: Prove in CI that generated text files are current

Create `tests/generated_files_test.py` (stdlib `unittest`, pattern `tests/test_repository_scope.py`). It:
1. Runs `generate_materialx_conformance.py`, `generate_groom_conformance.py` and `generate_reference_figures.py` main functions with their output paths redirected to a temp folder (monkeypatch the modules' `OUT` / `SPECS` constants after import), and compares each written file byte for byte with the committed one.
2. Checks `resources/plugInfo.json.in` contains, for every name in `generate_render_settings.SCHEMA_CLASSES`, both `"primTypeName": "<Name>"` and `"schemaIdentifier": "<Name>"`.
3. If plan 013 has landed, checks `src/hydra/render_setting_defaults.h` equals `generate_render_settings.defaults_header()` (skip with a message if that function does not exist yet).

Add it to the unit list in `scripts/Test.ps1` (it needs only the standard library: `uv run --python 3.11 python tests\generated_files_test.py`).

Add the two missing ABI asserts to `tests/abi_layout.c` next to their neighbours:

```c
_Static_assert(sizeof(HdueMeshTransform) == 24, "Mesh transform C ABI layout");
_Static_assert(sizeof(HduePostProcessDesc) == 56, "Post-process C ABI layout");
```

**Verify**: `uv run --python 3.11 python tests\generated_files_test.py` → OK. Change one character of the committed `samples/conformance/points_curves/groom_attributes.usda` (not committed) → the test fails; revert. CI-style native build (`cmake -S . -B build -DHDUE_BUILD_HYDRA=OFF` + build + `ctest`) → passes; if an assert fails, the compiler prints the real size: STOP and check the struct in `include/hdue/bridge.h` before changing the number (a size you did not expect may be an ABI bug).

### Step 10: Make the gallery charts deterministic

In `gallery_charts.py`, set `plt.rcParams["svg.hashsalt"] = "hdue"` in `main()` and pass `metadata={"Date": None}` to the SVG `savefig`. Pin matplotlib in the docstring command and AGENTS.md's gallery line only if you can name the version the committed SVGs record (`grep -o "Matplotlib v[0-9.]*" docs/images/gallery/chart-times-dark.svg`); use `--with matplotlib==<that version>` in the docstring. (AGENTS.md is out of scope; note the command change in the commit message instead.)

**Verify**: run `uv run --with matplotlib==<version> python scripts/gallery_charts.py` twice (it needs `out/gallery/results.json`; if that file is missing, skip the run and STOP-report that the determinism could not be checked) → the second run leaves `git diff --stat docs/images/gallery` identical to the first.

### Step 11: Final checks

**Verify**:
- Re-run every generator from Step 1 → `git status` shows only the intended changes listed in the commits.
- `.\scripts\Test-RenderSettings.ps1 -RuntimeConfig (Resolve-Path .local\runtime-houdini.json)` → "0 skipped, 0 console defaults differ".
- `.\scripts\Test.ps1 -Suite unit` → no error.
- Black, Ruff (pinned versions) → exit 0.

## Test plan

- `tests/generated_files_test.py` (Step 9): three stdlib generators reproduce their committed files; every schema class is registered in `plugInfo.json.in`; the defaults header is current.
- Two new `_Static_assert`s in `tests/abi_layout.c` (CI's native job runs them).
- The strict parsers are exercised by running the generator (Step 3–4 verifications); a one-off manual negative check for each builder `subn` guard (Step 6).

## Done criteria

- [ ] `python scripts/generate_render_settings.py` exits 0 and, apart from Step 2's removals and documented corrections, regenerates byte-identical outputs
- [ ] `grep -c 'RayTraced_Deprecated' integrations/houdini/soho/parameters/HdUnrealEngine_Global.ds` → 0
- [ ] `grep -n '"Hidden" in item' scripts/generate_render_settings.py` → no output
- [ ] `grep -n 'CreateIntensityAttr(intensity)' scripts/generate_substrate_textures.py scripts/generate_substrate_comparison.py` → no output, and the committed substrate samples match their generators
- [ ] `uv run --python 3.11 python tests\generated_files_test.py` → OK, and it is in `scripts\Test.ps1 -Suite unit`
- [ ] Native CI build with the new asserts passes
- [ ] `run_conformance.py --only materialx/lowered_nodes` → 0 failing runs
- [ ] `git status` shows only in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Step 1 shows the committed generated files already differ from their generators.
- A strict parse (Step 3–4) finds a default whose old generated value differs from what UE's source says, and the fix would change a setting users can see: report the list with both values; do not decide alone which is right if the UE source is ambiguous.
- More than about 15 properties need new override entries in Step 3 (the strict rule is too strict; report the expressions so the rule can be widened deliberately).
- Removing the deprecated-only settings (Step 2) changes anything besides those properties.
- The rebuilt HDAs lose parameters (compare the printed `N parameters` per HDA before and after).
- An ABI assert fails with a size other than the computed one.
- `lowered_nodes` conformance fails only after Step 8.

## Maintenance notes

- After any UE engine update: run the generator; a `SystemExit` naming a property means UE changed how it writes that default — read the engine source and update the override table on purpose. Then run `tests/generated_files_test.py` and `Test-RenderSettings.ps1`.
- New schema class: add both `plugInfo.json.in` entries; the unit test fails until you do.
- New struct in `bridge.h`: add its `_Static_assert` to `tests/abi_layout.c`.
- Deferred, for later plans: split `generate_render_settings.py` (parsing / data / writers) now that a regenerate-and-diff check exists; `convert_bistro.py` (unchecked image writes, a cache key that ignores the imperfection kind, dome path ignoring `--source`) and `generate_sponza_materials.py` (unclear errors on unresolved textures) — dev-only, local outputs; generate the `plugInfo.json.in` schema block if more schema classes are added.
- Reviewers: the key evidence is the `git diff --stat` after each regeneration; any change in a generated file must be explained in the commit message.
