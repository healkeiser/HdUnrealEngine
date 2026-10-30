#include "delegate.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/type.h"
#if defined(HDUE_HOUDINI)
#include "pxr/base/tf/stringUtils.h"
#include <SYS/SYS_Version.h>
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <vector>
#endif
PXR_NAMESPACE_OPEN_SCOPE
#if defined(HDUE_HOUDINI)
// The HDK only guarantees its ABI within one build (HDK_API_VERSION is the build number): say so when the Houdini
// running the delegate, read from libUT.dll's version resource, is not the build the delegate was compiled against.
static void HdUEWarnOnHoudiniBuildMismatch() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<wchar_t> path(32768);
        HMODULE ut = GetModuleHandleW(L"libUT.dll");
        DWORD size = ut && GetModuleFileNameW(ut, path.data(), DWORD(path.size()))
                         ? GetFileVersionInfoSizeW(path.data(), nullptr)
                         : 0;
        std::vector<char> info(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (!size || !GetFileVersionInfoW(path.data(), 0, size, info.data()) ||
            !VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || !fixed)
            return;
        const int major = HIWORD(fixed->dwFileVersionMS), minor = LOWORD(fixed->dwFileVersionMS),
                  build = LOWORD(fixed->dwFileVersionLS);
        if (major == SYS_VERSION_MAJOR_INT && minor == SYS_VERSION_MINOR_INT && build == SYS_VERSION_BUILD_INT) return;
        const std::string message = TfStringPrintf(
            "hdUE was built against Houdini %s but runs in Houdini %d.%d.%d. SideFX only guarantees the HDK ABI "
            "within one build: use an hdUE package built for this Houdini if rendering crashes or misbehaves.",
            SYS_VERSION_FULL, major, minor, build);
        TF_WARN("%s", message.c_str());
        std::fprintf(stderr, "hdUE warning: %s\n", message.c_str());  // husk prints stderr, not Tf diagnostics
        std::fflush(stderr);
    });
}
#endif
// One renderer id for every build (Houdini scenes, ROPs and husk command lines name it); each USD build finds its own
// library through PXR_PLUGINPATH_NAME.
class HdUEHoudiniRendererPlugin final : public HdRendererPlugin {
public:
    HdRenderDelegate* CreateRenderDelegate() override { return CreateRenderDelegate({}); }
    HdRenderDelegate* CreateRenderDelegate(const HdRenderSettingsMap& settings) override {
#if defined(HDUE_HOUDINI)
        HdUEWarnOnHoudiniBuildMismatch();
#endif
        try {
            return new HdUERenderDelegate(settings);
        } catch (const std::exception& e) {
            TF_RUNTIME_ERROR("hdUE: %s", e.what());
            return nullptr;
        }
    }
    void DeleteRenderDelegate(HdRenderDelegate* delegate) override { delete delegate; }
    bool IsSupported(const HdRendererCreateArgs&, std::string* reason) const override {
        if (reason) reason->clear();
        return true;
    }
};
TF_REGISTRY_FUNCTION(TfType) { HdRendererPluginRegistry::Define<HdUEHoudiniRendererPlugin>(); }
PXR_NAMESPACE_CLOSE_SCOPE
