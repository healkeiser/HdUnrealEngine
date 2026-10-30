#include "gpu_frames.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/base/tf/diagnostic.h"
#include <windows.h>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
void closeHandles(const HdueGpuFrame& frame) {
    for (uint32_t slot = 0; slot < HDUE_GPU_SLOTS; ++slot) {
        if (frame.color[slot]) CloseHandle(reinterpret_cast<HANDLE>(frame.color[slot]));
        if (frame.depth[slot]) CloseHandle(reinterpret_cast<HANDLE>(frame.depth[slot]));
    }
    if (frame.ready_fence) CloseHandle(reinterpret_cast<HANDLE>(frame.ready_fence));
    if (frame.release_fence) CloseHandle(reinterpret_cast<HANDLE>(frame.release_fence));
}
void clearErrors() {
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {
    }
}
}  // namespace
void HdUEGpuFrames::fail(const std::string& reason) {
    if (!failed_.exchange(true)) TF_WARN("hdUE: GPU viewport frames off (%s); using CPU frames", reason.c_str());
}
bool HdUEGpuFrames::supported() {
    if (!checked_) {
        checked_ = true;
        // HgiGL has loaded garch's GL entry points for this context.
        if (!GARCH_GL_EXT_memory_object_win32 || !GARCH_GL_EXT_semaphore_win32 || !glCopyImageSubData ||
            !glCreateTextures)
            fail("the GL driver has no GL_EXT_memory_object_win32 and GL_EXT_semaphore_win32");
    }
    return !failed_;
}
// The ring's textures and fences, imported once per ring (the backend makes a new one on resize).
bool HdUEGpuFrames::import(HdueGpuFrame& frame) {
    freeImports();
    ring_ = frame.ring;
    uint8_t luid[GL_LUID_SIZE_EXT] = {};
    glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, luid);
    bool ok = std::memcmp(luid, frame.adapter_luid, sizeof(luid)) == 0;
    if (!ok) fail("Houdini's GL and UE's D3D12 run on different GPUs");
    const auto texture = [&](uint64_t handle, uint64_t bytes, GLenum format, Imported& out) {
        if (!ok || !handle) {
            ok = false;
            return;
        }
        glCreateMemoryObjectsEXT(1, &out.memory);
        const GLint dedicated = GL_TRUE;  // D3D12 committed resources are dedicated allocations
        glMemoryObjectParameterivEXT(out.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        glImportMemoryWin32HandleEXT(out.memory, bytes, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT,
                                     reinterpret_cast<void*>(handle));
        glCreateTextures(GL_TEXTURE_2D, 1, &out.texture);
        glTextureStorageMem2DEXT(out.texture, 1, format, GLsizei(frame.width), GLsizei(frame.height), out.memory, 0);
    };
    for (uint32_t slot = 0; slot < HDUE_GPU_SLOTS; ++slot) {
        texture(frame.color[slot], frame.color_bytes, GL_RGBA16F, color_[slot]);
        if (frame.has_depth) texture(frame.depth[slot], frame.depth_bytes, GL_R32F, depth_[slot]);
    }
    if (ok) {
        glGenSemaphoresEXT(1, &ready_);
        glGenSemaphoresEXT(1, &release_);
        glImportSemaphoreWin32HandleEXT(ready_, GL_HANDLE_TYPE_D3D12_FENCE_EXT,
                                        reinterpret_cast<void*>(frame.ready_fence));
        glImportSemaphoreWin32HandleEXT(release_, GL_HANDLE_TYPE_D3D12_FENCE_EXT,
                                        reinterpret_cast<void*>(frame.release_fence));
    }
    closeHandles(frame);  // GL keeps its own references to imported NT handles
    if (const GLenum error = glGetError(); ok && error != GL_NO_ERROR)
        fail("importing UE's shared textures failed (GL error " + std::to_string(error) + ")");
    return ok && !failed_;
}
VtValue HdUEGpuFrames::color(uint32_t width, uint32_t height, bool converged) {
    depthCopied_ = false;
    const HdueSession session = session_;
    if (converged || released_ || !session || !supported()) return {};
    HdueGpuFrame frame{};
    frame.struct_size = sizeof(frame);
    frame.ring = ring_;
    if (backend_->gpu_api().acquire_gpu_frame(session, &frame) != HDUE_OK) return {};
    clearErrors();  // Houdini's own drawing may have left errors behind
    if (frame.ring != ring_ && !import(frame)) return {};
    // Copied again on every redraw: the tasks after HdxAovInputTask may draw into the texture they are given.
    const auto& source = color_[frame.slot];
    const unsigned textures[2] = {source.texture, depth_[frame.slot].texture};
    const GLenum layouts[2] = {GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT};
    const GLuint count = frame.has_depth ? 2 : 1;
    glSemaphoreParameterui64vEXT(ready_, GL_D3D12_FENCE_VALUE_EXT, &frame.frame_id);
    glWaitSemaphoreEXT(ready_, 0, nullptr, count, textures, layouts);
    const bool sized = frame.width == width && frame.height == height;  // a resized viewport waits for the new ring
    const auto copy = [&](HgiTextureHandle& target, unsigned from, HgiFormat format, const char* name) {
        if (!target || target->GetDescriptor().dimensions != GfVec3i(int(width), int(height), 1)) {
            if (target) hgi_->DestroyTexture(&target);
            HgiTextureDesc desc;
            desc.debugName = name;
            desc.format = format;
            desc.dimensions = GfVec3i(int(width), int(height), 1);
            desc.usage = HgiTextureUsageBitsColorTarget | HgiTextureUsageBitsShaderRead;
            target = hgi_->CreateTexture(desc);
        }
        glCopyImageSubData(from, GL_TEXTURE_2D, 0, 0, 0, 0, GLuint(target->GetRawResource()), GL_TEXTURE_2D, 0, 0, 0, 0,
                           GLsizei(width), GLsizei(height), 1);
    };
    if (sized) {
        copy(colorTexture_, source.texture, HgiFormatFloat16Vec4, "hdUE color");
        if (frame.has_depth) copy(depthTexture_, depth_[frame.slot].texture, HgiFormatFloat32, "hdUE depth");
    }
    // Always hand the slot back: UE does not rewrite it until this value passes.
    glSemaphoreParameterui64vEXT(release_, GL_D3D12_FENCE_VALUE_EXT, &frame.frame_id);
    glSignalSemaphoreEXT(release_, 0, nullptr, count, textures, layouts);
    glFlush();
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        fail("GL error " + std::to_string(error) + " reading UE's frame");
        return {};
    }
    if (!sized) return {};
    depthCopied_ = frame.has_depth != 0;
    return VtValue(colorTexture_);
}
void HdUEGpuFrames::freeImports() {
    for (auto* slots : {color_, depth_})
        for (uint32_t slot = 0; slot < HDUE_GPU_SLOTS; ++slot) {
            if (slots[slot].texture) glDeleteTextures(1, &slots[slot].texture);
            if (slots[slot].memory) glDeleteMemoryObjectsEXT(1, &slots[slot].memory);
            slots[slot] = {};
        }
    if (ready_) glDeleteSemaphoresEXT(1, &ready_);
    if (release_) glDeleteSemaphoresEXT(1, &release_);
    ready_ = release_ = 0;
    ring_ = 0;
}
void HdUEGpuFrames::release() {
    if (released_) return;
    released_ = true;
    // ponytail: without a current GL context the imported objects leak until Houdini exits; HgiGL defers its own.
    if (wglGetCurrentContext()) freeImports();
    if (colorTexture_) hgi_->DestroyTexture(&colorTexture_);
    if (depthTexture_) hgi_->DestroyTexture(&depthTexture_);
}
PXR_NAMESPACE_CLOSE_SCOPE
