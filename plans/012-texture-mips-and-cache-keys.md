# Plan 012: Data textures get linear mipmaps, and texture caches notice changed files and configs

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 6fe3e12..HEAD -- unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp src/hydra/color_management.cpp samples/conformance/`
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

A mipmap is a smaller copy of a texture, used when the surface is far away. hdUE builds mipmaps itself in the UE backend. For every 8-bit image it averages texels "in linear light", as for an sRGB colour image, even when the material reads the texture as data (roughness, metallic, opacity, normal maps, or any texture tagged `raw`). A 0/255 roughness checker then averages to 188 instead of 128 at the next mip, so surfaces look rougher at a distance and normal maps tilt. UE's own texture build filters non-sRGB textures linearly.

Three cache keys also miss inputs: the in-memory texture key ignores the file's time and size (a re-exported texture keeps showing the old image), the disk cache leaves the LOD strip out of most keys, and the OCIO texture cache ignores which OCIO config converted the image. After this plan, mips match UE's filtering, and changed files or configs produce new cache entries.

## Current state

Files:
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` — runtime textures: decode, mip chain, block compression, disk cache (`%LOCALAPPDATA%\hdUE\TextureCache`), virtual textures for UDIM sets.
- `unreal/HdUEBackend/Source/HdUEBackend/Private/VirtualTextureBuild.cpp` — builds and caches virtual textures (streaming tiles) from decoded blocks.
- `src/hydra/color_management.cpp` — OCIO conversion of colour textures to linear Rec.709, cached as EXRs in `%TEMP%\hdue_ocio`.

How UE tags decoded images. `FImageUtils::LoadImage` ends in `IImageWrapper::GetRawImage`, which does not know the image's gamma and guesses — `<engine_root>/Engine/Source/Runtime/ImageWrapper/Private/ImageWrapperBase.cpp:406-415`:

```cpp
	// ImageWrapper RGBFormat doesn't track if pixels are Gamma/sRGB or not
	//	just assume they are Default for now :
	EGammaSpace GammaSpace = ERawImageFormat::GetDefaultGammaSpace(RawFormat);
	...
	OutImage.GammaSpace = GammaSpace;
```

and `GetDefaultGammaSpace` returns sRGB for every 8-bit format — `<engine_root>/Engine/Source/Runtime/ImageCore/Public/ImageCore.h:97-123`:

```cpp
	// G8 and BGRA8 are affected by Gamma, 16/32 is NOT (they are always Linear)
	inline bool GetFormatNeedsGammaSpace(Type Format)
	{
		if ( Format == G8 || Format == BGRA8 ) return true;
		else return false;   // these formats ignore GammaSpace setting, always Linear
	}
	inline EGammaSpace GetDefaultGammaSpace(Type Format)
	{
		if ( GetFormatNeedsGammaSpace(Format) ) return EGammaSpace::sRGB;
		else return EGammaSpace::Linear;
	}
```

`FImage` also checks that formats which ignore gamma are tagged Linear (`ImageCore.h:225-230`, `check( GetFormatNeedsGammaSpace(Format) || GammaSpace == EGammaSpace::Linear )`). So the tag may only be set to sRGB on G8/BGRA8 images. `<engine_root>` is `engine_root` in `.local/paths.json`.

hdUE's 2x2 mip filter averages through sRGB tables whenever the image is tagged sRGB — `Scene.cpp:609-631` (`HalveImage`):

```cpp
        const bool SRGB = Source.GammaSpace == EGammaSpace::sRGB;
        ...
                Out[int64(Y) * TW + X] = FColor(Average(&FColor::R, SRGB), Average(&FColor::G, SRGB),
                                                Average(&FColor::B, SRGB), Average(&FColor::A, false));
```

`MipChain` (`Scene.cpp:644-659`) and the 16-bit-to-8-bit conversion keep the decoder's tag. `PrepareTexture` receives the slot's choice and never uses it — `Scene.cpp:805-812`:

```cpp
TUniquePtr<FTexturePlatformData> PrepareTexture(FImage&& Image, FHdueScene::FPreviewTexture& Texture, bool SRGB,
                                                const FCookLODBias& LOD) {
    if (!ERawImageFormat::IsHDR(Image.Format) && Image.Format != ERawImageFormat::BGRA8) {  // grey, 16-bit
        FImage Converted;
        Image.CopyTo(Converted, ERawImageFormat::BGRA8, Image.GammaSpace);
        Image = MoveTemp(Converted);
    }
    TArray<FImage> Chain = MipChain(MoveTemp(Image));
```

The slot's choice is `FPreviewTexture::SRGB` (`Scene.h:73`, "8-bit texels are sRGB-decoded"), set from `TextureSRGB` (`Scene.cpp:1004-1008`: `sourceColorSpace` sRGB/raw, or by slot for auto). The callers:
- `BuildTextureFile` (`Scene.cpp:1097-1117`): `FImageUtils::LoadImage(*File, Image)` at `:1112`, then `PrepareTexture(..., SRGB, LOD)`.
- `PrepareVirtualTexture` (`Scene.cpp:937-1003`): `FImageUtils::LoadImage(*Tiles[Index].Value, Blocks[Index].Image)` at `:960`, then halving at `:969-973`, conversion at `:981-985`, `MipChain` at `:990`.
- `VirtualTextureBuild.cpp:256` (`Image.ResizeTo(Resized, ..., Image.GammaSpace)`) works on the blocks it is given, so fixing the tag at decode fixes it too. `VirtualTextureBuild.cpp:299-301` builds its fallback texel from the first mip with an explicit Linear float image; it follows the block's tag.

The disk cache key — `Scene.cpp:844, 856-863`:

```cpp
constexpr int32 CacheVersion = 2;
...
    const FString Key =
        FString::Printf(TEXT("%s|%lld|%lld|%d|%d|%d|%d"), *FPaths::ConvertRelativePathToFull(File),
                        Stat.ModificationTime.GetTicks(), Stat.FileSize, SRGB, Texture.Read, Texture.Vector,
                        CacheVersion) +
        (Texture.Coverage > 0.f ? FString::Printf(TEXT("|coverage up %g"), Texture.Coverage) : FString()) +
        (LOD.Bias ? LOD.Key() : FString());  // keys of unbiased textures stay as they were
```

`LOD.Strip` (`Scene.cpp:792-795`) also strips mips when `MinLOD`/`MaxLOD` clamp (a 16k texture with the group's `MaxLODSize` 8192), which the key leaves out when `Bias` is 0. `ReadCache` (`Scene.cpp:868-893`) rejects entries whose stored `Version != CacheVersion`. The virtual texture key includes `CacheVersion` (`Scene.cpp:950-958`).

The in-memory key — `Scene.cpp:1088-1092`:

```cpp
FString TextureKey(const FString& File, const HdueTextureDesc& Texture, int32 Slot, float Coverage, bool Virtual) {
    return FString::Printf(TEXT("%s|%s|%d|%d|%g%s"), *File, TextureSRGB(Texture, Slot) ? TEXT("srgb") : TEXT("raw"),
                           int32(TextureWrap(Texture, 0)), int32(TextureWrap(Texture, 1)), Coverage,
                           Virtual ? TEXT("|vt") : TEXT(""));
}
```

`Apply` skips a key that already has a texture (`Scene.cpp:2021-2024`), so an image overwritten in place is never decoded again while the session lives. All three users of the key call `TextureKey` (`Scene.cpp:2015`, `:2161`, and the build completion at `:1717` uses the stored key), so changing the function is enough. `ImageSize` (`Scene.cpp:1028-1053`) already keys by `File|mtime|size` the same way.

The OCIO cache key — `src/hydra/color_management.cpp:245-252`:

```cpp
    std::string stamps;
    for (const auto& [source, tile] : images) {
        const auto stamp = std::filesystem::last_write_time(source, error);
        if (error) return file;
        stamps += "|" + tile + std::to_string(stamp.time_since_epoch().count());
    }
    if (images.empty()) return file;
    const auto key = std::to_string(std::hash<std::string>{}(file + "|" + from + "|" + m.sceneLinear + stamps));
```

`m.config` is an `OCIO::ConstConfigRcPtr` (`color_management.cpp:28-32`). The folder is `%TEMP%\hdue_ocio`, shared by every process and project. Two configs with the same space names but different definitions, or a change to the conversion code, reuse stale EXRs.

Conventions: comments explain why, one line where possible; UE-side code uses UE types (`FString`, `IFileManager`). Exemplar of a stat-keyed cache in this file: `ImageSize` (`Scene.cpp:1028-1053`).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build the backend | `.\scripts\Build-Backend.ps1` | exit 0 (~40 s incremental) |
| Stage it (Houdini may stay open) | `.\scripts\Update-HoudiniRuntime.ps1 -SetDefault` | exit 0, prints the hashed DLL name |
| Build the delegate (Houdini closed) | `.\scripts\Build-Houdini.ps1` | exit 0 |
| Unit suite | `.\scripts\Test.ps1 -Suite unit` | "Suite unit passed." |
| Conformance | `& "<houdini_root>\bin\hython.exe" scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only textures/* texture/* materialx/* preview_surface/*` | exit 0 |
| Format | `uvx --from clang-format==19.1.5 clang-format -i <files>` | no output |

(`samples/conformance/` has both `texture/` and `textures/`: `textures/` holds the image files, `texture/` the texture scenes. Check `ls samples/conformance/texture` before running.)

## Scope

**In scope**:
- `unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp`
- `src/hydra/color_management.cpp`
- `samples/conformance/texture/data_mips.usda` (create)
- `samples/conformance/textures/checker_1px.png` (create: a fixture image)

**Out of scope**:
- `VirtualTextureBuild.cpp` — it is fixed through the tag set at decode; do not change it unless Step 2's STOP condition fires.
- The delegate's material change detection (`src/hydra/materials.cpp:631-636`): Hydra does not re-sync a material when its file changes on disk, so a re-export shows only once the material is re-sent. Making the delegate notice file changes is a separate change (see Maintenance notes).
- Cache eviction policy, cache locations, HDR (EXR) textures.

## Git workflow

- Work on the branch or worktree the operator gave you; if none, `main` (AGENTS.md).
- One commit per step that changes files; plain English sentence subjects (e.g. `Data textures mip linearly, as UE filters non-sRGB textures`), a body with why and verification, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not push unless told.

## Steps

### Step 1: Add a conformance scene that sees the mip filter

Create `samples/conformance/textures/checker_1px.png`: 256 x 256, 8-bit RGB, texels alternating 0 and 255 in a one-texel checkerboard. Make it with Python's standard library only (no new dependency):

```powershell
uv run --python 3.11 python -c "import struct,zlib;w=256;rows=b''.join(b'\x00'+bytes(255 if (x+y)%2 else 0 for x in range(w) for _ in range(3)) for y in range(w));c=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d));open('samples/conformance/textures/checker_1px.png','wb').write(b'\x89PNG\r\n\x1a\n'+c(b'IHDR',struct.pack('>IIBBBBB',w,w,8,2,0,0,0))+c(b'IDAT',zlib.compress(rows))+c(b'IEND',b''))"
```

Create `samples/conformance/texture/data_mips.usda` (copy the header form of `samples/conformance/preview_surface/emissive_color.usda`, including its `over "Lights" { over "key" (active = false) }` so only emission shows):

- Material `Checker`: `UsdPreviewSurface` with `diffuseColor` (0, 0, 0), `emissiveColor` connected to a `UsdUVTexture` of `@../textures/checker_1px.png@` with `token inputs:sourceColorSpace = "raw"` (raw makes the slot data: `TextureSRGB` returns false), `st` from a `UsdPrimvarReader_float2` of `st`.
- Material `Grey`: the same with `@../textures/gray128.png@` (128/255 everywhere, also raw).
- Two 2 x 2 quads facing +Z with `st` (0,0)-(1,1): `Checker` at (-2.5, 0), `Grey` at (2.5, 0). At 128 x 128 pixels over 10 units, a 2-unit quad covers about 26 pixels, so the 256-texel checker is drawn from a mip around level 3: the average of the checker.
- Check `"data texture mips average linearly"`: `"ratio"` of `[-3, -0.5, -2, 0.5]` over `[2, -0.5, 3, 0.5]`, `"min": 0.9, "max": 1.1`. Linear averaging gives 0.5 / 0.502 ≈ 1.0; today's sRGB-weighted averaging gives about 188/255 ≈ 0.74 against 0.502, a ratio near 1.47.
- Doc string: explain the expected value and that the Path Tracer averages texels per pixel anyway, so this check discriminates in realtime mode.

**Verify**: build nothing yet; run `& "<houdini_root>\bin\hython.exe" scripts\run_conformance.py --runtime-config H:\github\hdUE\.local\runtime-houdini.json --only texture/data_mips --mode realtime` → the check fails with a ratio well above 1.1 (about 1.4).

### Step 2: Tag decoded images with the slot's encoding

In `Scene.cpp`, add one helper next to `PrepareTexture`:

```cpp
// UE's decoder guesses sRGB for every 8-bit image (ImageWrapperBase.cpp, GetDefaultGammaSpace); the slot decides:
// data textures (raw, scalar slots, normals) mip in their stored values, as UE's texture build filters non-sRGB textures.
void TagGamma(FImage& Image, bool SRGB) {
    if (ERawImageFormat::GetFormatNeedsGammaSpace(Image.Format)) Image.GammaSpace = SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
}
```

Call it:
- In `PrepareTexture`, first line: `TagGamma(Image, SRGB);` (before the 16-bit-to-BGRA8 `CopyTo`, which then keeps the right tag; for 16-bit sources `GetFormatNeedsGammaSpace` is false, so their Linear tag stays and `CopyTo(..., Image.GammaSpace)` produces a Linear BGRA8 image; then call `TagGamma(Image, SRGB)` once more after the conversion so an sRGB slot's converted image is tagged sRGB).
- In `PrepareVirtualTexture`, right after each successful `LoadImage` (inside the `ParallelFor` at `Scene.cpp:958-961`): `TagGamma(Blocks[Index].Image, Texture.SRGB);`.

This only re-tags; it does not convert pixel values. A data texture's stored bytes are the values the material reads, so re-tagging is correct.

**Verify**: `.\scripts\Build-Backend.ps1` → exit 0; `git grep -n "TagGamma" unreal/` → the helper and three calls.

STOP if `VirtualTextureBuild.cpp` or anything after `LoadImage` converts the image between gamma spaces before `MipChain` in a way that the re-tag would make wrong (look for `CopyTo(` with a different target gamma).

### Step 3: Bump the cache version and always key the LOD strip

In `Scene.cpp`:
- `constexpr int32 CacheVersion = 3;` with a comment `// 3: data textures mip linearly (TagGamma); every key names its LOD strip.` Every old disk entry and virtual texture is then rebuilt once.
- In `CachePath`, replace `(LOD.Bias ? LOD.Key() : FString())` and its comment with `LOD.Key()` (the version bump already changes every key, so keeping old keys stable no longer matters).

**Verify**: build → exit 0; `git grep -n "LOD.Bias ? LOD.Key" unreal/` → no match.

### Step 4: Key in-memory textures by file time and size

Change `TextureKey` to add the file's stat, as `ImageSize` does:

```cpp
FString TextureKey(const FString& File, const HdueTextureDesc& Texture, int32 Slot, float Coverage, bool Virtual) {
    // The file's time and size: an image overwritten in place loads again when its material is next sent.
    const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
    return FString::Printf(TEXT("%s|%lld|%lld|%s|%d|%d|%g%s"), *File, Stat.ModificationTime.GetTicks(), Stat.FileSize,
                           ...rest unchanged...);
}
```

For a UDIM pattern (`<UDIM>` in the name) the stat is invalid and its fields are 0; that is fine (the virtual texture build keys its tiles' stats itself at `Scene.cpp:950-955`).

**Verify**: build → exit 0. Then stage: `.\scripts\Update-HoudiniRuntime.ps1 -SetDefault` → exit 0.

### Step 5: Key the OCIO cache by config and conversion version

In `color_management.cpp:252`:

```cpp
    // The config's identity and this conversion's version: another config with the same space names (ACES 1 and 2),
    // or a change to the conversion below, must not reuse an EXR. Bump kConversion when the conversion changes.
    constexpr int kConversion = 1;
    const auto key = std::to_string(std::hash<std::string>{}(file + "|" + from + "|" + m.sceneLinear + "|" +
                                                             m.config->getCacheID() + "|" + std::to_string(kConversion) + stamps));
```

`m.config` is non-null here (the function returned earlier when it is null, `color_management.cpp:150`).

**Verify**: `.\scripts\Build-Houdini.ps1` → exit 0 (Houdini must be closed).

### Step 6: Run the checks

**Verify**, all of these:
- `run_conformance.py ... --only texture/data_mips --mode realtime` → passes (ratio between 0.9 and 1.1).
- `run_conformance.py ... --only textures/* texture/* materialx/* preview_surface/*` (both modes) → exit 0, 0 failing runs. `materialx/opacity_mips.usda` (alpha-coverage mips) must still pass.
- `.\scripts\Test.ps1 -Suite unit` → "Suite unit passed."
- Cornell husk render compared with the previous build: textured surfaces seen at a distance may change slightly; untextured ones must not.

## Test plan

- New: `samples/conformance/texture/data_mips.usda` with one realtime-discriminating `ratio` check and its fixture `checker_1px.png`. Modelled on `preview_surface/emissive_color.usda` (lights off, emission) and `preview_surface/normal_map.usda` (`ratio`).
- Regression: the check fails before Step 2 (Step 1 verifies it) and passes after.
- Existing: `materialx/opacity_mips.usda` guards alpha-coverage mips (`ScaleAlphaCoverage`), which this plan must not change; all texture and material scenes keep passing.
- The cache-key changes have no automated test (they need a live session with a file edited mid-session). Their done criteria are the code checks below.

## Done criteria

- [ ] `.\scripts\Build-Backend.ps1` and `.\scripts\Build-Houdini.ps1` exit 0.
- [ ] `run_conformance.py --only texture/* textures/* materialx/* preview_surface/*` exits 0 in both modes.
- [ ] `git grep -n "CacheVersion = 3" unreal/` matches once.
- [ ] `git grep -n "getCacheID" src/hydra/color_management.cpp` matches once.
- [ ] `git grep -n "GetStatData" unreal/HdUEBackend/Source/HdUEBackend/Private/Scene.cpp` includes a line inside `TextureKey`.
- [ ] `git status` lists only the in-scope paths (plus `plans/README.md`).
- [ ] `plans/README.md` status row updated.

## STOP conditions

Stop and report back (do not improvise) if:

- Step 1's check already passes before any change (the scene does not reach a low mip; report the measured ratio, do not change thresholds).
- `FImage`'s `check` (Linear for non-8-bit formats) fires in a debug run, or a build error shows `GammaSpace` cannot be assigned as written.
- `materialx/opacity_mips.usda` or any `texture/*` scene that passed before fails after Step 2.
- Colour textures (sRGB slots) visibly change brightness in the Cornell comparison: only data textures should change.
- `m.config->getCacheID()` does not exist in the OCIO version Houdini ships (compile error).

## Maintenance notes

- Any new place that decodes an image for a material must call `TagGamma` before mipping or converting. A reviewer should grep for `LoadImage(` in the backend.
- `CacheVersion` must be bumped whenever mip building, block compression or the key changes, and `kConversion` whenever the OCIO conversion changes.
- Deferred: making the delegate notice a texture file changed on disk (it compares `HdUEMaterialData`, which holds paths only). Until then, a re-exported texture shows once its material is re-sent (any material edit) or the session restarts.
- UE's own texture import also offers sharpening and other mip filters; hdUE keeps the 2x2 box filter for load time (see the `HalveImage` comment).
