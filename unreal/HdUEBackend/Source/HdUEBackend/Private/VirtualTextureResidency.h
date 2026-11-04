#pragma once
#include "CoreMinimal.h"
#if !WITH_EDITOR
#include "hdue/bridge.h"
#include "UObject/StrongObjectPtr.h"
#include "VirtualTexturing.h"
class UTexture2D;
class FRHICommandListImmediate;

// The Path Tracer's virtual texture warm-up, as Movie Render Queue's warm-up frames: path-traced draws whose camera
// rays write VT feedback (PathTracingMaterialHitShader.usf, FinalizeVirtualTextureFeedback), unthrottled
// (FSceneViewFamily::bOverrideVirtualTextureThrottle, as MoviePipelineImagePassBase.cpp sets it), so UE produces every
// page the feedback requests in the frame that reads it. Ready once the pages of the physical pools the scene's virtual
// textures use stay the same for r.vt.FeedbackLatency + 2 draws; the session then restarts accumulation (Bridge.cpp
// Tick), so no sample reads the coarser mips drawn before the pages arrived. UE's own Path Tracer accumulates while
// pages stream. Locking every tile of every mip instead needed 2.1M pages on NVIDIA's Da Vinci Workshop, against a pool
// of 7,224.
// Game Thread constructs/reads; Render Thread alone reads the VT system (the engine patch grants the friend access).
class FHdueVirtualTextureResidency {
public:
    FHdueVirtualTextureResidency(TArray<TStrongObjectPtr<UTexture2D>>&& Textures, uint64 Generation,
                                 const HdueResidencyOptions& Options);
    void Update(FRHICommandListImmediate& Commands);  // Render Thread, before each warm-up draw, outside a VT update.
    HdueResidencyStatus Status() const;

private:
    TArray<TStrongObjectPtr<UTexture2D>> Textures;
    TArray<FVirtualTextureProducerHandle> Handles;
    HdueResidencyOptions Options;
    mutable FCriticalSection Mutex;
    HdueResidencyStatus Progress{};
    double Started;
    uint32 StableFrames = 0;
    void Fail(const FString& Message);
};
#endif
