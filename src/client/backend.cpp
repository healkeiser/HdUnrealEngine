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
        api_.abi_version != HDUE_ABI_VERSION)
        throw std::runtime_error("Backend has no compatible hdUE C ABI (version " + std::to_string(HDUE_ABI_VERSION) +
                                 ")");
    // Never FreeLibrary: UE globals/TLS and rendering callbacks have process lifetime.
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
    arguments += " " + env("HDUE_UE_ARGS");
    if (const auto user = env("HDUE_USER_DIR"); !user.empty()) arguments += " -userdir=\"" + user + "\"";
    HdueInitDesc init{};
    init.struct_size = sizeof(init);
    init.instance_handle = instance;
    init.window_handle = window;
    init.project_path = project.c_str();
    init.map_path = "/Game/HdUETest";
    init.extra_arguments = arguments.c_str();
    init.flags = interactive ? HDUE_INIT_INTERACTIVE : 0;
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

}  // namespace hdue
