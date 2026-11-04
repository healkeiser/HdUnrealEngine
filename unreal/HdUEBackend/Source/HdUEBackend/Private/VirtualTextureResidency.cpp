#include "VirtualTextureResidency.h"
#include "Message.h"
#if !WITH_EDITOR
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Misc/ScopeLock.h"
#include "HAL/IConsoleManager.h"
#include "RHICommandList.h"
#include "VT/VirtualTextureSystem.h"
#include "VT/VirtualTextureProducer.h"
#include "VT/VirtualTexturePhysicalSpace.h"
#include "VT/TexturePagePool.h"

FHdueVirtualTextureResidency::FHdueVirtualTextureResidency(TArray<TStrongObjectPtr<UTexture2D>>&& InTextures,
                                                           uint64 Generation, const HdueResidencyOptions& InOptions)
    : Textures(MoveTemp(InTextures)), Options(InOptions), Started(FPlatformTime::Seconds()) {
    Progress.struct_size = sizeof(Progress);
    Progress.generation = Generation;
    Progress.textures = Textures.Num();
    Progress.phase = Textures.IsEmpty() ? HDUE_RESIDENCY_READY : HDUE_RESIDENCY_PREPARING;
    for (const auto& Texture : Textures) {
        auto* Resource = Texture->GetResource();
        auto* VT = Resource ? Resource->GetVirtualTexture2DResource() : nullptr;
        if (!VT) {
            Fail(TEXT("Virtual texture resource is not ready; no Path Tracer samples were started."));
            break;
        }
        Handles.AddUnique(VT->GetProducerHandle());
    }
}
HdueResidencyStatus FHdueVirtualTextureResidency::Status() const {
    FScopeLock Lock(&Mutex);
    return Progress;
}
void FHdueVirtualTextureResidency::Fail(const FString& Message) {
    Progress.phase = HDUE_RESIDENCY_FAILED;
    HdueCopyMessage(Progress.message, Message);
}
void FHdueVirtualTextureResidency::Update(FRHICommandListImmediate& Commands) {
    check(IsInRenderingThread());
    if (Status().phase != HDUE_RESIDENCY_PREPARING) return;
    // The previous warm-up draw finished on the GPU: its feedback is mapped in this draw's VT update and its pages
    // produced there. Counted in draws the CPU queued ahead, the pages looked settled after 30 ms with 16 of them.
    Commands.BlockUntilGPUIdle();
    FScopeLock Lock(&Mutex);
    Progress.preparation_ms = (FPlatformTime::Seconds() - Started) * 1000;
    ++Progress.preparation_frames;
    auto& System = FVirtualTextureSystem::Get();
    UE::TScopeLock SystemLock(System.Mutex);
    check(!System.bUpdating);
    TSet<FVirtualTexturePhysicalSpace*> Spaces;
    for (const auto& Handle : Handles) {
        auto* Producer = System.FindProducer(Handle);
        if (!Producer) {
            Fail(TEXT("Virtual texture producer disappeared during the Path Tracer's warm-up."));
            return;
        }
        for (uint32 Group = 0; Group < Producer->GetNumPhysicalGroups(); ++Group)
            Spaces.Add(Producer->GetPhysicalSpaceForPhysicalGroup(Group));
    }
    uint64 Allocated = 0, Mapped = 0, Capacity = 0;
    for (FVirtualTexturePhysicalSpace* Space : Spaces) {
        const FTexturePagePool& Pool = Space->GetPagePool();
        Allocated += Pool.GetNumAllocatedPages();
        Mapped += Pool.GetNumMappedPages();
        Capacity += Pool.GetNumPages();
    }
    const bool Changed = Allocated != Progress.resident_pages || Mapped != Progress.ready_mappings;
    Progress.resident_pages = Allocated;
    Progress.ready_mappings = Mapped;
    Progress.required_pages = Capacity;
    StableFrames = Changed ? 0 : StableFrames + 1;
    if (Options.maximum_pages && Allocated > Options.maximum_pages) {
        Fail(FString::Printf(TEXT("VT residency budget exceeded: %llu pages, budget %u. Path Tracer has not started."),
                             Allocated, Options.maximum_pages));
        return;
    }
    // Margin: UE maps a feedback buffer up to r.vt.FeedbackLatency frames later when it has more than one pending.
    static const IConsoleVariable* Latency = IConsoleManager::Get().FindConsoleVariable(TEXT("r.vt.FeedbackLatency"));
    if (StableFrames >= uint32(FMath::Max(Latency ? Latency->GetInt() : 3, 0)) + 2) {
        Progress.phase = HDUE_RESIDENCY_READY;
    } else if (Progress.preparation_ms > Options.timeout_seconds * 1000) {
        // Still changing: the view needs more pages than the pools hold and they replace each other, as in UE, which
        // samples what is resident. Start rather than never.
        UE_LOG(LogTemp, Warning,
               TEXT("hdUE: virtual texture pages still changed after %.0f s (%llu of %llu allocated); the Path Tracer "
                    "starts with them. A larger pool (r.VT.PoolSizeScale) keeps more resident."),
               Options.timeout_seconds, Allocated, Capacity);
        Progress.phase = HDUE_RESIDENCY_READY;
    }
}
#endif
