#include "color_management.h"
#include "pxr/base/work/loops.h"
#include <algorithm>
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/diagnostic.h"
#if defined(HDUE_HOUDINI)
#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
namespace OCIO = OCIO_NAMESPACE;
#endif
#include <filesystem>
#include <functional>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
struct ColorManagement {
    std::string sceneLinear = "Linear Rec.709 (sRGB)", rec709 = "Linear Rec.709 (sRGB)";
    bool identity = true;
#if defined(HDUE_HOUDINI)
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
            // UE's working space: linear Rec.709 under any of its usual names or aliases.
            OCIO::ConstColorSpaceRcPtr rec709;
            for (const char* name : {"lin_rec709", "Linear Rec.709 (sRGB)", "lin_srgb", "Utility - Linear - sRGB", "linear", "Linear"})
                if ((rec709 = config->getColorSpace(name))) break;
            if (!rec709) { TF_WARN("hdUE: OCIO config has no linear Rec.709 colour space; output stays linear Rec.709"); return; }
            result.rec709 = rec709->getName();
            if (std::string(rec709->getName()) == scene->getName()) return;
            result.toScene = config->getProcessor(rec709, scene)->getDefaultCPUProcessor();
            result.fromScene = config->getProcessor(scene, rec709)->getDefaultCPUProcessor();
            result.identity = result.toScene->isNoOp();
        } catch (const OCIO::Exception& error) {
            TF_WARN("hdUE: OCIO colour management disabled: %s", error.what());
        }
#endif
    });
    return result;
}
}
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
std::string HdUELinearRec709Texture(const std::string& file) {
#if defined(HDUE_HOUDINI)
    const auto& m = management();
    auto extension = std::filesystem::path(file).extension().string();
    for (auto& c : extension) c = char(tolower(c));
    if (m.identity || (extension != ".exr" && extension != ".hdr")) return file;
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(file, error);
    if (error) return file;
    const auto key = std::hash<std::string>{}(file + "|" + m.sceneLinear + "|" + std::to_string(stamp.time_since_epoch().count()));
    const auto folder = std::filesystem::temp_directory_path() / "hdue_ocio";
    const auto cached = folder / (std::to_string(key) + ".exr");
    if (std::filesystem::exists(cached, error)) return cached.string();
    static std::mutex convert;
    std::lock_guard lock(convert);
    if (std::filesystem::exists(cached, error)) return cached.string();
    std::filesystem::create_directories(folder, error);
    OIIO::ImageBuf source(file), converted;
    if (!OIIO::ImageBufAlgo::colorconvert(converted, source, m.sceneLinear, m.rec709) || !converted.write(cached.string())) {
        TF_WARN("hdUE: could not convert %s from %s to %s; using it as is", file.c_str(), m.sceneLinear.c_str(), m.rec709.c_str());
        return file;
    }
    return cached.string();
#else
    return file;
#endif
}
PXR_NAMESPACE_CLOSE_SCOPE
