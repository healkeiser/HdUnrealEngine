#pragma once
#include "hdue/bridge.h"
#include "hdue/material_api.h"
#include "hdue/residency_api.h"
#include "hdue/asset_api.h"
#include "hdue/depth_api.h"
#include "hdue/aov_api.h"
#include "hdue/motion_api.h"
#include "hdue/postprocess_api.h"
#include "hdue/volume_api.h"
#include "hdue/lighting_api.h"
#include "hdue/surface_api.h"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <optional>

namespace hdue {
class Backend {
public:
    explicit Backend(const std::filesystem::path& dll);
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    ~Backend();
    const HdueApi& api() const { return api_; }
    const HdueNativePickApi& native_pick_api() const { return nativePickApi_; }
    const HdueMaterialApi& material_api() const { return materialApi_; }
    const HdueResidencyApi& residency_api() const { return residencyApi_; }
    const HdueAssetApi& asset_api() const { return assetApi_; }
    const HdueDepthApi& depth_api() const { return depthApi_; }
    const HdueAovApi& aov_api() const { return aovApi_; }
    const HdueMotionApi& motion_api() const { return motionApi_; }
    const HduePostProcessApi& postprocess_api() const { return postProcessApi_; }
    const HdueVolumeApi& volume_api() const { return volumeApi_; }
    const HdueLightingApi& lighting_api() const { return lightingApi_; }
    const HdueSurfaceApi& surface_api() const { return surfaceApi_; }
    // hdue_resize_session: resize a session's render target, keeping its scene. HDUE_INVALID_STATE on runtimes
    // without it (the caller recreates the session instead).
    HdueResult resize_session(HdueSession session, uint32_t width, uint32_t height) const;
    void initialize(const HdueInitDesc& desc);
    void shutdown();
    bool initialized() const { return initialized_; }
    std::string status_message() const;
    void require(HdueResult result, const char* operation) const;
private:
    void* module_ = nullptr;
    HdueApi api_{};
    HdueNativePickApi nativePickApi_{};
    HdueMaterialApi materialApi_{};
    HdueResidencyApi residencyApi_{};
    HdueAssetApi assetApi_{};
    HdueDepthApi depthApi_{};
    HdueAovApi aovApi_{};
    HdueMotionApi motionApi_{};
    HduePostProcessApi postProcessApi_{};
    HdueVolumeApi volumeApi_{};
    HdueLightingApi lightingApi_{};
    HdueSurfaceApi surfaceApi_{};
    bool initialized_ = false;
};

void write_pfm(const std::filesystem::path& path, const HdueFrameInfo& frame, const float* rgba);
std::optional<uint64_t> gpu_memory_bytes();
}

