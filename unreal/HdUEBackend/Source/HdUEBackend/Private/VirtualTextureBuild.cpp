#include "VirtualTextureBuild.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UDIMUtilities.h"
#include "VT/VirtualTextureBuildSettings.h"
#include "VT/VirtualTextureBuiltData.h"  // Engine/Private (HdUEBackend.Build.cs)
#include "VirtualTexturing.h"

// UE builds virtual textures in the editor only (VirtualTextureDataBuilder.cpp, under WITH_EDITOR: it reads
// FTextureSource and the texture compressor modules). This is its layout and its choices, from the source images hdUE
// already decodes at run time: what differs is said where it does.

TMap<int32, FString> HdueUdimTiles(const FString& Pattern) {
    TMap<int32, FString> Tiles;
    const int32 Token = Pattern.Find(TEXT("<UDIM>"), ESearchCase::IgnoreCase);
    if (Token == INDEX_NONE) return Tiles;
    const FString Directory = FPaths::GetPath(Pattern), Head = FPaths::GetCleanFilename(Pattern.Left(Token)),
                  Tail = Pattern.Mid(Token + 6);
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *(Directory / (Head + TEXT("*") + Tail)), true, false);
    for (const FString& Name : Names) {
        if (Name.Len() != Head.Len() + 4 + Tail.Len()) continue;
        const FString Digits = Name.Mid(Head.Len(), 4);
        if (!Digits.IsNumeric() || !Name.StartsWith(Head) || !Name.EndsWith(Tail)) continue;
        const int32 Index = FCString::Atoi(*Digits);
        if (Index > 1000) Tiles.Add(Index, Directory / Name);
    }
    Tiles.KeySort(TLess<int32>());
    return Tiles;
}

// UploadingVirtualTexture.cpp (Apply-ResidencyPatch.ps1): where chunks without bulk data stream from.
extern ENGINE_API FString GHdueVirtualTextureChunkDirectory;

namespace {
// Built chunks are written to disk once and streamed from there, as the editor streams built chunks from its VT DDC
// cache (FVirtualTextureChunkDDCCache), so a set never stays in memory: NVIDIA's Da Vinci Workshop reads 62 gigapixels
// of UDIM tiles. One file per chunk, named by its hash, and a manifest per set (its layout and chunk hashes), keyed by
// its sources and build choices, so a reload skips decoding. HDUE_VIRTUAL_TEXTURE_CACHE moves the cache from
// %LOCALAPPDATA%/hdUE/VirtualTextureCache. ponytail: never evicted; delete the folder to reclaim space.
constexpr uint32 ManifestVersion = 1;
const FString& CacheDirectory() {
    static const FString Directory = [] {
        FString Path = FPlatformMisc::GetEnvironmentVariable(TEXT("HDUE_VIRTUAL_TEXTURE_CACHE"));
        if (Path.IsEmpty())
            Path = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA")), TEXT("hdUE"),
                                   TEXT("VirtualTextureCache"));
        if (!IFileManager::Get().MakeDirectory(*Path, true)) {
            UE_LOG(LogTemp, Warning,
                   TEXT("hdUE: cannot create the virtual texture cache %s; virtual textures stay in memory."), *Path);
            Path.Reset();
        }
        GHdueVirtualTextureChunkDirectory = Path;  // before any chunk of it can stream
        return Path;
    }();
    return Directory;
}
// A manifest also names the project's tile size and border, which lay out its chunks (r.VT.TileSize,
// r.VT.TileBorderSize).
FString ManifestPath(const FString& Key) {
    FVirtualTextureBuildSettings Settings;
    Settings.Init();
    return CacheDirectory() /
           FString::Printf(TEXT("vt_%s_%d_%d.manifest"), *Key, Settings.TileSize, Settings.TileBorderSize);
}
// The chunk's file, written once (another task may write the same chunk: a private file, then moved into place).
bool WriteChunk(const TArray<uint8>& Bytes, FSHAHash& Hash) {
    FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
    const FString Path = CacheDirectory() / Hash.ToString();
    if (IFileManager::Get().FileSize(*Path) == Bytes.Num()) return true;
    const FString Temporary = FPaths::CreateTempFilename(*CacheDirectory(), TEXT("chunk"));
    if (!FFileHelper::SaveArrayToFile(Bytes, *Temporary)) return false;
    if (!IFileManager::Get().Move(*Path, *Temporary, false, false, false, true))
        IFileManager::Get().Delete(*Temporary, false, false, true);
    return IFileManager::Get().FileSize(*Path) == Bytes.Num();
}
// The set's platform data, its chunks' hashes instead of their bytes, both ways.
void SerializeManifest(FArchive& Ar, FTexturePlatformData& Platform, FVirtualTextureBuiltData& Data) {
    uint32 Version = ManifestVersion;
    uint8 Format = uint8(Platform.PixelFormat), Layer = uint8(Data.LayerTypes[0]);
    int32 SizeX = Platform.SizeX, SizeY = Platform.SizeY, Chunks = Data.Chunks.Num();
    Ar << Version << SizeX << SizeY << Format << Data.NumLayers << Data.NumMips << Data.Width << Data.Height
       << Data.WidthInBlocks << Data.HeightInBlocks << Data.TileSize << Data.TileBorderSize << Layer
       << Data.LayerFallbackColors[0] << Data.TileDataOffsetPerLayer << Data.BaseOffsetPerMip << Data.ChunkIndexPerMip
       << Data.TileOffsetData << Chunks;
    if (Ar.IsLoading()) {
        if (Version != ManifestVersion || Chunks < 0 || Chunks > 4096) {
            Ar.SetError();
            return;
        }
        Platform.SizeX = SizeX;
        Platform.SizeY = SizeY;
        Platform.PixelFormat = EPixelFormat(Format);
        Data.LayerTypes[0] = EPixelFormat(Layer);
        Data.Chunks.SetNum(Chunks);
    }
    for (FVirtualTextureDataChunk& Chunk : Data.Chunks) {
        uint8 Codec = uint8(Chunk.CodecType[0]);
        Ar << Chunk.SizeInBytes << Chunk.CodecPayloadSize << Chunk.CodecPayloadOffset[0] << Codec << Chunk.BulkDataHash;
        Chunk.CodecType[0] = EVirtualTextureCodec(Codec);
    }
}

// VirtualTextureDataBuilder.cpp FPixelDataRectangle::ApplyBorderMode.
int32 BorderTexel(int32 X, int32 Size, TextureAddress Mode) {
    if (Mode == TA_Clamp) return FMath::Clamp(X, 0, Size - 1);
    if (Mode == TA_Mirror) {
        int32 Wrap = X % (2 * Size);
        Wrap += Wrap < 0 ? 2 * Size : 0;
        return Wrap < Size ? Wrap : Size - 1 - (Wrap - Size);
    }
    const int32 Wrap = X % Size;
    return Wrap + (Wrap < 0 ? Size : 0);
}
// A physical tile: TileSize texels from (X, Y) of Mip and BorderSize texels around them, outside texels by Address
// (CopyRectangleBordered).
FImage BorderedTile(const FImage& Mip, int32 X, int32 Y, int32 Physical, const uint8 Address[2]) {
    FImage Tile(Physical, Physical, Mip.Format, Mip.GammaSpace);
    const int64 Texel = Mip.GetBytesPerPixel();
    TArray<int32, TInlineAllocator<1032>> Columns;
    Columns.SetNumUninitialized(Physical);
    for (int32 I = 0; I < Physical; ++I) Columns[I] = BorderTexel(X + I, Mip.SizeX, TextureAddress(Address[0]));
    const uint8* In = Mip.RawData.GetData();
    uint8* Out = Tile.RawData.GetData();
    for (int32 Row = 0; Row < Physical; ++Row) {
        const uint8* Line = In + int64(BorderTexel(Y + Row, Mip.SizeY, TextureAddress(Address[1]))) * Mip.SizeX * Texel;
        for (int32 I = 0; I < Physical; ++I, Out += Texel) FMemory::Memcpy(Out, Line + Columns[I] * Texel, Texel);
    }
    return Tile;
}
struct FBlock {
    int32 X = 0, Y = 0;  // in blocks, Y in UE's flipped order
    int32 SizeInBlocksX = 1, SizeInBlocksY = 1;
    int32 SizeX = 0, SizeY = 0;  // top level of this block's chain
    int32 MipBias = 0, NumMips = 0;
    TArray<FImage> Mips;
    TArray<TArray<TArray<uint8>>> Tiles;  // per mip of the block, row-major tiles
};
}  // namespace

TUniquePtr<FTexturePlatformData> HdueLoadVirtualTexture(const FString& Key) {
    if (CacheDirectory().IsEmpty()) return nullptr;
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *ManifestPath(Key), FILEREAD_Silent)) return nullptr;
    auto Platform = MakeUnique<FTexturePlatformData>();
    auto* Data = new FVirtualTextureBuiltData;
    Platform->VTData = Data;
    FMemoryReader Reader(Bytes);
    SerializeManifest(Reader, *Platform, *Data);
    if (Reader.IsError() || !Data->IsInitialized()) return nullptr;
    for (const FVirtualTextureDataChunk& Chunk : Data->Chunks)
        if (IFileManager::Get().FileSize(*(CacheDirectory() / Chunk.BulkDataHash.ToString())) != Chunk.SizeInBytes)
            return nullptr;  // a chunk file deleted with the cache
    Platform->SetNumSlices(1);
    return Platform;
}

TUniquePtr<FTexturePlatformData> HdueBuildVirtualTexture(TArray<FHdueVirtualTextureBlock>&& Sources,
                                                         const FHdueVirtualTextureCodec& Codec, const uint8 Address[2],
                                                         const FString& Key, FString& Error) {
    check(Sources.Num() > 0);
    FVirtualTextureBuildSettings Settings;
    Settings.Init();  // the project's r.VT.TileSize and r.VT.TileBorderSize, as UE's texture build reads them
    const int32 TileSize = Settings.TileSize, Border = Settings.TileBorderSize, Physical = TileSize + 2 * Border;
    const FPixelFormatInfo& Format = GPixelFormats[Codec.Format];
    if (Physical % Format.BlockSizeX || Physical % Format.BlockSizeY) {
        Error = TEXT("Virtual texture tiles do not divide into the texture's blocks.");
        return nullptr;
    }
    // Block size: UE requires power-of-two blocks of one aspect ratio and at least a tile
    // (InitializeFromBuildSettings). Instead of failing, as its import does for other sizes, blocks are stretched to
    // the next power of two (ETexturePowerOfTwoSetting::StretchToPowerOfTwo) and to the largest block's aspect ratio.
    int32 BlockSizeX = 1, BlockSizeY = 1, SizeInBlocksX = 1, SizeInBlocksY = 1;
    for (const auto& Source : Sources) {
        BlockSizeX = FMath::Max(BlockSizeX, int32(FMath::RoundUpToPowerOfTwo(Source.Image.SizeX)));
        BlockSizeY = FMath::Max(BlockSizeY, int32(FMath::RoundUpToPowerOfTwo(Source.Image.SizeY)));
        SizeInBlocksX = FMath::Max(SizeInBlocksX, Source.X + 1);
        SizeInBlocksY = FMath::Max(SizeInBlocksY, Source.Y + 1);
    }
    while (BlockSizeX < TileSize || BlockSizeY < TileSize) BlockSizeX *= 2, BlockSizeY *= 2;
    const int64 Width = int64(BlockSizeX) * SizeInBlocksX, Height = int64(BlockSizeY) * SizeInBlocksY;
    if (FMath::CeilLogTwo64(FMath::Max(Width, Height) / TileSize) > VIRTUALTEXTURE_LOG2_MAX_PAGETABLE_SIZE) {
        Error = FString::Printf(TEXT("UDIM set of %lld x %lld texels is larger than UE's virtual textures."), Width,
                                Height);
        return nullptr;
    }
    const int32 NumMips = FMath::FloorLog2(uint32(FMath::Max(Width, Height))) + 1;
    const int32 BlockSizeInTilesX = FMath::DivideAndRoundUp(BlockSizeX, TileSize),
                BlockSizeInTilesY = FMath::DivideAndRoundUp(BlockSizeY, TileSize);
    // Mips a block keeps; with several blocks the coarser ones are in the mip tail block.
    const int32 MaxMipInBlock = FMath::CeilLogTwo(FMath::Min(BlockSizeInTilesX, BlockSizeInTilesY));

    TArray<FBlock> Blocks;
    Blocks.SetNum(Sources.Num());
    ParallelFor(Sources.Num(), [&](int32 Index) {
        FHdueVirtualTextureBlock& Source = Sources[Index];
        FBlock& Block = Blocks[Index];
        Block.X = Source.X;
        // UE applies a (1-y) transform to imported UVs, so UDIM rows are placed the same way (Build, BlockY).
        Block.Y = (SizeInBlocksY - Source.Y) % SizeInBlocksY;
        // The smallest power-of-two fraction of the block size the image fits, never below a tile: a smaller image
        // gets a mip bias and no texels at the finer levels, as UE stretches nothing but tile-sized blocks.
        while (Block.MipBias < MaxMipInBlock && (BlockSizeX >> (Block.MipBias + 1)) >= Source.Image.SizeX &&
               (BlockSizeY >> (Block.MipBias + 1)) >= Source.Image.SizeY)
            ++Block.MipBias;
        Block.SizeX = BlockSizeX >> Block.MipBias;
        Block.SizeY = BlockSizeY >> Block.MipBias;
        FImage Image = MoveTemp(Source.Image);
        if (Image.SizeX != Block.SizeX || Image.SizeY != Block.SizeY) {
            FImage Resized;
            Image.ResizeTo(Resized, Block.SizeX, Block.SizeY, Image.Format, Image.GammaSpace);
            Image = MoveTemp(Resized);
        }
        Block.Mips = Codec.Mips(MoveTemp(Image));
        const int32 Smallest = FMath::Min(Block.SizeX, Block.SizeY);
        Block.NumMips = FMath::Min<int32>(
            Block.Mips.Num(), Sources.Num() == 1 ? FMath::CeilLogTwo(Smallest) + 1
                                                 : FMath::CeilLogTwo(FMath::DivideAndRoundUp(Smallest, TileSize)) + 1);
        Block.Mips.SetNum(Block.NumMips);
    });
    // Mip tail: the blocks' coarsest mip, side by side in one image, and its mips (BuildLayerBlocks).
    if (Blocks.Num() > 1 && NumMips > MaxMipInBlock + 1) {
        const int32 MipWidth = FMath::Max(BlockSizeX >> MaxMipInBlock, 1),
                    MipHeight = FMath::Max(BlockSizeY >> MaxMipInBlock, 1);
        const FImage& First = Blocks[0].Mips[0];
        FImage Tail(FMath::RoundUpToPowerOfTwo(SizeInBlocksX * MipWidth),
                    FMath::RoundUpToPowerOfTwo(SizeInBlocksY * MipHeight), First.Format, First.GammaSpace);
        FMemory::Memzero(Tail.RawData.GetData(), Tail.RawData.Num());
        const int64 Texel = Tail.GetBytesPerPixel();
        for (const FBlock& Block : Blocks) {
            const FImage& Mip = Block.Mips[MaxMipInBlock - Block.MipBias];
            check(Mip.SizeX == MipWidth && Mip.SizeY == MipHeight);
            for (int32 Row = 0; Row < MipHeight; ++Row)
                FMemory::Memcpy(
                    &Tail.RawData[((int64(Block.Y) * MipHeight + Row) * Tail.SizeX + int64(Block.X) * MipWidth) *
                                  Texel],
                    &Mip.RawData[int64(Row) * MipWidth * Texel], MipWidth * Texel);
        }
        FBlock& Block = Blocks.AddDefaulted_GetRef();
        Block.SizeInBlocksX = SizeInBlocksX;
        Block.SizeInBlocksY = SizeInBlocksY;
        Block.MipBias = MaxMipInBlock + 1;
        Block.NumMips = NumMips - MaxMipInBlock - 1;
        Block.Mips = Codec.Mips(MoveTemp(Tail));
        Block.Mips.RemoveAt(0);  // the copy of the blocks' mip
        Block.NumMips = FMath::Min(Block.NumMips, Block.Mips.Num());
        Block.Mips.SetNum(Block.NumMips);
        Block.SizeX = Block.Mips[0].SizeX;
        Block.SizeY = Block.Mips[0].SizeY;
    }
    // Fallback colour, sampled before the root pages stream in: the coarsest level, averaged to one texel.
    FLinearColor Fallback;
    {
        FImage Texel(1, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
        Blocks.Last().Mips.Last().ResizeTo(Texel, 1, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
        Fallback = Texel.AsRGBA32F()[0];
    }
    // Bordered, encoded tiles of every block and mip (BuildBlockTiles).
    const int64 TileBytes = int64(Physical / Format.BlockSizeX) * (Physical / Format.BlockSizeY) * Format.BlockBytes;
    for (FBlock& Block : Blocks) {
        Block.Tiles.SetNum(Block.NumMips);
        for (int32 Mip = 0; Mip < Block.NumMips; ++Mip) {
            const FImage& Image = Block.Mips[Mip];
            const int32 TilesX = FMath::DivideAndRoundUp(FMath::Max(Block.SizeX >> Mip, 1), TileSize),
                        TilesY = FMath::DivideAndRoundUp(FMath::Max(Block.SizeY >> Mip, 1), TileSize);
            Block.Tiles[Mip].SetNum(TilesX * TilesY);
            ParallelFor(TilesX * TilesY, [&](int32 Index) {
                const FImage Tile = BorderedTile(Image, (Index % TilesX) * TileSize - Border,
                                                 (Index / TilesX) * TileSize - Border, Physical, Address);
                TArray<uint8>& Payload = Block.Tiles[Mip][Index];
                Payload.SetNumUninitialized(TileBytes);
                Codec.Encode(Tile, Payload.GetData());
            });
        }
        Block.Mips.Empty();
    }

    auto* Data = new FVirtualTextureBuiltData;
    Data->NumLayers = 1;
    Data->NumMips = NumMips;
    Data->Width = uint32(Width);
    Data->Height = uint32(Height);
    Data->WidthInBlocks = SizeInBlocksX;
    Data->HeightInBlocks = SizeInBlocksY;
    Data->TileSize = TileSize;
    Data->TileBorderSize = Border;
    Data->LayerTypes[0] = Codec.Format;
    Data->LayerFallbackColors[0] = Fallback;
    Data->TileDataOffsetPerLayer.Add(uint32(TileBytes));  // raw GPU tiles of one size: the compact tile offsets
    Data->BaseOffsetPerMip.Init(~0u, NumMips);
    // Chunks: tiles in Morton order per mip, a chunk per mip of at least 1024 x 1024 texels and the rest in one final
    // chunk, each a header then its tiles (BuildChunks, PushDataToChunk).
    struct FTileRef {
        const TArray<uint8>* Payload;
        int32 Mip;
    };
    TArray<FTileRef> InChunk;
    bool TooLarge = false, Cached = true;
    const auto Push = [&]() {
        const int64 Size = sizeof(FVirtualTextureChunkHeader) + InChunk.Num() * TileBytes;
        if (Size >= MAX_uint32) {  // the built data's offsets are 32-bit
            TooLarge = true;
            InChunk.Reset();
            return;
        }
        FVirtualTextureDataChunk& Chunk = Data->Chunks.AddDefaulted_GetRef();
        Chunk.SizeInBytes = uint32(Size);
        Chunk.CodecType[0] = EVirtualTextureCodec::RawGPU;
        Chunk.CodecPayloadOffset[0] = sizeof(FVirtualTextureChunkHeader);
        Chunk.CodecPayloadSize = sizeof(FVirtualTextureChunkHeader);
        TArray<uint8> Bytes;
        Bytes.SetNumZeroed(Size);
        int64 Offset = sizeof(FVirtualTextureChunkHeader);
        for (const FTileRef& Tile : InChunk) {
            if (Data->BaseOffsetPerMip[Tile.Mip] == ~0u) Data->BaseOffsetPerMip[Tile.Mip] = uint32(Offset);
            FMemory::Memcpy(Bytes.GetData() + Offset, Tile.Payload->GetData(), TileBytes);
            Offset += TileBytes;
        }
        // Without a cache (no writable folder) the chunk stays in memory and streams from there.
        if (CacheDirectory().IsEmpty() || !WriteChunk(Bytes, Chunk.BulkDataHash)) {
            Cached = false;
            Chunk.BulkData.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(Chunk.BulkData.Realloc(Size), Bytes.GetData(), Size);
            Chunk.BulkData.Unlock();
        }
        InChunk.Reset();
    };
    const uint32 MinTilesPerChunk = FMath::Square(FMath::DivideAndRoundUp<uint32>(1024u, TileSize));
    uint32 MipWidthInTiles = FMath::DivideAndRoundUp<uint32>(Width, TileSize),
           MipHeightInTiles = FMath::DivideAndRoundUp<uint32>(Height, TileSize);
    bool Final = false;
    for (int32 Mip = 0; Mip < NumMips; ++Mip) {
        Data->ChunkIndexPerMip.Add(Data->Chunks.Num());
        const int32 MipBlockSizeInTilesX = FMath::Max(BlockSizeInTilesX >> Mip, 1),
                    MipBlockSizeInTilesY = FMath::Max(BlockSizeInTilesY >> Mip, 1);
        // FVirtualTextureTileOffsetData Init / AddTile / Finalize (editor-only in UE): runs of present and missing
        // tiles over the mip's Morton addresses.
        FVirtualTextureTileOffsetData& Offsets = Data->TileOffsetData.AddDefaulted_GetRef();
        Offsets.Width = MipWidthInTiles;
        Offsets.Height = MipHeightInTiles;
        const uint32 Padded = FMath::RoundUpToPowerOfTwo(FMath::Max(MipWidthInTiles, MipHeightInTiles));
        Offsets.MaxAddress = Padded * Padded;
        uint32 Present = 0;
        int32 State = -1;
        const uint32 MaxTile =
            FMath::MortonCode2(MipWidthInTiles - 1) | (FMath::MortonCode2(MipHeightInTiles - 1) << 1);
        for (uint32 Morton = 0; Morton < Offsets.MaxAddress; ++Morton) {
            const TArray<uint8>* Payload = nullptr;
            const uint32 TileX = FMath::ReverseMortonCode2(Morton), TileY = FMath::ReverseMortonCode2(Morton >> 1);
            if (Morton <= MaxTile && TileX < MipWidthInTiles && TileY < MipHeightInTiles) {
                const int32 BlockX = TileX / MipBlockSizeInTilesX, BlockY = TileY / MipBlockSizeInTilesY;
                for (const FBlock& Block : Blocks)  // FindSourceBlockIndex
                    if (BlockX >= Block.X && BlockX < Block.X + Block.SizeInBlocksX && BlockY >= Block.Y &&
                        BlockY < Block.Y + Block.SizeInBlocksY && Mip >= Block.MipBias &&
                        Mip - Block.MipBias < Block.NumMips) {
                        const int32 InBlock = Mip - Block.MipBias;
                        const int32 BlockWidthInTiles =
                            FMath::DivideAndRoundUp(FMath::Max(Block.SizeX >> InBlock, 1), TileSize);
                        const int32 Index = (TileY - Block.Y * MipBlockSizeInTilesY) * BlockWidthInTiles +
                                            (TileX - Block.X * MipBlockSizeInTilesX);
                        Payload = &Block.Tiles[InBlock][Index];
                        break;
                    }
            }
            if (int32(Payload != nullptr) != State) {
                State = Payload != nullptr;
                Offsets.Addresses.Add(Morton);
                Offsets.Offsets.Add(State ? Present : ~0u);
            }
            if (Payload) {
                InChunk.Add({Payload, Mip});
                ++Present;
            }
        }
        if (!Final && uint32(InChunk.Num()) >= MinTilesPerChunk)
            Push();
        else
            Final = true;
        MipWidthInTiles = FMath::DivideAndRoundUp(MipWidthInTiles, 2u);
        MipHeightInTiles = FMath::DivideAndRoundUp(MipHeightInTiles, 2u);
    }
    if (InChunk.Num()) Push();
    if (TooLarge) {
        delete Data;
        Error = TEXT("UDIM set larger than a 4 GB virtual texture chunk.");
        return nullptr;
    }

    auto Platform = MakeUnique<FTexturePlatformData>();
    Platform->SizeX = Data->Width;
    Platform->SizeY = Data->Height;
    Platform->PixelFormat = Codec.Format;
    Platform->SetNumSlices(1);
    Platform->VTData = Data;
    if (Cached && !Key.IsEmpty()) {
        TArray<uint8> Manifest;
        FMemoryWriter Writer(Manifest);
        SerializeManifest(Writer, *Platform, *Data);
        const FString Temporary = FPaths::CreateTempFilename(*CacheDirectory(), TEXT("manifest"));
        if (FFileHelper::SaveArrayToFile(Manifest, *Temporary) &&
            !IFileManager::Get().Move(*ManifestPath(Key), *Temporary, true, false, false, true))
            IFileManager::Get().Delete(*Temporary, false, false, true);
    }
    return Platform;
}
