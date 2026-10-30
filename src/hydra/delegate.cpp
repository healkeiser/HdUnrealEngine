#include "delegate.h"
#include "depth.h"
#include "surface_settings.h"
#include "color_management.h"
#include "instancing.h"
#include "displacement.h"
#include "render_tags.h"
#include "cryptomatte.h"
#include "aov_table.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/base/gf/half.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/cameraUtil/framing.h"
#include "pxr/imaging/hd/extComputation.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/vt/types.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/diagnosticMgr.h"
#include "pxr/base/tf/getenv.h"
#include <optional>
#include "pxr/base/work/loops.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/js/json.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/assetPath.h"
#include <cmath>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <array>
#include <set>
#include <unordered_map>
#include <cstring>
#include <fstream>
#include <memory>
#include <limits>
#include <algorithm>
#include <sstream>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <thread>
#include <atomic>
#if defined(HDUE_HOUDINI)
#include <OpenImageDenoise/oidn.hpp>
#endif
#include <windows.h>
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hd/driver.h"
#if defined(HDUE_HOUDINI)
#define HDUE_VIEWPORT_EXPORTS 1  // the viewport_api.h exports are defined below
#include "../houdini/diagnostics.h"
#include "hdue/viewport_api.h"
#include "gpu_frames.h"
#include "pxr/imaging/hgi/tokens.h"
#endif

PXR_NAMESPACE_OPEN_SCOPE
void HdUETakeMeshSyncStats(double& milliseconds, int& count);  // prims.cpp
namespace {
// HDUE_TRACE_TIMING output: HDUE_TRACE_FILE when set (Houdini's UI has no visible stderr), else stderr.
FILE* traceSink() {
    static FILE* sink = [] {
        const auto path = TfGetenv("HDUE_TRACE_FILE");
        FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "a");
        return file ? file : stderr;
    }();
    return sink;
}
void trace(const char* format, ...);
}  // namespace
namespace {
void trace(const char* format, ...) {
    static const bool enabled = TfGetenvBool("HDUE_TRACE_TIMING", false);
    if (!enabled) return;
    static const auto origin = std::chrono::steady_clock::now();
    std::fprintf(traceSink(), "%9.1f ",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count());
    va_list args;
    va_start(args, format);
    std::vfprintf(traceSink(), format, args);
    va_end(args);
    std::fflush(traceSink());
}
// Hosts such as husk never start UE. The first delegate created while the
// runtime is cold starts it on its own thread and ticks it from Execute, as
// hdue_usd_render does. UE allows one lifetime per process, so it stays until the process exits (husk
// --restart-delegate creates the next delegate on the same UE).
struct HdUESelfHost {
    std::unique_ptr<hdue::Backend> backend;
    HWND window = nullptr;
    DWORD thread = 0;
    std::atomic<int> delegates{0}, paused{0};
    // Houdini's UI (houdiniUi): UE ticks continuously on this thread while a delegate
    // exists, Execute only submits and polls, and UE outlives individual viewports until hdue_shutdown_ue.
    bool continuous = false;
    // Houdini's UI boots UE on this thread without waiting (as Karma loads); a failed boot is reported here.
    std::mutex bootMutex;
    std::string bootError;
    std::string error() {
        std::lock_guard lock(bootMutex);
        return bootError;
    }
    std::filesystem::path directory;  // the host's working directory, which UE moves to its Binaries
    // UE runs on its own thread: husk calls from one thread, but Live Render creates the delegate on one
    // thread and executes it on others, and UE only accepts calls from the thread that started it.
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<std::function<void()>> queue;
    // Houdini's UI: called on this thread after each idle tick so finished frames reach the render buffers without
    // waiting for the host to call Execute (keyed by render pass; touched only on this thread).
    std::map<const void*, std::function<void()>> pumps;
    bool pumping = false;  // pumps run right after the loop's tick, so Execute skips its own
    bool stopping = false;
    template <class F>
    auto run(F&& f) -> decltype(f()) {
        if (!worker.joinable() || std::this_thread::get_id() == worker.get_id()) return f();
        std::packaged_task<decltype(f())()> task(std::forward<F>(f));
        auto result = task.get_future();
        {
            std::lock_guard lock(queueMutex);
            queue.emplace_back([&task] { task(); });
        }
        queueReady.notify_one();
        return result.get();
    }
    // Queues f on the UE thread without waiting (Houdini's UI must never block on a UE tick).
    void post(std::function<void()> f) {
        if (!worker.joinable() || std::this_thread::get_id() == worker.get_id()) {
            f();
            return;
        }
        {
            std::lock_guard lock(queueMutex);
            queue.emplace_back(std::move(f));
        }
        queueReady.notify_one();
    }
    void stop() {
        if (!worker.joinable()) return;
        {
            std::lock_guard lock(queueMutex);
            stopping = true;
        }
        queueReady.notify_one();
        worker.join();
    }
    void tick() const {
        if (!backend || GetCurrentThreadId() != thread) return;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto result = backend->api().tick();
        // husk resolves relative output paths against its working directory.
        std::error_code ignored;
        std::filesystem::current_path(directory, ignored);
        backend->require(result, "tick");
    }
};
HdUESelfHost& selfHost() {
    static HdUESelfHost host;
    return host;
}
// husk prints stderr, not Tf diagnostics: what a batch render should know goes there.
void batchWarning(const std::string& message) {
    std::fprintf(stderr, "hdUE warning: %s\n", message.c_str());
    std::fflush(stderr);
}
// husk shows no Tf warning of a render delegate, even at -V 6: hdUE's own (issued from its sources) also go through
// batchWarning, once per message. Houdini's UI and other hosts keep their Tf output.
class HdUEBatchWarnings final : public TfDiagnosticMgr::Delegate {
public:
    HdUEBatchWarnings() { TfDiagnosticMgr::GetInstance().AddDelegate(this); }
    ~HdUEBatchWarnings() override { TfDiagnosticMgr::GetInstance().RemoveDelegate(this); }
    void IssueError(const TfError&) override {}
    void IssueFatalError(const TfCallContext&, const std::string&) override {}
    void IssueStatus(const TfStatus&) override {}
    void IssueWarning(const TfWarning& warning) override {
        static const auto sources = TfStringToLower(std::filesystem::path(__FILE__).parent_path().string());
        if (!TfStringStartsWith(TfStringToLower(warning.GetSourceFileName()), sources)) return;
        auto message = warning.GetCommentary();
        if (TfStringStartsWith(message, "hdUE: ")) message.erase(0, 6);
        std::lock_guard lock(mutex_);
        if (seen_.insert(message).second) batchWarning(message);
    }

private:
    std::mutex mutex_;
    std::set<std::string> seen_;
};
// Ends UE on its own thread and stops that thread. Houdini's UI calls it when Houdini quits (hdue_shutdown_ue); it is
// also the exit handler of every process that started UE. std::atexit in this DLL runs at its detach, after ExitProcess
// has ended UE's thread: UE can no longer shut down, and waiting for that thread, or UE's own teardown, hung the exit.
// The process ends there with its exit code instead (already set during detach). UE cannot shut down from exit()
// either: exit() holds the CRT's exit lock, which UE's module unloads wait for.
void shutdownSelfHost() {
    auto& host = selfHost();
    if (!host.backend) return;
    if (WaitForSingleObject(host.worker.native_handle(), 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(GetCurrentProcess(), &code);
        TerminateProcess(GetCurrentProcess(), code);
    }
    host.run([&] {
        try {
            host.tick();
            host.backend->shutdown();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "hdUE shutdown: %s\n", e.what());
        }
        if (host.window) DestroyWindow(host.window);
        host.window = nullptr;
    });
    host.stop();
    host.backend.reset();
}
// Runs f on the thread that owns UE: hdUE's own UE thread, or inline.
void ueRun(const std::function<void()>& f) {
    auto& host = selfHost();
    if (host.worker.joinable()) {
        host.run(f);
        return;
    }
    f();
}
}  // namespace
// Karma and RenderMan name the beauty Ci or C; treat them like Hydra's color.
TfToken HdUEBeauty(const TfToken& name) {
    return name == TfToken("Ci") || name == TfToken("C") || name == TfToken("beauty") ? HdAovTokens->color : name;
}
namespace {
LRESULT CALLBACK selfHostWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto& host = selfHost();
    if (host.backend && host.backend->initialized()) {
        int64_t result = 0;
        if (host.backend->api().window_message(reinterpret_cast<uint64_t>(window), message, uint64_t(wparam),
                                               int64_t(lparam), &result) == HDUE_OK)
            return LRESULT(result);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
// backend_dll / runtime_project from the runtime profile named by HDUE_RUNTIME_CONFIG.
// Relative paths resolve against the profile's folder, as in hdue_houdini.runtime_config.
std::string runtimeConfig(const char* key) {
    const auto path = TfGetenv("HDUE_RUNTIME_CONFIG");
    if (path.empty()) return {};
    const auto file_path = std::filesystem::u8path(path);
    std::ifstream file(file_path);
    std::stringstream text;
    text << file.rdbuf();
    const auto root = JsParseString(text.str());
    if (!root.IsObject()) return {};
    const auto& object = root.GetJsObject();
    const auto it = object.find(key);
    if (it == object.end() || !it->second.IsString()) return {};
    const auto value = std::filesystem::u8path(it->second.GetString());
    if (value.is_absolute()) return it->second.GetString();
    const auto resolved = (std::filesystem::absolute(file_path).parent_path() / value).lexically_normal().u8string();
    return std::string(resolved.begin(), resolved.end());
}
// Houdini's interactive UI (not husk or hython), from the process executable. UE starts with the first delegate, and
// Hydra says nothing usable by then: the viewport's delegate is created with no settings (houdini:viewport follows),
// and both the viewport and husk send houdini:renderer = husk.
std::wstring processName() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto name = std::filesystem::path(path).filename().wstring();
    for (auto& c : name) c = wchar_t(towlower(c));
    return name;
}
bool houdiniUi() {
    const auto name = processName();
    for (const auto* gui :
         {L"houdini.exe", L"houdinifx.exe", L"houdinicore.exe", L"basehoudini.exe", L"happrentice.exe", L"hindie.exe",
          L"hescape.exe", L"hedu.exe", L"heducation.exe", L"hexper.exe"})
        if (name == gui) return true;
    return false;
}
// Starts UE on hdUE's thread and ticks it until its world and RHI are ready. wait = false (Houdini's UI) returns at
// once: work posted meanwhile runs once UE is ready, and a failure is kept in HdUESelfHost::bootError.
void startSelfHost(const std::string& dll, const std::string& project, bool wait) {
    auto& host = selfHost();
    if (project.empty())
        throw std::runtime_error("hdUE cannot start UE without runtime_project in HDUE_RUNTIME_CONFIG");
    auto started = std::make_shared<std::promise<void>>();
    auto ready = started->get_future();
    host.stopping = false;
    host.continuous = houdiniUi();
    host.worker = std::thread([&host, dll, project, started] {
        try {
            const auto instance = GetModuleHandleW(nullptr);
            WNDCLASSW cls{};
            cls.hInstance = instance;
            cls.lpfnWndProc = selfHostWindow;
            cls.lpszClassName = L"HdUESelfHost";
            RegisterClassW(&cls);
            host.window = CreateWindowExW(0, cls.lpszClassName, L"hdUE", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr,
                                          nullptr, instance, nullptr);
            if (!host.window) throw std::runtime_error("hdUE cannot create its UE host window");
            host.backend = std::make_unique<hdue::Backend>(std::filesystem::u8path(dll));
            host.thread = GetCurrentThreadId();
            // Returns once UE's world and RHI are up (the backend is then ready), or throws.
            host.backend->boot(reinterpret_cast<uint64_t>(instance), reinterpret_cast<uint64_t>(host.window), project,
                               host.continuous);
            std::atexit([] { shutdownSelfHost(); });
            started->set_value();
        } catch (const std::exception& e) {
            {
                std::lock_guard lock(host.bootMutex);
                host.bootError = e.what();
            }
            host.backend.reset();
            started->set_exception(std::current_exception());
            // Keep serving the queue: work posted meanwhile fails with UE's errors instead of waiting forever.
        }
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(host.queueMutex);
                const auto due = [&host] { return host.stopping || !host.queue.empty(); };
                const bool ticking =
                    host.continuous && host.delegates > host.paused;  // UE idles while every viewport is paused
                if (ticking)
                    host.queueReady.wait_for(lock, std::chrono::milliseconds(2), due);
                else
                    host.queueReady.wait(lock, due);
                if (host.stopping && host.queue.empty()) return;
                if (!host.queue.empty()) {
                    task = std::move(host.queue.front());
                    host.queue.pop_front();
                }
            }
            if (task) {
                const auto start = std::chrono::steady_clock::now();
                task();
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (ms > 20) trace("hdUE slow task: %.1f ms\n", ms);
            } else {
                const auto start = std::chrono::steady_clock::now();
                try {
                    host.tick();
                } catch (const std::exception& e) {
                    TF_RUNTIME_ERROR("hdUE tick: %s", e.what());
                }
                host.pumping = true;
                for (auto& [key, pump] : host.pumps) pump();
                host.pumping = false;
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (ms > 20) trace("hdUE slow tick: %.1f ms\n", ms);
            }
        }
    });
    if (!wait) return;
    try {
        ready.get();
    } catch (...) {
        host.stop();  // the thread keeps serving its queue after a failed boot: join alone waited forever
        throw;
    }
}
}  // namespace
HdRprim* HdUECreateMesh(const SdfPath&);
HdRprim* HdUECreatePoints(const SdfPath&);
HdRprim* HdUECreateCurves(const SdfPath&);
HdRprim* HdUECreateVolume(const SdfPath&);
HdBprim* HdUECreateField(const SdfPath&);
HdInstancer* HdUECreateInstancer(HdSceneDelegate*, const SdfPath&);
HdSprim* HdUECreateMaterial(const SdfPath&);
HdSprim* HdUECreateCamera(const SdfPath&);
HdSprim* HdUECreateLight(const SdfPath&, uint32_t);
HdSprim* HdUECreateUnsupportedLight(const SdfPath&, const TfToken&);
void HdUERefreshWetMasks(HdUEState&, HdRenderIndex*);

HdUEState::~HdUEState() {
    if (session && backend) backend->api().destroy_session(session);
}
void HdUEState::erase(const std::string& id) {
    std::lock_guard lock(mutex);
    if (const auto mesh = meshes.find(id); mesh != meshes.end()) {
        erasedMeshes[id] = std::move(mesh->second);
        meshes.erase(mesh);
    }
    materials.erase(id);
    lights.erase(id);
    volumes.erase(id);
    dirtyVolumes.erase(id);
    // geometryRevisions[id] stays until the removal reaches UE (submit), so a revived mesh keeps its revision.
    dirtyMeshes.erase(id);
    movedMeshes.erase(id);
    dirtyMaterials.erase(id);
    dirtyLights.erase(id);
    removed.insert(id);
    ++revision;
}
namespace {
constexpr const char* AutoKey = "hdUE:viewer:key";
constexpr const char* AutoFill = "hdUE:viewer:fill";
bool active(const HdUELightData& light) {
    return light.visible && std::isfinite(light.intensity) && light.intensity > 0 &&
           (light.color[0] > 0 || light.color[1] > 0 || light.color[2] > 0);
}
// Callers read the mapping (a host setting) once, not once per prim.
std::string asset(const VtDictionary& mapping, const std::string& id) {
    const auto it = mapping.find(id);
    return it != mapping.end() && it->second.IsHolding<std::string>() ? it->second.UncheckedGet<std::string>() : "";
}
// One scene upload, with copies of everything its descriptors point to. prepareScene fills it under the state lock;
// sendScene hands it to UE without the lock, so Hydra Sync and Houdini's UI never wait for UE's apply (seconds on
// large scenes); finishScene records the result under the lock again.
struct ScenePacket {
    // Owned storage. Deques: their elements never move, so descriptor pointers into them stay valid.
    std::deque<std::string> text;
    std::deque<std::vector<float>> floats;
    std::deque<std::vector<double>> doubles;
    std::deque<std::vector<uint32_t>> indices;
    std::deque<std::vector<int32_t>> ids;
    std::deque<std::vector<HdueMaterialParameter>> parameters;
    const char* own(std::string value) { return text.emplace_back(std::move(value)).c_str(); }
    template <class T, class Store>
    const T* own(Store& store, std::vector<T> values) {
        return values.empty() ? nullptr : store.emplace_back(std::move(values)).data();
    }
    // What this upload covers; Sync may dirty more meanwhile, for the next upload.
    std::set<std::string> dirtyMeshes, movedMeshes, dirtyMaterials, dirtyLights, dirtyVolumes, removedIds;
    // Motion blur: deforming meshes uploaded with their points at a sub-frame time instead of their stored points.
    std::map<std::string, std::vector<GfVec3f>> points;
    // Takes the state's dirty sets (state lock held).
    void take(HdUEState& state) {
        dirtyMeshes.swap(state.dirtyMeshes);
        movedMeshes.swap(state.movedMeshes);
        dirtyMaterials.swap(state.dirtyMaterials);
        dirtyLights.swap(state.dirtyLights);
        dirtyVolumes.swap(state.dirtyVolumes);
        removedIds.swap(state.removed);
    }
    // Gives them back after a failed upload (state lock held). Sync may have erased or revived prims meanwhile:
    // only prims that still exist stay dirty, and only prims still gone stay removed.
    void giveBack(HdUEState& state) const {
        const auto restore = [](const std::set<std::string>& from, std::set<std::string>& to, const auto& prims) {
            for (const auto& id : from)
                if (prims.count(id)) to.insert(id);
        };
        restore(dirtyMeshes, state.dirtyMeshes, state.meshes);
        restore(movedMeshes, state.movedMeshes, state.meshes);
        restore(dirtyMaterials, state.dirtyMaterials, state.materials);
        restore(dirtyLights, state.dirtyLights, state.lights);
        restore(dirtyVolumes, state.dirtyVolumes, state.volumes);
        for (const auto& id : removedIds)
            if (!state.meshes.count(id) && !state.materials.count(id) && !state.lights.count(id) &&
                !state.volumes.count(id))
                state.removed.insert(id);
    }
    uint64_t revision = 0;
    bool firstBatch = false;
    // Descriptors.
    std::vector<HdueMeshDesc> meshes;
    std::vector<HdueMeshTransform> moves;  // movedMeshes, through set_transforms after apply_scene
    std::vector<HdueMaterialDesc> materials;
    std::vector<HdueLight> extendedLights;
    std::vector<HdueAssetDesc> sceneAssets;
    std::vector<HdueVolumeDesc> volumes;
    std::vector<HdueMaterialUpdate> materialUpdates;
    std::vector<HdueWetBinding> wet;
    std::vector<const char*> removed, removedAssets;
    std::vector<std::string> genericMeshes;  // non-native meshes, whose geometry revision the upload advances
    bool autoLight = false, surfaceUpdate = false, sendLinks = false;
    std::map<std::string, uint32_t> linkedLights, linkedObjects;  // prepareLinks
    int cameraLights = 0;
    uint32_t nativeLights = 0;
};
GfMatrix4d sceneBasis(const HdUEState& state) {
    GfMatrix4d basis(1);
    for (int r = 0; r < 3; ++r) {
        GfVec3d e(0);
        e[r] = 1;
        const auto v = state.convert(e);
        for (int c = 0; c < 3; ++c) basis[r][c] = v[c];
    }
    return basis;
}
void rowMajor(const GfMatrix4d& m, double* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[r][c];
}
HdueMaterialParameter materialParameter(ScenePacket& packet, const std::string& name, const VtValue& value) {
    HdueMaterialParameter p{};
    p.name = packet.own(name);
    if (value.IsHolding<float>() || value.IsHolding<double>() || value.IsHolding<int>()) {
        p.type = HDUE_PARAMETER_SCALAR;
        p.value[0] = value.IsHolding<float>()    ? value.UncheckedGet<float>()
                     : value.IsHolding<double>() ? float(value.UncheckedGet<double>())
                                                 : float(value.UncheckedGet<int>());
    } else if (value.IsHolding<GfVec3f>() || value.IsHolding<GfVec3d>()) {
        p.type = HDUE_PARAMETER_VECTOR;
        p.value[3] = 1;
        const auto v =
            value.IsHolding<GfVec3f>() ? GfVec3d(value.UncheckedGet<GfVec3f>()) : value.UncheckedGet<GfVec3d>();
        for (int c = 0; c < 3; ++c) p.value[c] = float(v[c]);
    } else if (value.IsHolding<GfVec4f>()) {
        p.type = HDUE_PARAMETER_VECTOR;
        for (int c = 0; c < 4; ++c) p.value[c] = value.UncheckedGet<GfVec4f>()[c];
    } else if (value.IsHolding<std::string>()) {
        p.type = HDUE_PARAMETER_TEXTURE;
        p.texture_asset = packet.own(value.UncheckedGet<std::string>());
    } else
        throw std::runtime_error("Unsupported native material parameter value: " + name);
    return p;
}
// The file for a texture path: host images (op:, opdef:) through the host's map, empty until it exported them.
// Colour images are converted to linear Rec.709 from colorSpace (HdUELinearRec709Texture).
std::string textureFile(HdUEState& state, const std::string& file, bool convert = false,
                        const std::string& colorSpace = {}) {
    if (!HdUEHostTexture(file)) return file;
    const auto files = state.setting<VtDictionary>("opTextures", {});
    const auto it = files.find(file);
    if (it == files.end() || !it->second.IsHolding<std::string>()) {
        // Only Houdini's UI evaluates op: paths (its hdUE host exports the images): husk has no node to cook.
        static std::mutex warnedMutex;
        static std::set<std::string> warned;
        std::lock_guard lock(warnedMutex);
        if (!selfHost().continuous && warned.insert(file).second)
            batchWarning(
                "renders " + file +
                " untextured: only Houdini's viewport evaluates op: paths. Save the image to a file for husk.");
        return {};
    }
    return convert ? HdUELinearRec709Texture(it->second.UncheckedGet<std::string>(), colorSpace)
                   : it->second.UncheckedGet<std::string>();
}
// A displacement image as floats; empty when it cannot be read.
HdUEImage displacementImage(const std::string& file) {
    HdUEImage image;
    const auto source = file.empty() ? nullptr : HioImage::OpenForReading(file);
    if (!source) return image;
    HioImage::StorageSpec spec;
    spec.width = source->GetWidth();
    spec.height = source->GetHeight();
    spec.format = source->GetFormat();
    std::vector<uint8_t> raw(size_t(spec.width) * spec.height * source->GetBytesPerPixel());
    spec.data = raw.data();
    const auto type = HioGetHioType(spec.format);
    if (raw.empty() || !source->Read(spec)) return image;
    const size_t count = size_t(spec.width) * spec.height * HioGetComponentCount(spec.format);
    image.texels.resize(count);
    for (size_t i = 0; i < count; ++i) switch (type) {
            case HioTypeUnsignedByte:
            case HioTypeUnsignedByteSRGB:
                image.texels[i] = raw[i] / 255.f;
                break;
            case HioTypeUnsignedShort:
                image.texels[i] = reinterpret_cast<const uint16_t*>(raw.data())[i] / 65535.f;
                break;
            case HioTypeHalfFloat:
                image.texels[i] = reinterpret_cast<const GfHalf*>(raw.data())[i];
                break;
            case HioTypeFloat:
                image.texels[i] = reinterpret_cast<const float*>(raw.data())[i];
                break;
            default:
                return {};
        }
    image.width = spec.width;
    image.height = spec.height;
    image.channels = HioGetComponentCount(spec.format);
    return image;
}
void prepareMaterials(HdUEState& state, ScenePacket& packet) {
    const auto nativeMaterials = state.setting<VtDictionary>("nativeMaterialAssets", {});
    const float units = float(state.setting<double>("metersPerUnit", 0.01) * 100);
    for (const auto& id : packet.dirtyMaterials) {
        const auto& data = state.materials.at(id);
        HdueMaterialDesc desc{};
        desc.id = packet.own(data.id);
        desc.native_asset = packet.own(asset(nativeMaterials, id));
        for (int i = 0; i < 3; ++i) {
            desc.base_color[i] = data.color[i];
            desc.emissive_color[i] = data.emissive[i];
        }
        desc.base_color[3] = data.opacity;
        desc.metallic = data.metallic;
        desc.roughness = data.roughness;
        desc.opacity_threshold = data.opacityThreshold;
        desc.flags = data.vertexColor ? HDUE_MATERIAL_VERTEX_COLOR : 0;
        std::copy(data.uvTransform.begin(), data.uvTransform.end(), desc.uv_transform);
        for (int slot = 0; slot < HDUE_TEXTURE_SLOT_COUNT; ++slot) {
            const auto& texture = data.textures[slot];
            auto& out = desc.textures[slot];
            out.file = packet.own(textureFile(state, texture.file, texture.convert, texture.colorSpace));
            out.wrap[0] = texture.wrap[0];
            out.wrap[1] = texture.wrap[1];
            out.encoding = texture.encoding;
            out.uv_set = texture.uvSet;
            for (int i = 0; i < 4; ++i) {
                out.scale[i] = texture.scale[i];
                out.bias[i] = texture.bias[i];
                out.channel_mask[i] = texture.mask[i];
            }
        }
        // Displacement moves the mesh's vertices when it is packed (prepareMeshes).
        desc.textures[HDUE_TEXTURE_DISPLACEMENT] = {};
        desc.occlusion = data.occlusion;
        desc.clearcoat = data.clearcoat;
        desc.clearcoat_roughness = data.clearcoatRoughness;
        desc.ior = data.ior;
        for (int i = 0; i < 3; ++i) desc.specular_color[i] = data.specularColor[i];
        if (data.specularWorkflow) desc.flags |= HDUE_MATERIAL_SPECULAR_WORKFLOW;
        desc.subsurface = data.subsurface;
        desc.sheen = data.sheen;
        desc.sheen_roughness = data.sheenRoughness;
        for (int i = 0; i < 3; ++i) {
            desc.subsurface_color[i] = data.subsurfaceColor[i];
            desc.subsurface_radius[i] = data.subsurfaceRadius[i] * units;
            desc.sheen_color[i] = data.sheenColor[i];
            desc.coat_color[i] = data.coatColor[i];
        }
        if (data.thinWalled) desc.flags |= HDUE_MATERIAL_THIN_WALLED;
        if (data.subsurfaceBaseTexture) desc.flags |= HDUE_MATERIAL_SUBSURFACE_BASE_TEXTURE;
        if (data.sheenBaseTexture) desc.flags |= HDUE_MATERIAL_SHEEN_BASE_TEXTURE;
        packet.materials.push_back(desc);
        if (HdUETraceScene())
            TF_STATUS("hdUE material %s rgb=(%g,%g,%g)", desc.id, desc.base_color[0], desc.base_color[1],
                      desc.base_color[2]);
    }
    // Native material parameter overrides.
    const auto overrides = state.setting<VtDictionary>("nativeMaterialParameters", {});
    for (const auto& id : packet.dirtyMaterials) {
        if (state.materials.find(id) == state.materials.end()) continue;
        const auto entry = overrides.find(id);
        if (entry == overrides.end()) continue;
        if (asset(nativeMaterials, id).empty() || !entry->second.IsHolding<VtDictionary>())
            throw std::runtime_error("Native material parameters require a mapped cooked material and a dictionary");
        std::vector<HdueMaterialParameter> parameters;
        for (const auto& [name, value] : entry->second.UncheckedGet<VtDictionary>())
            parameters.push_back(materialParameter(packet, name, value));
        HdueMaterialUpdate update{};
        update.struct_size = sizeof(update);
        update.material_id = packet.own(id);
        update.parameter_count = uint32_t(parameters.size());
        update.parameters = packet.own(packet.parameters, std::move(parameters));
        packet.materialUpdates.push_back(update);
    }
}
// The mesh diced and displaced by its material's displacement (displacement.h), in object space and units; nullopt when
// its material does not displace. ponytail: meshes with surface effects keep their vertices (their masks are per
// uploaded vertex).
std::optional<HdUEDicedMesh> displaced(HdUEState& state, const HdUEMeshData& data, const std::vector<GfVec3f>& points) {
    const auto material = state.materials.find(data.material);
    if (material == state.materials.end() || data.normals.size() != points.size() ||
        (state.wetBindings && !HdUEWetSettings(*state.wetBindings, data).empty()))
        return std::nullopt;
    const auto& texture = material->second.textures[HDUE_TEXTURE_DISPLACEMENT];
    const float constant = material->second.displacement;
    if (texture.file.empty() && constant == 0) return std::nullopt;
    HdUEDicedMesh mesh{points, data.normals, data.colors, {}, data.indices};
    if (data.uv.size() == points.size()) {
        mesh.uvs.push_back(data.uv);
        mesh.uvs.insert(mesh.uvs.end(), data.uvSets.begin(), data.uvSets.end());
    }
    const auto image = texture.file.empty() ? HdUEImage() : displacementImage(textureFile(state, texture.file));
    const bool textured = !image.texels.empty() && texture.uvSet < mesh.uvs.size();
    if (!texture.file.empty() && !textured)
        batchWarning("cannot displace with " + texture.file + " (unreadable, or the mesh lacks its UVs)");
    if (textured) HdUEDice(mesh, HdUEDiceLevels(mesh.indices.size() / 3, size_t(image.width) * image.height));
    const auto& m = material->second.uvTransform;
    HdUEDisplace(
        mesh,
        [&](size_t i) {
            if (!textured) return constant;
            const float u = mesh.uvs[texture.uvSet][i][0], v = 1 - mesh.uvs[texture.uvSet][i][1];  // UE UVs
            return image.sample(GfVec2f(m[0] * u + m[1] * v + m[2], m[3] * u + m[4] * v + m[5]), texture.wrap,
                                texture.scale, texture.bias, texture.mask);
        },
        textured);
    return mesh;
}
void prepareMeshes(HdUEState& state, ScenePacket& packet) {
    const auto convert = state.converter();
    const auto basis = sceneBasis(state);
    const bool requireNanite = state.setting<bool>("requireNanite", false);
    // Single-sided meshes (USD doubleSided off) render their front faces only, as UE's USD importer does;
    // hdUE:cullBackface = 0 renders both sides, as Karma (karma:object:cull_backface defaults to off).
    const bool cullBackface = state.setting<bool>("cullBackface", true);
    const auto inverse = basis.GetInverse();
    const auto nativeMeshes = state.setting<VtDictionary>("nativeMeshAssets", {});
    // A moved mesh keeps its geometry in UE; only its actor's transform is sent (HdUEPlacedOnly).
    for (const auto& id : packet.movedMeshes) {
        if (packet.dirtyMeshes.count(id)) continue;
        if (packet.firstBatch) {
            packet.dirtyMeshes.insert(id);
            continue;
        }
        std::vector<double> matrix(16);
        rowMajor(inverse * state.meshes.at(id).transform * basis, matrix.data());
        packet.moves.push_back({packet.own(id), 1, packet.own(packet.doubles, std::move(matrix))});
    }
    for (const auto& id : packet.dirtyMeshes) {
        const auto& data = state.meshes.at(id);
        if (!data.sceneAsset.empty()) {
            HdueAssetDesc d{};
            d.struct_size = sizeof(d);
            d.id = packet.own(data.id);
            d.asset_path = packet.own(data.sceneAsset);
            d.visible = data.visible && state.shown(data.renderTag);
            rowMajor(inverse * data.transform * basis, d.transform);
            packet.sceneAssets.push_back(d);
            packet.removed.push_back(d.id);
            continue;
        }
        packet.removedAssets.push_back(packet.own(id));  // in case it was a scene asset before
        const auto nativeAsset = asset(nativeMeshes, id);
        // Vertices are local: the actor's transform or each instance matrix carries the placement. A shearing
        // transform, which a UE transform cannot hold, is baked into the vertices of a generic mesh.
        const bool baked = !data.instanced && nativeAsset.empty() && HdUEShears(data.transform);
        const GfMatrix4d bake = baked ? data.transform : GfMatrix4d(1);
        const auto normalTransform = bake.GetInverse().GetTranspose();
        const auto moved = packet.points.find(id);
        const auto& points = moved != packet.points.end() ? moved->second : data.points;
        const auto diced = nativeAsset.empty() ? displaced(state, data, points) : std::nullopt;
        const auto& vertices = diced ? diced->points : points;
        const auto& vertexNormals = diced ? diced->normals : data.normals;
        const size_t count = vertices.size();
        // Nothing to draw (and UE's Nanite builder asserts on it): every triangle degenerate, as a draw-mode stand-in
        // of a model without extentsHint.
        bool area = false;
        for (size_t t = 0; !area && t + 2 < data.indices.size(); t += 3)
            if (std::max({data.indices[t], data.indices[t + 1], data.indices[t + 2]}) < count)
                area = GfCross(vertices[data.indices[t + 1]] - vertices[data.indices[t]],
                               vertices[data.indices[t + 2]] - vertices[data.indices[t]])
                           .GetLengthSq() > 0;
        if (!area) {
            packet.removed.push_back(packet.own(id));
            continue;
        }
        // UV sets one after another (HdueMeshDesc uv_set_count).
        std::vector<const std::vector<GfVec2f>*> sets;
        if (diced)
            for (const auto& set : diced->uvs) sets.push_back(&set);
        else if (data.uv.size() == count) {
            sets.push_back(&data.uv);
            for (const auto& set : data.uvSets) sets.push_back(&set);
        }
        const bool normals = vertexNormals.size() == count;
        std::vector<float> p(count * 3), n(normals ? count * 3 : 0), uv(count * 2 * sets.size());
        WorkParallelForN(count, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const auto point = convert(bake.Transform(GfVec3d(vertices[i])));
                for (int c = 0; c < 3; ++c) p[i * 3 + c] = float(point[c]);
                if (normals) {
                    const auto normal =
                        convert(normalTransform.TransformDir(GfVec3d(vertexNormals[i])), true).GetNormalized();
                    for (int c = 0; c < 3; ++c) n[i * 3 + c] = float(normal[c]);
                }
                for (size_t k = 0; k < sets.size(); ++k) {
                    uv[(k * count + i) * 2] = (*sets[k])[i][0];
                    uv[(k * count + i) * 2 + 1] = 1 - (*sets[k])[i][1];
                }
            }
        });
        auto index = diced ? diced->indices : data.indices;
        // Preserve front faces when a baked transform adds another handedness reversal.
        if (bake.GetDeterminant() < 0)
            for (size_t i = 0; i + 2 < index.size(); i += 3) std::swap(index[i + 1], index[i + 2]);
        std::vector<float> colors;
        const auto& vertexColors = diced ? diced->colors : data.colors;
        colors.reserve(vertexColors.size() * 3);
        for (const auto& c : vertexColors) colors.insert(colors.end(), {c[0], c[1], c[2]});
        HdueMeshDesc desc{};
        desc.id = packet.own(data.id);
        desc.material_id = packet.own(data.material);
        desc.native_asset = packet.own(nativeAsset);
        desc.vertex_count = uint32_t(count);
        desc.index_count = uint32_t(index.size());
        desc.visible = data.visible && state.shown(data.renderTag);
        desc.prim_id = data.primId;
        if (colors.size() == p.size()) desc.colors = packet.own(packet.floats, std::move(colors));
        desc.positions = packet.own(packet.floats, std::move(p));
        desc.normals = packet.own(packet.floats, std::move(n));
        desc.uv = packet.own(packet.floats, std::move(uv));
        desc.uv_set_count = uint32_t(sets.size());
        desc.indices = packet.own(packet.indices, std::move(index));
        if (data.instanced) {
            // USD row-vector matrices into UE: S^-1 * M * S, S = the USD -> UE basis and units.
            std::vector<double> matrices(data.instances.size() * 16);
            for (size_t i = 0; i < data.instances.size(); ++i) {
                rowMajor(inverse * data.instances[i] * basis, &matrices[i * 16]);
                // ponytail: UE instances are FTransforms, so shear is dropped; bake sheared instances if it matters.
                static std::once_flag sheared;
                if (HdUEHasShear(data.instances[i]))
                    std::call_once(sheared,
                                   [&] { TF_WARN("Sheared instances of %s render without shear", data.id.c_str()); });
            }
            desc.instance_count = uint32_t(data.instances.size());
            std::vector<float> instanceColors;
            for (const auto& c : data.instanceColors) instanceColors.insert(instanceColors.end(), {c[0], c[1], c[2]});
            if (instanceColors.size() == size_t(desc.instance_count) * 3 && desc.instance_count)
                desc.instance_colors = packet.own(packet.floats, std::move(instanceColors));
            if (data.instanceIds.size() == desc.instance_count && desc.instance_count)
                desc.instance_ids =
                    packet.own(packet.ids, std::vector<int32_t>(data.instanceIds.begin(), data.instanceIds.end()));
            static const double none = 0;
            desc.instance_transforms = matrices.empty() ? &none : packet.own(packet.doubles, std::move(matrices));
        }
        desc.flags = requireNanite && !nativeAsset.empty() ? HDUE_MESH_REQUIRE_NANITE : 0;
        // Surface effects edit vertex colours after upload, so their meshes stay editable (dynamic) in UE.
        if (state.wetBindings && !HdUEWetSettings(*state.wetBindings, data).empty()) desc.flags |= HDUE_MESH_EDITABLE;
        if (cullBackface && !data.doubleSided) desc.flags |= HDUE_MESH_SINGLE_SIDED;
        for (int i = 0; i < 4; ++i) desc.transform[i * 4 + i] = 1;
        if (!baked) rowMajor(inverse * data.transform * basis, desc.transform);
        if (nativeAsset.empty()) packet.genericMeshes.push_back(id);
        packet.meshes.push_back(desc);
        if (HdUETraceScene())
            TF_STATUS("hdUE mesh %s material=%s vertices=%u", desc.id, desc.material_id, desc.vertex_count);
    }
}
// Light linking on UE's three lighting channels: each lightLink collection gets a channel. Unlinked lights keep every
// channel; channel 0 is then every object's, so they still reach objects outside all collections. UE has no per-light
// shadow exclusion: an object stops casting only when it is outside every shadow-casting light's shadowLink.
void prepareLinks(HdUEState& state, ScenePacket& packet) {
    std::set<TfToken> links, shadowLinks;
    bool base = false, shadows = false, allShadows = false;
    static std::once_flag domeWarned, channelsWarned, shadowWarned;
    for (const auto& [id, light] : state.lights) {
        if (!active(light)) continue;
        if (light.type == HDUE_DOME_LIGHT && !light.lightLink.IsEmpty())
            std::call_once(domeWarned, [&] {
                TF_WARN("hdUE: UE sky lights light everything; lightLink ignored (%s)", id.c_str());
            });
        else if (light.type != HDUE_DOME_LIGHT)
            light.lightLink.IsEmpty() ? void(base = true) : void(links.insert(light.lightLink));
        if (light.flags & HDUE_LIGHT_SHADOWS) {
            shadows = true;
            light.shadowLink.IsEmpty() ? void(allShadows = true) : void(shadowLinks.insert(light.shadowLink));
        }
    }
    if (links.size() > 3) base = true;  // lights beyond the channels light everything
    std::map<TfToken, uint32_t> channels;
    for (const auto& link : links)
        if (channels.size() < 3u - base) channels.emplace(link, 1u << (channels.size() + base));
    if (channels.size() < links.size())
        std::call_once(channelsWarned, [&] {
            TF_WARN(
                "hdUE: %zu light-link collections; UE has 3 lighting channels, so %zu linked lights light everything",
                links.size(), links.size() - channels.size());
        });
    if (!shadowLinks.empty() && (allShadows || shadowLinks.size() > 1))
        std::call_once(shadowWarned, [] {
            TF_WARN("hdUE: shadow linking only removes the shadows of objects outside every light's shadowLink");
        });
    for (const auto& [id, light] : state.lights) {
        const auto channel = channels.find(light.lightLink);
        packet.linkedLights[id] = channel == channels.end() ? 0 : channel->second;
        const auto sent = state.linkedLights.find(id);
        if ((sent == state.linkedLights.end() ? 0 : sent->second) != packet.linkedLights[id])
            packet.dirtyLights.insert(id);
    }
    if (!channels.empty() || (shadows && !allShadows))
        for (const auto& [id, mesh] : state.meshes) {
            uint32_t flags = channels.empty() ? HDUE_LINK_CHANNELS : base;
            bool casts = !shadows || allShadows;
            for (const auto& category : mesh.categories) {
                if (const auto channel = channels.find(category); channel != channels.end()) flags |= channel->second;
                casts |= shadowLinks.count(category) > 0;
            }
            if (!casts) flags |= HDUE_LINK_NO_SHADOW;
            if (flags != HDUE_LINK_CHANNELS) packet.linkedObjects[id] = flags;
        }
    for (const auto& [id, flags] : packet.linkedObjects) trace("hdUE link %s flags=%u\n", id.c_str(), flags);
    for (const auto& [id, channels] : packet.linkedLights)
        trace("hdUE light %s channels=%u (%s)\n", id.c_str(), channels, state.lights[id].lightLink.GetText());
    packet.sendLinks =
        packet.linkedObjects != (packet.firstBatch ? decltype(packet.linkedObjects){} : state.linkedObjects);
}
void prepareLights(HdUEState& state, ScenePacket& packet) {
    const double viewerIntensity =
        state.setting<double>("viewerLightIntensity", state.setting<float>("viewerLightIntensity", 4.f));
    if (!std::isfinite(viewerIntensity) || viewerIntensity < 0 || viewerIntensity > 100)
        throw std::runtime_error("viewerLightIntensity must be between 0 and 100");
    int sceneLights = 0;
    for (const auto& [id, light] : state.lights)
        if (active(light)) {
            if (light.viewer)
                ++packet.cameraLights;
            else if (state.setting<bool>("sceneLightsEnabled", true))
                ++sceneLights;
        }
    packet.autoLight = state.setting<bool>("autoViewerLight", false) && !sceneLights && !packet.cameraLights &&
                       !state.nativeAssets.lights;
    const double units = state.setting<double>("metersPerUnit", .01) * 100;
    const auto domeResolution = uint32_t(state.setting<int>("domeResolution", 512));
    const auto extend = [&](const HdueLightDesc& legacy, const HdUELightData* source) {
        HdueLight light{};
        light.struct_size = sizeof(light);
        light.type = legacy.type;
        light.id = legacy.id;
        light.visible = legacy.visible;
        light.intensity = legacy.intensity;
        light.flags = source ? source->flags : HDUE_LIGHT_SHADOWS | HDUE_LIGHT_MEGALIGHTS;
        light.diffuse = source ? source->diffuse : 1;
        light.specular = source ? source->specular : 1;
        light.cone_angle = source ? source->coneAngle : 180;
        light.cone_softness = source ? source->coneSoftness : 0;
        light.angle_degrees = source ? source->angle : .53f;
        light.cube_resolution = domeResolution;
        light.texture_format = source ? source->textureFormat : HDUE_ENVIRONMENT_AUTOMATIC;
        light.texture_file =
            packet.own(source ? textureFile(state, source->texture, true) : "");  // scene_linear, as material colours
        light.ies_file = packet.own(source ? source->ies : "");
        light.ies_angle_scale = source ? source->iesAngleScale : 0;
        light.ies_normalize = source && source->iesNormalize;
        if (source) light.lighting_channels = packet.linkedLights[source->id];
        for (int c = 0; c < 3; ++c) {
            light.position_cm[c] = legacy.position[c];
            light.color[c] = legacy.color[c];
        }
        for (int axis = 0; axis < 3; ++axis) {
            GfVec3d direction(0);
            direction[axis] = 1;
            direction =
                source ? state.convert(source->transform.TransformDir(direction), true).GetNormalized() : direction;
            for (int c = 0; c < 3; ++c) light.axes[axis * 3 + c] = direction[c];
        }
        light.width_cm = legacy.width_cm;
        light.height_cm = legacy.height_cm;
        if (source) {
            light.length_cm =
                float(source->length * source->transform.TransformDir(GfVec3d(1, 0, 0)).GetLength() * units);
            if (source->type == HDUE_CYLINDER_LIGHT || source->type == HDUE_SPHERE_LIGHT) {
                const double x = source->transform.TransformDir(GfVec3d(1, 0, 0)).GetLength();
                const double y = source->transform.TransformDir(GfVec3d(0, 1, 0)).GetLength();
                const double z = source->transform.TransformDir(GfVec3d(0, 0, 1)).GetLength();
                light.width_cm =
                    float(source->width * (source->type == HDUE_CYLINDER_LIGHT ? std::max(y, z) : std::max({x, y, z})) *
                          units);
                if (light.width_cm == 0 && source->type == HDUE_SPHERE_LIGHT) light.flags |= HDUE_LIGHT_POINT;
            }
        }
        if (legacy.type == HDUE_CAMERA_DISTANT_LIGHT) {
            for (int i = 0; i < 9; ++i) light.axes[i] = (i % 4) == 0 ? 1. : 0.;
            for (int c = 0; c < 3; ++c) light.axes[c] = legacy.direction[c];
        }
        packet.extendedLights.push_back(light);
    };
    for (const auto& id : packet.dirtyLights) {
        const auto& data = state.lights.at(id);
        HdueLightDesc desc{};
        desc.id = packet.own(data.id);
        desc.type = data.type;
        desc.visible = data.visible && (data.viewer || state.setting<bool>("sceneLightsEnabled", true));
        const auto position = state.convert(data.transform.ExtractTranslation());
        const auto direction = state.convert(data.transform.TransformDir(GfVec3d(0, 0, -1)), true).GetNormalized();
        for (int c = 0; c < 3; ++c) {
            desc.position[c] = position[c];
            desc.direction[c] = direction[c];
            desc.color[c] = data.color[c];
        }
        desc.intensity = data.intensity;
        if (data.viewer) {
            desc.intensity = float(viewerIntensity);
            desc.direction[0] = 1;
            desc.direction[1] = desc.direction[2] = 0;
        }
        desc.width_cm = float(data.width * data.transform.TransformDir(GfVec3d(1, 0, 0)).GetLength() * units);
        desc.height_cm = float(data.height * data.transform.TransformDir(GfVec3d(0, 1, 0)).GetLength() * units);
        extend(desc, &data);
        if (HdUETraceScene())
            TF_STATUS("hdUE light %s intensity=%g rgb=(%g,%g,%g)", desc.id, desc.intensity, desc.color[0],
                      desc.color[1], desc.color[2]);
    }
    if (packet.autoLight) {
        for (int i = 0; i < 2; ++i) {
            HdueLightDesc desc{};
            desc.id = i ? AutoFill : AutoKey;
            desc.type = HDUE_CAMERA_DISTANT_LIGHT;
            desc.visible = 1;
            desc.color[0] = desc.color[1] = desc.color[2] = 1;
            desc.intensity = float(viewerIntensity * (i ? 0.1875 : 1.0));
            desc.direction[0] = 1;
            desc.direction[1] = i ? -0.6 : 0.4;
            desc.direction[2] = i ? -0.2 : -0.5;
            extend(desc, nullptr);
        }
    } else if (state.autoLightActive) {
        packet.removed.push_back(AutoKey);
        packet.removed.push_back(AutoFill);
    }
}
void prepareVolumes(HdUEState& state, ScenePacket& packet) {
    if (packet.dirtyVolumes.empty()) return;
    // Voxel space to UE world: USD placement, then the USD -> UE basis and units (row vectors).
    const auto basis = sceneBasis(state);
    const double perCentimeter = 1 / (state.setting<double>("metersPerUnit", .01) * 100);
    for (const auto& id : packet.dirtyVolumes) {
        const auto it = state.volumes.find(id);
        if (it == state.volumes.end()) continue;
        const auto& volume = it->second;
        HdueVolumeDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.id = packet.own(volume.id);
        desc.visible = volume.visible && state.shown(volume.renderTag);
        desc.width = volume.width;
        desc.height = volume.height;
        desc.depth = volume.depth;
        desc.density = packet.own(packet.floats, std::vector<float>(volume.density.begin(), volume.density.end()));
        const auto m = volume.localToWorld * basis;
        rowMajor(m, desc.transform);
        for (int c = 0; c < 3; ++c) desc.albedo[c] = volume.albedo[c];
        // Density is extinction per scene unit; UE's ray marcher steps in voxel units (measured: halving
        // the resolution halves optical depth), so scale by the voxel's edge length in cm.
        const double voxelCm = std::cbrt(std::abs(m.ExtractRotationMatrix().GetDeterminant()));
        desc.extinction_scale = float(perCentimeter * voxelCm);
        packet.volumes.push_back(desc);
    }
}
void prepareSurfaceEffects(HdUEState& state, ScenePacket& packet) {
    const auto effects = state.setting<VtDictionary>("surfaceEffects", {});
    const auto nativeMeshes = state.setting<VtDictionary>("nativeMeshAssets", {});
    const bool preview = state.setting<bool>("wetMaskPreview", false);
    for (const auto& [id, data] : state.meshes) {
        const auto fields = HdUEWetSettings(effects, data);
        if (fields.empty()) continue;
        const float amount = HdUEWetValue<float>(fields, "amount", float(HdUEWetValue<double>(fields, "amount", 1.)));
        if (amount == 0.f && !preview) continue;
        const auto maskName = HdUEWetValue<std::string>(fields, "maskPrimvar", "");
        const auto mask = data.masks.find(maskName);
        if (!maskName.empty() && mask == data.masks.end()) {
            const auto error = data.maskErrors.find(maskName);
            throw std::runtime_error(
                data.bindingPath + ": " +
                (error != data.maskErrors.end() ? error->second : "Wet mask not synchronized: " + maskName));
        }
        HdueWetBinding binding{};
        binding.struct_size = sizeof(binding);
        binding.object_id = packet.own(data.id);
        binding.variant_asset = packet.own(HdUEWetValue<std::string>(fields, "asset", ""));
        binding.preview_asset = packet.own(HdUEWetValue<std::string>(fields, "previewAsset", ""));
        binding.amount = amount;
        binding.roughness =
            HdUEWetValue<float>(fields, "roughness", float(HdUEWetValue<double>(fields, "roughness", .08)));
        binding.flags = preview ? HDUE_WET_MASK_PREVIEW : 0;
        if (HdUEWetValue<bool>(fields, "graph", false)) binding.flags |= HDUE_SURFACE_GRAPH;
        if (!maskName.empty()) {
            binding.mask = packet.own(packet.floats, std::vector<float>(mask->second.begin(), mask->second.end()));
            binding.mask_count = uint32_t(mask->second.size());
        }
        // The revision this upload leaves the mesh at: generic meshes advance by one when re-uploaded.
        const bool reuploaded =
            packet.dirtyMeshes.count(id) && data.sceneAsset.empty() && asset(nativeMeshes, id).empty();
        binding.geometry_revision = state.geometryRevisions[data.id] + uint64_t(reuploaded);
        packet.wet.push_back(binding);
    }
    packet.surfaceUpdate = !packet.wet.empty() || state.surfaceStatus.binding_count;
}
// Under the state lock: takes the dirty sets and copies what UE needs.
void prepareScene(HdUEState& state, ScenePacket& packet) {
    packet.take(state);
    packet.revision = state.revision;
    packet.firstBatch = state.firstBatch;
    packet.nativeLights = state.nativeAssets.lights;
    prepareSurfaceEffects(state, packet);
    prepareMaterials(state, packet);
    prepareMeshes(state, packet);
    prepareLinks(state, packet);
    prepareLights(state, packet);
    prepareVolumes(state, packet);
    for (const auto& id : packet.removedIds) packet.removed.push_back(packet.own(id));
}
// Without the state lock: only the backend and the packet (the UE thread owns the session).
void sendScene(const HdUEState& state, ScenePacket& packet) {
    const auto& backend = *state.backend;
    const auto session = state.session;
    const auto& api = backend.api();
    HdueSceneBatch batch{};
    batch.struct_size = sizeof(batch);
    batch.reset_scene = packet.firstBatch;
    batch.meshes = packet.meshes.data();
    batch.mesh_count = uint32_t(packet.meshes.size());
    batch.materials = packet.materials.data();
    batch.material_count = uint32_t(packet.materials.size());
    batch.removed_ids = packet.removed.data();
    batch.removed_count = uint32_t(packet.removed.size());
    if (batch.reset_scene || batch.mesh_count || batch.material_count || batch.light_count || batch.removed_count) {
        const auto applyStart = std::chrono::steady_clock::now();
        backend.require(api.apply_scene(session, &batch), "apply_scene");
        uint64_t vertices = 0, triangles = 0;
        for (const auto& mesh : packet.meshes) {
            vertices += mesh.vertex_count;
            triangles += mesh.index_count / 3;
        }
        trace(
            "hdUE scene submit: UE apply %.1f ms (%u meshes: %llu vertices, %llu triangles; %u materials, %u lights)\n",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count(),
            batch.mesh_count, (unsigned long long)vertices, (unsigned long long)triangles, batch.material_count,
            batch.light_count);
        double syncMs = 0;
        int syncs = 0;
        HdUETakeMeshSyncStats(syncMs, syncs);
        if (syncs)
            trace("hdUE mesh sync since last submit: %d meshes, %.1f ms summed over Hydra threads\n", syncs, syncMs);
    }
    if (!packet.extendedLights.empty() || packet.sendLinks) {
        HdueLightBatch update{sizeof(HdueLightBatch), uint32_t(packet.extendedLights.size()),
                              packet.extendedLights.data()};
        std::vector<const char*> ids;
        std::vector<uint32_t> links;
        for (const auto& [id, flags] : packet.linkedObjects) {
            ids.push_back(id.c_str());
            links.push_back(flags);
        }
        if (packet.sendLinks) {
            update.flags = HDUE_LIGHT_BATCH_LINKS;
            update.object_count = uint32_t(ids.size());
            update.object_ids = ids.data();
            update.object_links = links.data();
        }
        backend.require(api.upsert_lights(session, &update), "upsert_lights");
    }
    if (!packet.moves.empty()) {
        backend.require(api.set_transforms(session, packet.moves.data(), uint32_t(packet.moves.size())),
                        "set_transforms");
        trace("hdUE scene submit: %zu meshes moved, geometry kept\n", packet.moves.size());
    }
    for (const auto& id : packet.removedIds) backend.require(api.remove_asset(session, id.c_str()), "remove_asset");
    for (const auto* id : packet.removedAssets) backend.require(api.remove_asset(session, id), "remove_asset");
    for (const auto& desc : packet.sceneAssets) backend.require(api.upsert_asset(session, &desc), "upsert_asset");
    for (const auto& desc : packet.volumes) backend.require(api.upsert_volume(session, &desc), "upsert_volume");
    for (const auto& update : packet.materialUpdates)
        backend.require(api.set_material_parameters(session, &update), "set_material_parameters");
}
// Under the state lock again: records what reached UE.
void finishScene(HdUEState& state, const ScenePacket& packet, const HdueSurfaceStatus& surface) {
    for (const auto& id : packet.removedIds) {
        if (state.meshes.count(id)) {
            state.dirtyMeshes.insert(id);
            ++state.revision;
            continue;
        }  // revived during the send
        state.geometryRevisions.erase(id);
        state.erasedMeshes.erase(id);
    }
    for (const auto& id : packet.genericMeshes) ++state.geometryRevisions[id];
    state.submittedMeshes += packet.meshes.size();
    state.submittedMaterials += packet.materials.size();
    state.materialParameterUpdates += packet.materialUpdates.size();
    state.autoLightActive = packet.autoLight;
    state.viewerLights = packet.cameraLights + (packet.autoLight ? 2 : 0);
    if (packet.surfaceUpdate) state.surfaceStatus = surface;
    state.submittedNativeLights = packet.nativeLights;
    state.linkedLights = packet.linkedLights;
    if (packet.sendLinks) state.linkedObjects = packet.linkedObjects;
    ++state.sceneSubmissions;
    state.submittedRevision = packet.revision;
    state.firstBatch = false;
}
// Uploads the scene changes since the last call. Runs on the UE thread; holds the state lock only to copy and record.
void submit(HdUEState& state, std::map<std::string, std::vector<GfVec3f>> points = {}) {
    const auto start = std::chrono::steady_clock::now();
    ScenePacket packet;
    packet.points = std::move(points);
    {
        std::lock_guard lock(state.mutex);
        try {
            prepareScene(state, packet);
        } catch (...) {
            packet.giveBack(state);
            throw;
        }
    }
    trace("hdUE scene prepare: %.1f ms under the state lock\n",
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    HdueSurfaceStatus surface{sizeof(HdueSurfaceStatus)};
    if (!packet.meshes.empty() || !packet.materials.empty())
        state.setStage("Uploading scene: " + std::to_string(packet.meshes.size()) + " meshes, " +
                       std::to_string(packet.materials.size()) + " materials");
    try {
        sendScene(state, packet);
        if (packet.surfaceUpdate) {
            const auto& api = state.backend->api();
            HdueSurfaceBatch batch{sizeof(batch), uint32_t(packet.wet.size()), packet.wet.data()};
            state.backend->require(api.apply_surfaces(state.session, &batch), "apply_surface_effects");
            state.backend->require(api.get_surface_status(state.session, &surface), "surface_status");
        }
    } catch (...) {
        std::lock_guard lock(state.mutex);  // not uploaded: dirty again, for the next attempt
        packet.giveBack(state);
        throw;
    }
    state.setStage("Rendering");
    std::lock_guard lock(state.mutex);
    finishScene(state, packet, surface);
}
class Pass final : public HdRenderPass {
    std::shared_ptr<HdUEState> state_;
    GfMatrix4d view_{0.0}, projection_{0.0};
    GfVec4f lens_{0.f};  // fStop, focus distance, focal length, linear exposure scale of the requested camera
    // Motion blur: the beauty averages subframes_ converged renders at times spread over the shutter.
    int subframe_ = 0, subframes_ = 1;
    uint32_t subframeSamples_ = 0;  // Path Tracer samples of the finished sub-frames
    bool subframeDue_ = false, motionApplied_ = false;
    std::vector<float> accum_;
    // Moves every moving mesh to frame-relative time t through set_transforms; t = 0 restores the submitted pose.
    void applyMotion(HdUEState& s, float t) {
        if (!t && !motionApplied_) return;
        const auto basis = sceneBasis(s);
        const auto inverse = basis.GetInverse();
        std::unique_lock lock(s.mutex);
        std::vector<std::vector<double>> storage;
        std::vector<const std::string*> ids;
        const auto nativeMeshes = s.setting<VtDictionary>("nativeMeshAssets", {});
        for (const auto& [id, mesh] : s.meshes) {
            if (!mesh.moving()) continue;
            const auto left = mesh.motionLeft.at(t), right = mesh.motionRight.at(t);
            const auto place = [&](GfMatrix4d m, size_t instance) {
                m = left * m * right;
                const bool velocity = mesh.velocities.size() > instance,
                           acceleration = mesh.accelerations.size() > instance;
                for (int c = 0; c < 3; ++c)
                    m[3][c] += (velocity ? mesh.velocities[instance][c] * t : 0.f) +
                               (acceleration ? mesh.accelerations[instance][c] * t * t / 2 : 0.f);
                return m;
            };
            auto& out = storage.emplace_back();
            const auto push = [&](const GfMatrix4d& usd) {
                const auto m = inverse * usd * basis;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) out.push_back(m[r][c]);
            };
            if (mesh.instanced) {
                for (size_t i = 0; i < mesh.instances.size(); ++i) push(place(mesh.instances[i], i));
            } else if (!asset(nativeMeshes, id).empty() || !HdUEShears(mesh.transform))
                push(place(mesh.transform, SIZE_MAX));
            else
                push(mesh.transform.GetInverse() * place(mesh.transform, SIZE_MAX));  // baked into world space
            ids.push_back(&id);
        }
        // Deforming meshes are resubmitted with their points at t, before set_transforms moves them.
        std::map<std::string, std::vector<GfVec3f>> deformed;
        for (const auto& [id, mesh] : s.meshes)
            if (!mesh.deformTimes.empty() && mesh.sceneAsset.empty()) {
                deformed[id] = mesh.pointsAt(t);
                s.dirtyMeshes.insert(id);
            }
        std::vector<std::string> names;
        names.reserve(ids.size());
        for (const auto* id : ids) names.push_back(*id);
        lock.unlock();
        if (!deformed.empty()) submit(s, std::move(deformed));
        std::vector<HdueMeshTransform> moves;
        for (size_t i = 0; i < names.size(); ++i)
            moves.push_back({names[i].c_str(), uint32_t(storage[i].size() / 16), storage[i].data()});
        if (!moves.empty())
            s.backend->require(s.backend->api().set_transforms(s.session, moves.data(), uint32_t(moves.size())),
                               "set_transforms");
        motionApplied_ = t != 0;
    }
    // Read by IsConverged on the host's thread while the UE thread writes them.
    std::atomic<uint64_t> settingsVersion_{~uint64_t(0)};
    uint64_t lastFrame_ = 0;
    double centimetersPerUnit_ = 1;
    bool depthEnabled_ = false;
    std::atomic<bool> converged_{false};
    std::atomic<bool> executeQueued_{false};  // Houdini's UI: an Execute posted to the UE thread and not yet started
    std::set<TfToken> warnedVars_;            // render vars hdUE does not render, warned once each
    std::vector<HdUERenderBuffer*>
        extraBeauties_;  // further render vars of the beauty (C in one product, Ci in another)
    std::chrono::steady_clock::time_point lastRequestAt_{};
    // GBuffer AOVs render one by one after the beauty converges, from its camera.
    std::unique_ptr<HdUERenderBuffer> internalColor_;  // beauty target when no color AOV is bound
    HdueRenderRequest beautyRequest_{};
    std::vector<std::pair<HdUERenderBuffer*, uint32_t>> aovs_;
    // hdUE:denoiser = oidn: the converged Path Tracer beauty waits for albedo and normal guides, then Open Image
    // Denoise (Houdini's, GPU when available) replaces it. Guides render as AOVs, internally when not bound.
    std::unique_ptr<HdUERenderBuffer> guideAlbedo_, guideNormal_;
    std::vector<float> beautyPixels_;  // top-down RGBA of the converged beauty, before denoising
    HdueFrameInfo beautyFrame_{};
    std::atomic<bool> denoisePending_{false};
#if defined(HDUE_HOUDINI)
    // Houdini's GL viewport takes every frame from the backend's shared GPU ring (gpu_frames.h); the CPU buffers then
    // receive only converged frames, for Map (the Python viewer, scripts). The CPU path stays for what it computes
    // after readback: motion blur sub-frame averaging, the denoiser and a non-identity OCIO scene_linear.
    HdueGpuOptions gpuOptions_{};
    HdUERenderBuffer* gpuBuffers_[2] = {};
    uint64_t gpuFrame_ = 0;
    void syncGpuFrames(HdUEState& s, HdUERenderBuffer& buffer, HdUERenderBuffer* depth, bool denoising) {
        const auto frames = s.locked([&] { return s.gpuFrames; });
        const auto& api = s.backend->api();
        HdueGpuOptions options{sizeof(options)};
        options.enabled = frames && !frames->failed() && subframes_ <= 1 && !denoising && HdUESceneLinearIsRec709();
        options.depth = depth != nullptr;
        options.color_scale = s.exposed() ? lens_[3] : 1.f;  // camera exposure, as the CPU path applies it
        options.depth_scale = float(1 / centimetersPerUnit_);
        if (std::memcmp(&options, &gpuOptions_, sizeof(options)) || gpuBuffers_[0] != &buffer ||
            gpuBuffers_[1] != depth) {
            if (api.configure_gpu_frames(s.session, &options) != HDUE_OK && options.enabled) {  // not a D3D12 runtime
                options.enabled = 0;
                s.backend->require(api.configure_gpu_frames(s.session, &options), "configure_gpu_frames");
            }
            for (auto* bound : {&buffer, depth})
                if (bound) bound->setResource(nullptr);
            if (options.enabled) {
                frames->setSession(s.session);
                buffer.setResource([frames, target = &buffer] {
                    return frames->color(target->GetWidth(), target->GetHeight(), target->IsConverged());
                });
                if (depth) depth->setResource([frames] { return frames->depth(); });
            }
            gpuOptions_ = options;
            gpuBuffers_[0] = &buffer;
            gpuBuffers_[1] = depth;
        }
        if (!options.enabled) return;
        HdueFrameInfo frame{};
        frame.struct_size = sizeof(frame);
        const auto polled = api.poll_gpu_frame(s.session, &frame);
        if (polled == HDUE_NOT_SUPPORTED) {  // UE cannot share textures here; its draws are all read back again
            buffer.setResource(nullptr);
            if (depth) depth->setResource(nullptr);
            gpuOptions_.enabled = 0;
            return;
        }
        if (polled != HDUE_PENDING) s.backend->require(polled, "poll_gpu_frame");
        // The host redraws the viewport when publishedFrames changes.
        if (polled == HDUE_OK && frame.frame_id != gpuFrame_) {
            gpuFrame_ = frame.frame_id;
            ++s.publishedFrames;
        }
    }
#endif
    static std::vector<float> topDown(HdUERenderBuffer& buffer, size_t channels) {
        std::vector<float> out;
        const auto* data = static_cast<const float*>(buffer.Map());
        if (!data) return out;
        const size_t width = buffer.GetWidth(), height = buffer.GetHeight(), row = width * channels;
        out.resize(row * height);
        for (size_t y = 0; y < height; ++y) std::copy_n(data + (height - 1 - y) * row, row, out.begin() + y * row);
        buffer.Unmap();
        return out;
    }
    void denoise(HdUERenderBuffer& beauty) {
#if defined(HDUE_HOUDINI)
        HdUERenderBuffer *albedo = nullptr, *normal = nullptr;
        for (const auto& [target, id] : aovs_) {
            if (id == HDUE_AOV_ALBEDO) albedo = target;
            if (id == HDUE_AOV_NORMAL) normal = target;
        }
        const auto albedoPixels = albedo ? topDown(*albedo, 3) : std::vector<float>();
        const auto normalPixels = normal ? topDown(*normal, 3) : std::vector<float>();
        const size_t width = beautyFrame_.width, height = beautyFrame_.height, count = width * height;
        static oidn::DeviceRef device = [] {
            auto d = oidn::newDevice();
            d.commit();
            return d;
        }();
        auto color = device.newBuffer(count * 4 * sizeof(float)), output = device.newBuffer(count * 4 * sizeof(float));
        color.write(0, count * 4 * sizeof(float), beautyPixels_.data());
        auto filter = device.newFilter("RT");
        filter.setImage("color", color, oidn::Format::Float3, width, height, 0, 4 * sizeof(float));
        filter.setImage("output", output, oidn::Format::Float3, width, height, 0, 4 * sizeof(float));
        oidn::BufferRef albedoBuffer, normalBuffer;
        if (albedoPixels.size() == count * 3) {
            albedoBuffer = device.newBuffer(count * 3 * sizeof(float));
            albedoBuffer.write(0, count * 3 * sizeof(float), albedoPixels.data());
            filter.setImage("albedo", albedoBuffer, oidn::Format::Float3, width, height);
            if (normalPixels.size() == count * 3) {
                normalBuffer = device.newBuffer(count * 3 * sizeof(float));
                normalBuffer.write(0, count * 3 * sizeof(float), normalPixels.data());
                filter.setImage("normal", normalBuffer, oidn::Format::Float3, width, height);
            }
            // The guides are the Path Tracer's own albedo and normal (averages of its samples), not prefiltered.
            filter.set("cleanAux", false);
        }
        filter.set("hdr", true);
        filter.commit();
        filter.execute();
        const char* message = nullptr;
        if (device.getError(message) != oidn::Error::None)
            throw std::runtime_error(std::string("Open Image Denoise: ") + (message ? message : "error"));
        std::vector<float> pixels(count * 4);
        output.read(0, count * 4 * sizeof(float), pixels.data());
        for (size_t i = 0; i < count; ++i) pixels[i * 4 + 3] = beautyPixels_[i * 4 + 3];
        HdUEToSceneLinear(pixels);  // OCIO scene_linear (ACEScg in ACES pipelines)
        for (auto* extra : extraBeauties_) extra->publish(beautyFrame_, pixels, 4);
        beauty.publish(beautyFrame_, std::move(pixels), 4);
#else
        for (auto* extra : extraBeauties_) extra->publish(beautyFrame_, beautyPixels_, 4);
        beauty.publish(beautyFrame_, beautyPixels_, 4);  // the standalone build has no denoiser
#endif
    }
    size_t aovSlot_ = 0;
    bool aovInFlight_ = false;
    std::atomic<bool> aovsDone_{true};  // also true while no AOV is bound
    void publishAov(HdUERenderBuffer& target, HdUEAovFormat format, const HdueFrameInfo& frame,
                    std::vector<float> rgba) const {
        const size_t count = size_t(frame.width) * frame.height;
        if (format == HdUEAovFormat::Int) {
            // Int32 buffers hold the integer bit patterns in the float storage.
            std::vector<float> out(count);
            for (size_t i = 0; i < count; ++i) {
                const int32_t value = rgba.empty() ? -1 : idAt(&rgba[i * 4]);
                std::memcpy(&out[i], &value, sizeof(value));
            }
            target.publish(frame, std::move(out), 1);
            return;
        }
        if (format == HdUEAovFormat::Beauty) {
            // A lighting component: exposed and converted like the beauty, so components add up to it.
            if (lens_[3] != 1.f && state_->exposed())
                for (size_t i = 0; i + 3 < rgba.size(); i += 4)
                    for (int c = 0; c < 3; ++c) rgba[i + c] *= lens_[3];
            HdUEToSceneLinear(rgba);
            target.publish(frame, std::move(rgba), 4);
            return;
        }
        const bool vector = format != HdUEAovFormat::Float;
        const bool zUp = state_->setting<std::string>("upAxis", "Y") == "Z";
        std::vector<float> out(count * (vector ? 3 : 1));
        for (size_t i = 0; i < count; ++i) {
            const float* p = &rgba[i * 4];
            if (!vector) {
                out[i] = p[0];
                continue;
            }
            float* o = &out[i * 3];
            // UE world directions back to USD world: the inverse of HdUEState::convert.
            if (format == HdUEAovFormat::Vector && zUp) {
                o[0] = p[0];
                o[1] = -p[1];
                o[2] = p[2];
            } else if (format == HdUEAovFormat::Vector) {
                o[0] = p[1];
                o[1] = p[2];
                o[2] = -p[0];
            } else {
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
            }
            // GBuffer normals are not normalized; background pixels stay zero.
            if (const float length = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                format == HdUEAovFormat::Vector && length > 1e-6f)
                for (int c = 0; c < 3; ++c) o[c] /= length;
        }
        target.publish(frame, std::move(out), vector ? 3 : 1);
    }
    // Buffer visualization render vars (HdUEAovTable Buffer rows), rendered together in one HDUE_AOV_BUFFERS pass.
    struct BufferVar {
        HdUERenderBuffer* target = nullptr;
        const HdUEAovInfo* row = nullptr;
        bool operator==(const BufferVar&) const = default;
    };
    std::vector<BufferVar> bufferVars_;
    void publishBuffers(HdUEState& s, const HdueFrameInfo& frame) {
        std::vector<const char*> names;
        for (const auto& var : bufferVars_) {
            const auto index = std::find(names.begin(), names.end(), var.row->buffer) - names.begin();
            if (size_t(index) == names.size()) names.push_back(var.row->buffer);
            std::vector<float> rgba(size_t(frame.required_bytes / sizeof(float)));
            s.backend->require(s.backend->api().copy_aov_buffer(s.session, frame.frame_id, uint32_t(index), rgba.data(),
                                                                frame.required_bytes),
                               "copy_buffer");
            // UE's direction buffers (WorldNormal, WorldTangent) are encoded as 0.5 + 0.5 * direction.
            if (var.row->format == HdUEAovFormat::Vector)
                for (auto& value : rgba) value = 2 * value - 1;
            publishAov(*var.target, var.row->format, frame, std::move(rgba));
        }
    }
    // The ID passes' encoding (HDUE_AOV_PRIM_ID): R = id / 2048, G = id % 2048, B = 1 where a mesh is visible.
    static int32_t idAt(const float* p) {
        return p[2] > .5f ? int32_t(std::lround(p[0])) * 2048 + int32_t(std::lround(p[1])) : -1;
    }
    // Cryptomatte render vars (CryptoObject00, CryptoMaterial01, ...): their buffer, kind (HdUECryptomatteLayer) and
    // layer.
    struct CryptoVar {
        HdUERenderBuffer* target = nullptr;
        int kind = 0, layer = 0;
        bool operator==(const CryptoVar&) const = default;
    };
    static constexpr size_t kCryptomatteSamples = 16;
    std::unique_ptr<HdUERenderBuffer> cryptoIds_;  // the target of the Cryptomatte ID passes in aovs_
    std::vector<CryptoVar> cryptoVars_;
    std::vector<int32_t> cryptoSamples_;  // kCryptomatteSamples prim IDs per pixel
    size_t cryptoSample() const {
        return aovSlot_ - size_t(std::find_if(aovs_.begin(), aovs_.end(),
                                              [&](const auto& aov) { return aov.first == cryptoIds_.get(); }) -
                                 aovs_.begin());
    }
    // One Cryptomatte ID pass joins the samples; the last publishes the layers and their metadata (render stats).
    void accumulateCryptomatte(HdUEState& s, const HdueFrameInfo& frame, const std::vector<float>& rgba) {
        constexpr size_t n = kCryptomatteSamples;
        const size_t count = size_t(frame.width) * frame.height, sample = cryptoSample();
        if (sample == 0) cryptoSamples_.assign(count * n, -1);
        if (cryptoSamples_.size() != count * n || rgba.size() < count * 4) return;
        for (size_t i = 0; i < count; ++i) cryptoSamples_[i * n + sample] = idAt(&rgba[i * 4]);
        if (sample + 1 < n) return;
        // Names: the prim's scene path, and its bound material (none for display colours); a GeomSubset's material is
        // not told apart, the ID pass draws the whole mesh with one ID.
        std::map<int32_t, std::array<std::string, 2>> names;
        s.locked([&] {
            for (const auto& [id, mesh] : s.meshes) {
                if (mesh.primId < 0) continue;
                auto& entry = names[mesh.primId];
                entry[0] = mesh.scenePath;
                if (mesh.id == mesh.sourceId || entry[1].empty())
                    entry[1] = TfStringEndsWith(mesh.material, ".displayColor") ? std::string() : mesh.material;
            }
        });
        std::set<int32_t> seen(cryptoSamples_.begin(), cryptoSamples_.end());
        std::unordered_map<int32_t, std::array<uint32_t, 2>> hashes;
        std::map<std::string, uint32_t> manifests[2];
        for (const int32_t id : seen) {
            const auto it = names.find(id);
            if (it == names.end()) continue;
            for (int kind = 0; kind < 2; ++kind)
                if (!it->second[kind].empty()) {
                    hashes[id][kind] = HdUECryptomatteHash(it->second[kind]);
                    manifests[kind][it->second[kind]] = hashes[id][kind];
                }
        }
        size_t layers[2] = {0, 0};
        for (const auto& var : cryptoVars_) layers[var.kind] = std::max(layers[var.kind], size_t(var.layer) + 1);
        std::vector<float> ranks[2];
        for (int kind = 0; kind < 2; ++kind) ranks[kind].resize(count * layers[kind] * 4);
        WorkParallelForN(count, [&](size_t begin, size_t end) {
            uint32_t ids[n];
            for (size_t i = begin; i < end; ++i)
                for (int kind = 0; kind < 2; ++kind) {
                    if (!layers[kind]) continue;
                    for (size_t k = 0; k < n; ++k) {
                        const auto it = hashes.find(cryptoSamples_[i * n + k]);
                        ids[k] = it == hashes.end() ? 0 : it->second[kind];
                    }
                    HdUECryptomatteRanks(ids, n, &ranks[kind][i * layers[kind] * 4], layers[kind] * 2);
                }
        });
        for (const auto& var : cryptoVars_) {
            std::vector<float> out(count * 4);
            for (size_t i = 0; i < count; ++i)
                std::copy_n(&ranks[var.kind][(i * layers[var.kind] + var.layer) * 4], 4, &out[i * 4]);
            var.target->publish(frame, std::move(out), 4);
        }
        // husk writes these as each layer's cryptomatte/<key>/ metadata (UsdRenderers.json husk.metadata). Prim and
        // material paths need no JSON escaping.
        VtDictionary metadata;
        for (int kind = 0; kind < 2; ++kind) {
            if (!layers[kind]) continue;
            std::string manifest;
            char hex[9];
            for (const auto& [name, hash] : manifests[kind]) {
                std::snprintf(hex, sizeof(hex), "%08x", hash);
                manifest += (manifest.empty() ? "{\"" : ",\"") + name + "\":\"" + hex + "\"";
            }
            const std::string layer = kind ? "CryptoMaterial" : "CryptoObject";
            metadata[layer + "_name"] = VtValue(layer);
            metadata[layer + "_hash"] = VtValue(std::string("MurmurHash3_32"));
            metadata[layer + "_conversion"] = VtValue(std::string("uint32_to_float32"));
            metadata[layer + "_manifest"] = VtValue(manifest.empty() ? "{}" : manifest + "}");
        }
        s.locked([&] { s.cryptomatte = std::move(metadata); });
    }
    // A bound render var hdUE does not render (other light path expressions, primvars, unknown names): its clear value,
    // converged, as HdArnold does, so hosts never wait for it; -1 in integer buffers ("no prim").
    static void clear(HdUERenderBuffer& target) {
        if (!target.GetWidth() || target.IsConverged()) return;
        HdueFrameInfo blank{};
        blank.struct_size = sizeof(blank);
        blank.width = target.GetWidth();
        blank.height = target.GetHeight();
        blank.converged = 1;
        const size_t channels = HdGetComponentCount(target.GetFormat());
        std::vector<float> values(size_t(blank.width) * blank.height * channels, 0.f);
        if (HdGetComponentFormat(target.GetFormat()) == HdFormatInt32) {
            const int32_t none = -1;
            for (auto& v : values) std::memcpy(&v, &none, sizeof(none));
        }
        target.publish(blank, std::move(values), channels);
    }

public:
    Pass(HdRenderIndex* index, const HdRprimCollection& collection, std::shared_ptr<HdUEState> state)
        : HdRenderPass(index, collection), state_(std::move(state)) {}
    // A posted Execute must not outlive the pass: wait for the UE thread to drain its queue.
    ~Pass() override {
        if (selfHost().continuous && selfHost().worker.joinable())
            selfHost().run([this] { selfHost().pumps.erase(this); });
    }
    bool IsConverged() const override {
        if (const auto error = state_->setting<std::string>("wetValidationError", ""); !error.empty())
            return converged_ && state_->error == error;
        if (!state_->setting<bool>("materialGraphValid", true)) return true;
        return converged_ && aovsDone_ && !denoisePending_ && settingsVersion_ == state_->requestSettingsRevision &&
               state_->submittedRevision == state_->revision;
    }
    // What Execute reads from Hydra's render pass state, copied on the calling thread: Houdini reuses that object for
    // its other drawing once Execute returns, so a posted Execute must not read it later.
    // The camera's values and the bound buffers are copied or resolved here too: a posted Execute runs while Houdini
    // syncs or deletes the camera and buffers, so it never dereferences Hydra's camera or looks up the render index.
    // Buffers are deleted on the UE thread (DestroyBprim), so one alive at the start of an Execute stays alive in it.
    struct PassSnapshot {
        HdRenderPassAovBindingVector aovs;
        std::vector<HdRenderBuffer*> buffers;
        GfMatrix4d view, projection;
        SdfPath cameraId;
        bool camera = false;
        double shutterOpen = 0, shutterClose = 0;
        float nearClip = .1f, farClip = 0;
        GfVec4f lens{0.f};
        std::vector<GfVec2i> sizes;
        bool same(const PassSnapshot& o) const {
            return aovs == o.aovs && view == o.view && projection == o.projection && cameraId == o.cameraId &&
                   lens == o.lens && sizes == o.sizes;
        }
    };
    // Framing (crops, tiles, overscan, non-square pixels): UE fills the whole render buffer, so the projection maps the
    // data window onto it. With Hydra framing, GetProjectionMatrix maps the data window to NDC; the data window may be
    // a part of the buffer (CameraUtilFraming), so it is remapped. husk sets no framing: it passes the data window as
    // the viewport (x, y from the bottom, in pixels of the resolution) with the buffer at its size, the resolution,
    // pixel aspect and conform policy as settings, and conforms the camera to the data window rather than the image.
    GfMatrix4d framedProjection(const HdRenderPassState& state, const HdCamera& camera,
                                const std::vector<GfVec2i>& sizes) const {
        auto& s = *state_;
        const auto resolution = s.setting<GfVec2i>("resolution", GfVec2i(0));
        if (!state.GetFraming().IsValid() && resolution[0] > 0 && resolution[1] > 0 &&
            s.setting<std::string>("houdini:renderer", "") == "husk") {
            const auto vp = state.GetViewport();
            const CameraUtilFraming framing(
                GfRange2f(GfVec2f(0), GfVec2f(resolution)),
                GfRect2i(GfVec2i(int(vp[0]), resolution[1] - int(vp[1]) - int(vp[3])), int(vp[2]), int(vp[3])),
                s.setting<float>("pixelAspectRatio", 1.f));
            const auto policy = s.setting<std::string>("aspectRatioConformPolicy", "expandAperture");
            return framing.ApplyToProjectionMatrix(camera.ComputeProjectionMatrix(),
                                                   policy == "cropAperture"             ? CameraUtilCrop
                                                   : policy == "adjustApertureWidth"    ? CameraUtilMatchVertically
                                                   : policy == "adjustApertureHeight"   ? CameraUtilMatchHorizontally
                                                   : policy == "adjustPixelAspectRatio" ? CameraUtilDontConform
                                                                                        : CameraUtilFit);
        }
        auto projection = state.GetProjectionMatrix();
        const auto& framing = state.GetFraming();
        const auto size = sizes.empty() ? GfVec2i(0) : sizes[0];
        if (!framing.IsValid() || size[0] <= 0 || size[1] <= 0) return projection;
        const auto& w = framing.dataWindow;  // pixels of the buffer, y down
        GfMatrix4d remap(1);
        remap[0][0] = double(w.GetWidth()) / size[0];
        remap[1][1] = double(w.GetHeight()) / size[1];
        remap[3][0] = double(2 * w.GetMinX() + w.GetWidth() - size[0]) / size[0];
        remap[3][1] = double(size[1] - 2 * w.GetMinY() - w.GetHeight()) / size[1];
        return projection * remap;
    }
    void _Execute(const HdRenderPassStateSharedPtr& state, const TfTokenVector& tags) override {
        const auto* camera = state->GetCamera();
        PassSnapshot snapshot{state->GetAovBindings(), {}, state->GetWorldToViewMatrix(), state->GetProjectionMatrix()};
        if (camera) {
            snapshot.camera = true;
            snapshot.cameraId = camera->GetId();
            snapshot.shutterOpen = camera->GetShutterOpen();
            snapshot.shutterClose = camera->GetShutterClose();
            snapshot.nearClip = camera->GetClippingRange().GetMin();
            snapshot.farClip = camera->GetClippingRange().GetMax();
            snapshot.lens = GfVec4f(camera->GetFStop(), camera->GetFocusDistance(), camera->GetFocalLength(),
                                    camera->GetLinearExposureScale());
        }
        for (const auto& aov : snapshot.aovs) {
            auto* b = aov.renderBuffer ? aov.renderBuffer
                                       : static_cast<HdRenderBuffer*>(GetRenderIndex()->GetBprim(
                                             HdPrimTypeTokens->renderBuffer, aov.renderBufferId));
            snapshot.buffers.push_back(b);
            snapshot.sizes.emplace_back(b ? int(b->GetWidth()) : 0, b ? int(b->GetHeight()) : 0);
        }
        if (camera) snapshot.projection = framedProjection(*state, *camera, snapshot.sizes);
        const auto pass = std::make_shared<const PassSnapshot>(std::move(snapshot));
        setRenderTags(tags);
        // Wet masks read the scene delegate, which only this thread may use while Hydra syncs.
        if (state_->locked([&] { return state_->submittedRevision != state_->revision; }))
            HdUERefreshWetMasks(*state_, GetRenderIndex());
        // husk polls Execute at its own pace and UE advances one tick per call; when this process owns UE,
        // keep ticking for a while so a realtime frame takes its GPU time rather than husk's polling interval.
        auto& host = selfHost();
        if (!host.worker.joinable()) {
            // Houdini is quitting: UE ended (hdue_shutdown_ue) with its session, under viewports that still draw.
            if (host.continuous) {
                converged_ = true;
                return;
            }
            ueRun([&] { executeOnce(pass); });
            return;
        }
        if (host.continuous) {
            if (const auto error = host.error(); !error.empty()) {
                std::lock_guard lock(state_->mutex);
                state_->error = error;
                converged_ = true;
                return;
            }
            // Houdini's UI: post and return; Hydra calls again while not converged. A UE tick can take hundreds of ms
            // (scene uploads, ID-pass material swaps), which froze the UI while it waited.
            // Calls arriving while one is queued are coalesced, so the queued call and the pump always render the
            // newest camera: keeping the camera of the call that was queued lost Houdini's last move (it then saw a
            // converged image and stopped calling).
            // Only a new camera, AOV or size restarts convergence (scene and settings changes are in IsConverged):
            // Houdini calls again while a pass is not converged, so resetting it on every call kept Houdini redrawing
            // without end and a flipbook waiting for convergence until its own timeout.
            bool moved;
            {
                std::lock_guard lock(latestMutex_);
                moved = !latestPass_ || !latestPass_->same(*pass);
                latestPass_ = pass;
            }
            if (moved) converged_ = false;
            const auto eye = pass->view.GetInverse().ExtractTranslation();
            const bool skipped = executeQueued_.exchange(true);
            trace("hdUE execute posted=%d eye=(%.2f, %.2f, %.2f)\n", int(!skipped), eye[0], eye[1], eye[2]);
            if (skipped) return;
            host.post([this] {
                executeQueued_ = false;
                executeLatest();
                // Between Executes, keep publishing frames UE finishes for the newest camera.
                selfHost().pumps[this] = [this] {
                    if (!executeQueued_ && !IsConverged() && !state_->paused) executeLatest();
                };
            });
            return;
        }
        const auto waitStart = std::chrono::steady_clock::now();
        const auto ran = std::make_shared<std::chrono::steady_clock::time_point>();
        struct Report {
            std::chrono::steady_clock::time_point start;
            std::shared_ptr<std::chrono::steady_clock::time_point> ran;
            ~Report() {
                using namespace std::chrono;
                const auto end = steady_clock::now();
                if (ran->time_since_epoch().count())
                    trace("hdUE execute call: waited %.1f ms for the UE thread, %.1f ms total\n",
                          duration<double, std::milli>(*ran - start).count(),
                          duration<double, std::milli>(end - start).count());
            }
        } report{waitStart, ran};
        host.run([&] {
            *ran = std::chrono::steady_clock::now();
            // Houdini's UI: UE ticks between calls on its thread, so submit and poll without holding the UI thread.
            if (host.continuous) {
                executeOnce(pass);
                return;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            do executeOnce(pass);
            while (!IsConverged() && state_->error.empty() && std::chrono::steady_clock::now() < deadline);
        });
    }
    std::mutex latestMutex_;
    std::shared_ptr<const PassSnapshot> latestPass_;
    void executeLatest() {
        std::shared_ptr<const PassSnapshot> pass;
        {
            std::lock_guard lock(latestMutex_);
            pass = latestPass_;
        }
        if (pass) executeOnce(pass);
    }
    // Render tags (purposes): prims whose tag the pass does not draw are sent hidden; a change re-sends only the prims
    // whose visibility it flips. Empty tags draw everything.
    void setRenderTags(TfTokenVector tags) {
        auto& s = *state_;
        // husk does not apply UsdRenderSettings includedPurposes to the pass's tags.
        tags = HdUEIncludedTags(std::move(tags), s.setting<VtTokenArray>("includedPurposes", {}));
        std::sort(tags.begin(), tags.end());
        std::lock_guard lock(s.mutex);
        if (tags == s.renderTags) return;
        const auto flips = [&](const TfToken& tag) {
            return HdUEState::tagShown(tags, tag) != HdUEState::tagShown(s.renderTags, tag);
        };
        for (const auto& [id, mesh] : s.meshes)
            if (flips(mesh.renderTag)) s.dirtyMeshes.insert(id);
        for (const auto& [id, volume] : s.volumes)
            if (flips(volume.renderTag)) s.dirtyVolumes.insert(id);
        s.renderTags = std::move(tags);
        ++s.revision;
    }
    void executeOnce(const std::shared_ptr<const PassSnapshot>& pass) {
        // HDUE_TRACE_TIMING=1: one line per Execute that did work, with where its time went.
        static const bool timing = TfGetenvBool("HDUE_TRACE_TIMING", false);
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        double copyMs = 0, publishMs = 0, aovMs = 0;
        const auto ms = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        HdueFrameInfo traced{};
        bool requested = false;
        try {
            auto& s = *state_;
            if (const auto error = s.setting<std::string>("wetValidationError", ""); !error.empty()) {
                // Preparation/undo must change the graph before rendering can
                // resume. Stop Hydra's update loop, retaining the displayed AOV
                // while keeping the invalid request out of completed-frame APIs.
                s.setError(error);
                s.lastFrame = {};
                converged_ = true;
                return;
            }
#if defined(HDUE_HOUDINI)
            s.executeThread = GetCurrentThreadId();
#endif
            if (!selfHost().pumping) {
                const auto tickStart = Clock::now();
                selfHost()
                    .tick();  // pumps run right after the loop's tick: a second one doubled each frame's engine cost
                if (const double tickMs = ms(tickStart, Clock::now()); tickMs > 50)
                    trace("hdUE slow execute tick: %.1f ms\n", tickMs);
            }
            // Keep the last valid AOV while a prepared material graph is
            // invalid. Dirty USD data stays queued until preparation/undo.
            if (!s.setting<bool>("materialGraphValid", true)) {
                converged_ = true;
                return;
            }
            HdUERenderBuffer* buffer = nullptr;
            HdUERenderBuffer* depth = nullptr;
            HdUERenderBuffer* cameraDepth = nullptr;  // linear, in scene units
            std::vector<std::pair<HdUERenderBuffer*, uint32_t>> aovs;
            std::vector<CryptoVar> crypto;
            std::vector<BufferVar> bufferVars;
            const bool pathTraced = s.setting<std::string>("renderMode", "realtime") == "pathtraced";
            extraBeauties_.clear();
            std::vector<HdRenderBuffer*> bound;  // alive buffers, by binding
            for (auto* b : pass->buffers) bound.push_back(HdUERenderBuffer::alive(b) ? b : nullptr);
            for (size_t binding = 0; binding < pass->aovs.size(); ++binding) {
                const auto& aov = pass->aovs[binding];
                auto* renderBuffer = bound[binding];
                // husk names bindings after the render var's AOV name; a raw var's sourceName says what to render.
                const auto setting = [&](const char* key) {
                    const auto it = aov.aovSettings.find(TfToken(key));
                    if (it == aov.aovSettings.end()) return TfToken();
                    return it->second.IsHolding<TfToken>()       ? it->second.UncheckedGet<TfToken>()
                           : it->second.IsHolding<std::string>() ? TfToken(it->second.UncheckedGet<std::string>())
                                                                 : TfToken();
                };
                TfToken name = aov.aovName;
                const auto sourceType = setting("sourceType"), sourceName = setting("sourceName");
                // Light path expressions: the whole beauty (Karma's C.*[LO], or C.*), else a lighting component of
                // the AOV table (Karma's names: C<RD>.*L, ...); primvar vars are not rendered.
                if (sourceType == TfToken("lpe")) {
                    const auto lpe = TfStringReplace(sourceName.GetString(), " ", "");
                    name = lpe == "C.*" || lpe == "C.*[LO]" || lpe == "C.*[OL]" ? HdAovTokens->color : TfToken(lpe);
                } else if (sourceType == TfToken("primvar"))
                    name = TfToken();
                else if (!sourceName.IsEmpty() && (sourceType.IsEmpty() || sourceType == TfToken("raw")))
                    name = sourceName;
                name = HdUEBeauty(name);
                if (const auto layer = HdUECryptomatteLayer(name.GetString()); layer.first >= 0) {
                    if (auto* target = dynamic_cast<HdUERenderBuffer*>(renderBuffer))
                        crypto.push_back({target, layer.first, layer.second});
                    continue;
                }
                const auto* row = HdUEFindAov(name.GetString());
                if (name != HdAovTokens->color && name != HdAovTokens->depth && name != HdAovTokens->cameraDepth &&
                    (!row || !(row->modes & (pathTraced ? HdUEPathTraced : HdUERealtime)))) {
                    if (warnedVars_.insert(aov.aovName).second)
                        TF_WARN("hdUE does not render the render var %s (%s %s)%s; it receives its clear value",
                                aov.aovName.GetText(), sourceType.GetText(), sourceName.GetText(),
                                row ? (pathTraced ? " in path-traced mode" : " in realtime mode") : "");
                    if (auto* target = dynamic_cast<HdUERenderBuffer*>(renderBuffer)) clear(*target);
                    continue;
                }
                if (name == HdAovTokens->color)
                    if (auto* b = dynamic_cast<HdUERenderBuffer*>(renderBuffer))
                        (buffer ? extraBeauties_.push_back(b) : void(buffer = b));
                if (name == HdAovTokens->depth) depth = dynamic_cast<HdUERenderBuffer*>(renderBuffer);
                if (name == HdAovTokens->cameraDepth) cameraDepth = dynamic_cast<HdUERenderBuffer*>(renderBuffer);
                // Houdini's viewport draws its selection highlight from primId/instanceId. Rendering them swaps every
                // material twice per view (hundreds of ms on large scenes), so the viewport skips them while the user
                // tumbles, scrubs or plays and renders them once the view settles; hdUE:viewportIdAovs = 0 skips them
                // always (no highlight). husk renders what its render vars ask for.
                const bool viewportIds = s.setting<bool>("viewportIdAovs", true) && !s.interactive();
                const bool skipped = row && row->kind == HdUEAovKind::Id && selfHost().continuous && !viewportIds;
                // Buffer rows render together in one HDUE_AOV_BUFFERS pass; path traced, albedo and normal are the
                // Path Tracer's own buffers instead.
                if (auto* target = dynamic_cast<HdUERenderBuffer*>(renderBuffer); row && target && !skipped) {
                    if (row->kind == HdUEAovKind::Buffer && !(pathTraced && row->pathTraced))
                        bufferVars.push_back({target, row});
                    else
                        aovs.emplace_back(target, pathTraced && row->pathTraced ? row->pathTraced : row->aov);
                }
                // "No prim" everywhere, so Houdini never reads stale IDs.
                if (skipped)
                    if (auto* target = dynamic_cast<HdUERenderBuffer*>(renderBuffer)) clear(*target);
            }
            const bool denoising = s.setting<std::string>("renderMode", "realtime") == "pathtraced" &&
                                   TfStringToLower(s.setting<std::string>("denoiser", "off")) == "oidn";
            if (denoising && buffer && buffer->GetWidth()) {
                const auto guide = [&](std::unique_ptr<HdUERenderBuffer>& owned, uint32_t id, const char* path) {
                    if (std::any_of(aovs.begin(), aovs.end(), [&](const auto& aov) { return aov.second == id; }))
                        return;
                    if (!owned || owned->GetWidth() != buffer->GetWidth() ||
                        owned->GetHeight() != buffer->GetHeight()) {
                        owned = std::make_unique<HdUERenderBuffer>(SdfPath(path));
                        owned->Allocate(GfVec3i(int(buffer->GetWidth()), int(buffer->GetHeight()), 1),
                                        HdFormatFloat32Vec3, false);
                    }
                    aovs.emplace_back(owned.get(), id);
                };
                guide(guideAlbedo_, HDUE_AOV_ALBEDO, "/__hdUE/denoiseAlbedo");
                guide(guideNormal_, HDUE_AOV_NORMAL, "/__hdUE/denoiseNormal");
            }
            if (!bufferVars.empty()) aovs.emplace_back(bufferVars.front().target, HDUE_AOV_BUFFERS);
            // Cryptomatte: kCryptomatteSamples primId passes, each shifted by a sub-pixel offset of a 4 x 4 grid, as
            // MRQ's Object ID pass accumulates its spatial samples; their ids' share of each pixel is the coverage.
            if (!crypto.empty() && crypto.front().target->GetWidth()) {
                const auto* first = crypto.front().target;
                if (!cryptoIds_ || cryptoIds_->GetWidth() != first->GetWidth() ||
                    cryptoIds_->GetHeight() != first->GetHeight()) {
                    cryptoIds_ = std::make_unique<HdUERenderBuffer>(SdfPath("/__hdUE/cryptomatteIds"));
                    cryptoIds_->Allocate(GfVec3i(int(first->GetWidth()), int(first->GetHeight()), 1), HdFormatInt32,
                                         false);
                }
                aovs.insert(aovs.end(), kCryptomatteSamples, {cryptoIds_.get(), HDUE_AOV_PRIM_ID});
            }
            if (aovs != aovs_ || crypto != cryptoVars_ || bufferVars != bufferVars_) {
                if (crypto.empty()) s.locked([&] { s.cryptomatte.clear(); });  // no layer: no metadata
                cryptoVars_ = crypto;
                bufferVars_ = bufferVars;
                aovs_ = aovs;
                aovSlot_ = 0;
                aovInFlight_ = false;
                aovsDone_ = aovs_.empty();
            }
            if (!buffer && !aovs_.empty() && aovs_.front().first->GetWidth()) {
                const auto* first = aovs_.front().first;
                if (!internalColor_ || internalColor_->GetWidth() != first->GetWidth() ||
                    internalColor_->GetHeight() != first->GetHeight()) {
                    internalColor_ = std::make_unique<HdUERenderBuffer>(SdfPath("/__hdUE/beauty"));
                    internalColor_->Allocate(GfVec3i(int(first->GetWidth()), int(first->GetHeight()), 1),
                                             HdFormatFloat32Vec4, false);
                }
                buffer = internalColor_.get();
            }
            if (!buffer &&
                std::any_of(bound.begin(), bound.end(), [](const HdRenderBuffer* b) { return b && b->GetWidth(); })) {
                // Allocated outputs, none of them renderable: fail the frame instead of leaving husk waiting.
                std::string names;
                for (const auto& aov : pass->aovs) names += " " + aov.aovName.GetString();
                if (s.error.empty())
                    TF_RUNTIME_ERROR("hdUE renders none of the requested render vars:%s", names.c_str());
                s.setError("hdUE renders none of the requested render vars:" + names);
                converged_ = true;
                aovsDone_ = true;
                return;
            }
            if (!buffer || !buffer->GetWidth() || !buffer->GetHeight()) {
                std::string bindings;
                for (const auto& aov : pass->aovs)
                    bindings += " " + aov.aovName.GetString() + ":" + aov.renderBufferId.GetString();
                throw std::runtime_error("hdUE requires an allocated float32 RGBA color AOV; bindings:" + bindings +
                                         (buffer ? " allocation: " + buffer->allocationError() : " (missing buffer)"));
            }
            bool changed = s.locked([&] { return s.submittedRevision != s.revision; });
            // A new size keeps the session and its scene (leaving a camera in Houdini changes the size); recreating
            // it re-sent every mesh, material and texture.
            if (s.session && (s.width != buffer->GetWidth() || s.height != buffer->GetHeight())) {
                const auto resized =
                    s.backend->api().resize_session(s.session, buffer->GetWidth(), buffer->GetHeight());
                trace("hdUE resize %ux%u -> %ux%u: result %d\n", s.width, s.height, buffer->GetWidth(),
                      buffer->GetHeight(), int(resized));
                if (resized != HDUE_OK) goto recreate;
                s.width = buffer->GetWidth();
                s.height = buffer->GetHeight();
                s.lastFrame = {};
                lastFrame_ = 0;
                settingsVersion_ = ~uint64_t(0);  // render the new size from scratch
            }
        recreate:
            if (!s.session || s.width != buffer->GetWidth() || s.height != buffer->GetHeight()) {
                trace("hdUE create session %ux%u (had session %d)\n", buffer->GetWidth(), buffer->GetHeight(),
                      int(s.session != 0));
                if (s.session) s.backend->require(s.backend->api().destroy_session(s.session), "destroy_session");
                HdueSessionDesc desc{};
                desc.struct_size = sizeof(desc);
                desc.width = buffer->GetWidth();
                desc.height = buffer->GetHeight();
                s.backend->require(s.backend->api().create_session(&desc, &s.session), "create_session");
                s.width = desc.width;
                s.height = desc.height;
                s.firstBatch = true;
                s.geometryRevisions.clear();
                s.surfaceStatus = {sizeof(HdueSurfaceStatus)};
                depthEnabled_ = false;
#if defined(HDUE_HOUDINI)
                gpuOptions_ = {};
#endif
                for (const auto& [id, data] : s.meshes) s.dirtyMeshes.insert(id);
                for (const auto& [id, data] : s.materials) s.dirtyMaterials.insert(id);
                for (const auto& [id, data] : s.lights) s.dirtyLights.insert(id);
                changed = true;
            }
            if ((depth || cameraDepth) && !depthEnabled_) {
                s.backend->require(s.backend->api().set_depth_enabled(s.session, 1), "enable_depth");
                depthEnabled_ = true;
                changed = true;
            }
            if (changed) submit(s);
            {
                HdueAssetSceneStatus assets{};
                assets.struct_size = sizeof(assets);
                s.backend->require(s.backend->api().get_asset_scene_status(s.session, &assets), "asset_scene_status");
                const bool lightsChanged = assets.lights != s.submittedNativeLights;
                s.nativeAssets = assets;
                if (assets.loading) {
                    s.error.clear();
                    s.assetRequestPending = true;
                    converged_ = false;
                    return;
                }
                if (assets.failed) {
                    s.setError(assets.message);
                    s.assetRequestPending = true;
                    converged_ = true;
                    return;
                }
                if (lightsChanged) {
                    s.locked([&] { ++s.revision; });
                    submit(s);
                    changed = true;
                }
            }
            if (s.assetRequestPending) {
                changed = true;
                s.assetRequestPending = false;
            }
            const auto view = pass->view;
            const auto projection = pass->projection;
            const GfVec4f lens = pass->camera ? pass->lens : GfVec4f(0.f, 0.f, 0.f, 1.f);
            // Sub-frame times are spread over the camera shutter; the camera moves with its own samples.
            std::optional<HdUEMotionSamples> cameraMotion;
            if (pass->camera) {
                std::lock_guard cameraLock(s.cameraMutex);
                if (const auto it = s.cameraMotion.find(pass->cameraId.GetString()); it != s.cameraMotion.end())
                    cameraMotion = it->second;
            }
            const auto subframeTime = [&](int k) {
                return float(pass->shutterOpen + (k + .5) / subframes_ * (pass->shutterClose - pass->shutterOpen));
            };
            const auto viewAt = [&](float t) {
                if (subframes_ < 2 || !cameraMotion) return view;
                return view * cameraMotion->at(0) * cameraMotion->at(t).GetInverse();
            };
            const uint64_t settingsVersion = s.requestSettingsRevision;
            // Consume a completed image before changing its generation. Otherwise
            // continuous camera input can discard every image before presentation.
            HdueFrameInfo frame{};
            frame.struct_size = sizeof(frame);
            const auto result = s.backend->api().poll_frame(s.session, &frame);
            // The newest finished frame always shows, as in UE's own viewport: one for a previous camera, setting or
            // scene stays up until the current request's frames arrive, so the viewport never blanks or stalls. Only
            // frames of the current request count as converged.
            if (result == HDUE_OK && frame.generation <= s.generation && frame.frame_id != lastFrame_) {
                if (frame.generation != s.generation) frame.converged = 0;
                const auto copyStart = Clock::now();
                std::vector<float> pixels = buffer->takeSpare(size_t(frame.required_bytes / sizeof(float)));
                const auto copy =
                    s.backend->api().copy_frame(s.session, frame.frame_id, pixels.data(), frame.required_bytes);
                copyMs = ms(copyStart, Clock::now());
                traced = frame;
                std::vector<float> depthPixels;
                HdueResult depthCopy = HDUE_OK;
                if (copy == HDUE_OK && (depth || cameraDepth)) {
                    depthPixels = (depth ? depth : cameraDepth)->takeSpare(size_t(frame.width) * frame.height);
                    depthCopy = s.backend->api().copy_depth(s.session, frame.frame_id, depthPixels.data(),
                                                            depthPixels.size() * sizeof(float));
                }
                if (copy == HDUE_STALE_FRAME || depthCopy == HDUE_STALE_FRAME) {
                    // The Render Thread may publish another completed frame
                    // between poll and copy. Retry on the next Execute without
                    // blocking or publishing bytes from the wrong frame.
                    ++s.staleFrameRetries;
                } else {
                    s.backend->require(copy, "copy_frame");
                    if (depth || cameraDepth) {
                        s.backend->require(depthCopy, "copy_depth");
                        // cameraDepth is linear; so is depth for Houdini (UsdRenderers.json depthstyle linear), else
                        // clip-space [0,1].
                        std::vector<float> clip;
#if !defined(HDUE_HOUDINI)
                        if (depth) clip.resize(depthPixels.size());
#endif
                        WorkParallelForN(depthPixels.size(), [&](size_t begin, size_t end) {
                            for (size_t i = begin; i < end; ++i) {
                                float& value = depthPixels[i];
                                if (!clip.empty()) clip[i] = HdUEProjectDepth(value, projection_, centimetersPerUnit_);
                                value = value > 0 && std::isfinite(value) ? float(value / centimetersPerUnit_)
                                                                          : std::numeric_limits<float>::max();
                            }
                        });
                        auto& depthValues = clip.empty() ? depthPixels : clip;
                        if (cameraDepth) {
                            if (depth && &depthValues == &depthPixels)
                                cameraDepth->publish(frame, depthPixels, 1);
                            else
                                cameraDepth->publish(frame, std::move(depthPixels), 1);
                        }
                        if (depth) depth->publish(frame, std::move(depthValues), 1);
                    }
                    if (subframes_ > 1 && frame.converged) {
                        // One converged sub-frame: add it and publish the running average.
                        if (accum_.size() != pixels.size()) accum_.assign(pixels.size(), 0.f);
                        const float weight = 1.f / float(++subframe_);
                        WorkParallelForN(pixels.size(), [&](size_t begin, size_t end) {
                            for (size_t i = begin; i < end; ++i) {
                                accum_[i] += pixels[i];
                                pixels[i] = accum_[i] * weight;
                            }
                        });
                        subframeSamples_ += frame.completed_samples;
                        frame.completed_samples = subframeSamples_;
                        frame.converged = subframe_ >= subframes_;
                        subframeDue_ = !frame.converged;
                        if (frame.converged) applyMotion(s, 0);
                    } else if (subframes_ > 1)
                        lastFrame_ = frame.frame_id;  // an unfinished sub-frame stays out of the average
                    if (frame.frame_id != lastFrame_) {
                        if (lens_[3] != 1.f && s.exposed())  // camera exposure, as Karma applies it to the beauty only
                            for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
                                pixels[i] *= lens_[3];
                                pixels[i + 1] *= lens_[3];
                                pixels[i + 2] *= lens_[3];
                            }
                        if (denoising && frame.converged) {
                            beautyPixels_ = pixels;
                            beautyFrame_ = frame;
                            denoisePending_ = true;
                        }
                        const auto publishStart = Clock::now();
                        // UE renders linear Rec.709 (tone-mapped: display-linear); hosts expect the OCIO scene_linear
                        // role.
                        HdUEToSceneLinear(pixels);
                        for (auto* extra : extraBeauties_) extra->publish(frame, pixels, 4);
                        buffer->publish(frame, std::move(pixels), 4);
                        publishMs = ms(publishStart, Clock::now());
                        ++s.publishedFrames;
                        s.lastFrame = frame;
                        lastFrame_ = frame.frame_id;
                    }
                }
            } else if (result != HDUE_PENDING && result != HDUE_OK)
                s.backend->require(result, "poll_frame");
            if (aovInFlight_) {
                // AOV frames have their own slot (poll_aov); the beauty above is untouched.
                HdueFrameInfo aovFrame{};
                aovFrame.struct_size = sizeof(aovFrame);
                const auto polled = s.backend->api().poll_aov(s.session, &aovFrame);
                if (polled == HDUE_OK && aovFrame.generation == s.aovGeneration && aovFrame.converged &&
                    aovSlot_ < aovs_.size()) {
                    const auto aovStart = Clock::now();
                    std::vector<float> pixels(size_t(aovFrame.required_bytes / sizeof(float)));
                    const auto copied = s.backend->api().copy_frame(s.session, aovFrame.frame_id, pixels.data(),
                                                                    aovFrame.required_bytes);
                    aovMs = ms(aovStart, Clock::now());
                    if (copied == HDUE_STALE_FRAME)
                        ++s.staleFrameRetries;
                    else {
                        s.backend->require(copied, "copy_frame");
                        const uint32_t id = aovs_[aovSlot_].second;
                        if (aovs_[aovSlot_].first == cryptoIds_.get())
                            accumulateCryptomatte(s, aovFrame, pixels);
                        else if (id == HDUE_AOV_BUFFERS)
                            publishBuffers(s, aovFrame);
                        else
                            publishAov(*aovs_[aovSlot_].first,
                                       (id & 0xffu) == HDUE_AOV_LIGHTING                      ? HdUEAovFormat::Beauty
                                       : id == HDUE_AOV_ALBEDO                                ? HdUEAovFormat::Color
                                       : id == HDUE_AOV_NORMAL                                ? HdUEAovFormat::Vector
                                       : id == HDUE_AOV_PRIM_ID || id == HDUE_AOV_INSTANCE_ID ? HdUEAovFormat::Int
                                                                                              : HdUEAovFormat::Float,
                                       aovFrame, std::move(pixels));
                        aovInFlight_ = false;
                        ++aovSlot_;
                    }
                } else if (polled != HDUE_PENDING && polled != HDUE_OK)
                    s.backend->require(polled, "poll_aov");
            }
            converged_ = s.lastFrame.frame_id && s.lastFrame.converged;
            const bool settingsChanged = settingsVersion != settingsVersion_;
            const bool cameraChanged = view != view_ || projection != projection_ || lens != lens_;
            const bool needsRequest = changed || settingsChanged || cameraChanged;
            if (HdUETracePass()) {
                std::fprintf(stderr,
                             "hdUE pass changed=%d settings=%d camera=%d converged=%d frame=%llu gen=%llu buffer=%ux%u "
                             "aovs=%zu slot=%zu inflight=%d done=%d error=%s\n",
                             changed, settingsChanged, cameraChanged, int(converged_.load()),
                             (unsigned long long)s.lastFrame.frame_id, (unsigned long long)s.generation,
                             buffer->GetWidth(), buffer->GetHeight(), aovs_.size(), aovSlot_, aovInFlight_,
                             bool(aovsDone_), s.error.c_str());
                std::fflush(stderr);
            }
            // Batch hosts (husk) coalesce camera updates until a frame arrives; Houdini's viewport requests the newest
            // camera at once and keeps showing finished frames meanwhile, so navigation runs at UE's frame rate.
            const bool deferCamera = !selfHost().continuous && needsRequest && !changed && !settingsChanged &&
                                     s.generation && !s.lastFrame.frame_id;
            if (deferCamera) {
                // Read the newest matrices on the next Execute; never queue every
                // mouse event or invalidate an in-flight camera image repeatedly.
                ++s.coalescedCameraUpdates;
                converged_ = false;
            } else if (needsRequest || subframeDue_) {
                requested = true;
                lastRequestAt_ = Clock::now();
                const auto build = [&](const GfMatrix4d& view) {
                    HdueRenderRequest request{};
                    request.struct_size = sizeof(request);
                    request.generation = std::max(s.generation, s.aovGeneration) + 1;
                    request.mode = s.setting<std::string>("renderMode", "realtime") == "pathtraced" ? HDUE_PATH_TRACED
                                                                                                    : HDUE_REALTIME;
                    request.samples_per_pixel = uint32_t(s.samplesPerPixel());
                    request.warmup_frames = uint32_t(std::max(1, s.setting<int>("warmupFrames", 32)));
                    request.lumen_enabled = s.setting<bool>("lumenEnabled", true);
                    request.time_seconds = s.setting<double>("timeSeconds", 0);
                    const auto camera = view.GetInverse();
                    const auto eye = s.convert(camera.ExtractTranslation());
                    // Directions only, normalized: a camera under a scaled (e.g. 0.01) or sheared transform gave a
                    // target and up so short or skewed that the backend rejected the camera as degenerate.
                    const auto forward = s.convert(camera.TransformDir(GfVec3d(0, 0, -1)), true).GetNormalized();
                    auto up = s.convert(camera.TransformDir(GfVec3d(0, 1, 0)), true);
                    up = (up - forward * GfDot(up, forward)).GetNormalized();
                    const auto target = eye + forward * 100;  // 1 m ahead
                    for (int c = 0; c < 3; ++c) {
                        request.eye[c] = eye[c];
                        request.target[c] = target[c];
                        request.up[c] = up[c];
                    }
                    // The backend takes the projection from projection_scale; the field of view only feeds its range
                    // check and texture streaming, so a narrow crop or tile is not rejected as under 1 degree.
                    request.horizontal_fov_degrees = std::clamp(
                        float(2 * std::atan(1.0 / projection[0][0]) * 180 / 3.14159265358979323846), 1.f, 170.f);
                    if (HdUETraceScene())
                        TF_STATUS("hdUE camera %s eye=(%g,%g,%g) target=(%g,%g,%g) fov=%g size=%ux%u units=%g axis=%s",
                                  pass->camera ? pass->cameraId.GetText() : "none", eye[0], eye[1], eye[2], target[0],
                                  target[1], target[2], request.horizontal_fov_degrees, s.width, s.height,
                                  s.setting<double>("metersPerUnit", .01),
                                  s.setting<std::string>("upAxis", "Y").c_str());
                    const double cmPerUnit = s.setting<double>("metersPerUnit", 0.01) * 100;
                    request.near_clip_cm = pass->camera ? float(pass->nearClip * cmPerUnit) : 0.1f;
                    if (pass->camera && std::isfinite(pass->farClip) && pass->farClip > pass->nearClip)
                        request.far_clip_cm = float(pass->farClip * cmPerUnit);
                    // Hydra's projection already holds the conformed window and aperture offsets (GfFrustum layout).
                    if (projection[3][3] == 1) {
                        request.orthographic = 1;
                        request.projection_scale[0] = projection[0][0] / cmPerUnit;
                        request.projection_scale[1] = projection[1][1] / cmPerUnit;
                        request.projection_offset[0] = projection[3][0];
                        request.projection_offset[1] = projection[3][1];
                        request.near_clip_cm = float((projection[3][2] + 1) / projection[2][2] * cmPerUnit);
                        request.far_clip_cm = float((projection[3][2] - 1) / projection[2][2] * cmPerUnit);
                        request.horizontal_fov_degrees = 90;
                    } else {
                        request.projection_scale[0] = projection[0][0];
                        request.projection_scale[1] = projection[1][1];
                        request.projection_offset[0] = -projection[2][0];
                        request.projection_offset[1] = -projection[2][1];
                    }
                    // UsdRenderSettings disableDepthOfField renders every camera in focus.
                    if (lens[0] > 0 && lens[1] > 0 && !s.setting<bool>("disableDepthOfField", false)) {
                        request.fstop = lens[0];
                        request.focus_distance_cm = float(lens[1] * cmPerUnit);
                        request.focal_length_cm =
                            float(lens[2] * cmPerUnit);  // world units, as Hydra and Karma read USD lens attributes
                    }
                    return request;
                };
                if (needsRequest) {
                    // hdUE:postProcess:<FPostProcessSettings property>, hdUE:postProcessMaterials, hdUE:tonemapper, and
                    // hdUE:cvar:<console variable, ':' for '.'> (USD attribute names cannot hold dots), and
                    // hdUE:debugView. With the tonemapper on, the camera exposure goes in before it rather than onto
                    // display values.
                    std::vector<std::string> names, values, materials;
                    const bool tonemapper = s.toneMapped();  // debug views are drawn for display, as in UE's viewport
                    // The tone-mapped beauty is display-linear. It is still converted to scene_linear's primaries
                    // (only a matrix), so an un-tone-mapped view shows UE's image in any config; ACES or AgX views
                    // would apply a second tone curve.
                    static std::once_flag displayReferred;
                    if (s.setting<bool>("tonemapper", false))
                        std::call_once(displayReferred, [] {
                            TF_WARN(
                                "hdUE: with the Unreal tonemapper on, the beauty is UE's display-referred image; "
                                "view it with an un-tone-mapped view (not ACES or AgX, which tone-map it again)");
                        });
                    const auto settings = s.settingsSnapshot();
                    for (const auto& [key, value] : settings) {
                        if (TfStringStartsWith(key.GetString(), "postProcess:"))
                            names.push_back(key.GetString().substr(12));
                        else if (TfStringStartsWith(key.GetString(), "cvar:"))
                            names.push_back(TfStringReplace(key.GetString().substr(5), ":", "."));
                        else if (key == TfToken("debugView"))
                            names.push_back("DebugView");
                        else
                            continue;
                        if (value.IsHolding<SdfAssetPath>()) {  // texture settings: an image file or a cooked path
                            const auto& asset = value.UncheckedGet<SdfAssetPath>();
                            values.push_back(asset.GetResolvedPath().empty() ? asset.GetAssetPath()
                                                                             : asset.GetResolvedPath());
                        } else
                            values.push_back(value.IsHolding<bool>() ? (value.UncheckedGet<bool>() ? "True" : "False")
                                                                     : TfStringify(value));
                    }
                    if (tonemapper) {
                        // UE's own physical camera exposure (ISO, shutter, aperture) stays off unless the scene asks
                        // for it.
                        if (std::find(names.begin(), names.end(), "AutoExposureApplyPhysicalCameraExposure") ==
                            names.end()) {
                            names.push_back("AutoExposureApplyPhysicalCameraExposure");
                            values.push_back("False");
                        }
                    }
                    const auto list = settings.find(TfToken("postProcessMaterials"));
                    if (list != settings.end()) {
                        if (list->second.IsHolding<VtStringArray>())
                            for (const auto& m : list->second.UncheckedGet<VtStringArray>()) materials.push_back(m);
                        else if (list->second.IsHolding<VtTokenArray>())
                            for (const auto& m : list->second.UncheckedGet<VtTokenArray>())
                                materials.push_back(m.GetString());
                        else
                            for (const auto& m :
                                 TfStringTokenize(s.setting<std::string>("postProcessMaterials", ""), " ,;"))
                                materials.push_back(m);
                    }
                    std::vector<const char*> namePtrs, valuePtrs, materialPtrs;
                    for (size_t i = 0; i < names.size(); ++i) {
                        namePtrs.push_back(names[i].c_str());
                        valuePtrs.push_back(values[i].c_str());
                    }
                    for (const auto& m : materials) materialPtrs.push_back(m.c_str());
                    const HduePostProcessDesc desc{sizeof(HduePostProcessDesc),
                                                   namePtrs.data(),
                                                   valuePtrs.data(),
                                                   uint32_t(names.size()),
                                                   materialPtrs.data(),
                                                   uint32_t(materials.size()),
                                                   tonemapper ? 1 : 0,
                                                   tonemapper
                                                       ? std::log2(std::max(lens[3], 1e-6f))
                                                       : 0.f};  // the USD camera exposure adds to AutoExposureBias
                    s.backend->require(s.backend->api().set_post_process(s.session, &desc), "set_post_process");
                }
                if (needsRequest) {
                    applyMotion(s, 0);
                    // Motion blur needs a shutter, sub-frames and something moving.
                    const int samples = std::max(1, s.setting<int>("motionSamples", 16));
                    // UsdRenderSettings disableMotionBlur (and the deprecated instantaneousShutter) close the shutter.
                    const bool shutter = pass->camera && pass->shutterClose > pass->shutterOpen &&
                                         !s.setting<bool>("disableMotionBlur", false) &&
                                         !s.setting<bool>("instantaneousShutter", false);
                    const bool moving = cameraMotion.has_value() || s.locked([&] {
                        return std::any_of(s.meshes.begin(), s.meshes.end(),
                                           [](const auto& entry) { return entry.second.moving(); });
                    });
                    subframes_ = s.setting<bool>("motionBlur", true) && shutter && moving && samples > 1 ? samples : 1;
                    subframe_ = 0;
                    subframeSamples_ = 0;
                    accum_.clear();
                }
                subframeDue_ = false;
                auto request = build(viewAt(subframes_ > 1 ? subframeTime(subframe_) : 0.f));
                if (subframes_ > 1) {
                    applyMotion(s, subframeTime(subframe_));
                    request.samples_per_pixel =
                        std::max(1u, (request.samples_per_pixel + uint32_t(subframes_) - 1) / uint32_t(subframes_));
                    // Lumen keeps its history between sub-frames, so the realtime warmup is shared too.
                    request.warmup_frames =
                        std::max(2u, (request.warmup_frames + uint32_t(subframes_) - 1) / uint32_t(subframes_));
                }
                const auto requestStart = std::chrono::steady_clock::now();
                const auto requested = s.backend->api().request_render(s.session, &request);
                if (const double requestMs = ms(requestStart, Clock::now()); requestMs > 50)
                    trace("hdUE slow request_render: %.1f ms (result %d)\n", requestMs, int(requested));
                if (requested == HDUE_PENDING) {
                    trace("hdUE request pending (UE not ready)\n");
                    if (!needsRequest) {
                        subframeDue_ = true;
                        converged_ = false;
                        s.error.clear();
                        return;
                    }  // retry the sub-frame
                    s.assetRequestPending = true;
                    converged_ = false;
                    s.lastFrame = {};
                    buffer->invalidate();
                    for (auto* extra : extraBeauties_) extra->invalidate();
                    for (auto* d : {depth, cameraDepth})
                        if (d) d->invalidate();
                    s.error.clear();
                    return;
                }
                s.backend->require(requested, "request_render");
                s.generation = request.generation;
                s.requestCpuMsTotal +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requestStart).count();
                converged_ = false;
                if (needsRequest) {
                    view_ = view;
                    projection_ = projection;
                    lens_ = lens;
                    settingsVersion_ = settingsVersion;
                    s.submittedRequestSettingsRevision = settingsVersion;
                    centimetersPerUnit_ = s.setting<double>("metersPerUnit", .01) * 100.;
                    s.lastFrame = {};
                    buffer->invalidate();
                    for (auto* extra : extraBeauties_) extra->invalidate();
                    for (auto* d : {depth, cameraDepth})
                        if (d) d->invalidate();
                    beautyRequest_ = build(view);  // AOVs render the unblurred pose at the current time
                    aovSlot_ = 0;
                    aovInFlight_ = false;
                    aovsDone_ = aovs_.empty();
                    denoisePending_ = false;
                    for (const auto& [aovBuffer, id] : aovs_) aovBuffer->invalidate();
                    for (const auto& var : cryptoVars_) var.target->invalidate();
                    for (const auto& var : bufferVars_) var.target->invalidate();
                }
            }
            // In Houdini's UI the AOV passes (Solaris' primId/instanceId swap every material, ~300 ms each) wait until
            // the camera has been still for 1 s, so navigation never pays for them.
            const bool still =
                !selfHost().continuous || Clock::now() - lastRequestAt_ > std::chrono::milliseconds(1000);
            // While Houdini tumbles, scrubs or plays, the beauty alone counts: the AOVs render once it is back to
            // normal (a houdini:interactive change is a new request, which restarts them).
            if (!needsRequest && converged_ && !aovInFlight_ && s.interactive()) aovsDone_ = true;
            if (!needsRequest && converged_ && !aovsDone_ && !aovInFlight_ && still) {
                if (aovSlot_ >= aovs_.size())
                    aovsDone_ = true;
                else {
                    // Same camera and time as the beauty; GBuffer channels converge after a couple of draws.
                    auto request = beautyRequest_;
                    request.generation = std::max(s.generation, s.aovGeneration) + 1;
                    request.aov = aovs_[aovSlot_].second;
                    // A lighting component renders as the beauty. Path-traced albedo and normal come from the Path
                    // Tracer's own buffers of the converged beauty (ABI 11); everything else is a raster pass.
                    if ((request.aov & 0xffu) != HDUE_AOV_LIGHTING) {
                        if (request.mode != HDUE_PATH_TRACED ||
                            (request.aov != HDUE_AOV_ALBEDO && request.aov != HDUE_AOV_NORMAL))
                            request.mode = HDUE_REALTIME;
                        request.lumen_enabled = 0;
                        request.warmup_frames = 2;
                    }
                    if (request.aov == HDUE_AOV_BUFFERS) {
                        std::vector<const char*> names;
                        for (const auto& var : bufferVars_)
                            if (std::find(names.begin(), names.end(), var.row->buffer) == names.end())
                                names.push_back(var.row->buffer);
                        s.backend->require(
                            s.backend->api().set_aov_buffers(s.session, names.data(), uint32_t(names.size())),
                            "set_buffers");
                    }
                    if (aovs_[aovSlot_].first == cryptoIds_.get()) {
                        // The sample's cell centre in a 4 x 4 grid over the pixel; NDC spans 2 per image size.
                        static_assert(kCryptomatteSamples == 16);
                        const size_t sample = cryptoSample();
                        request.projection_offset[0] += ((sample % 4 + .5) / 4 - .5) * 2 / s.width;
                        request.projection_offset[1] += ((sample / 4 + .5) / 4 - .5) * 2 / s.height;
                    }
                    const auto requested = s.backend->api().request_render(s.session, &request);
                    if (requested != HDUE_PENDING) {
                        s.backend->require(requested, "request_render");
                        s.aovGeneration = request.generation;
                        aovInFlight_ = true;
                    }
                }
            }
            if (denoisePending_ && aovsDone_) {
                denoise(*buffer);
                denoisePending_ = false;
            }
#if defined(HDUE_HOUDINI)
            const auto gpuStart = Clock::now();
            syncGpuFrames(s, *buffer, depth, denoising);
            if (const double gpuMs = ms(gpuStart, Clock::now()); gpuMs > 50)
                trace("hdUE slow GPU frame sync: %.1f ms\n", gpuMs);
#endif
            {
                std::lock_guard lock(s.mutex);
                s.error.clear();
            }
            const double total = ms(start, Clock::now());
            if (timing && (traced.frame_id || requested || aovMs > 0 || total > 5)) {
                trace(
                    "hdUE timing: execute %.1f ms | request %d | frame %llu %ux%u converged %u (UE request->frame %.1f "
                    "ms, readback %.1f ms) "
                    "copy %.1f ms publish %.1f ms | aov slot %zu/%zu copy+publish %.1f ms\n",
                    total, int(requested), (unsigned long long)traced.frame_id, traced.width, traced.height,
                    traced.converged, traced.request_to_frame_ms, traced.readback_ms, copyMs, publishMs, aovSlot_,
                    aovs_.size(), aovMs);
                std::fflush(stderr);
            }
        } catch (const std::exception& error) {
            if (HdUETracePass()) {
                std::fprintf(stderr, "hdUE pass error: %s\n", error.what());
                std::fflush(stderr);
            }
            std::lock_guard lock(state_->mutex);
            state_->error = error.what();
            converged_ = false;
        }
    }
};
}  // namespace

// The hdUE setting a host's key stands for: hdUE:<name>, or hdue:<name> as lights and prims spell the namespace (USD
// namespaces are case-sensitive), and the standard names hosts send: Hydra's enableSceneLights (usdview's "Enable Scene
// Lights"), stageMetersPerUnit (husk, Houdini) and stageUpAxis (the hdUE host in Houdini's viewport). namespaced: the
// key was hdUE:<name>, which the bare <name> never overrides (Houdini sends its own renderMode: batch, interactive).
static TfToken HdUECanonicalKey(const TfToken& key, bool& namespaced) {
    const auto& name = key.GetString();
    namespaced = name.size() > 5 && (name.starts_with("hdUE:") || name.starts_with("hdue:"));
    if (namespaced) return TfToken(name.substr(5));
    if (key == HdRenderSettingsTokens->enableSceneLights) return TfToken("sceneLightsEnabled");
    if (key == TfToken("stageMetersPerUnit")) return TfToken("metersPerUnit");
    if (key == TfToken("stageUpAxis")) return TfToken("upAxis");
    return key;
}
HdUERenderDelegate::HdUERenderDelegate(const HdRenderSettingsMap& settings)
    : HdRenderDelegate(settings),
      state_(std::make_shared<HdUEState>()),
      registry_(std::make_shared<HdResourceRegistry>()) {
    if (processName() == L"husk.exe") static HdUEBatchWarnings warnings;
    // Before the backend DLL loads: UE changes the working directory while it loads and starts.
    const auto hostDirectory = std::filesystem::current_path();
    state_->settings = settings;
    // As SetRenderSetting does: aliases, then hdUE:<name> over both.
    for (const bool namespacedPass : {false, true})
        for (const auto& [key, value] : settings) {
            if (HdUETraceSettings() && !namespacedPass)
                std::fprintf(stderr, "hdUE initial setting %s = %s\n", key.GetText(), TfStringify(value).c_str());
            bool namespaced = false;
            const auto canonical = HdUECanonicalKey(key, namespaced);
            if (canonical == key || namespaced != namespacedPass) continue;
            if (namespaced) state_->namespaced.insert(canonical);
            state_->settings[canonical] = value;
        }
    state_->wetBindings = std::make_shared<const VtDictionary>(state_->setting<VtDictionary>("surfaceEffects", {}));
    const auto stageUnits = settings.find(TfToken("stageMetersPerUnit"));
    const bool hostUnits = stageUnits != settings.end() && !VtValue::Cast<double>(stageUnits->second).IsEmpty();
    // husk names its USD file (usdFilename) but, unlike stageMetersPerUnit, passes no up axis: read the root layer's
    // (already open in husk). Houdini's viewport sends stageUpAxis from the hdUE host. hdUE:upAxis overrides both.
    if (const auto file = settings.find(TfToken("usdFilename"));
        file != settings.end() && file->second.IsHolding<std::string>() &&
        !state_->namespaced.count(TfToken("upAxis")) && !settings.count(TfToken("stageUpAxis")))
        if (const auto path =
                (hostDirectory / std::filesystem::u8path(file->second.UncheckedGet<std::string>())).u8string();
            const auto layer = SdfLayer::FindOrOpen(std::string(path.begin(), path.end()))) {
            VtValue axis;
            if (layer->HasField(SdfPath::AbsoluteRootPath(), TfToken("upAxis"), &axis) && axis.IsHolding<TfToken>())
                state_->settings[TfToken("upAxis")] = VtValue(axis.UncheckedGet<TfToken>().GetString());
            // husk passes no stage units either: without them a metre-scale stage reached UE at 1 cm per unit, too
            // small for Lumen's surface cache (no cards on a 5 cm room), which left only screen traces for indirect
            // light.
            if (VtValue units; !hostUnits && !state_->namespaced.count(TfToken("metersPerUnit")) &&
                               layer->HasField(SdfPath::AbsoluteRootPath(), TfToken("metersPerUnit"), &units) &&
                               !VtValue::Cast<double>(units).IsEmpty())
                state_->settings[TfToken("metersPerUnit")] = VtValue::Cast<double>(units);
            if (HdUETraceSettings())
                std::fprintf(stderr, "hdUE stage up axis of %s: %s\n", layer->GetIdentifier().c_str(),
                             TfStringify(axis).c_str());
        }
    auto dll = state_->setting<std::string>("backendLibrary", TfGetenv("HDUE_BACKEND_DLL"));
    if (dll.empty()) dll = runtimeConfig("backend_dll");
    if (dll.empty()) throw std::runtime_error("hdUE requires backendLibrary, HDUE_BACKEND_DLL or HDUE_RUNTIME_CONFIG");
    state_->backend = std::make_shared<hdue::Backend>(std::filesystem::u8path(dll));
    auto& host = selfHost();
    const auto rendererInfo = [state = state_] {
        state->backend->require(state->backend->api().get_renderer_info(&state->rendererInfo), "get_renderer_info");
    };
    if (host.worker.joinable()) {
        // UE runs (or boots) on hdUE's thread.
        if (const auto error = host.error(); !error.empty()) throw std::runtime_error(error);
        ++host.delegates;
        selfHosted_ = true;
        if (host.continuous)
            host.post(rendererInfo);
        else
            host.run(rendererInfo);
    } else {
        HdueStatus status{};
        status.struct_size = sizeof(status);
        ueRun([&] { state_->backend->require(state_->backend->api().get_status(&status), "get_status"); });
        if (status.state == HDUE_STATE_COLD) {
            // This process owns UE. Houdini's UI does not wait for the boot: Execute's work queues behind it, and
            // the viewport shows Hydra's progress until frames arrive.
            host.directory = hostDirectory;
            const bool wait = !houdiniUi();
            startSelfHost(dll, state_->setting<std::string>("runtimeProject", runtimeConfig("runtime_project")), wait);
            std::error_code ignored;
            std::filesystem::current_path(host.directory, ignored);
            ++host.delegates;
            selfHosted_ = true;
            if (host.continuous)
                host.post(rendererInfo);
            else
                host.run(rendererInfo);
        } else {
            // A host (usdview, or a test) started UE and ticks it.
            if (status.state != HDUE_STATE_READY || !status.world_ready || !status.rhi_ready)
                throw std::runtime_error(
                    "hdUE requires a ready runtime; launch usdview with scripts/Launch-Usdview.ps1");
            ueRun(rendererInfo);
        }
    }
#if defined(HDUE_HOUDINI)
    // Houdini's viewport delegate registers for the hdUE host (diagnostics.cpp). With background scene updates Houdini
    // creates it on a worker thread, so the creating thread no longer identifies it: houdini:viewport does
    // (SetRenderSetting).
    HdUERegisterViewport(state_, this);
#endif
    for (const auto& desc : GetRenderSettingDescriptors()) {
        if (!state_->settings.count(desc.key)) state_->settings[desc.key] = desc.defaultValue;
        HdRenderDelegate::SetRenderSetting(desc.key, state_->settings[desc.key]);
    }
    // Cooked MaterialX and prepared Substrate libraries are applied by the hdUE host in Houdini's UI (a Python stage
    // walk); batch renders without that host (husk) keep the Preview Surface fallback.
    if (selfHosted_ && !host.continuous)
        for (const char* library : {"materialxLibrary", "substrateLibrary"})
            if (!state_->setting<std::string>(library, "").empty())
                batchWarning(std::string("hdUE:") + library +
                             " is set, but husk renders MaterialX and Substrate Builder materials with their Preview "
                             "Surface fallback: cooked libraries apply in Houdini's viewport only.");
}
HdUERenderDelegate::~HdUERenderDelegate() {
#if defined(HDUE_HOUDINI)
    HdUEUnregisterViewport(this);
    if (const auto frames = state_->locked([&] { return state_->gpuFrames; }))
        frames->release();  // GL objects, on Houdini's GL thread
#endif
    auto& host = selfHost();
    if (!selfHosted_) {
        try {
            ueRun([&] {
                std::lock_guard lock(state_->mutex);
                if (state_->session) {
                    state_->backend->api().destroy_session(state_->session);
                    state_->session = 0;
                }
            });
        } catch (const std::exception& e) {
            TF_RUNTIME_ERROR("hdUE: %s", e.what());
        }
        return;
    }
    if (selfHosted_ && state_->paused) --host.paused;
    --host.delegates;
    // UE stays for the next delegate (one UE lifetime per process; husk --restart-delegate, Houdini's next viewport)
    // and ends at process exit or hdue_shutdown_ue. A paused (deactivated) delegate can outlive that shutdown: its
    // session ended with UE.
    if (!host.worker.joinable()) return;
    host.run([&] {
        std::lock_guard lock(state_->mutex);
        if (state_->session) {
            state_->backend->api().destroy_session(state_->session);
            state_->session = 0;
        }
    });
    // husk may write its images after releasing the delegate.
    std::error_code ignored;
    std::filesystem::current_path(host.directory, ignored);
}
const TfTokenVector& HdUERenderDelegate::GetSupportedRprimTypes() const {
#if defined(HDUE_HOUDINI)
    static TfTokenVector t{HdPrimTypeTokens->mesh, HdPrimTypeTokens->points, HdPrimTypeTokens->basisCurves,
                           HdPrimTypeTokens->volume};
#else
    static TfTokenVector t{HdPrimTypeTokens->mesh, HdPrimTypeTokens->points, HdPrimTypeTokens->basisCurves};
#endif
    return t;
}
const TfTokenVector& HdUERenderDelegate::GetSupportedSprimTypes() const {
    static TfTokenVector t{
        HdPrimTypeTokens->camera,       HdPrimTypeTokens->material,    HdPrimTypeTokens->extComputation,
        HdPrimTypeTokens->distantLight, HdPrimTypeTokens->sphereLight, HdPrimTypeTokens->rectLight,
        HdPrimTypeTokens->domeLight,    HdPrimTypeTokens->diskLight,   HdPrimTypeTokens->cylinderLight,
        HdPrimTypeTokens->meshLight,    HdPrimTypeTokens->pluginLight, TfToken("portalLight"),
        HdPrimTypeTokens->light};
    return t;
}
const TfTokenVector& HdUERenderDelegate::GetSupportedBprimTypes() const {
#if defined(HDUE_HOUDINI)
    static TfTokenVector t{HdPrimTypeTokens->renderBuffer, TfToken("openvdbAsset"), TfToken("houdiniFieldAsset")};
#else
    static TfTokenVector t{HdPrimTypeTokens->renderBuffer};
#endif
    return t;
}
HdRenderPassSharedPtr HdUERenderDelegate::CreateRenderPass(HdRenderIndex* index, const HdRprimCollection& collection) {
    state_->renderIndex = index;
    return std::make_shared<Pass>(index, collection, state_);
}
HdInstancer* HdUERenderDelegate::CreateInstancer(HdSceneDelegate* delegate, const SdfPath& id) {
    return HdUECreateInstancer(delegate, id);
}
void HdUERenderDelegate::DestroyInstancer(HdInstancer* p) { delete p; }
HdRprim* HdUERenderDelegate::CreateRprim(const TfToken& type, const SdfPath& id) {
    if (type == HdPrimTypeTokens->points) return HdUECreatePoints(id);
    if (type == HdPrimTypeTokens->basisCurves) return HdUECreateCurves(id);
#if defined(HDUE_HOUDINI)
    if (type == HdPrimTypeTokens->volume) return HdUECreateVolume(id);
#endif
    return type == HdPrimTypeTokens->mesh ? HdUECreateMesh(id) : nullptr;
}
void HdUERenderDelegate::DestroyRprim(HdRprim* p) { delete p; }
HdSprim* HdUERenderDelegate::CreateSprim(const TfToken& type, const SdfPath& id) {
    if (type == HdPrimTypeTokens->camera) return HdUECreateCamera(id);
    if (type == HdPrimTypeTokens->material) return HdUECreateMaterial(id);
    if (type == HdPrimTypeTokens->extComputation)
        return new HdExtComputation(id);  // evaluated by the rprims reading it
    if (type == HdPrimTypeTokens->distantLight) return HdUECreateLight(id, HDUE_DISTANT_LIGHT);
    if (type == HdPrimTypeTokens->sphereLight) return HdUECreateLight(id, HDUE_SPHERE_LIGHT);
    if (type == HdPrimTypeTokens->rectLight) return HdUECreateLight(id, HDUE_RECT_LIGHT);
    if (type == HdPrimTypeTokens->domeLight) return HdUECreateLight(id, HDUE_DOME_LIGHT);
    if (type == HdPrimTypeTokens->diskLight) return HdUECreateLight(id, HDUE_DISK_LIGHT);
    if (type == HdPrimTypeTokens->cylinderLight) return HdUECreateLight(id, HDUE_CYLINDER_LIGHT);
    if (type == HdPrimTypeTokens->meshLight || type == HdPrimTypeTokens->pluginLight || type == TfToken("portalLight"))
        return HdUECreateUnsupportedLight(id, type);
    if (type == HdPrimTypeTokens->light)  // what UsdImaging makes of a UsdLuxGeometryLight
        return HdUECreateUnsupportedLight(id, TfToken("GeometryLight"));
    return nullptr;
}
HdSprim* HdUERenderDelegate::CreateFallbackSprim(const TfToken& type) {
    return CreateSprim(type, SdfPath::EmptyPath());
}
void HdUERenderDelegate::DestroySprim(HdSprim* p) { delete p; }
HdBprim* HdUERenderDelegate::CreateBprim(const TfToken& type, const SdfPath& id) {
#if defined(HDUE_HOUDINI)
    if (type == TfToken("openvdbAsset") || type == TfToken("houdiniFieldAsset")) return HdUECreateField(id);
#endif
    return type == HdPrimTypeTokens->renderBuffer ? new HdUERenderBuffer(id) : nullptr;
}
HdBprim* HdUERenderDelegate::CreateFallbackBprim(const TfToken& type) {
    return CreateBprim(type, SdfPath::EmptyPath());
}
void HdUERenderDelegate::DestroyBprim(HdBprim* p) {
    // Render buffers die on the UE thread, after any Execute that is using them (PassSnapshot).
    if (dynamic_cast<HdUERenderBuffer*>(p) && selfHost().worker.joinable()) {
        selfHost().post([p] { delete p; });
        return;
    }
    delete p;
}
void HdUERenderDelegate::CommitResources(HdChangeTracker*) {}
void HdUERenderDelegate::SetDrivers(const HdDriverVector& drivers) {
    for (const auto* driver : drivers) {
        std::string kind = driver->driver.GetTypeName();
        if (driver->driver.IsHolding<Hgi*>())
            if (auto* hgi = driver->driver.UncheckedGet<Hgi*>()) kind += " " + hgi->GetAPIName().GetString();
        trace("hdUE driver %s: %s\n", driver->name.GetText(), kind.c_str());
#if defined(HDUE_HOUDINI)
        // Houdini's GL viewport composites UE's frames from shared GPU textures (gpu_frames.h); HDUE_GPU_FRAMES=0
        // keeps the CPU buffers.
        if (driver->driver.IsHolding<Hgi*>())
            if (auto* hgi = driver->driver.UncheckedGet<Hgi*>(); hgi && hgi->GetAPIName() == HgiTokens->OpenGL &&
                                                                 selfHost().continuous &&
                                                                 TfGetenvBool("HDUE_GPU_FRAMES", true))
                state_->locked([&] {
                    if (!state_->gpuFrames) state_->gpuFrames = std::make_shared<HdUEGpuFrames>(hgi, state_->backend);
                });
#endif
    }
}
bool HdUERenderDelegate::Pause() { return setPaused(true); }
bool HdUERenderDelegate::Resume() { return setPaused(false); }
// Stopping is pausing: UE keeps its scene, and Restart renders on from it (usdview's stop button).
bool HdUERenderDelegate::IsStopped() const { return state_->paused; }
bool HdUERenderDelegate::Stop(bool) { return setPaused(true); }
bool HdUERenderDelegate::Restart() { return setPaused(false); }
bool HdUERenderDelegate::setPaused(bool paused) {
    trace("hdUE %s (was %s)\n", paused ? "pause" : "resume", state_->paused ? "paused" : "running");
    if (state_->paused.exchange(paused) == paused) return true;
    auto& host = selfHost();
    if (selfHosted_) host.paused += paused ? 1 : -1;
    host.queueReady.notify_one();  // the UE thread re-evaluates whether to tick
    return true;
}
HdAovDescriptor HdUERenderDelegate::GetDefaultAovDescriptor(const TfToken& name) const {
    // Half float: UE renders in it, and Houdini's viewport uploads the CPU colour buffer on every redraw (a 2.5 MP
    // float32 image cost its UI thread ~100 ms per frame).
    // Viewports drop AOVs without a descriptor, so the beauty's other names need one too.
    if (HdUEBeauty(name) == HdAovTokens->color) return HdAovDescriptor(HdFormatFloat16Vec4, false, VtValue(GfVec4f(0)));
    const auto* row = HdUEFindAov(name.GetString());
    if (row && row->format == HdUEAovFormat::Int) return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    if (HdUECryptomatteLayer(name.GetString()).first >= 0)  // ids are float32 bit patterns: never half
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0)));
    if (row && row->format == HdUEAovFormat::Beauty)
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0)));
    if (row)
        return row->format == HdUEAovFormat::Float ? HdAovDescriptor(HdFormatFloat32, false, VtValue(0.f))
                                                   : HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0)));
#if defined(HDUE_HOUDINI)
    if (name == HdAovTokens->depth)
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(std::numeric_limits<float>::max()));
#else
    if (name == HdAovTokens->depth) return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.f));
#endif
    if (name == HdAovTokens->cameraDepth)
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(std::numeric_limits<float>::max()));
    return HdAovDescriptor();
}
// UsdRenderSettings materialBindingPurposes (husk passes it; Hydra resolves one purpose, then allPurpose), else full
// as a final-quality renderer, as Karma (Hydra's default is preview).
TfToken HdUERenderDelegate::GetMaterialBindingPurpose() const {
    const auto purposes = state_->setting<VtTokenArray>("materialBindingPurposes", {});
    return purposes.empty() ? HdTokens->full : purposes[0];
}
// Every scalar setting hdUE reads, with the default its readers use (hosts such as usdview build their UI from these).
// Prefixed families (postProcess:*, cvar:*) and dictionaries set by the Houdini host are not listed.
HdRenderSettingDescriptorList HdUERenderDelegate::GetRenderSettingDescriptors() const {
    return {{"Render mode", TfToken("renderMode"), VtValue(std::string("realtime"))},
            {"Samples per pixel", TfToken("samplesPerPixel"), VtValue(64)},
            {"Warmup frames", TfToken("warmupFrames"), VtValue(32)},
            {"Lumen", TfToken("lumenEnabled"), VtValue(true)},
            {"Viewer light intensity", TfToken("viewerLightIntensity"), VtValue(4.f)},
            {"Environment cube resolution", TfToken("domeResolution"), VtValue(512)},
            {"Scene lights", TfToken("sceneLightsEnabled"), VtValue(true)},
            {"Viewer light without scene lights", TfToken("autoViewerLight"), VtValue(false)},
            {"Motion blur", TfToken("motionBlur"), VtValue(true)},
            {"Motion blur samples", TfToken("motionSamples"), VtValue(16)},
            {"Subdivision level", TfToken("subdivisionLevel"), VtValue(2)},
            {"Tonemapper", TfToken("tonemapper"), VtValue(false)},
            {"Denoiser", TfToken("denoiser"), VtValue(std::string("off"))},
            {"Debug view", TfToken("debugView"), VtValue(std::string("off"))},
            {"Require Nanite", TfToken("requireNanite"), VtValue(false)},
            {"Cull Backface", TfToken("cullBackface"), VtValue(true)}};
}
namespace {
// Ids whose entry differs between two id-keyed setting dictionaries (an unset setting is an empty one) and that name
// a synced prim: only those need uploading again.
template <class Prims>
void dirtyChangedEntries(const VtDictionary& before, const VtDictionary& after, const Prims& prims,
                         std::set<std::string>& dirty) {
    const auto changed = [&](const VtDictionary& from, const VtDictionary& to) {
        for (const auto& [id, entry] : from)
            if (prims.count(id) && (!to.count(id) || to.find(id)->second != entry)) dirty.insert(id);
    };
    changed(before, after);
    changed(after, before);
}
}  // namespace
void HdUERenderDelegate::SetRenderSetting(const TfToken& key, const VtValue& value) {
    SetRenderSetting(key, value, false);
}
void HdUERenderDelegate::SetRenderSetting(const TfToken& key, const VtValue& value, bool fromNamespace) {
    if (HdUETraceSettings()) {
        std::fprintf(stderr, "hdUE setting %s = %s\n", key.GetText(), TfStringify(value).c_str());
        std::fflush(stderr);
    }
    bool namespaced = false;
    if (const auto canonical = HdUECanonicalKey(key, namespaced); canonical != key) {
        if (namespaced) {
            std::lock_guard lock(state_->settingsMutex);
            state_->namespaced.insert(canonical);
        }
        SetRenderSetting(canonical, value, namespaced);
        return;
    }
    if (!fromNamespace) {
        std::lock_guard lock(state_->settingsMutex);
        if (state_->namespaced.count(key)) return;
    }
#if defined(HDUE_HOUDINI)
    if (key == TfToken("houdini:viewport") && value.IsHolding<bool>() && value.UncheckedGet<bool>())
        HdUERegisterViewport(state_, this);
#endif
    if (key == TfToken("houdini:render_pause")) {
        setPaused(value.IsHolding<bool>() ? value.UncheckedGet<bool>()
                                          : value.IsHolding<int>() && value.UncheckedGet<int>());
        return;
    }
    // Houdini repeats settings on redraws: an unchanged value returns without the state lock (held while an upload is
    // prepared). The state lock is taken first, as everywhere else (setting() takes settingsMutex inside it). Houdini
    // sends an empty value for a setting no longer authored (a parameter back at its default, a blocked attribute):
    // that removes it, so its default applies again (UE's own for postProcess:*, the variable's for cvar:*).
    const auto unchanged = [&] {
        const auto old = state_->settings.find(key);
        return old == state_->settings.end() ? value.IsEmpty() : old->second == value;
    };
    {
        std::lock_guard settingsLock(state_->settingsMutex);
        if (unchanged()) return;
    }
    std::lock_guard lock(state_->mutex);
    std::lock_guard settingsLock(state_->settingsMutex);
    if (unchanged()) return;
    HdRenderDelegate::SetRenderSetting(key, value);
    trace("hdUE setting changed: %s\n", key.GetText());
    // Settings that re-upload prims compare against their effective previous value (unset = the default), so the first
    // value a host sends does not re-upload a scene it already matches.
    const auto previousDictionary = state_->setting<VtDictionary>(key.GetText(), {});
    const double previousUnits = state_->setting<double>("metersPerUnit", 0.01);
    const std::string previousAxis = state_->setting<std::string>("upAxis", "Y");
    const bool previousNanite = state_->setting<bool>("requireNanite", false);
    const bool previousCull = state_->setting<bool>("cullBackface", true);
    const bool previousInteractive = state_->interactive();
    if (value.IsEmpty())
        state_->settings.erase(key);
    else
        state_->settings[key] = value;
    // Into or out of tumbling/scrubbing/playing: capped samples and skipped AOVs (or the full image) need a new
    // request.
    if (state_->interactive() != previousInteractive) ++state_->requestSettingsRevision;
    const auto currentDictionary =
        value.IsHolding<VtDictionary>() ? value.UncheckedGet<VtDictionary>() : VtDictionary();
    if (key == TfToken("nativeMaterialParameters") || key == TfToken("nativeMaterialAssets")) {
        dirtyChangedEntries(previousDictionary, currentDictionary, state_->materials, state_->dirtyMaterials);
        ++state_->revision;
    }
    if (key == TfToken("nativeMeshAssets")) {
        dirtyChangedEntries(previousDictionary, currentDictionary, state_->meshes, state_->dirtyMeshes);
        ++state_->revision;
    }
    if (key == TfToken("opTextures")) {  // re-pack what reads a host image
        for (const auto& [id, material] : state_->materials)
            if (std::any_of(material.textures.begin(), material.textures.end(),
                            [](const auto& t) { return HdUEHostTexture(t.file); }))
                state_->dirtyMaterials.insert(id);
        for (const auto& [id, light] : state_->lights)
            if (HdUEHostTexture(light.texture)) state_->dirtyLights.insert(id);
        ++state_->revision;
    }
    if (key == TfToken("surfaceEffects"))
        state_->wetBindings = std::make_shared<const VtDictionary>(state_->setting<VtDictionary>("surfaceEffects", {}));
    if (key == TfToken("surfaceEffects") || key == TfToken("wetMaskPreview") || key == TfToken("wetValidationError")) {
        ++state_->revision;
        if (key == TfToken("surfaceEffects") && state_->renderIndex)
            for (const auto& id : state_->renderIndex->GetRprimIds())
                state_->renderIndex->GetChangeTracker().MarkRprimDirty(id, HdChangeTracker::DirtyPrimvar);
        if (key == TfToken("surfaceEffects") && HdUETraceScene())
            TF_STATUS("hdUE wet settings: renderIndex=%p rprims=%zu", state_->renderIndex,
                      state_->renderIndex ? state_->renderIndex->GetRprimIds().size() : 0);
    }
    if (key == TfToken("renderMode") || key == TfToken("samplesPerPixel") || key == TfToken("warmupFrames") ||
        key == TfToken("lumenEnabled") || key == TfToken("timeSeconds") || key == TfToken("motionBlur") ||
        key == TfToken("motionSamples") || key == TfToken("framesPerSecond") || key == TfToken("disableMotionBlur") ||
        key == TfToken("instantaneousShutter") || key == TfToken("disableDepthOfField") ||
        key == TfToken("tonemapper") || key == TfToken("postProcessMaterials") || key == TfToken("denoiser") ||
        key == TfToken("debugView") || key == TfToken("viewportIdAovs") ||
        TfStringStartsWith(key.GetString(), "postProcess:") || TfStringStartsWith(key.GetString(), "cvar:"))
        ++state_->requestSettingsRevision;
    if (key == TfToken("houdini:frame") || key == TfToken("houdini:fps")) {
        const double frame = state_->setting<double>("houdini:frame", 1.);
        const double fps = state_->setting<double>("houdini:fps", 24.);
        if (std::isfinite(frame) && std::isfinite(fps) && fps > 0) {
            state_->settings[TfToken("timeSeconds")] = VtValue(frame / fps);
            ++state_->requestSettingsRevision;
        }
    }
    if (key == TfToken("subdivisionLevel") && state_->renderIndex) {
        for (const auto& id : state_->renderIndex->GetRprimIds())
            state_->renderIndex->GetChangeTracker().MarkRprimDirty(id, HdChangeTracker::DirtyTopology);
        ++state_->revision;
    }
    if (key == TfToken("autoViewerLight") || key == TfToken("viewerLightIntensity")) {
        for (const auto& [id, light] : state_->lights)
            if (light.viewer) state_->dirtyLights.insert(id);
        ++state_->revision;
    }
    if (key == TfToken("sceneLightsEnabled") || key == TfToken("domeResolution")) {
        for (const auto& [id, light] : state_->lights)
            if (!light.viewer) state_->dirtyLights.insert(id);
        ++state_->revision;
    }
    const bool sceneSpaceChanged =
        (key == TfToken("metersPerUnit") && state_->setting<double>("metersPerUnit", 0.01) != previousUnits) ||
        (key == TfToken("upAxis") && state_->setting<std::string>("upAxis", "Y") != previousAxis) ||
        (key == TfToken("requireNanite") && state_->setting<bool>("requireNanite", false) != previousNanite);
    if (key == TfToken("cullBackface") && state_->setting<bool>("cullBackface", true) != previousCull) {
        for (const auto& [id, v] : state_->meshes) state_->dirtyMeshes.insert(id);
        ++state_->revision;
    }
    if (sceneSpaceChanged) {
        for (const auto& [id, v] : state_->materials) state_->dirtyMaterials.insert(id);
        for (const auto& [id, v] : state_->meshes) state_->dirtyMeshes.insert(id);
        for (const auto& [id, v] : state_->lights) state_->dirtyLights.insert(id);
        ++state_->revision;
    }
}
VtDictionary HdUERenderDelegate::GetRenderStats() const {
    auto& host = selfHost();
    if (host.continuous && host.worker.joinable()) {
        // Answer from the last refresh and queue the next: a synchronous call waited out whole UE ticks (seconds while
        // loading) on Houdini's UI thread.
        if (!state_->statsPending.exchange(true)) {
            host.post([state = state_] {
                VtDictionary fresh;
                try {
                    fresh = renderStats(*state);
                } catch (const std::exception& e) {
                    fresh["error"] = VtValue(std::string(e.what()));
                } catch (...) {
                    fresh["error"] = VtValue(std::string("unknown error"));
                }
                std::lock_guard lock(state->statsMutex);
                state->stats = std::move(fresh);
                state->statsPending = false;
            });
        }
        VtDictionary stats;
        {
            std::lock_guard lock(state_->statsMutex);
            stats = state_->stats;
        }
        auto error = host.error();
        if (const auto it = stats.find("error");
            error.empty() && it != stats.end() && it->second.IsHolding<std::string>())
            error = it->second.UncheckedGet<std::string>();
        const auto stage = state_->currentStage();
        stats["rendererStage"] = VtValue(!error.empty() ? error : stage);
        // The last refresh's progress is stale while UE starts or uploads.
        if (!error.empty() || stage != "Rendering")
            stats["renderProgressAnnotation"] = VtValue(!error.empty() ? "Error: " + error : stage);
        return stats;
    }
    VtDictionary stats;
    try {
        ueRun([&] { stats = renderStats(*state_); });
    } catch (const std::exception& e) {
        stats["error"] = VtValue(std::string(e.what()));
    }
    return stats;
}
VtDictionary HdUERenderDelegate::renderStats(HdUEState& state) {
    std::lock_guard lock(state.mutex);
    HdueResidencyStatus residency{};
    residency.struct_size = sizeof(residency);
    const auto& api = state.backend->api();
    if (state.session) {
        api.get_surface_status(state.session, &state.surfaceStatus);
        api.get_residency_status(state.session, &residency);
    }
    VtDictionary nativeAssetIds;
    int triangles = 0;
    for (const auto& [id, mesh] : state.meshes) {
        triangles += int(mesh.indices.size() / 3);
        if (!mesh.sceneAsset.empty()) nativeAssetIds[mesh.scenePath] = VtValue(id);
    }
    int translucentMaterials = 0;
    const auto nativeMaterials = state.setting<VtDictionary>("nativeMaterialAssets", {});
    for (const auto& [id, material] : state.materials)
        if (material.opacity < 1.f && asset(nativeMaterials, id).empty()) ++translucentMaterials;
    int sceneLights = int(state.nativeAssets.lights);
    HdueLightingStatus lighting{};
    lighting.struct_size = sizeof(lighting);
    if (state.session) state.backend->require(api.get_lighting_status(state.session, &lighting), "lighting_status");
    if (state.setting<bool>("sceneLightsEnabled", true))
        for (const auto& [id, light] : state.lights)
            if (!light.viewer && active(light)) ++sceneLights;
    const auto error = residency.phase == HDUE_RESIDENCY_FAILED ? std::string(residency.message) : state.error;
    // husk prints only the exit code: say why.
    if (!error.empty() && !selfHost().continuous) std::fprintf(stderr, "hdUE error: %s\n", error.c_str());
    // Houdini shows these under the viewport's progress bar and in its render stats; husk -V5 prints the second.
    const auto spp = state.samplesPerPixel();
    std::string progress =
        !error.empty() ? "Error: " + error
        : state.setting<std::string>("renderMode", "realtime") == "pathtraced"
            ? "Path Tracer: " + std::to_string(std::min(int(state.lastFrame.completed_samples), spp)) + "/" +
                  std::to_string(spp) + " samples"
        : state.lastFrame.converged ? "Realtime: converged"
                                    : "Realtime: warming up";
    if (residency.phase == HDUE_RESIDENCY_PREPARING)
        progress += "\nVirtual textures: " + std::to_string(residency.resident_pages) + "/" +
                    std::to_string(residency.required_pages) + " pages";
    if (state.nativeAssets.loading) progress += "\nLoading " + std::to_string(state.nativeAssets.loading) + " assets";
    char frameTimes[96];
    std::snprintf(frameTimes, sizeof(frameTimes), "\nLast frame: %.0f ms, readback %.0f ms",
                  state.lastFrame.request_to_frame_ms, state.lastFrame.readback_ms);
    const auto statsText = "Meshes: " + std::to_string(state.meshes.size()) + " (" + std::to_string(triangles) +
                           " triangles)\nMaterials: " + std::to_string(state.materials.size()) +
                           "\nLights: " + std::to_string(sceneLights) +
                           "\nNative actors: " + std::to_string(state.nativeAssets.actors) + frameTimes;
    VtDictionary stats{
        {"renderProgressAnnotation", VtValue(progress)},
        {"renderStatsAnnotation", VtValue(statsText)},
        {"error", VtValue(error)},
        {"generation", VtValue(state.generation)},
        // husk exits with this code (farm managers judge frames by it); Houdini's UI keeps rendering after an error.
        {"huskErrorStatus", VtValue(int(!error.empty() && !selfHost().continuous))},
        {"wetBindings", VtValue(int(state.surfaceStatus.binding_count))},
        {"wetUploadPending", VtValue(bool(state.surfaceStatus.upload_pending))},
        {"wetMaskUploads", VtValue(state.surfaceStatus.mask_uploads)},
        {"wetMaskBytes", VtValue(state.surfaceStatus.mask_bytes)},
        {"wetGeometryUploads", VtValue(state.surfaceStatus.geometry_uploads)},
        {"wetUpdates", VtValue(state.surfaceStatus.updates)},
        {"vtResidencyPhase", VtValue(int(residency.phase))},
        {"vtResidencyGeneration", VtValue(residency.generation)},
        {"vtRequiredPages", VtValue(residency.required_pages)},
        {"vtResidentPages", VtValue(residency.resident_pages)},
        {"vtLockedTiles", VtValue(residency.locked_tiles)},
        {"vtRequiredMappings", VtValue(residency.required_mappings)},
        {"vtReadyMappings", VtValue(residency.ready_mappings)},
        {"vtTextures", VtValue(int(residency.textures))},
        {"vtPreparationFrames", VtValue(int(residency.preparation_frames))},
        {"vtPreparationMs", VtValue(residency.preparation_ms)},
        {"materialGraphBlocked", VtValue(!state.setting<bool>("materialGraphValid", true))},
        {"translucentMaterials", VtValue(translucentMaterials)},
        {"megaLightsProject", VtValue(bool(lighting.mega_lights_project))},
        {"megaLightsAllowed", VtValue(bool(lighting.mega_lights_allowed))},
        {"megaLightsActive", VtValue(bool(lighting.mega_lights_active))},
        {"domeLights", VtValue(int(lighting.dome_count))},
        {"environmentPending", VtValue(bool(lighting.environment_pending))},
        {"environmentCubeResolution", VtValue(int(lighting.cube_resolution))},
        {"environmentMaxRadiance", VtValue(lighting.environment_max_radiance)},
        {"nativeAssetInstances", VtValue(int(state.nativeAssets.instances))},
        {"nativeAssetIds", VtValue(nativeAssetIds)},
        {"nativeAssetLoading", VtValue(int(state.nativeAssets.loading))},
        {"nativeAssetFailed", VtValue(int(state.nativeAssets.failed))},
        {"nativeActors", VtValue(int(state.nativeAssets.actors))},
        {"nativeMeshes", VtValue(int(state.nativeAssets.meshes))},
        {"nativeLights", VtValue(int(state.nativeAssets.lights))},
        {"sessionHandle", VtValue(uint64_t(state.session))},
        {"submittedMeshes", VtValue(state.submittedMeshes)},
        {"submittedMaterials", VtValue(state.submittedMaterials)},
        {"gpuLocalMemoryBytes", VtValue(state.gpuLocalMemoryBytes)},
        {"gpuMemoryAvailable", VtValue(state.gpuMemoryAvailable)},
        {"materialParameterUpdates", VtValue(state.materialParameterUpdates)},
        {"substrateEnabled", VtValue(bool(state.rendererInfo.substrate_enabled))},
        {"adaptiveGBuffer", VtValue(bool(state.rendererInfo.adaptive_gbuffer))},
        {"substrateClosureBudget", VtValue(int(state.rendererInfo.project_closures_per_pixel))},
        {"substrateBytesPerPixel", VtValue(int(state.rendererInfo.bytes_per_pixel))},
        {"pick", VtValue(state.pickResult)},
        {"publishedFrames", VtValue(state.publishedFrames)},
        {"sceneSubmissions", VtValue(state.sceneSubmissions)},
        {"staleFrameRetries", VtValue(state.staleFrameRetries)},
        {"coalescedCameraUpdates", VtValue(state.coalescedCameraUpdates)},
        {"requestCpuMsTotal", VtValue(state.requestCpuMsTotal)},
        {"meshes", VtValue(int(state.meshes.size()))},
        {"materials", VtValue(int(state.materials.size()))},
        {"lights", VtValue(int(state.lights.size()))},
        {"sceneLights", VtValue(sceneLights)},
        {"viewerLights", VtValue(state.viewerLights)},
        {"autoLightActive", VtValue(state.autoLightActive)},
        {"triangles", VtValue(triangles)},
        {"frameId", VtValue(state.lastFrame.frame_id)},
        {"completedSamples", VtValue(int(state.lastFrame.completed_samples))},
        {"percentDone",
         VtValue(state.lastFrame.converged ? 100.f
                 : state.lastFrame.frame_id && state.setting<std::string>("renderMode", "realtime") == "pathtraced"
                     ? std::min(99.f, 100.f * float(state.lastFrame.completed_samples) / float(state.samplesPerPixel()))
                     : 0.f)},
        {"totalClockTime", VtValue(state.lastFrame.request_to_frame_ms / 1000.)},
        {"peakMemory", VtValue(int64_t(hdue::peak_memory_bytes()))},
        {"requestToFrameMs", VtValue(state.lastFrame.request_to_frame_ms)},
        {"readbackMs", VtValue(state.lastFrame.readback_ms)}};
    // The Cryptomatte layers' name, hash, conversion and manifest: husk writes them into images (UsdRenderers.json).
    for (const auto& [key, value] : state.cryptomatte) stats[key] = value;
    return stats;
}
bool HdUERenderDelegate::InvokeCommand(const TfToken& command, const HdCommandArgs& args) {
    if (command == TfToken("hdue:sampleMemory")) {
        const auto memory = hdue::gpu_memory_bytes();
        std::lock_guard lock(state_->mutex);
        state_->gpuMemoryAvailable = memory.has_value();
        state_->gpuLocalMemoryBytes = memory.value_or(0);
        return true;
    }
    if (command != TfToken("hdue:pickRay")) return false;
    std::lock_guard lock(state_->mutex);
    auto& result = state_->pickResult;
    result = {{"hit", VtValue(false)}, {"path", VtValue(std::string())}, {"error", VtValue(std::string())}};
    const auto argument = [&](const char* key) {
        const auto it = args.find(key);
        return it == args.end() ? VtValue() : it->second;
    };
    const auto originValue = argument("origin"), directionValue = argument("direction");
    const auto minValue = argument("minimum"), maxValue = argument("maximum");
    if (!originValue.IsHolding<GfVec3d>() || !directionValue.IsHolding<GfVec3d>() || !minValue.IsHolding<double>() ||
        !maxValue.IsHolding<double>())
        return false;
    const auto origin = originValue.UncheckedGet<GfVec3d>();
    auto direction = directionValue.UncheckedGet<GfVec3d>();
    double minimum = minValue.UncheckedGet<double>(), maximum = maxValue.UncheckedGet<double>();
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(origin[i]) || !std::isfinite(direction[i])) return false;
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < 0 || maximum < minimum ||
        direction.GetLengthSq() < 1e-20)
        return false;
    direction.Normalize();
    bool nativeScene = false;
    const auto nativeMeshes = state_->setting<VtDictionary>("nativeMeshAssets", {});
    const auto nativeMaterials = state_->setting<VtDictionary>("nativeMaterialAssets", {});
    for (const auto& [id, mesh] : state_->meshes) {
        if (mesh.visible && (!asset(nativeMeshes, id).empty() || !asset(nativeMaterials, mesh.material).empty()))
            nativeScene = true;
    }
    const auto store = [&](const std::string& path, const GfVec3d& point, const GfVec3d& normal, double distance,
                           const char* geometry, bool naniteFallback = false) {
        result["hit"] = VtValue(true);
        result["path"] = VtValue(path);
        result["px"] = VtValue(point[0]);
        result["py"] = VtValue(point[1]);
        result["pz"] = VtValue(point[2]);
        result["nx"] = VtValue(normal[0]);
        result["ny"] = VtValue(normal[1]);
        result["nz"] = VtValue(normal[2]);
        result["distance"] = VtValue(distance);
        result["geometry"] = VtValue(std::string(geometry));
        result["naniteFallback"] = VtValue(naniteFallback);
    };
    if (nativeScene) {
        if (!state_->backend || !state_->session || state_->firstBatch ||
            state_->submittedRevision != state_->revision) {
            result["error"] = VtValue(std::string("Native picking requires the current scene to be submitted to UE"));
            return true;
        }
        const auto& api = state_->backend->api();
        // Native materials can change culling or deform even a generic USD
        // mesh. Validate them before returning any hit in the scene.
        for (const auto& [id, mesh] : state_->meshes) {
            if (!mesh.visible) continue;
            const auto path = asset(nativeMaterials, mesh.material);
            if (path.empty() || state_->pickMaterialFlags.count(path)) continue;
            HduePickMaterialInfo info{};
            info.struct_size = sizeof(info);
            const auto code = api.get_pick_material_info(path.c_str(), &info);
            if (code != HDUE_OK || !(info.flags & HDUE_PICK_MATERIAL_OPAQUE) ||
                (info.flags & HDUE_PICK_MATERIAL_DEFORMED)) {
                result["error"] = VtValue(
                    code == HDUE_OK ? std::string("Picking requires opaque native materials without deformation")
                                    : state_->backend->status_message());
                return true;
            }
            state_->pickMaterialFlags[path] = info.flags;
        }
        const double scale = state_->setting<double>("metersPerUnit", 0.01) * 100;
        if (!(scale > 0) || !std::isfinite(scale)) return false;
        HdueNativeRay ray{};
        ray.struct_size = sizeof(ray);
        const auto ueOrigin = state_->convert(origin), ueDirection = state_->convert(direction, true);
        for (int i = 0; i < 3; ++i) {
            ray.origin[i] = ueOrigin[i];
            ray.direction[i] = ueDirection[i];
        }
        ray.minimum_cm = minimum * scale;
        ray.maximum_cm = maximum * scale;
        HdueNativeHit hit{};
        hit.struct_size = sizeof(hit);
        std::vector<char> objectId(256);
        auto code = api.pick_native(state_->session, &ray, &hit, objectId.data(), uint32_t(objectId.size()));
        if (code == HDUE_BUFFER_TOO_SMALL && hit.required_id_bytes > objectId.size()) {
            objectId.resize(hit.required_id_bytes);
            code = api.pick_native(state_->session, &ray, &hit, objectId.data(), uint32_t(objectId.size()));
        }
        if (code != HDUE_OK) {
            result["error"] = VtValue(state_->backend->status_message());
            return true;
        }
        if (hit.hit) {
            const auto mesh = state_->meshes.find(objectId.data());
            if (mesh == state_->meshes.end()) {
                result["error"] = VtValue(std::string("UE returned an unknown pick object"));
                return true;
            }
            const auto fromUE = [&](const double* v) {
                return state_->setting<std::string>("upAxis", "Y") == "Z" ? GfVec3d(v[0], -v[1], v[2])
                                                                          : GfVec3d(v[1], v[2], -v[0]);
            };
            maximum = hit.distance_cm / scale;
            store(mesh->second.scenePath, fromUE(hit.point) / scale, fromUE(hit.normal).GetNormalized(), maximum,
                  "ue-lod0", (hit.flags & HDUE_NATIVE_PICK_NANITE_FALLBACK) != 0);
        }
    }
    for (auto& [id, mesh] : state_->meshes) {
        if (!mesh.visible || mesh.points.empty() || !asset(nativeMeshes, id).empty()) continue;
        if (!mesh.pickMesh)
            mesh.pickMesh = std::make_shared<HdUEPickMesh>(mesh.points, mesh.indices);  // lazily, on the first pick
        const auto material = asset(nativeMaterials, mesh.material);
        const bool twoSided =
            !material.empty() && (state_->pickMaterialFlags.at(material) & HDUE_PICK_MATERIAL_TWO_SIDED);
        // Each placement: the prim's transform, or every instance of a Hydra instancer's prototype.
        const std::vector<GfMatrix4d> single{mesh.transform};
        for (const auto& placement : mesh.instanced ? mesh.instances : single) {
            if (std::abs(placement.GetDeterminant()) < 1e-20) continue;
            const auto inverse = placement.GetInverse();
            // Preserve the ray parameter through nonuniform/negative scale.
            const GfRay local(inverse.Transform(origin), inverse.TransformDir(direction));
            HdUEPickHit hit;
            if (!mesh.pickMesh->intersect(mesh.points, mesh.indices, local, minimum, maximum, &hit, twoSided)) continue;
            maximum = hit.distance;
            const GfVec3d point = origin + direction * hit.distance;
            GfVec3d normal(0);
            for (int c = 0; c < 3; ++c)
                normal += GfVec3d(mesh.normals[mesh.indices[size_t(hit.triangle) * 3 + c]]) * hit.barycentric[c];
            normal = inverse.GetTranspose().TransformDir(normal).GetNormalized();
            if (twoSided && GfDot(normal, direction) > 0) normal = -normal;
            store(mesh.scenePath, point, normal, hit.distance, mesh.sceneAsset.empty() ? "usd" : "ue-asset-bounds");
        }
    }
    return true;
}
PXR_NAMESPACE_CLOSE_SCOPE

#if defined(HDUE_HOUDINI)
// Ends UE that hdUE runs on its own thread in Houdini's UI (hdue_houdini calls this when Houdini quits).
extern "C" HDUE_VIEWPORT_EXPORT void HDUE_CALL hdue_shutdown_ue() { PXR_NS::shutdownSelfHost(); }
// Runs task(context) on the thread that owns UE and waits for it (inline on that thread, or while nothing owns UE): the
// backend accepts calls from its owner only, and in Houdini's UI that is hdUE's UE thread, not the host's.
extern "C" HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_run_on_ue_thread(void(HDUE_CALL* task)(void*),
                                                                           void* context) {
    if (!task) return HDUE_INVALID_ARGUMENT;
    try {
        PXR_NS::ueRun([&] { task(context); });
    } catch (const std::exception&) {
        return HDUE_INVALID_STATE;
    }  // the owner did not pick the task up
    return HDUE_OK;
}
#endif
