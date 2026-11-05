#pragma once
// Houdini's GL viewport reads UE's frames straight from the backend's shared D3D12 ring (hdue/bridge.h): each redraw
// makes GL wait for UE's fence on the GPU and copies the newest frame into HgiGL textures, which HdxAovInputTask
// composites instead of mapping and uploading the CPU render buffers. Any failure switches to the CPU path for good.
#include "hdue/backend.hpp"
#include "pxr/pxr.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hgi/texture.h"
#include <atomic>
#include <memory>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE
class Hgi;
class HdUEGpuFrames : public std::enable_shared_from_this<HdUEGpuFrames> {
public:
    HdUEGpuFrames(Hgi* hgi, std::shared_ptr<hdue::Backend> backend) : hgi_(hgi), backend_(std::move(backend)) {}
    // UE thread: the session the frames come from.
    void setSession(HdueSession session) { session_ = session; }
    bool failed() const { return failed_; }
    // GL thread (HdUERenderBuffer::GetResource). color copies the newest frame, colour and depth, and returns its
    // colour texture; depth returns the depth copied with it. Empty (the CPU path) without a frame of this size, and
    // once the CPU buffers hold the converged frame: Solaris draws its selection highlight from the CPU primId and
    // instanceId buffers, and only lines them up with a CPU colour buffer (a GPU texture of another size than the
    // viewport, as through a camera, left the highlight out).
    VtValue color(uint32_t width, uint32_t height, bool converged);
    VtValue depth() const { return depthCopied_ ? VtValue(depthTexture_) : VtValue(); }
    // GL thread, before the delegate goes: its GL objects are freed at once with the context they were made in current
    // (or any context when `last`: UE has shut down, so no delegate draws again), else by the next delegate's frame.
    // Later calls return empty.
    void release(bool last);

private:
    bool supported();
    bool import(HdueGpuFrame& frame);
    void freeImports();
    void fail(const std::string& reason);
    void freeRetired(bool now);
    void free(bool now);
    Hgi* hgi_;
    std::shared_ptr<hdue::Backend> backend_;
    std::atomic<HdueSession> session_{0};
    std::atomic<bool> failed_{false};
    // GL thread only.
    bool checked_ = false, released_ = false, depthCopied_ = false;
    uint64_t ring_ = 0;        // ring the GL objects below were imported from
    void* context_ = nullptr;  // the GL context (HGLRC) they were imported in
    struct Imported {
        unsigned memory = 0, texture = 0;
    };
    Imported color_[HDUE_GPU_SLOTS], depth_[HDUE_GPU_SLOTS];
    unsigned ready_ = 0, release_ = 0;  // semaphores on UE's ready and release fences
    HgiTextureHandle colorTexture_, depthTexture_;
};
PXR_NAMESPACE_CLOSE_SCOPE
