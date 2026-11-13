# Plan 026: The user guide, README and architecture page say what the code does

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- README.md VERIFICATION.md docs/ integrations/houdini/python3.13libs/hdue_houdini.py`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live files before proceeding; on a
> mismatch, treat it as a STOP condition for that item only (skip it and note
> it), and continue with the others.

## Status

- **Priority**: P3
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none. Item 7 reads better after plan 017 (if 017 adds the material-preparation controls to the Unreal menu), and the `autoViewerLight` default is fixed by plan 013. Do not edit either topic's code here.
- **Category**: docs
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

Several pages of the user guide and the developer docs contradict the code, or each other. Users are told to delete a cache that cleans itself, to look for logs in the wrong folder, to use a settings node no menu creates, and to edit a camera prim that does not exist. AGENTS.md tells agents to read `docs/architecture.md` first, and parts of it are out of date. Every fix here is a text change. Each item says what the code does, with the line that proves it.

## Current state

Every quote below was read at `6fe3e12`. Fix each item in place and keep the page's existing style. Docs are plain, concrete and short, with no marketing. Look at the sentences around each one and match them.

### Item 1: the virtual texture cache is evicted, not "never trimmed"

- `docs/materials.md:95`: "The cache is never trimmed: delete the folder to reclaim space."
- `docs/developer/internals/materials.md:53` (long line): "...it is never evicted (delete the folder, or use the Unreal menu, to reclaim space)."
- Code: `unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp` deletes manifests unused for 7 days (around line 64), deletes chunks no manifest names after 1 day (around lines 77-80), and `CacheDirectory()` runs the eviction (around line 91). Read lines 55-95 to word it exactly.
- Two pages already say it right; reuse their wording: `docs/houdini/viewport.md:92` and `docs/architecture.md:190`.

### Item 2: where per-user logs and caches go

- `docs/getting-started/installation.md:55`: "each user's logs and shader cache go to their own Houdini preferences folder."
- Code (`src/client/backend.cpp:53-57`):

  ```cpp
      auto user = env("HDUE_USER_DIR");
      if (user.empty() && !env("LOCALAPPDATA").empty()) user = env("LOCALAPPDATA") + "\\hdUE";
      if (!user.empty()) arguments += " -userdir=\"" + user + "\"";
  ```

- `docs/getting-started/environment-variables.md` already says `%LOCALAPPDATA%\hdUE`.

### Item 3: the Unreal Tonemapper is off by default

- `docs/cameras.md:144`: "With the **Unreal Tonemapper** on (the default), the camera's exposure adds to [Exposure Compensation](...). With it off, it scales the beauty and leaves the other AOVs untouched, as Karma does."
- Code: `src/hydra/delegate.h:516` reads `setting<bool>("tonemapper", false)`. `docs/rendering/render-settings.md` and the generated reference also say off.

### Item 4: Nanite debug views work in husk too

- `docs/rendering/debug-views.md:62`: "In husk, meshes from the stage are not Nanite, so these views show nothing for them."
- `docs/rendering/debug-views.md:79`: the table row "| A Nanite view is black or empty | no Nanite meshes in view (husk, or meshes not converted) | Use it in Houdini's viewport, or on native Nanite assets. |"
- Code and the other pages: `docs/geometry/nanite.md` says batch renders build Nanite data before the first frame. `docs/architecture.md:150` says offline hosts build "inline in `ApplyScene`". The backend builds Nanite for static meshes and prototypes in every host: read `Scene.cpp` around 2230-2245 (`Nanite`, `naniteTriangleThreshold`) and word the threshold rule from it.

### Item 5: VERIFICATION.md's gallery paragraph is stale

`VERIFICATION.md:102` claims all of these:
- "Lumen reaches about 0.8x";
- "the hdUE Path Tracer is 2-13x faster than XPU per frame on the Cornell scenes (5x over all fifteen)";
- "the gallery renders without hdUE's optional denoiser";
- a comparison against Karma CPU only.

The current tables are `README.md` (gallery section, around lines 84-99) and `docs/gallery.md` (around lines 24-39). `scripts/render_gallery.py` (around lines 812-813) says the Path Tracer image is denoised. Replace the measured claims with a short pointer to `docs/gallery.md`, and keep the method description (resolution, sample count, bounce depth, `render_gallery.py`). Do not copy numbers by hand: they go stale again.

### Item 6: volume materials are supported

- `README.md:161`, the **Volumes** row, lists "Volume materials" in the "Not yet" column.
- `docs/environment/volumes.md:86`: "In Houdini's viewport, a bound Substrate Builder volume material replaces it; ... Other volume materials are not translated."
- Code and docs: `docs/substrate-builder.md:121-123` and `docs/developer/internals/substrate-builder.md:245-249` say a Substrate Builder volume material "replaces hdUE's default volume material in the viewport, husk and usdview". `integrations/houdini/python3.13libs/hdue_substrate.py` has the `preset == "volume"` path.
- Fix: in README, move it to "What you get" as "Substrate Builder volume materials", and keep "other volume materials" in "Not yet". In volumes.md, say "in the viewport, husk and usdview".

### Item 7: the "hdUE settings subnet" no menu creates

- `docs/substrate-builder.md:146`: "**Retry material preparation** (on the hdUE settings subnet) retries a failed graph..."
- `docs/substrate-builder.md:161`: the troubleshooting row "...then **Retry material preparation**".
- `docs/wet-surface-layer.md:101`: "turn on **Preview Primvar mask** on the hdUE settings subnet".
- `docs/developer/internals/substrate-builder.md:193`: "...and the mask-preview toggle in the hdUE settings subnet."
- Code: only `hdue_houdini.settings_node()` creates that subnet (`integrations/houdini/python3.13libs/hdue_houdini.py:830-848`). Its only callers are `scripts/generate_substrate_comparison.py:125` and `scripts/generate_substrate_textures.py:256` (plus tests). No menu, shelf tool or HDA calls it. The code calls it "an older hdUE_settings subnet" (`hdue_houdini.py:1153`, `:1171`).
- Fix:
  - If plan 017 has landed and added these controls to the **Unreal** menu (check `integrations/houdini/MainMenuCommon.xml` for "Retry" or "mask"), point the four places to that menu.
  - Otherwise, say how to get the controls: in Houdini's Python shell, `import hdue_houdini; hdue_houdini.settings_node(hou.node("<your display LOP>"))`. Check the function's signature at `hdue_houdini.py:830` and use its real argument. The subnet created has both toggles.

### Item 8: the Bistro camera path in the exposure tutorial

- `docs/tutorials/exposure-and-tone.md:14`: "Render with **Unreal Engine** through `camera`".
- `docs/tutorials/exposure-and-tone.md:53`: "On a **Camera** LOP in **Edit** mode for `/scene/camera`, set **Exposure** to 1."
- The scene: `samples/docs/bistro.usda:76` has `rel camera = </BistroInterior/group13/Camera>`, and line 19 is `over "Camera"`. `docs/tutorials/lighting-an-interior.md:16` already uses "the Bistro's `Camera`".
- Fix: `Camera` at line 14, `/BistroInterior/group13/Camera` at line 53.

### Item 9: references to `docs/houdini/index.md` for content it lacks

- `docs/building.md:63`: `... -UnrealLicenseesOnly  # licensees only: see docs/houdini/index.md`. The licensing text is in `docs/getting-started/installation.md` (top of the page, around lines 5-6).
- `docs/building.md:107`: "distribute the package only as `docs/houdini/index.md` describes". Same target.
- `docs/architecture.md:138`: "... keep the CPU path (`docs/houdini/index.md`)." The CPU-path content is in `docs/houdini/viewport.md` (around line 122; grep `HDUE_GPU_FRAMES`).
- `docs/architecture.md:192`: "...exported `op:` texture images, removed on shutdown (`docs/houdini/index.md`)." That content is in `docs/developer/internals/materials.md` (around line 69; grep `op:`).
- Fix each to the page that holds the text. Grep before linking.

### Item 10: the Live Render renderer label

- `docs/houdini/live-render.md:8`: "Set **Render Delegate** to **HdUnrealEngine**."
- `integrations/houdini/UsdRenderers.json` sets `"menulabel": "Unreal Engine"`. Every other page says **Unreal Engine** (`docs/houdini/husk.md`, `docs/getting-started/first-render.md`, `README.md`).

### Item 11: the broken test link

- `docs/developer/internals/substrate-builder.md:289`: links to `tests/houdini/python3.13libs/hdue_live_materials_test.py`, which does not exist.
- The suite that holds the regression is `tests/houdini/python3.13libs/hdue_substrate_live_test.py`. It is the only `*live*_test.py` that mentions topology or "Obsolete wet".
- The stale comment `integrations/houdini/python3.13libs/hdue_houdini.py:268-270` says "...in hdue_live_materials_test)". Fix the name in the comment. This is a comment-only change in a code file, in scope.

### Item 12: conformance counts and the UDIM sentence

- `docs/conformance.md:21`: "whole set (150 scenes, both modes) takes about 35 minutes".
- `docs/conformance.md:57-59`: "Last full run: this branch at `5ebd75f` ..., 150 scenes x 2 modes: all 150 pass in both modes".
- The set has 175 scenes today: `ls samples/conformance/*/*.usda`, excluding `common/`.
- `docs/conformance.md:92-94`: "UDIM patterns (warned in the UE log: the texture is not imported and BaseColor stays untextured)". The same page's matrix shows `<UDIM>` scenes passing in both modes, and `VirtualTextureBuild.cpp` builds runtime streaming virtual textures for them.
- Fix:
  - Line 21: drop the exact count ("the whole set").
  - Lines 92-94: say UDIM patterns load as runtime streaming virtual textures.
  - Lines 57-59: leave them unless you do the optional Step 6, a full run.

### Item 13: the render settings count

- `docs/rendering/render-settings.md:68`: "Three families cover all 371 settings:". The generated `docs/reference/render-settings/index.md` (around line 5) says 372.
- Fix: drop the number ("Three families cover every setting:"), so it cannot drift again.

### Item 14: the "Unreal Tonemapper is on" step in the interior tutorial (investigate first)

- `docs/tutorials/lighting-an-interior.md:17`: "Choose **Unreal > Create hdUE Render Settings**. Keep **Render Mode** on **Realtime (Lumen)** and check that **Unreal Tonemapper** is on."
- The Bistro's own settings prim sets it (`samples/docs/bistro.usda:82`: `bool unreal:tonemapper = 1`, on `/Render/Settings`).
- The menu action builds a new hdUE Render Settings LOP. Read `render_settings_node` in `hdue_houdini.py` (around lines 1049-1095) to see whether it copies `tonemapper`.
- `docs/tutorials/exposure-and-tone.md:16` says to turn it **off** to start, which is consistent either way. Leave that page.
- Fix:
  - If the LOP does not copy `tonemapper`, the user must turn it on themselves. Change "check that ... is on" to "turn **Unreal Tonemapper** on (the Bistro's own settings have it on; the new LOP starts with it off)".
  - If it does copy it, leave the line.

### Item 15: docs/architecture.md, four stale facts

- `:70`: "its types are `HdUnrealEngine` and two scene index plugins." `resources/plugInfo.json.in` registers 9 `HdSceneIndexPlugin` types (`grep -c HdSceneIndexPlugin resources/plugInfo.json.in` → 9), plus adapter and schema types. Say "and its scene index plugins, adapters and schemas (listed in `resources/plugInfo.json.in`)".
- `:82`: "`-userdir="<HDUE_USER_DIR>"` when set". The backend always passes `-userdir`, defaulting to `%LOCALAPPDATA%\hdUE` (Item 2's code).
- `:172`: "UE's log: `<runtime project>\Saved\Logs\HdUEBackend.log` ..., or under `HDUE_USER_DIR` (`-userdir`) when the package sets it." Change it to `%LOCALAPPDATA%\hdUE\Saved\Logs\HdUEBackend.log`, or under `HDUE_USER_DIR` when set. AGENTS.md and README already say this.
- `:196-214`: the ABI table "45 function pointers" omits six functions. Compare the table rows (`docs/architecture.md:200-214`) with the `HDUE_CALL*` members of `HdueApi` in `include/hdue/bridge.h` (around lines 665-762). At `6fe3e12` exactly these six are missing from the rows (they appear only in the "Recent versions" prose): `update_meshes`, `upsert_local_fog_volume`, `upsert_groom`, `upsert_post_process_volume`, `upsert_decal`, `get_load_status`. Add them to the rows in the table's style, in an existing row whose purpose fits or as new rows.
- `:218`: "(each descriptor version 1)". `hdue_set_viewport_surfaces` uses version 2 (`src/houdini/diagnostics.cpp:36` checks `abi_version != 2`). Give the version per export, or say "versioned per descriptor (see `viewport_api.h`)".

### Item 16: unpinned formatter commands in building.md

- `docs/building.md:89`: "`uvx black scripts tests integrations` and `uvx ruff check --fix ...`". CI pins Black 24.10.0 and Ruff 0.7.4 (`.github/workflows/ci.yml:37-38`). The latest versions format differently.
- Fix: `uvx black@24.10.0 scripts tests integrations` and `uvx ruff@0.7.4 check --fix scripts tests integrations`.

### Not in this plan

- The `bridge.h:62-64` threading comment belongs to plan 014.
- The `HdueGroomDesc.simulate` comment belongs to plan 007.
- `autoViewerLight` documented On while the delegate reads Off belongs to plan 013, which decides the default. Do not change its docs here.
- The generated pages under `docs/reference/` are written by `scripts/generate_render_settings.py` and `scripts/generate_node_docs.py`. Never edit them by hand (AGENTS.md).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Install the docs tool (once) | `uv tool install zensical==0.1.0` | exit 0 |
| Strict docs build | `zensical build --clean --strict` | exit 0, no warnings about links |
| Find a phrase | `git grep -n "<phrase>" -- docs README.md VERIFICATION.md` | the expected lines |
| Format the one Python comment edit | `uvx black@24.10.0 --check integrations/houdini/python3.13libs/hdue_houdini.py` | `1 file would be left unchanged` |
| Optional full conformance run | `hython scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --matrix` | updates the matrix between the `<!-- matrix:start -->` markers; over an hour |

`zensical build` writes the `site/` folder, which `.gitignore:42` ignores (`/site/`), so it never shows in `git status`.

## Scope

**In scope**:
- `README.md`, `VERIFICATION.md`
- `docs/materials.md`, `docs/developer/internals/materials.md`, `docs/getting-started/installation.md`, `docs/cameras.md`, `docs/rendering/debug-views.md`, `docs/environment/volumes.md`, `docs/substrate-builder.md`, `docs/wet-surface-layer.md`, `docs/developer/internals/substrate-builder.md`, `docs/tutorials/exposure-and-tone.md`, `docs/tutorials/lighting-an-interior.md`, `docs/building.md`, `docs/architecture.md`, `docs/houdini/live-render.md`, `docs/conformance.md`, `docs/rendering/render-settings.md`
- `integrations/houdini/python3.13libs/hdue_houdini.py`: the comment at lines 268-270 only.

**Out of scope**:
- Everything under `docs/reference/` (generated) and `docs/audit/` (audit records).
- Any code change, including adding menu entries for Item 7. That is plan 017's.
- Benchmark numbers: no re-measuring. Item 5 removes numbers rather than updating them.
- `AGENTS.md` and `CLAUDE.md`.

## Git workflow

- Work on the operator's branch or worktree; if none, `main` (AGENTS.md).
- Make one commit for items 1-14 and 16, and one for item 15 (architecture.md). Subjects are plain English, like `git log`: for example "User guide: the caches, logs, tonemapper and Nanite views as the code has them", and "Architecture: plugin types, log folder, ABI table and descriptor versions". The body lists the items fixed and the code lines each relied on, and ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Never push unless told. The docs workflow publishes on push to `main`.

## Steps

### Step 1: Fix items 1-4, 6, 8, 10, 13 and 16 (direct text corrections)

For each item, open the file at the cited line, confirm the quoted text is there, and replace it with the corrected statement. Re-read the code line cited in the item first.

**Verify**: each of these returns no match:
- `git grep -n "never trimmed\|never evicted" -- docs` → nothing
- `git grep -n "Houdini preferences folder" -- docs` → nothing
- `git grep -n "Tonemapper\*\* on (the default)" -- docs` → nothing
- `git grep -n "In husk, meshes from the stage are not Nanite" -- docs` → nothing
- `git grep -n "/scene/camera" -- docs/tutorials/exposure-and-tone.md` → nothing
- `git grep -n "to \*\*HdUnrealEngine\*\*" -- docs` → nothing
- `git grep -n "all 371 settings" -- docs` → nothing
- `git grep -n "uvx black scripts" -- docs` → nothing

### Step 2: Fix items 5, 9, 11 and 12 (references and stale claims)

**Verify**:
- `git grep -n "docs/houdini/index.md" -- docs` → only lines whose content really is in `docs/houdini/index.md`, or nothing.
- `git grep -n "hdue_live_materials_test" -- docs integrations` → nothing.
- `git grep -n "BaseColor stays untextured" -- docs` → nothing.
- `git grep -n "2-13x faster" -- VERIFICATION.md` → nothing.

### Step 3: Item 7, the settings subnet

Check plan 017's status in `plans/README.md`, and check `integrations/houdini/MainMenuCommon.xml` for retry or mask-preview entries. Then write the version described in Item 7.

**Verify**: `git grep -n "hdUE settings subnet" -- docs` → nothing, or only lines that also say how to create the subnet.

### Step 4: Item 14, investigate, then fix

Read `render_settings_node` in `hdue_houdini.py`. Write down whether it copies `tonemapper`, and fix `lighting-an-interior.md:17` as Item 14 says.

**Verify**: `git grep -n "check that \*\*Unreal Tonemapper\*\* is on" -- docs` → nothing, or the line is unchanged and your finding (it copies `tonemapper`) is noted in the commit body.

### Step 5: Item 15, architecture.md

**Verify**:
- `git grep -n "two scene index plugins" -- docs` → nothing.
- `git grep -n "each descriptor version 1" -- docs` → nothing.
- Every `HdueApi` function is in a table row (the names already appear in the prose below the table, so check the rows only), run in Git Bash:
  `for f in $(grep -o 'HDUE_CALL\* [a-z_]*' include/hdue/bridge.h | awk '{print $2}'); do grep '^| ' docs/architecture.md | grep -q "\b$f\b" || echo "missing: $f"; done` → prints nothing (at `6fe3e12` it prints the six names above).

### Step 6 (optional, heavy): refresh the conformance run line

Only if the operator asked for a full conformance run (it takes over an hour, AGENTS.md): run the `--matrix` command, then update `docs/conformance.md:57-59` with the new commit, scene count and pass count. Otherwise leave those lines, and write "conformance run line not refreshed" in the status row.

### Step 7: Build the docs strictly

**Verify**: `zensical build --clean --strict` → exit 0. Then `git status --short` lists only in-scope files.

## Test plan

- No code tests. The strict docs build checks links and anchors.
- Every changed sentence must trace to a code line. Put the item number and the code line you relied on in the commit body, so a reviewer can check each one.

## Done criteria

- [ ] All `git grep` checks in Steps 1-5 return what they state.
- [ ] `zensical build --clean --strict` exits 0.
- [ ] `git diff --stat 6fe3e12..HEAD` lists only in-scope files. The `hdue_houdini.py` diff is a comment-only change on lines 268-270.
- [ ] `git diff 6fe3e12..HEAD -- docs/reference docs/audit` is empty.
- [ ] `plans/README.md` status row updated, noting whether Step 6 ran.

## STOP conditions

Stop and report back (do not improvise) if:

- A quoted sentence is not at its cited line and cannot be found with `git grep`. Skip that item, note it, and continue with the others.
- A code line an item relies on says something different from this plan. For example, the virtual texture cache really has no eviction, or `tonemapper` defaults true. Then the doc may be right and the code changed: skip the item and report.
- Fixing an item seems to need a code change beyond the one comment in `hdue_houdini.py`.
- `zensical build --strict` fails on a page this plan did not touch. Report it; do not fix unrelated pages.

## Maintenance notes

- Counts in prose (scenes, settings, functions) drift. This plan removes several of them. Prefer "every setting" or a pointer to the generated reference over a number.
- When plan 013 settles the `autoViewerLight` default, check `docs/reference/` (regenerated) and any hand-written mention (`git grep -n "Light Unlit Scenes" -- docs`).
- When plan 017 adds the retry and mask-preview controls to a menu, revisit Item 7's four places.
- Reviewers: check each changed sentence against the cited code line, not against memory.
