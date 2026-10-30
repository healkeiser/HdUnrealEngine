#pragma once
#include "hdue/bridge.h"
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
    void initialize(const HdueInitDesc& desc);
    // initialize with the command line every host starts UE with: the cooked map, headless (HDUE_SHOW_WINDOW=1 shows
    // the window), -stdout, HDUE_UE_ARGS, -userdir from HDUE_USER_DIR; HDUE_INIT_INTERACTIVE for Houdini's UI.
    void boot(uint64_t instance, uint64_t window, const std::string& project, bool interactive);
    void shutdown();
    bool initialized() const { return initialized_; }
    std::string status_message() const;
    void require(HdueResult result, const char* operation) const;

private:
    void* module_ = nullptr;
    HdueApi api_{};
    bool initialized_ = false;
};

void write_pfm(const std::filesystem::path& path, const HdueFrameInfo& frame, const float* rgba);
std::optional<uint64_t> gpu_memory_bytes();
uint64_t peak_memory_bytes();  // the process's peak working set (Houdini's peakMemory render stat)
}  // namespace hdue
