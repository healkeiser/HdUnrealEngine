#include "hdue/backend.hpp"
#include <windows.h>
#include <cstdlib>

namespace hdue {
Backend::Backend(const std::filesystem::path& dll) {
    const auto absolute = std::filesystem::absolute(dll);
    module_ =
        LoadLibraryExW(absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) throw std::runtime_error("Cannot load backend DLL, Win32 error " + std::to_string(GetLastError()));
    const auto get = reinterpret_cast<HdueGetApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_api"));
    if (!get || get(HDUE_ABI_VERSION, sizeof(api_), &api_) != HDUE_OK || api_.struct_size != sizeof(api_) ||
        api_.abi_version != HDUE_ABI_VERSION || !api_.initialize || !api_.get_status || !api_.tick ||
        !api_.create_session || !api_.destroy_session || !api_.request_render || !api_.poll_frame || !api_.copy_frame ||
        !api_.window_message || !api_.shutdown || !api_.apply_scene) {
        throw std::runtime_error("Backend has no compatible, complete hdUE C ABI");
    }
    // Never FreeLibrary: UE globals/TLS and rendering callbacks have process lifetime.
    const auto surface =
        reinterpret_cast<HdueGetSurfaceApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_surface_api"));
    if (surface) {
        HdueSurfaceApi candidate{};
        if (surface(HDUE_SURFACE_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_SURFACE_ABI_VERSION &&
            candidate.apply && candidate.get_status)
            surfaceApi_ = candidate;
    }
    const auto gpu =
        reinterpret_cast<HdueGetGpuApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_gpu_api"));
    if (gpu) {
        HdueGpuApi candidate{};
        if (gpu(HDUE_GPU_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_GPU_ABI_VERSION &&
            candidate.configure && candidate.poll_gpu_frame && candidate.acquire_gpu_frame)
            gpuApi_ = candidate;
    }
    const auto lighting =
        reinterpret_cast<HdueGetLightingApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_lighting_api"));
    if (lighting) {
        HdueLightingApi candidate{};
        if (lighting(HDUE_LIGHTING_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_LIGHTING_ABI_VERSION &&
            candidate.upsert && candidate.get_status)
            lightingApi_ = candidate;
    }
    const auto depth =
        reinterpret_cast<HdueGetDepthApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_depth_api"));
    if (depth) {
        HdueDepthApi candidate{};
        if (depth(HDUE_DEPTH_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_DEPTH_ABI_VERSION &&
            candidate.set_enabled && candidate.copy_depth)
            depthApi_ = candidate;
    }
    const auto aov =
        reinterpret_cast<HdueGetAovApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_aov_api"));
    if (aov) {
        HdueAovApi candidate{};
        if (aov(HDUE_AOV_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_AOV_ABI_VERSION &&
            candidate.poll_aov)
            aovApi_ = candidate;
    }
    const auto motion =
        reinterpret_cast<HdueGetMotionApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_motion_api"));
    if (motion) {
        HdueMotionApi candidate{};
        if (motion(HDUE_MOTION_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_MOTION_ABI_VERSION &&
            candidate.set_transforms)
            motionApi_ = candidate;
    }
    const auto postProcess = reinterpret_cast<HdueGetPostProcessApiFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_postprocess_api"));
    if (postProcess) {
        HduePostProcessApi candidate{};
        if (postProcess(HDUE_POSTPROCESS_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_POSTPROCESS_ABI_VERSION &&
            candidate.set_post_process)
            postProcessApi_ = candidate;
    }
    const auto volume =
        reinterpret_cast<HdueGetVolumeApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_volume_api"));
    if (volume) {
        HdueVolumeApi candidate{};
        if (volume(HDUE_VOLUME_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_VOLUME_ABI_VERSION &&
            candidate.upsert && candidate.remove)
            volumeApi_ = candidate;
    }
    const auto assets =
        reinterpret_cast<HdueGetAssetApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_asset_api"));
    if (assets) {
        HdueAssetApi candidate{};
        if (assets(HDUE_ASSET_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_ASSET_ABI_VERSION &&
            candidate.upsert && candidate.remove && candidate.get_instance_status && candidate.get_scene_status)
            assetApi_ = candidate;
    }
    const auto residency = reinterpret_cast<HdueGetResidencyApiFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_residency_api"));
    if (residency) {
        HdueResidencyApi candidate{};
        if (residency(HDUE_RESIDENCY_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_RESIDENCY_ABI_VERSION &&
            candidate.configure && candidate.get_status)
            residencyApi_ = candidate;
    }
    const auto native = reinterpret_cast<HdueGetNativePickApiFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_native_pick_api"));
    if (native) {
        HdueNativePickApi candidate{};
        if (native(HDUE_NATIVE_PICK_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_NATIVE_PICK_ABI_VERSION &&
            candidate.pick_native && candidate.get_material_info)
            nativePickApi_ = candidate;
    }
    const auto materials =
        reinterpret_cast<HdueGetMaterialApiFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_get_material_api"));
    if (materials) {
        HdueMaterialApi candidate{};
        if (materials(HDUE_MATERIAL_ABI_VERSION, sizeof(candidate), &candidate) == HDUE_OK &&
            candidate.struct_size == sizeof(candidate) && candidate.abi_version == HDUE_MATERIAL_ABI_VERSION &&
            candidate.get_renderer_info && candidate.set_parameters && candidate.get_material_stats)
            materialApi_ = candidate;
    }
}
Backend::~Backend() {
    if (initialized_) api_.shutdown();
}
void Backend::initialize(const HdueInitDesc& desc) {
    const auto result = api_.initialize(&desc);
    if (result != HDUE_OK) {
        const auto message = status_message();
        api_.shutdown();  // Launch may have partially initialized even on failure.
        throw std::runtime_error("initialize failed (" + std::to_string(result) + "): " + message);
    }
    initialized_ = true;
}
void Backend::boot(uint64_t instance, uint64_t window, const std::string& project, bool interactive) {
    const auto env = [](const char* name) {
        const char* value = std::getenv(name);
        return std::string(value ? value : "");
    };
    const auto show = env("HDUE_SHOW_WINDOW");
    // Frames are read back from render targets, so UE needs no visible window. Without -stdout, UE echoes
    // Display/Warning/Error lines with wprintf, which reached the host's console (Houdini, husk) as UTF-16 and showed
    // as spaced-out text; -stdout writes the same lines as UTF-8. HDUE_USER_DIR (set by the Houdini package): UE's
    // Saved (logs, shader caches) per user, not in a possibly shared or read-only install folder.
    std::string arguments = (show.empty() || show == "0" || show == "false" ? "-RenderOffscreen -stdout" : "-stdout");
    if (interactive) arguments += " -hdUEInteractive";
    arguments += " " + env("HDUE_UE_ARGS");
    if (const auto user = env("HDUE_USER_DIR"); !user.empty()) arguments += " -userdir=\"" + user + "\"";
    HdueInitDesc init{};
    init.struct_size = sizeof(init);
    init.instance_handle = instance;
    init.window_handle = window;
    init.project_path = project.c_str();
    init.map_path = "/Game/HdUETest";
    init.extra_arguments = arguments.c_str();
    initialize(init);
}
void Backend::shutdown() {
    if (initialized_) {
        require(api_.shutdown(), "shutdown");
        initialized_ = false;
    }
}
std::string Backend::status_message() const {
    HdueStatus status{};
    status.struct_size = sizeof(status);
    if (api_.get_status(&status) != HDUE_OK) return "status unavailable";
    return status.message;
}
void Backend::require(HdueResult result, const char* operation) const {
    if (result != HDUE_OK)
        throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) +
                                 "): " + status_message());
}

HdueResult Backend::resize_session(HdueSession session, uint32_t width, uint32_t height) const {
    using ResizeFn = HdueResult(HDUE_CALL*)(HdueSession, uint32_t, uint32_t);
    const auto resize =
        reinterpret_cast<ResizeFn>(GetProcAddress(static_cast<HMODULE>(module_), "hdue_resize_session"));
    return resize ? resize(session, width, height) : HDUE_INVALID_STATE;
}
}  // namespace hdue
