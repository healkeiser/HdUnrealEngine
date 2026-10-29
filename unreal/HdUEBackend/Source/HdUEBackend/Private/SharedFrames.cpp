#include "SharedFrames.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "Misc/ScopeLock.h"

class FHdueSharedColorCS final : public FGlobalShader {
public:
    DECLARE_GLOBAL_SHADER(FHdueSharedColorCS);
    SHADER_USE_PARAMETER_STRUCT(FHdueSharedColorCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FUintVector2, Size)
        SHADER_PARAMETER(float, ColorScale)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SourceColor)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
    }
};
IMPLEMENT_GLOBAL_SHADER(FHdueSharedColorCS, "/HdUE/SharedFrame.usf", "ColorCS", SF_Compute);

class FHdueSharedDepthCS final : public FGlobalShader {
public:
    DECLARE_GLOBAL_SHADER(FHdueSharedDepthCS);
    SHADER_USE_PARAMETER_STRUCT(FHdueSharedDepthCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FUintVector2, Size)
        SHADER_PARAMETER(FIntPoint, ViewMin)
        SHADER_PARAMETER(FVector2f, ViewScale)
        SHADER_PARAMETER(float, NearClip)
        SHADER_PARAMETER(float, FarClip)
        SHADER_PARAMETER(uint32, Orthographic)
        SHADER_PARAMETER(float, DepthScale)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputDepth)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
    }
};
IMPLEMENT_GLOBAL_SHADER(FHdueSharedDepthCS, "/HdUE/SharedFrame.usf", "DepthCS", SF_Compute);

// The editor registers the shaders above so the cook compiles them; the rest needs the runtime target's D3D12RHI.
#if !WITH_EDITOR
#include "ID3D12DynamicRHI.h"

namespace {
// One ring per session size: textures, the fence UE signals once a frame is written (ready) and the fence the host
// signals once it has read one (release). Each texture and fence also has an NT handle the host imports.
struct FRing {
    uint64 Id = 0;
    uint32 Width = 0, Height = 0;
    bool Depth = false;
    FTextureRHIRef Color[HDUE_GPU_SLOTS], DepthTextures[HDUE_GPU_SLOTS];
    HANDLE ColorHandles[HDUE_GPU_SLOTS] = {}, DepthHandles[HDUE_GPU_SLOTS] = {}, ReadyHandle = nullptr, ReleaseHandle = nullptr;
    TRefCountPtr<ID3D12Fence> Ready, Release;
    uint64 ColorBytes = 0, DepthBytes = 0;
    uint64 Frames[HDUE_GPU_SLOTS] = {}; // ready fence value of the frame each slot holds
    uint8 Luid[8] = {};
    ~FRing() {
        for (HANDLE Handle : ColorHandles) if (Handle) CloseHandle(Handle);
        for (HANDLE Handle : DepthHandles) if (Handle) CloseHandle(Handle);
        if (ReadyHandle) CloseHandle(ReadyHandle);
        if (ReleaseHandle) CloseHandle(ReleaseHandle);
    }
};
using FRingRef = TSharedPtr<FRing, ESPMode::ThreadSafe>;
struct FState {
    FCriticalSection Mutex; // Acquire runs on the host's graphics thread, the ring is written on the render thread
    HdueSession Session = 0;
    HdueGpuOptions Options{};
    FRingRef Ring;
    bool Failed = false; // this RHI or device cannot share textures: the session stays on CPU frames
    uint64 NextRing = 1, NextValue = 0;
    int32 Newest = -1;
    uint64 Generation = 0;
    bool Converged = false;
    // The newest frame the host acquired: until the release fence reaches it, slots holding frames above the
    // release value and up to it may still be read and are not rewritten.
    uint64 Acquired = 0;
};
FState State;

FRingRef CreateRing(FRHICommandListImmediate& RHICmdList, uint32 Width, uint32 Height, bool Depth) {
    if (!IsRHID3D12()) return nullptr;
    ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
    ID3D12Device* Device = RHI->RHIGetDevice(0);
    FRingRef Ring = MakeShared<FRing, ESPMode::ThreadSafe>();
    Ring->Width = Width; Ring->Height = Height; Ring->Depth = Depth;
    const LUID Luid = Device->GetAdapterLuid();
    FMemory::Memcpy(Ring->Luid, &Luid, sizeof(Ring->Luid));
    const auto Share = [Device](ID3D12DeviceChild* Object, HANDLE& Handle) {
        return SUCCEEDED(Device->CreateSharedHandle(Object, nullptr, GENERIC_ALL, nullptr, &Handle));
    };
    // TexCreate_Shared: a committed resource on a shared heap with simultaneous access, so the host reads it while
    // UE writes the other slots without resource barriers on its side.
    const auto Create = [&](const TCHAR* Name, EPixelFormat Format, FTextureRHIRef& Texture, HANDLE& Handle, uint64& Bytes) {
        Texture = RHICmdList.CreateTexture(FRHITextureCreateDesc::Create2D(Name, int32(Width), int32(Height), Format)
            .SetFlags(TexCreate_Shared | TexCreate_UAV | TexCreate_ShaderResource));
        ID3D12Resource* Resource = Texture ? RHI->RHIGetResource(Texture) : nullptr;
        if (!Resource) return false;
        const D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
        Bytes = Device->GetResourceAllocationInfo(0, 1, &Desc).SizeInBytes;
        return Share(Resource, Handle);
    };
    for (uint32 Slot = 0; Slot < HDUE_GPU_SLOTS; ++Slot) {
        if (!Create(TEXT("Hdue.SharedColor"), PF_FloatRGBA, Ring->Color[Slot], Ring->ColorHandles[Slot], Ring->ColorBytes)) return nullptr;
        if (Depth && !Create(TEXT("Hdue.SharedDepth"), PF_R32_FLOAT, Ring->DepthTextures[Slot], Ring->DepthHandles[Slot], Ring->DepthBytes)) return nullptr;
    }
    if (FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(Ring->Ready.GetInitReference()))) ||
        FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(Ring->Release.GetInitReference()))) ||
        !Share(Ring->Ready, Ring->ReadyHandle) || !Share(Ring->Release, Ring->ReleaseHandle)) return nullptr;
    return Ring;
}
FRingRef CurrentRing() { FScopeLock Lock(&State.Mutex); return State.Ring; }
uint64 Duplicate(HANDLE Handle) {
    HANDLE Copy = nullptr;
    if (Handle) DuplicateHandle(GetCurrentProcess(), Handle, GetCurrentProcess(), &Copy, 0, false, DUPLICATE_SAME_ACCESS);
    return reinterpret_cast<uint64>(Copy);
}
FRDGTextureRef Register(FRDGBuilder& GraphBuilder, FRHITexture* Texture, const TCHAR* Name) {
    return GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Texture, Name));
}
}

namespace HdueSharedFrames {
HdueResult Configure(HdueSession Session, const HdueGpuOptions& Options) {
    if (Options.struct_size != sizeof(Options) || !FMath::IsFinite(Options.color_scale) || !FMath::IsFinite(Options.depth_scale))
        return HDUE_INVALID_ARGUMENT;
    if (Options.enabled && !IsRHID3D12()) return HDUE_NOT_SUPPORTED;
    FScopeLock Lock(&State.Mutex);
    // A new session, a disabled path or a depth change starts from a new ring (made on the next draw).
    if (Session != State.Session || !Options.enabled || Options.depth != State.Options.depth) { State.Ring.Reset(); State.Newest = -1; State.Acquired = 0; }
    if (Session != State.Session) State.Failed = false;
    State.Session = Session;
    State.Options = Options;
    return HDUE_OK;
}
HdueResult Poll(HdueSession Session, HdueFrameInfo& Frame) {
    if (Frame.struct_size != sizeof(Frame)) return HDUE_INVALID_ARGUMENT;
    FScopeLock Lock(&State.Mutex);
    if (State.Failed) return HDUE_NOT_SUPPORTED;
    if (Session != State.Session || !State.Ring || State.Newest < 0) return HDUE_PENDING;
    Frame = HdueFrameInfo{};
    Frame.struct_size = sizeof(Frame);
    Frame.width = State.Ring->Width; Frame.height = State.Ring->Height;
    Frame.generation = State.Generation; Frame.frame_id = State.Ring->Frames[State.Newest]; Frame.converged = State.Converged;
    return HDUE_OK;
}
bool Enabled(HdueSession Session, HdueGpuOptions& Options) {
    FScopeLock Lock(&State.Mutex);
    Options = State.Options;
    return Session == State.Session && State.Options.enabled && !State.Failed;
}
void Reset() {
    FScopeLock Lock(&State.Mutex);
    State.Ring.Reset(); State.Session = 0; State.Options = {}; State.Newest = -1; State.Acquired = 0; State.Failed = false;
}
HdueResult Acquire(HdueSession Session, HdueGpuFrame& Frame) {
    if (Frame.struct_size != sizeof(Frame)) return HDUE_INVALID_ARGUMENT;
    FScopeLock Lock(&State.Mutex);
    if (!Session || Session != State.Session || !State.Ring || State.Newest < 0) return HDUE_PENDING;
    const FRing& Ring = *State.Ring;
    const bool Fresh = Frame.ring != Ring.Id;
    Frame = HdueGpuFrame{};
    Frame.struct_size = sizeof(Frame);
    Frame.slot = uint32(State.Newest); Frame.ring = Ring.Id; Frame.frame_id = Ring.Frames[State.Newest];
    Frame.generation = State.Generation; Frame.converged = State.Converged;
    Frame.width = Ring.Width; Frame.height = Ring.Height; Frame.has_depth = Ring.Depth;
    Frame.color_bytes = Ring.ColorBytes; Frame.depth_bytes = Ring.DepthBytes;
    FMemory::Memcpy(Frame.adapter_luid, Ring.Luid, sizeof(Ring.Luid));
    if (Fresh) {
        // The caller owns these copies, so a ring replaced meanwhile cannot close a handle it is importing.
        for (uint32 Slot = 0; Slot < HDUE_GPU_SLOTS; ++Slot) {
            Frame.color[Slot] = Duplicate(Ring.ColorHandles[Slot]);
            Frame.depth[Slot] = Duplicate(Ring.DepthHandles[Slot]);
        }
        Frame.ready_fence = Duplicate(Ring.ReadyHandle);
        Frame.release_fence = Duplicate(Ring.ReleaseHandle);
    }
    State.Acquired = Frame.frame_id;
    return HDUE_OK;
}
int32 BeginFrame(FRHICommandListImmediate& RHICmdList, uint32 Width, uint32 Height, bool Depth) {
    FRingRef Ring = CurrentRing();
    if (!Ring || Ring->Width != Width || Ring->Height != Height || Ring->Depth != Depth) {
        Ring = CreateRing(RHICmdList, Width, Height, Depth);
        FScopeLock Lock(&State.Mutex);
        if (!Ring) {
            if (!State.Failed) UE_LOG(LogTemp, Warning, TEXT("hdUE: cannot create shared frame textures; the host uses CPU frames."));
            State.Failed = true;
            return -1;
        }
        Ring->Id = State.NextRing++;
        State.Ring = Ring; State.Newest = -1; State.Acquired = 0;
    }
    static const HANDLE Event = CreateEventW(nullptr, false, false, nullptr);
    for (int32 Attempt = 0; Attempt < 2; ++Attempt) {
        uint64 Busy = 0;
        {
            FScopeLock Lock(&State.Mutex);
            if (State.Ring != Ring) return -1;
            const uint64 Released = Ring->Release->GetCompletedValue();
            for (int32 Slot = 0; Slot < int32(HDUE_GPU_SLOTS); ++Slot) {
                if (Slot == State.Newest) continue; // the host acquires the newest frame next
                const uint64 Frame = Ring->Frames[Slot];
                if (Frame <= Released || Frame > State.Acquired) return Slot;
                Busy = Busy ? FMath::Min(Busy, Frame) : Frame;
            }
        }
        // Every other slot is still being copied by the host, which signals right after its copy.
        // ponytail: blocks the render thread up to 50 ms, then skips the ring for one draw; only a stalled host gets here.
        if (Attempt == 0 && SUCCEEDED(Ring->Release->SetEventOnCompletion(Busy, Event))) WaitForSingleObject(Event, 50);
    }
    return -1;
}
void WriteDepth(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTexture* SceneDepth, int32 Slot,
    const HdueRenderRequest& Request, float DepthScale, float ScreenFraction) {
    const FRingRef Ring = CurrentRing();
    if (!Ring || !Ring->Depth || Slot < 0 || !SceneDepth) return;
    auto* Parameters = GraphBuilder.AllocParameters<FHdueSharedDepthCS::FParameters>();
    Parameters->Size = FUintVector2(Ring->Width, Ring->Height);
    // The scene depth covers the view rect at the draw's screen percentage (reduced while navigating).
    Parameters->ViewMin = View.UnscaledViewRect.Min;
    Parameters->ViewScale = FVector2f(ScreenFraction, ScreenFraction);
    Parameters->NearClip = Request.near_clip_cm; Parameters->FarClip = Request.far_clip_cm;
    Parameters->Orthographic = Request.orthographic; Parameters->DepthScale = DepthScale;
    Parameters->SceneDepth = SceneDepth;
    Parameters->OutputDepth = GraphBuilder.CreateUAV(Register(GraphBuilder, Ring->DepthTextures[Slot], TEXT("Hdue.SharedDepth")));
    TShaderMapRef<FHdueSharedDepthCS> Shader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("hdUE shared depth"), Shader, Parameters,
        FComputeShaderUtils::GetGroupCount(FIntPoint(Ring->Width, Ring->Height), 8));
}
void WriteColor(FRHICommandListImmediate& RHICmdList, FRHITexture* Source, int32 Slot, float ColorScale,
    uint64 Generation, bool Converged) {
    const FRingRef Ring = CurrentRing();
    if (!Ring || Slot < 0 || !Source) return;
    FRDGBuilder GraphBuilder(RHICmdList);
    auto* Parameters = GraphBuilder.AllocParameters<FHdueSharedColorCS::FParameters>();
    Parameters->Size = FUintVector2(Ring->Width, Ring->Height);
    Parameters->ColorScale = ColorScale;
    Parameters->SourceColor = Register(GraphBuilder, Source, TEXT("Hdue.FrameColor"));
    Parameters->OutputColor = GraphBuilder.CreateUAV(Register(GraphBuilder, Ring->Color[Slot], TEXT("Hdue.SharedColor")));
    TShaderMapRef<FHdueSharedColorCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("hdUE shared colour"), Shader, Parameters,
        FComputeShaderUtils::GetGroupCount(FIntPoint(Ring->Width, Ring->Height), 8));
    GraphBuilder.Execute();
    const uint64 Value = ++State.NextValue; // render thread only
    RHICmdList.EnqueueLambda([Fence = Ring->Ready, Value](FRHICommandList& Commands) {
        GetID3D12DynamicRHI()->RHISignalManualFence(Commands, Fence, Value);
    });
    // Submit now: the host's GPU waits on this value as soon as it sees the frame.
    RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
    FScopeLock Lock(&State.Mutex);
    if (State.Ring != Ring) return;
    Ring->Frames[Slot] = Value;
    State.Newest = Slot; State.Generation = Generation; State.Converged = Converged;
}
}
#endif
