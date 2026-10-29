#include "color_management.h"
#include "pxr/base/work/loops.h"
#include <algorithm>
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/stringUtils.h"
#if defined(HDUE_HOUDINI)
#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/imagebuf.h>
#include <chrono>
namespace OCIO = OCIO_NAMESPACE;
#endif
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
// UE's working space, by its name in OCIO's built-in config: OCIO relates it to the host config through the config's
// interchange role (ACES2065-1), whatever the config calls its own linear Rec.709 space, if it has one.
constexpr const char* Rec709 = "Linear Rec.709 (sRGB)";
struct ColorManagement {
    std::string sceneLinear = Rec709;
    bool identity = true;
#if defined(HDUE_HOUDINI)
    OCIO::ConstConfigRcPtr config;  // null: no colour management (disabled, or not relatable to linear Rec.709)
    OCIO::ConstCPUProcessorRcPtr toScene, fromScene;
#endif
};
const ColorManagement& management() {
    static ColorManagement result;
    static std::once_flag once;
    std::call_once(once, [] {
#if defined(HDUE_HOUDINI)
        if (!TfGetenvBool("HDUE_COLOR_MANAGEMENT", true)) return;
        try {
            const auto config = OCIO::GetCurrentConfig();
            const auto scene = config->getColorSpace(OCIO::ROLE_SCENE_LINEAR);
            if (!scene) return;
            result.sceneLinear = scene->getName();
            result.toScene = OCIO::Config::GetProcessorFromBuiltinColorSpace(Rec709, config, scene->getName())
                                 ->getDefaultCPUProcessor();
            result.fromScene = OCIO::Config::GetProcessorToBuiltinColorSpace(config, scene->getName(), Rec709)
                                   ->getDefaultCPUProcessor();
            result.identity = result.toScene->isNoOp();
            result.config = config;
        } catch (const OCIO::Exception& error) {
            TF_WARN("hdUE: OCIO colour management disabled (output stays linear Rec.709): %s", error.what());
        }
#endif
    });
    return result;
}
}  // namespace
const std::string& HdUESceneLinearName() { return management().sceneLinear; }
bool HdUESceneLinearIsRec709() { return management().identity; }
void HdUEToSceneLinear(std::vector<float>& rgba) {
#if defined(HDUE_HOUDINI)
    const auto& m = management();
    if (m.identity || rgba.size() < 4) return;
    // Rows of 16k pixels in parallel: one thread took 10-30 ms per 1080p frame.
    const size_t pixels = rgba.size() / 4, chunk = 16384;
    WorkParallelForN((pixels + chunk - 1) / chunk, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) {
            OCIO::PackedImageDesc image(rgba.data() + c * chunk * 4, long(std::min(chunk, pixels - c * chunk)), 1, 4);
            m.toScene->apply(image);
        }
    });
#else
    (void)rgba;
#endif
}
GfVec3f HdUEFromSceneLinear(const GfVec3f& color) {
#if defined(HDUE_HOUDINI)
    const auto& m = management();
    if (m.identity) return color;
    float rgb[3] = {color[0], color[1], color[2]};
    m.fromScene->applyRGB(rgb);
    return GfVec3f(rgb[0], rgb[1], rgb[2]);
#else
    return color;
#endif
}
bool HdUERawColorSpace(const std::string& space) {
    const auto lower = TfStringToLower(space);
    return lower.find("raw") != std::string::npos || lower == "data";
}
bool HdUESRGBColorSpace(const std::string& space) {
    const auto lower = TfStringToLower(space);
    return lower.find("srgb") != std::string::npos && lower.find("lin") == std::string::npos;
}
std::string HdUELinearRec709Texture(const std::string& file, const std::string& space) {
#if defined(HDUE_HOUDINI)
    const auto& m = management();
    auto extension = std::filesystem::path(file).extension().string();
    for (auto& c : extension) c = char(tolower(c));
    const bool floating = extension == ".exr" || extension == ".hdr";
    const bool unnamed = space.empty() || TfStringToLower(space) == "auto";
    if (!m.config || HdUERawColorSpace(space) || HdUESRGBColorSpace(space) || (unnamed && !floating)) return file;
    const std::string from = unnamed ? m.sceneLinear : space;
    OCIO::ConstCPUProcessorRcPtr processor;
    try {
        const auto source = m.config->getColorSpace(from.c_str());
        if (!source) {
            static std::mutex mutex;
            static std::set<std::string> warned;
            std::lock_guard lock(mutex);
            if (warned.insert(from).second)
                TF_WARN("hdUE: the OCIO config has no colour space %s; textures tagged with it are used as is",
                        from.c_str());
            return file;
        }
        if (source->isData()) return file;
        processor = OCIO::Config::GetProcessorToBuiltinColorSpace(m.config, source->getName(), Rec709)
                        ->getDefaultCPUProcessor();
        if (processor->isNoOp()) return file;
    } catch (const OCIO::Exception& error) {
        TF_WARN("hdUE: %s: %s", from.c_str(), error.what());
        return file;
    }
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(file, error);
    if (error) return file;
    const auto key = std::hash<std::string>{}(file + "|" + from + "|" + m.sceneLinear + "|" +
                                              std::to_string(stamp.time_since_epoch().count()));
    const auto folder = std::filesystem::temp_directory_path() / "hdue_ocio";
    const auto cached = folder / (std::to_string(key) + ".exr");
    static std::mutex convert;
    std::lock_guard lock(convert);
    // UE copies textures when they load, so the cache only saves converting again. Each process first evicts the
    // conversions it has not used for a week, and a hit counts as a use.
    static std::once_flag evict;
    std::call_once(evict, [&] {
        const auto expired = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            std::error_code ignored;
            if (entry.last_write_time(ignored) < expired) std::filesystem::remove(entry.path(), ignored);
        }
    });
    if (std::filesystem::exists(cached, error)) {
        std::filesystem::last_write_time(cached, std::filesystem::file_time_type::clock::now(), error);
        return cached.string();
    }
    std::filesystem::create_directories(folder, error);
    // Read as float: an 8-bit image in a linear space (acescg, lin_rec709 primaries aside) would band if kept 8-bit.
    OIIO::ImageBuf image(file);
    const bool read = image.read(0, 0, true, OIIO::TypeDesc::FLOAT) && image.localpixels() && image.nchannels() >= 3;
    if (read) {
        const auto& spec = image.spec();
        const auto pixel = std::ptrdiff_t(spec.nchannels * sizeof(float));
        OCIO::PackedImageDesc pixels(image.localpixels(), spec.width, spec.height * spec.depth,
                                     std::min(spec.nchannels, 4), OCIO::BIT_DEPTH_F32, sizeof(float), pixel,
                                     pixel * spec.width);
        processor->apply(pixels);
    }
    if (!read || !image.write(cached.string(), OIIO::TypeDesc::HALF)) {
        TF_WARN("hdUE: could not convert %s from %s to linear Rec.709; using it as is", file.c_str(), from.c_str());
        return file;
    }
    return cached.string();
#else
    (void)space;
    return file;
#endif
}
PXR_NAMESPACE_CLOSE_SCOPE
