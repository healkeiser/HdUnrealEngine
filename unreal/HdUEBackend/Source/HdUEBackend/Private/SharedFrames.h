#pragma once
#include "CoreMinimal.h"
#include "hdue/bridge.h"

class FRDGBuilder;
class FRDGTexture;
class FRHITexture;
class FRHICommandListImmediate;
class FSceneView;

// configure_gpu_frames (hdue/bridge.h): beauty frames shared with the host's graphics API (Houdini's GL viewport)
// through a ring of shared D3D12 textures and fences, so the host composites them without a GPU->CPU readback or a CPU
// upload.
namespace HdueSharedFrames {
// Runtime owner thread.
HdueResult Configure(HdueSession Session, const HdueGpuOptions& Options);
HdueResult Poll(HdueSession Session, HdueFrameInfo& Frame);
bool Enabled(HdueSession Session, HdueGpuOptions& Options);  // draws for Session write the ring with Options
void Reset();                                                // the session ended
// Any thread (the host's graphics thread).
HdueResult Acquire(HdueSession Session, HdueGpuFrame& Frame);
// Render thread, in this order for each beauty draw. BeginFrame returns the slot to write, or -1 to skip the ring.
int32 BeginFrame(FRHICommandListImmediate& RHICmdList, uint32 Width, uint32 Height, bool Depth);
void WriteDepth(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTexture* SceneDepth, int32 Slot,
                const HdueRenderRequest& Request, float DepthScale, float ScreenFraction);
void WriteColor(FRHICommandListImmediate& RHICmdList, FRHITexture* Source, int32 Slot, float ColorScale,
                uint64 Generation, bool Converged);
}  // namespace HdueSharedFrames
