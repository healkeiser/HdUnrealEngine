#pragma once
#include "CoreMinimal.h"
#include "ImageCore.h"
#include "PixelFormat.h"

struct FTexturePlatformData;

// The tiles of a UDIM file pattern ("<UDIM>" in its name), by UDIM index (1001, 1002, ...), as UE's importers find them
// (UE::TextureUtilitiesCommon::GetUDIMBlocksFromSourceFile). Empty when the pattern has no tile on disk.
TMap<int32, FString> HdueUdimTiles(const FString& Pattern);

// One source image of a virtual texture and its UDIM block (U, V tile offsets: 1001 is 0, 0; 1012 is 1, 1).
struct FHdueVirtualTextureBlock {
    int32 X = 0, Y = 0;
    FImage Image;
};
struct FHdueVirtualTextureCodec {
    EPixelFormat Format = PF_Unknown;  // what every physical tile is stored as
    // The mip chain of a block's image, top level first (the 2D textures' filter and alpha coverage).
    TFunction<TArray<FImage>(FImage&&)> Mips;
    // One physical tile (an image of the chain's format) into Format, GPixelFormats[Format]-sized blocks.
    TFunction<void(const FImage&, uint8*)> Encode;
};

// UE's streaming virtual texture data for Blocks (FVirtualTextureDataBuilder's layout: blocks with mip biases, a mip
// tail block shared by all blocks, bordered tiles in Morton order, raw GPU chunks streamed from hdUE's chunk cache on
// disk), as platform data of a UTexture2D with VirtualTextureStreaming. Address is the texture's TextureAddress on u
// and v. Key, when not empty, names the set's manifest in the cache (its sources and build choices) for
// HdueLoadVirtualTexture. Null, with Error, when UE's virtual textures cannot hold the set.
TUniquePtr<FTexturePlatformData> HdueBuildVirtualTexture(TArray<FHdueVirtualTextureBlock>&& Blocks,
                                                         const FHdueVirtualTextureCodec& Codec, const uint8 Address[2],
                                                         const FString& Key, FString& Error);
// The platform data a build with this Key cached, or null (none, or a chunk file missing).
TUniquePtr<FTexturePlatformData> HdueLoadVirtualTexture(const FString& Key);
