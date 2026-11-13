# Plan 021: Cache files read back from disk are checked before UE uses them

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp src/client/backend.cpp docs/getting-started/environment-variables.md`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P3
- **Effort**: M
- **Risk**: LOW
- **Depends on**: plans/012 (texture mip and cache-key fixes bump `CacheVersion` in the same file; land 012 first so
  both changes share one version bump, or bump once more here). plans/006 owns the virtual-texture eviction guard.
- **Category**: security
- **Planned at**: commit `6fe3e12`, 2026-11-13

## Why this matters

To avoid rebuilding work, the UE backend keeps three disk caches: compressed textures
(`%LOCALAPPDATA%\hdUE\TextureCache`), Nanite mesh builds (`<UE Saved>\NaniteCache`) and runtime virtual textures for
UDIM images (`%LOCALAPPDATA%\hdUE\VirtualTextureCache`, or `HDUE_VIRTUAL_TEXTURE_CACHE`). Reading them back trusts the
sizes and offsets stored in the files. Files are written atomically, so a crash does not corrupt them, but a disk
error, a hand-edited file, or a cache folder shared across users (the code comment even suggests "a farm's shared
disk") can feed wrong sizes to the GPU upload and to UE's streaming. That reads past buffers or allocates huge amounts
of memory, which can crash every render node using the cache. After this plan, each entry's structure is checked when
read; a bad entry is ignored and rebuilt, and the docs say caches must not be shared across users.

## Current state

### Texture cache

`unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp:844` sets `constexpr int32 CacheVersion = 2;`. The reader
(`Scene.cpp:868-893`) checks the header, then trusts each mip's `Width`, `Height` and `Size`:

```cpp
TUniquePtr<FTexturePlatformData> ReadCache(const FString& Path, FHdueScene::FPreviewTexture& Texture) {
    TUniquePtr<FArchive> In(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!In) return nullptr;
    int32 Version = 0, SizeX = 0, SizeY = 0, Format = 0, Mips = 0;
    *In << Version << SizeX << SizeY << Format << Mips;
    if (In->IsError() || Version != CacheVersion || Format != BlockFormat(Texture)) return nullptr;
    ...
    for (int32 Level = 0; Level < Mips && !In->IsError(); ++Level) {
        int32 Width = 0, Height = 0;
        int64 Size = 0;
        *In << Width << Height << Size;
        if (In->IsError() || Size <= 0 || Size > In->TotalSize()) return nullptr;
        auto* Mip = new FTexture2DMipMap(Width, Height, 1);
        Data->Mips.Add(Mip);
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        In->Serialize(Mip->BulkData.Realloc(Size), Size);
```

`SizeX`, `SizeY`, `Mips` and each mip's `Width`/`Height` are never compared with each other or with `Size`. The writer
is `WriteCache` (`Scene.cpp:894-912`): it writes `Version, SizeX, SizeY, Format, Mips`, then per mip
`Width, Height, Size` and the bytes, to a temporary file renamed into place. The caller is `Scene.cpp:1111`:
`if (!Cache.IsEmpty() && (Entry.Pending = ReadCache(Cache, Entry))) continue;` — returning `nullptr` already makes it
rebuild the texture.

### Nanite cache

`Scene.cpp:1484-1512` (`BuildNanite`) loads `<Saved>/NaniteCache/<hash>.nanite`, runs UE's
`Nanite::FResources::Serialize` on it, then reads the fallback mesh:

```cpp
            TArray<FStaticMeshBuildVertex> Vertices;
            TArray<uint32> Indices;
            SerializeRaw(Reader, Vertices);
            SerializeRaw(Reader, Indices);
            if (!Reader.IsError() && Reader.AtEnd() && Indices.Num() >= 3) {
                Fallback = FallbackRenderData(Vertices, Indices);
```

The indices are never checked against `Vertices.Num()` before `FallbackRenderData` → `InitLOD` (`Scene.cpp:1417-1423`)
builds GPU buffers from them. On any failure the code resets the resources (`*Resources = Nanite::FResources();`) and
rebuilds, and the builder later rewrites the entry (`Scene.cpp:1578`).

### Virtual-texture cache

`unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp`:

- `SerializeManifest` (`:129-152`) reads sizes, `NumMips`, `TileOffsetData`, `BaseOffsetPerMip`, `ChunkIndexPerMip`,
  and per chunk `SizeInBytes`, `CodecPayloadSize`, `CodecPayloadOffset[0]`, and checks only the version and
  `0 <= Chunks <= 4096`.
- `HdueLoadVirtualTexture` (`:185-200`) checks each chunk file's size equals `SizeInBytes`, never the offsets that point
  into those chunks:

```cpp
    SerializeManifest(Reader, *Platform, *Data);
    if (Reader.IsError() || !Data->IsInitialized()) return nullptr;
    for (const FVirtualTextureDataChunk& Chunk : Data->Chunks)
        if (IFileManager::Get().FileSize(*(CacheDirectory() / Chunk.BulkDataHash.ToString())) != Chunk.SizeInBytes)
            return nullptr;                                                     // a chunk file deleted with the cache
```

UE's streaming then reads tiles from the chunk files at those offsets (the engine patch in
`scripts/Apply-ResidencyPatch.ps1:235-237` points it at `<cache>/<BulkDataHash>`). The struct is UE's
`FVirtualTextureBuiltData`; read its fields and how UE's own loader validates them in the engine source
(`Engine/Source/Runtime/Engine/Public/VT/VirtualTextureBuiltData.h` and `.../Private/VT/VirtualTextureBuiltData.cpp`,
engine root = `engine_root` in `.local/paths.json`).

Hashing every chunk on load is not an option: a large UDIM scene has tens of GB of chunks.

### Docs that encourage sharing

`src/client/backend.cpp:50-52` comment: "UE's Saved (logs, Nanite cache, config) per user in local app data ...
HDUE_USER_DIR moves it (a farm's shared disk)." `docs/getting-started/environment-variables.md:10` and `:35` describe
`HDUE_USER_DIR` and `HDUE_VIRTUAL_TEXTURE_CACHE` without saying who may write there.

### Conventions

- "Unreal is the benchmark": validate the way UE's own loaders do where they exist.
- A bad entry is not an error for the user: return `nullptr`/`false`, log one `UE_LOG(LogTemp, Warning, ...)` naming the
  file, and let the existing rebuild path run. Never silence the warning.
- C++ Google style, 4 spaces, 120 columns.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build backend | `.\scripts\Build-Backend.ps1` | exit 0 |
| Stage to a side profile | `.\scripts\Update-HoudiniRuntime.ps1 -OutputConfig H:\github\hdUE\.local\runtime-plan021.json` | exit 0 |
| Stage as default (end) | `.\scripts\Update-HoudiniRuntime.ps1 -SetDefault` | exit 0 |
| Targeted conformance | `& "<houdini_root>\bin\hython.exe" scripts\run_conformance.py --runtime-config <absolute profile path> --only texture/*` | `0 failing runs` |
| Format | `uvx --from clang-format==19.1.5 clang-format --dry-run --Werror <changed files>` | exit 0 |

Run the conformance areas `texture/*` (includes `texture/udim.usda`), `materialx/udim` (the other UDIM scene;
`git grep -l "<UDIM>" samples/conformance` lists both) and `mesh/*` (Nanite). Run each twice: the first run fills the
caches, the second reads them.

## Scope

**In scope**:
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` (`ReadCache`, the Nanite cache read, `CacheVersion`)
- `unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp` (`SerializeManifest` loading branch,
  `HdueLoadVirtualTexture`)
- `src/client/backend.cpp` (the comment at lines 50-52 only)
- `docs/getting-started/environment-variables.md` (the two rows)

**Out of scope**:
- Cache eviction and the `HDUE_VIRTUAL_TEXTURE_CACHE` delete guard — plan 006.
- Cache keys (file identity, LOD strip) and mip gamma — plan 012.
- UE's `Nanite::FResources::Serialize` internals: you cannot validate inside it; do not patch the engine.
- `scripts/Apply-ResidencyPatch.ps1` and any engine source.

## Git workflow

- Commit on the branch or worktree the operator gave you; if none, `main`.
- One commit per step. Subject: a plain English sentence describing the result, as in `git log`. Body: why and how
  verified. Last line: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Do NOT push unless told. Never push to Epic's or the upstream fork's repositories.

## Steps

### Step 1: Check the texture cache's mip chain

In `ReadCache`, before allocating, require:

- `SizeX` and `SizeY` in `1..GetMax2DTextureDimension()`, and `Mips` in `1..MAX_TEXTURE_MIP_COUNT`.
- For each level: `Width` and `Height` positive and equal to `max(1, SizeX >> Level)` / `max(1, SizeY >> Level)`
  **or** match what the writer stores for stripped LODs. Read `WriteCache` and the LOD-strip code
  (`FCookLODBias::Strip`, `Scene.cpp:792-795`) first: if the first stored mip is not `SizeX`×`SizeY`, use the first
  mip's size as the base for the halving check.
- `Size == CalculateImageBytes(Width, Height, 1, EPixelFormat(Format))` (UE's `RenderUtils.h`).

Return `nullptr` on any mismatch, with one warning naming the cache file. Bump `CacheVersion` only if you change what
is written (you should not need to).

**Verify**: `.\scripts\Build-Backend.ps1` → exit 0. Stage to the side profile, run `texture/*` twice →
`0 failing runs` both times, and the second run's backend log (`%LOCALAPPDATA%\hdUE\Saved\Logs\HdUEBackend.log`) has
no new cache warning (the valid entries still load).

### Step 2: Check the Nanite fallback mesh

After `SerializeRaw(Reader, Indices);`, require `Vertices.Num() > 0` and every index `< Vertices.Num()` (a loop, or
`Algo::AnyOf`). On failure fall through to the existing reset-and-rebuild path, with one warning naming the cache file.

**Verify**: build → exit 0; `mesh/*` twice → `0 failing runs`; no new warning on the second run.

### Step 3: Check the virtual-texture manifest's indices and offsets

In `HdueLoadVirtualTexture`, after the chunk-size loop, reject the manifest (return `nullptr` and warn once) unless:

- every `ChunkIndexPerMip` entry is `< Data->Chunks.Num()`;
- every chunk's `CodecPayloadOffset[0]` and `CodecPayloadSize` lie within its `SizeInBytes`;
- `NumMips` and the per-mip arrays (`BaseOffsetPerMip`, `ChunkIndexPerMip`) have consistent lengths, and every
  `BaseOffsetPerMip` / `TileOffsetData` value that is an offset into a chunk is below that chunk's `SizeInBytes`. Read
  `VirtualTextureBuiltData.h/.cpp` to learn which fields are offsets and what "unused" values look like (e.g. `~0u`).
- `Width`/`Height` are positive and `WidthInBlocks`/`HeightInBlocks` and `TileSize` are within the limits UE's builder
  uses.

Also delete a rejected manifest file (not its chunks) so the next build rewrites it; eviction (plan 006) cleans
orphaned chunks.

**Verify**: build → exit 0; the UDIM conformance scenes twice → `0 failing runs`; no new warning on the second run.

### Step 4: A deliberate corruption check (manual)

Copy one existing cache entry of each kind to a scratch folder first (to restore it). Then truncate or overwrite a few
bytes in the middle of one texture cache file, one `.nanite` file and one `.manifest` file, run the matching
conformance scene once, and restore nothing afterwards (the code rebuilds).

**Verify**: each run passes (`0 failing runs`), the backend log has exactly one warning per corrupted file naming it,
and no crash.

### Step 5: Say that caches must not be shared across users

- `src/client/backend.cpp:50-52`: replace "(a farm's shared disk)" with wording that `HDUE_USER_DIR` may point to
  another drive, but only to a folder that one user (or one farm account) writes.
- `docs/getting-started/environment-variables.md` rows for `HDUE_USER_DIR` and `HDUE_VIRTUAL_TEXTURE_CACHE`: add that
  the folder must be writable only by the user or render account using it, because hdUE trusts its contents.

**Verify**: `Select-String -Path src/client/backend.cpp -Pattern "shared disk"` → no match;
`Select-String -Path docs/getting-started/environment-variables.md -Pattern "only"` shows both rows.

### Step 6: Stage and run the touched areas once more

`.\scripts\Update-HoudiniRuntime.ps1 -SetDefault`; run `texture/*`, the UDIM scenes and `mesh/*`; one Cornell husk
render compared with the previous build.

**Verify**: `0 failing runs` in each; the Cornell image matches.

## Test plan

- No unit test can load UE caches without UE. The regression net is the conformance areas run twice (fill, then read),
  plus Step 4's manual corruption check.

## Done criteria

- [ ] `ReadCache` checks dimensions, mip count and `CalculateImageBytes` per mip
- [ ] The Nanite cache read checks fallback indices against the vertex count
- [ ] `HdueLoadVirtualTexture` checks chunk indices and offsets, and deletes a rejected manifest
- [ ] Step 4 manual check done; result in the commit body
- [ ] `src/client/backend.cpp` no longer suggests a shared disk; the env-var doc rows warn about sharing
- [ ] Backend build exits 0; all targeted conformance runs `0 failing runs`
- [ ] clang-format exits 0 on changed files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Valid cache entries written by the current build fail the new checks (the second conformance run logs warnings): the
  writer's layout differs from what this plan assumes; report which check fails.
- Step 3 needs fields of `FVirtualTextureBuiltData` whose meaning you cannot confirm from the engine source.
- Any check would need a change to UE's engine source or to `Apply-ResidencyPatch.ps1`.
- The excerpts above do not match the code.

## Maintenance notes

- Whenever a cache's write format changes, extend its read check in the same commit and bump that cache's version
  (`CacheVersion`, the Nanite key's "fallback key" tag, `ManifestVersion`).
- Residual risk, accepted: `Nanite::FResources::Serialize` trusts its bytes. Keeping caches per user (Step 5) is the
  mitigation. A content hash per entry would also catch tampering; deferred because it costs a full read of large
  entries on every load.
