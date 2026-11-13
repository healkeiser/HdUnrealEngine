# Plan 003: A failed UE boot, a failed GPU-frame import or a short AOV buffer no longer crashes the host

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- src/hydra/delegate.cpp src/hydra/gpu_frames.cpp`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

Three places on the boundary between hdUE's delegate and the UE backend can take down the
whole host process. (1) In Houdini's viewport, hdUE boots UE on its own thread without waiting;
a task queued behind the boot throws when the boot failed, nothing catches it on that thread,
and the C++ runtime calls `std::terminate`: Houdini closes instead of showing the boot error.
(2) When importing UE's shared GPU frames into Houdini's GL fails because a handle is missing,
the GPU path is not switched off, so the next redraw uses GL names that were never created.
(3) Two AOV publish functions index the backend's pixel buffer by `width * height * 4` without
checking the buffer's size, so a size mismatch reads past a heap buffer. After this plan each
case reports an error and the host keeps running.

## Current state

Files:

- `src/hydra/delegate.cpp` — the render delegate. The self-host worker loop at lines 340-375;
  the delegate constructor's renderer-info task at lines 3612-3640; `publishAov` at 2164-2219;
  `publishBuffers` at 2231-2255.
- `src/hydra/gpu_frames.cpp` — Houdini's zero-copy GL frames; `import()` at lines 46-83.
- `src/hydra/delegate.h` — `HdUEState::setError` at lines 355-358 (records the message shown
  to the user, under the state lock).

**(1) The worker runs tasks with no catch** (`src/hydra/delegate.cpp:357-375`):

```cpp
            if (task) {
                const auto start = std::chrono::steady_clock::now();
                task();
                ...
            } else {
                const auto start = std::chrono::steady_clock::now();
                try {
                    host.tick();
                } catch (const std::exception& e) {
                    TF_RUNTIME_ERROR("hdUE tick: %s", e.what());
                }
                host.pumping = true;
                for (auto& [key, pump] : host.pumps) pump();
                host.pumping = false;
```

After a failed boot the worker keeps serving the queue (`delegate.cpp:331-339`: "Keep serving
the queue: work posted meanwhile fails with UE's errors instead of waiting forever"). In
Houdini's UI the constructor does not wait for the boot and posts the renderer-info task
(`delegate.cpp:3612-3614` and `:3627-3639`):

```cpp
    const auto rendererInfo = [state = state_] {
        state->backend->require(state->backend->api().get_renderer_info(&state->rendererInfo), "get_renderer_info");
    };
    ...
            const bool wait = !houdiniUi();
            startSelfHost(dll, state_->setting<std::string>("runtimeProject", runtimeConfig("runtime_project")), wait);
            ...
            if (host.continuous)
                host.post(rendererInfo);
```

`hdue::Backend::require` throws `std::runtime_error` on any non-OK result
(`src/client/backend.cpp:80-84`), and the backend answers a not-ready runtime with
`HDUE_INVALID_STATE` (`unreal/HdUEBackend/Source/HdUEBackend/Private/Bridge.cpp:334-336`). So
a failed boot → the posted task throws on the worker thread → `std::terminate`.

Other posted work already protects itself: the streaming upload (`delegate.cpp:1974` onward)
and the render-stats refresh (`:4139-4150`) catch inside the lambda, and `executeOnce` catches
`std::exception` (`:3510`). `host.run()` wraps its work in a `std::packaged_task`, which stores
exceptions in the future, so `run()` callers are already safe. Only `post()`ed tasks and the
`pumps` run bare.

**(2) A null handle fails the import silently** (`src/hydra/gpu_frames.cpp:46-58`):

```cpp
bool HdUEGpuFrames::import(HdueGpuFrame& frame) {
    freeImports();
    ring_ = frame.ring;
    ...
    const auto texture = [&](uint64_t handle, uint64_t bytes, GLenum format, Imported& out) {
        if (!ok || !handle) {
            ok = false;
            return;
        }
```

`ok = false` is returned without calling `fail()`, so `failed_` stays false. `ring_` was
already set to the new ring (line 48), so the next `color()` call (line 95: `if (frame.ring !=
ring_ && !import(frame)) return {};`) skips the import and uses zero textures and semaphores.
`fail()` (lines 32-34) is the existing way to switch the GPU path off: it warns once and makes
`supported()` return false; the delegate then stops asking for GPU frames
(`delegate.cpp:2116`: `frames && !frames->failed() && ...`). Once the path is off, UE stops
writing the shared ring, so the acquired-but-unreleased slot no longer matters.

**(3) AOV publishing trusts the buffer size** (`src/hydra/delegate.cpp:2164-2172`):

```cpp
    void publishAov(HdUERenderBuffer& target, HdUEAovFormat format, const HdueFrameInfo& frame,
                    std::vector<float> rgba) const {
        const size_t count = size_t(frame.width) * frame.height;
        if (format == HdUEAovFormat::Int) {
            // Int32 buffers hold the integer bit patterns in the float storage.
            std::vector<float> out(count);
            for (size_t i = 0; i < count; ++i) {
                const int32_t value = rgba.empty() ? -1 : idAt(&rgba[i * 4]);
```

and further down (`:2189-2191`) `const float* p = &rgba[i * 4];` for every pixel. The buffer
is sized from the backend's `required_bytes` (`:3141-3143` and `:2242`), not from
`width * height`. `publishBuffers` hands each copy to `publishAov` (`:2253`), so one guard at
the top of `publishAov` covers both. An empty `rgba` is legal for the Int format (it means
"no hit": -1). `accumulateCryptomatte` already checks (`:2280`: `rgba.size() < count * 4`).

Conventions: errors that must reach the user go through `HdUEState::setError` (see
`delegate.cpp:1996`, `:2716`); one-time warnings use a `static std::once_flag` with
`std::call_once` (see `delegate.cpp:1166-1191`). Never silence a warning (AGENTS.md). C++ is
Google style, 4 spaces, 120 columns.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build the Houdini delegate (Houdini closed; runs ctest) | `.\scripts\Build-Houdini.ps1` | exit 0, ctest passes |
| Build the standalone delegate | `.\scripts\Build-Host.ps1` | exit 0 |
| Format check | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp src/hydra/gpu_frames.cpp` | no output |
| In-Houdini tests touched (viewport frames, recreation) | `.\scripts\Launch-Houdini.ps1 -TestKind integration` then `-TestKind recreation` | each exits 0 |
| AOV conformance | `hython scripts\run_conformance.py --runtime-config <repo>\.local\runtime-houdini.json --only render_settings/*` | "0 failing runs" |

## Scope

**In scope**:

- `src/hydra/delegate.cpp`
- `src/hydra/gpu_frames.cpp`

**Out of scope**:

- The boot itself (`startSelfHost`'s `try` block, `bootError`): it already records the error.
- `unreal/` backend code, `include/hdue/bridge.h` (no ABI change).
- Changing `executeOnce`'s catch handlers.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, on `main`. Never push unless told.
- One commit per step group (1-2, 3, 4) or one commit overall. Subject: a plain English sentence
  describing the result (e.g. "A failed UE boot in Houdini's viewport reports its error instead
  of closing Houdini"); body: why and how verified; last line
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Steps

### Step 1: Reproduce the boot crash (before changing code)

Make a broken runtime profile: copy `.local/runtime-houdini.json` to a scratch folder outside
the repo and set its `runtime_project` to a `.uproject` path that does not exist (keep
`backend_dll` absolute and valid). Start Houdini with it and an input scene:

`.\scripts\Launch-Houdini.ps1 -RuntimeConfig <scratch>\broken.json -InputUsd samples\conformance\mesh\visibility.usda`

Wait about 60 seconds after the viewport switches to Unreal Engine.

**Verify**: `Get-Process houdini -ErrorAction SilentlyContinue` → no process (Houdini closed),
or the Houdini console shows an abort. Close Houdini if it is still running. Record what you saw.

### Step 2: Catch exceptions from posted tasks and pumps

In the worker loop (`delegate.cpp:357-372`):

- Wrap `task();` in `try { task(); } catch (const std::exception& e) { TF_RUNTIME_ERROR("hdUE UE-thread task: %s", e.what()); } catch (...) { TF_RUNTIME_ERROR("hdUE UE-thread task failed"); }`.
- Wrap the pump loop body the same way (one `try` per pump, so one failing pump does not skip
  the others).

Then make the renderer-info task record the error where the user sees it. Change the lambda at
`:3612-3614` to catch and call `state->setError(e.what())` (keep `require` inside the `try`).
Leave the synchronous callers (`host.run(rendererInfo)`, `ueRun(rendererInfo)`) unchanged in
behaviour: they must still throw out of the constructor for husk. The simplest way is to keep
the throwing lambda and post a separate wrapper:

```cpp
            if (host.continuous)
                host.post([state = state_, rendererInfo] {
                    try {
                        rendererInfo();
                    } catch (const std::exception& e) {
                        state->setError(e.what());  // Houdini's UI did not wait for the boot: shown in the viewport
                    }
                });
```

Apply it at both `host.post(rendererInfo)` sites (`:3621` and `:3639`).

**Verify**: `grep -n "host.post(rendererInfo)" src/hydra/delegate.cpp` → no matches;
`grep -n "UE-thread task" src/hydra/delegate.cpp` → the new handlers.

### Step 3: Switch the GPU path off when an import has a missing handle

In `gpu_frames.cpp`, inside the `texture` lambda, call `fail("UE's shared frame has no handle")`
where it sets `ok = false` for a null handle (only when `ok` was still true, so the message
names the first cause). `fail()` is idempotent (it uses `failed_.exchange`).

**Verify**: `sed -n 54,60p src/hydra/gpu_frames.cpp` shows the `fail(` call.

### Step 4: Check the AOV buffer size before indexing it

At the top of `publishAov`, after computing `count`, add:

```cpp
        if (!rgba.empty() && rgba.size() < count * 4) {
            static std::once_flag warned;
            std::call_once(warned, [&] {
                TF_WARN("hdUE: an AOV frame holds %zu values for %ux%u pixels; it is not published", rgba.size(),
                        frame.width, frame.height);
            });
            return;
        }
```

**Verify**: `sed -n 2164,2178p src/hydra/delegate.cpp` shows the guard before the first `rgba[`.

### Step 5: Build, format, and confirm the boot no longer crashes

- `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror src/hydra/delegate.cpp src/hydra/gpu_frames.cpp` → no output.
- `.\scripts\Build-Houdini.ps1` → exit 0; `.\scripts\Build-Host.ps1` → exit 0.
- Repeat Step 1 with the new build.

**Verify**: after 60 seconds `Get-Process houdini` still lists Houdini, and the viewport (or the
console) shows the boot error message (the missing project). Close Houdini.

### Step 6: Regression runs

**Verify**:
- `.\scripts\Launch-Houdini.ps1 -TestKind integration` → exits 0.
- `.\scripts\Launch-Houdini.ps1 -TestKind recreation` → exits 0.
- `hython scripts\run_conformance.py --runtime-config <repo>\.local\runtime-houdini.json --only render_settings/*` → "0 failing runs" (covers AOV and Cryptomatte output).
- A Cornell husk render matches the previous build's.

## Test plan

- Step 1 / Step 5 are the regression test for (1): a broken runtime profile must not close Houdini.
- (2) and (3) are guards on conditions the current backend does not produce; they are covered by
  review and by the regression runs in Step 6 staying green. Do not add fault injection to the backend.

## Done criteria

- [ ] `grep -n "host.post(rendererInfo)" src/hydra/delegate.cpp` → no matches.
- [ ] `grep -c "UE-thread task" src/hydra/delegate.cpp` → at least 2.
- [ ] `grep -n "has no handle" src/hydra/gpu_frames.cpp` → 1 match.
- [ ] `grep -n "is not published" src/hydra/delegate.cpp` → 1 match.
- [ ] Both builds exit 0; clang-format clean.
- [ ] Step 5's broken-profile run leaves Houdini running and shows the boot error.
- [ ] `-TestKind integration`, `-TestKind recreation` and the `render_settings/*` conformance run pass.
- [ ] `git status --short` lists only the two in-scope files and `plans/README.md`.

## STOP conditions

- Step 1 does not crash Houdini on the old build. Then the crash path is different from the one
  described; report what happened (console output, the UE log under `%LOCALAPPDATA%\hdUE\Saved\Logs`)
  and stop before changing code.
- After Step 2, husk with the broken profile no longer exits non-zero
  (`husk --renderer "Unreal Engine" samples\conformance\mesh\visibility.usda` with
  `HDUE_RUNTIME_CONFIG` set to the broken profile must still fail): you changed the synchronous path.
- Any in-Houdini test or conformance run that passed before fails after.

## Maintenance notes

- Any new `host.post(...)` lambda should catch its own errors and route them to `setError`; the
  worker-level catch is a last line of defence that only logs.
- The AOV guard assumes RGBA float frames (4 values per pixel), which is what `copy_frame` and
  `copy_aov_buffer` return today (`include/hdue/bridge.h`). If a backend ever returns fewer
  channels, change the guard together with the format.
- Deferred: the acquired GPU slot is not explicitly released on a failed import; turning the
  GPU path off makes UE stop writing the ring, so no slot is waited on afterwards.
