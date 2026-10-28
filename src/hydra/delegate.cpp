#include "delegate.h"
#include "depth.h"
#include "surface_settings.h"
#include "color_management.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/vt/types.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/getenv.h"
#include <optional>
#include "pxr/base/work/loops.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/js/json.h"
#include <cmath>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
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
#if defined(HDUE_HOUDINI)
#define HDUE_VIEWPORT_EXPORTS 1 // hdue_run_ui_tasks is defined below
#include "../houdini/diagnostics.h"
#include "hdue/viewport_api.h"
#endif

PXR_NAMESPACE_OPEN_SCOPE
void HdUETakeMeshSyncStats(double& milliseconds, int& count); // prims.cpp
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
void trace(const char* format, ...) {
    static const bool enabled = TfGetenvBool("HDUE_TRACE_TIMING", false);
    if (!enabled) return;
    static const auto origin = std::chrono::steady_clock::now();
    std::fprintf(traceSink(), "%9.1f ", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count());
    va_list args; va_start(args, format);
    std::vfprintf(traceSink(), format, args);
    va_end(args);
    std::fflush(traceSink());
}
// Hosts such as husk never start UE. The first delegate created while the
// runtime is cold starts it on its own thread and ticks it from Execute, as
// hdue_usd_render does; the last delegate shuts it down.
struct HdUESelfHost {
    std::unique_ptr<hdue::Backend> backend;
    HWND window = nullptr;
    DWORD thread = 0;
    std::atomic<int> delegates{0};
    // Houdini's UI (HDUE_HOUDINI_UI, set by hdue_houdini): UE ticks continuously on this thread while a delegate
    // exists, Execute only submits and polls, and UE outlives individual viewports until hdue_shutdown_ue.
    bool continuous = false;
    // Houdini's UI boots UE on this thread without waiting (as Karma loads); a failed boot is reported here.
    std::mutex bootMutex; std::string bootError;
    std::string error() { std::lock_guard lock(bootMutex); return bootError; }
    std::filesystem::path directory; // the host's working directory, which UE moves to its Binaries
    // UE runs on its own thread: husk calls from one thread, but Live Render creates the delegate on one
    // thread and executes it on others, and UE only accepts calls from the thread that started it.
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<std::function<void()>> queue;
    // Houdini's UI: called on this thread after each idle tick so finished frames reach the render buffers without
    // waiting for the host to call Execute (keyed by render pass; touched only on this thread).
    std::map<const void*, std::function<void()>> pumps;
    bool pumping = false; // pumps run right after the loop's tick, so Execute skips its own
    bool stopping = false;
    template<class F> auto run(F&& f) -> decltype(f()) {
        if (!worker.joinable() || std::this_thread::get_id() == worker.get_id()) return f();
        std::packaged_task<decltype(f())()> task(std::forward<F>(f));
        auto result = task.get_future();
        { std::lock_guard lock(queueMutex); queue.emplace_back([&task] { task(); }); }
        queueReady.notify_one();
        return result.get();
    }
    // Queues f on the UE thread without waiting (Houdini's UI must never block on a UE tick).
    void post(std::function<void()> f) {
        if (!worker.joinable() || std::this_thread::get_id() == worker.get_id()) { f(); return; }
        { std::lock_guard lock(queueMutex); queue.emplace_back(std::move(f)); }
        queueReady.notify_one();
    }
    void stop() {
        if (!worker.joinable()) return;
        { std::lock_guard lock(queueMutex); stopping = true; }
        queueReady.notify_one();
        worker.join();
    }
    void tick() const {
        if (!backend || GetCurrentThreadId() != thread) return;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        const auto result = backend->api().tick();
        // husk resolves relative output paths against its working directory.
        std::error_code ignored; std::filesystem::current_path(directory, ignored);
        backend->require(result, "tick");
    }
};
HdUESelfHost& selfHost() { static HdUESelfHost host; return host; }
// When a host (Houdini's UI thread) owns UE, delegates executing on other threads (Live Render) queue their
// UE work here; the host runs it from hdue_run_ui_tasks after each tick.
struct HdUEOwnerQueue {
    struct Task { std::function<void()> run; int state = 0; std::exception_ptr error; }; // 0 queued, 1 running, 2 done
    std::mutex mutex;
    std::condition_variable done;
    std::deque<std::shared_ptr<Task>> tasks;
    std::atomic<DWORD> owner{0};
};
HdUEOwnerQueue& ownerQueue() { static HdUEOwnerQueue queue; return queue; }
// Runs f on the thread that owns UE: hdUE's own UE thread, the host's thread, or inline.
void ueRun(const std::function<void()>& f) {
    auto& host = selfHost();
    if (host.worker.joinable()) { host.run(f); return; }
    auto& queue = ownerQueue();
    const DWORD owner = queue.owner;
    if (!owner || owner == GetCurrentThreadId()) { f(); return; }
    auto task = std::make_shared<HdUEOwnerQueue::Task>();
    task->run = f;
    std::unique_lock lock(queue.mutex);
    queue.tasks.push_back(task);
    // A UI thread that waits for this thread (stopping a render) never drains: give up on work it has not started.
    if (!queue.done.wait_for(lock, std::chrono::seconds(5), [&] { return task->state != 0; })) {
        queue.tasks.erase(std::find(queue.tasks.begin(), queue.tasks.end(), task));
        throw std::runtime_error("hdUE: the thread that owns UE did not run queued work within 5 s");
    }
    queue.done.wait(lock, [&] { return task->state == 2; });
    if (task->error) std::rethrow_exception(task->error);
}
}
// Hydra AOV names hdUE renders after the beauty; 0 for color and unsupported names.
uint32_t HdUEAovId(const TfToken& name) {
    static const std::pair<TfToken, uint32_t> names[] = {{TfToken("albedo"), HDUE_AOV_ALBEDO}, {TfToken("baseColor"), HDUE_AOV_ALBEDO},
        {HdAovTokens->normal, HDUE_AOV_NORMAL}, {TfToken("roughness"), HDUE_AOV_ROUGHNESS}, {TfToken("metallic"), HDUE_AOV_METALLIC},
        {TfToken("specular"), HDUE_AOV_SPECULAR}, {TfToken("occlusion"), HDUE_AOV_OCCLUSION},
        {HdAovTokens->primId, HDUE_AOV_PRIM_ID}, {HdAovTokens->instanceId, HDUE_AOV_INSTANCE_ID}};
    for (const auto& [token, id] : names) if (name == token) return id;
    return 0;
}
namespace {
// Offscreen UE (-RenderOffscreen) has no window of its own to route messages to.
const bool selfHostVisible = TfGetenvBool("HDUE_SHOW_WINDOW", false);
LRESULT CALLBACK selfHostWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto& host = selfHost();
    if (selfHostVisible && host.backend && host.backend->initialized()) {
        int64_t result = 0;
        if (host.backend->api().window_message(reinterpret_cast<uint64_t>(window), message, uint64_t(wparam), int64_t(lparam), &result) == HDUE_OK) return LRESULT(result);
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
    std::stringstream text; text << file.rdbuf();
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
// Houdini's interactive UI (not husk or hython), from the process executable: a .hip opened at startup creates the
// viewport's delegate before hdue_houdini can set HDUE_HOUDINI_UI.
bool houdiniUi() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto name = std::filesystem::path(path).filename().wstring();
    for (auto& c : name) c = wchar_t(towlower(c));
    return name == L"houdini.exe" || name == L"houdinifx.exe" || name == L"houdinicore.exe" || name == L"happrentice.exe" || name == L"houdinicore.exe";
}
// Starts UE on hdUE's thread and ticks it until its world and RHI are ready. wait = false (Houdini's UI) returns at once:
// work posted meanwhile runs once UE is ready, and a failure is kept in HdUESelfHost::bootError.
void startSelfHost(const std::string& dll, const std::string& project, bool wait) {
    auto& host = selfHost();
    if (project.empty()) throw std::runtime_error("hdUE cannot start UE without runtime_project in HDUE_RUNTIME_CONFIG");
    auto started = std::make_shared<std::promise<void>>();
    auto ready = started->get_future();
    host.stopping = false;
    host.continuous = TfGetenvBool("HDUE_HOUDINI_UI", false) || houdiniUi();
    host.worker = std::thread([&host, dll, project, started] {
        try {
            const auto instance = GetModuleHandleW(nullptr);
            WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = selfHostWindow; cls.lpszClassName = L"HdUESelfHost";
            RegisterClassW(&cls);
            host.window = CreateWindowExW(0, cls.lpszClassName, L"hdUE", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
            if (!host.window) throw std::runtime_error("hdUE cannot create its UE host window");
            host.backend = std::make_unique<hdue::Backend>(std::filesystem::u8path(dll));
            HdueInitDesc init{}; init.struct_size = sizeof(init);
            init.instance_handle = reinterpret_cast<uint64_t>(instance); init.window_handle = reinterpret_cast<uint64_t>(host.window);
            init.project_path = project.c_str(); init.map_path = "/Game/HdUETest";
            // Frames are read back from render targets, so UE needs no visible window (HDUE_SHOW_WINDOW=1 shows it).
            // Without -stdout, UE echoes Display/Warning/Error lines with wprintf, which reached the host's console
            // (Houdini, husk) as UTF-16 and showed as spaced-out text; -stdout writes the same lines as UTF-8.
            // HDUE_UE_ARGS appends UE command-line arguments (profiling, -dpcvars=...).
            const std::string arguments = std::string(host.continuous ? (selfHostVisible ? "-stdout -hdUEInteractive" : "-RenderOffscreen -stdout -hdUEInteractive")
                                                                      : (selfHostVisible ? "-stdout" : "-RenderOffscreen -stdout")) +
                                          " " + TfGetenv("HDUE_UE_ARGS");
            init.extra_arguments = arguments.c_str();
            host.thread = GetCurrentThreadId();
            host.backend->initialize(init);
            // UE reports ready only after its first ticks.
            HdueStatus status{}; status.struct_size = sizeof(status);
            for (int i = 0; i < 600 && !(status.state == HDUE_STATE_READY && status.world_ready && status.rhi_ready); ++i) {
                host.tick();
                host.backend->require(host.backend->api().get_status(&status), "get_status");
                if (status.state == HDUE_STATE_FAILED) break;
                Sleep(10);
            }
            if (status.state != HDUE_STATE_READY || !status.world_ready || !status.rhi_ready)
                throw std::runtime_error(std::string("hdUE: UE did not become ready: ") + status.message);
            started->set_value();
        } catch (const std::exception& e) {
            { std::lock_guard lock(host.bootMutex); host.bootError = e.what(); }
            host.backend.reset(); started->set_exception(std::current_exception());
            // Keep serving the queue: work posted meanwhile fails with UE's errors instead of waiting forever.
        }
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(host.queueMutex);
                const auto due = [&host] { return host.stopping || !host.queue.empty(); };
                const bool ticking = host.continuous && host.delegates > 0;
                if (ticking) host.queueReady.wait_for(lock, std::chrono::milliseconds(2), due);
                else host.queueReady.wait(lock, due);
                if (host.stopping && host.queue.empty()) return;
                if (!host.queue.empty()) { task = std::move(host.queue.front()); host.queue.pop_front(); }
            }
            if (task) task();
            else {
                const auto start = std::chrono::steady_clock::now();
                try { host.tick(); } catch (const std::exception& e) { TF_RUNTIME_ERROR("hdUE tick: %s", e.what()); }
                host.pumping = true;
                for (auto& [key, pump] : host.pumps) pump();
                host.pumping = false;
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (ms > 20) trace("hdUE slow tick: %.1f ms\n", ms);
            }
        }
    });
    if (!wait) return;
    try { ready.get(); } catch (...) { host.worker.join(); throw; }
}
}
HdRprim* HdUECreateMesh(const SdfPath&);
HdRprim* HdUECreatePoints(const SdfPath&);
HdRprim* HdUECreateCurves(const SdfPath&);
HdRprim* HdUECreateVolume(const SdfPath&);
HdBprim* HdUECreateField(const SdfPath&);
HdInstancer* HdUECreateInstancer(HdSceneDelegate*, const SdfPath&);
HdSprim* HdUECreateMaterial(const SdfPath&);
HdSprim* HdUECreateCamera(const SdfPath&);
HdSprim* HdUECreateLight(const SdfPath&, uint32_t);
void HdUERefreshWetMasks(HdUEState&, HdRenderIndex*);

HdUEState::~HdUEState() { if (session && backend) backend->api().destroy_session(session); }
void HdUEState::erase(const std::string& id) {
    std::lock_guard lock(mutex);
    if (const auto mesh = meshes.find(id); mesh != meshes.end()) { erasedMeshes[id] = std::move(mesh->second); meshes.erase(mesh); }
    materials.erase(id); lights.erase(id); volumes.erase(id); dirtyVolumes.erase(id);
    // geometryRevisions[id] stays until the removal reaches UE (submit), so a revived mesh keeps its revision.
    dirtyMeshes.erase(id); dirtyMaterials.erase(id); dirtyLights.erase(id);
    removed.insert(id); ++revision;
}
namespace {
constexpr const char* AutoKey = "hdUE:viewer:key";
constexpr const char* AutoFill = "hdUE:viewer:fill";
bool active(const HdUELightData& light) {
    return light.visible && std::isfinite(light.intensity) && light.intensity > 0 &&
        (light.color[0] > 0 || light.color[1] > 0 || light.color[2] > 0);
}
std::string asset(const HdUEState& state, const char* key, const std::string& id) {
    const auto mapping = state.setting<VtDictionary>(key, {});
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
    template <class T, class Store> const T* own(Store& store, std::vector<T> values) {
        return values.empty() ? nullptr : store.emplace_back(std::move(values)).data();
    }
    // What this upload covers; Sync may dirty more meanwhile, for the next upload.
    std::set<std::string> dirtyMeshes, dirtyMaterials, dirtyLights, dirtyVolumes, removedIds;
    // Motion blur: deforming meshes uploaded with their points at a sub-frame time instead of their stored points.
    std::map<std::string, std::vector<GfVec3f>> points;
    // Takes the state's dirty sets (state lock held).
    void take(HdUEState& state) {
        dirtyMeshes.swap(state.dirtyMeshes); dirtyMaterials.swap(state.dirtyMaterials);
        dirtyLights.swap(state.dirtyLights); dirtyVolumes.swap(state.dirtyVolumes); removedIds.swap(state.removed);
    }
    // Gives them back after a failed upload (state lock held). Sync may have erased or revived prims meanwhile:
    // only prims that still exist stay dirty, and only prims still gone stay removed.
    void giveBack(HdUEState& state) const {
        const auto restore = [](const std::set<std::string>& from, std::set<std::string>& to, const auto& prims) {
            for (const auto& id : from) if (prims.count(id)) to.insert(id);
        };
        restore(dirtyMeshes, state.dirtyMeshes, state.meshes); restore(dirtyMaterials, state.dirtyMaterials, state.materials);
        restore(dirtyLights, state.dirtyLights, state.lights); restore(dirtyVolumes, state.dirtyVolumes, state.volumes);
        for (const auto& id : removedIds)
            if (!state.meshes.count(id) && !state.materials.count(id) && !state.lights.count(id) && !state.volumes.count(id)) state.removed.insert(id);
    }
    uint64_t revision = 0;
    bool firstBatch = false;
    // Descriptors.
    std::vector<HdueMeshDesc> meshes;
    std::vector<HdueMaterialDesc> materials;
    std::vector<HdueLightDesc> lights;
    std::vector<HdueLight> extendedLights;
    std::vector<HdueAssetDesc> sceneAssets;
    std::vector<HdueVolumeDesc> volumes;
    std::vector<HdueMaterialUpdate> materialUpdates;
    std::vector<HdueWetBinding> wet;
    std::vector<const char*> removed, removedAssets;
    std::vector<std::string> genericMeshes; // non-native meshes, whose geometry revision the upload advances
    bool autoLight = false, surfaceUpdate = false;
    int cameraLights = 0;
    uint32_t nativeLights = 0;
};
GfMatrix4d sceneBasis(const HdUEState& state) {
    GfMatrix4d basis(1);
    for (int r = 0; r < 3; ++r) { GfVec3d e(0); e[r] = 1; const auto v = state.convert(e); for (int c = 0; c < 3; ++c) basis[r][c] = v[c]; }
    return basis;
}
void rowMajor(const GfMatrix4d& m, double* out) { for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[r][c]; }
HdueMaterialParameter materialParameter(ScenePacket& packet, const std::string& name, const VtValue& value) {
    HdueMaterialParameter p{}; p.name = packet.own(name);
    if (value.IsHolding<float>() || value.IsHolding<double>() || value.IsHolding<int>()) {
        p.type = HDUE_PARAMETER_SCALAR;
        p.value[0] = value.IsHolding<float>() ? value.UncheckedGet<float>() : value.IsHolding<double>() ? float(value.UncheckedGet<double>()) : float(value.UncheckedGet<int>());
    } else if (value.IsHolding<GfVec3f>() || value.IsHolding<GfVec3d>()) {
        p.type = HDUE_PARAMETER_VECTOR; p.value[3] = 1;
        const auto v = value.IsHolding<GfVec3f>() ? GfVec3d(value.UncheckedGet<GfVec3f>()) : value.UncheckedGet<GfVec3d>();
        for (int c = 0; c < 3; ++c) p.value[c] = float(v[c]);
    } else if (value.IsHolding<GfVec4f>()) {
        p.type = HDUE_PARAMETER_VECTOR;
        for (int c = 0; c < 4; ++c) p.value[c] = value.UncheckedGet<GfVec4f>()[c];
    } else if (value.IsHolding<std::string>()) {
        p.type = HDUE_PARAMETER_TEXTURE; p.texture_asset = packet.own(value.UncheckedGet<std::string>());
    } else throw std::runtime_error("Unsupported native material parameter value: " + name);
    return p;
}
void prepareMaterials(HdUEState& state, ScenePacket& packet) {
    const float units = float(state.setting<double>("metersPerUnit", 0.01) * 100);
    for (const auto& id : packet.dirtyMaterials) {
        const auto& data = state.materials.at(id);
        HdueMaterialDesc desc{};
        desc.id = packet.own(data.id); desc.native_asset = packet.own(asset(state, "nativeMaterialAssets", id));
        for (int i = 0; i < 3; ++i) { desc.base_color[i] = data.color[i]; desc.emissive_color[i] = data.emissive[i]; }
        desc.base_color[3] = data.opacity; desc.metallic = data.metallic; desc.roughness = data.roughness;
        desc.opacity_threshold = data.opacityThreshold;
        desc.flags = data.vertexColor ? HDUE_MATERIAL_VERTEX_COLOR : 0;
        std::copy(data.uvTransform.begin(), data.uvTransform.end(), desc.uv_transform);
        for (int slot = 0; slot < HDUE_TEXTURE_SLOT_COUNT; ++slot) {
            const auto& texture = data.textures[slot];
            auto& out = desc.textures[slot];
            out.file = packet.own(texture.file);
            // Displacement is a distance: scene units to UE centimetres.
            const float scale = slot == HDUE_TEXTURE_DISPLACEMENT ? units : 1.f;
            for (int i = 0; i < 4; ++i) { out.scale[i] = texture.scale[i] * scale; out.bias[i] = texture.bias[i] * scale; out.channel_mask[i] = texture.mask[i]; }
        }
        desc.displacement = data.displacement * units;
        desc.occlusion = data.occlusion; desc.clearcoat = data.clearcoat; desc.clearcoat_roughness = data.clearcoatRoughness; desc.ior = data.ior;
        for (int i = 0; i < 3; ++i) desc.specular_color[i] = data.specularColor[i];
        if (data.specularWorkflow) desc.flags |= HDUE_MATERIAL_SPECULAR_WORKFLOW;
        packet.materials.push_back(desc);
        if (TfGetenvBool("HDUE_TRACE_SCENE", false)) TF_STATUS("hdUE material %s rgb=(%g,%g,%g)", desc.id, desc.base_color[0], desc.base_color[1], desc.base_color[2]);
    }
    // Native material parameter overrides.
    const auto overrides = state.setting<VtDictionary>("nativeMaterialParameters", {});
    for (const auto& id : packet.dirtyMaterials) {
        if (state.materials.find(id) == state.materials.end()) continue;
        const auto entry = overrides.find(id);
        if (entry == overrides.end()) continue;
        if (asset(state, "nativeMaterialAssets", id).empty() || !entry->second.IsHolding<VtDictionary>())
            throw std::runtime_error("Native material parameters require a mapped cooked material and a dictionary");
        if (!state.backend->material_api().set_parameters) throw std::runtime_error("Backend has no material parameter extension");
        std::vector<HdueMaterialParameter> parameters;
        for (const auto& [name, value] : entry->second.UncheckedGet<VtDictionary>()) parameters.push_back(materialParameter(packet, name, value));
        HdueMaterialUpdate update{}; update.struct_size = sizeof(update); update.material_id = packet.own(id);
        update.parameter_count = uint32_t(parameters.size()); update.parameters = packet.own(packet.parameters, std::move(parameters));
        packet.materialUpdates.push_back(update);
    }
}
void prepareMeshes(HdUEState& state, ScenePacket& packet) {
    const auto convert = state.converter();
    const auto basis = sceneBasis(state);
    const bool requireNanite = state.setting<bool>("requireNanite", false);
    const auto inverse = basis.GetInverse();
    for (const auto& id : packet.dirtyMeshes) {
        const auto& data = state.meshes.at(id);
        if (!data.sceneAsset.empty()) {
            HdueAssetDesc d{}; d.struct_size = sizeof(d); d.id = packet.own(data.id); d.asset_path = packet.own(data.sceneAsset); d.visible = data.visible;
            rowMajor(inverse * data.transform * basis, d.transform);
            packet.sceneAssets.push_back(d); packet.removed.push_back(d.id);
            continue;
        }
        packet.removedAssets.push_back(packet.own(id)); // in case it was a scene asset before
        const auto nativeAsset = asset(state, "nativeMeshAssets", id);
        // Instanced prototypes keep local vertices; each instance matrix carries the placement.
        const GfMatrix4d bake = data.instanced ? GfMatrix4d(1) : data.transform;
        const auto normalTransform = bake.GetInverse().GetTranspose();
        const auto moved = packet.points.find(id);
        const auto& points = moved != packet.points.end() ? moved->second : data.points;
        std::vector<float> p, n, uv;
        p.reserve(points.size() * 3);
        for (size_t i = 0; i < points.size(); ++i) {
            const auto point = convert(bake.Transform(GfVec3d(points[i])));
            for (int c = 0; c < 3; ++c) p.push_back(float(point[c]));
            if (data.normals.size() == data.points.size()) {
                const auto normal = convert(normalTransform.TransformDir(GfVec3d(data.normals[i])), true).GetNormalized();
                for (int c = 0; c < 3; ++c) n.push_back(float(normal[c]));
            }
            if (data.uv.size() == data.points.size()) { uv.push_back(data.uv[i][0]); uv.push_back(1 - data.uv[i][1]); }
        }
        std::vector<uint32_t> index(data.points.size()); std::iota(index.begin(), index.end(), 0u);
        // World transforms are baked into dynamic vertices. Preserve front
        // faces when an authored transform adds another handedness reversal.
        if (bake.GetDeterminant() < 0)
            for (size_t i = 0; i + 2 < index.size(); i += 3) std::swap(index[i + 1], index[i + 2]);
        std::vector<float> colors;
        for (const auto& c : data.colors) colors.insert(colors.end(), {c[0], c[1], c[2]});
        HdueMeshDesc desc{};
        desc.id = packet.own(data.id); desc.material_id = packet.own(data.material); desc.native_asset = packet.own(nativeAsset);
        desc.vertex_count = uint32_t(data.points.size()); desc.index_count = uint32_t(index.size()); desc.visible = data.visible;
        desc.prim_id = data.primId;
        if (colors.size() == p.size()) desc.colors = packet.own(packet.floats, std::move(colors));
        desc.positions = packet.own(packet.floats, std::move(p));
        desc.normals = packet.own(packet.floats, std::move(n));
        desc.uv = packet.own(packet.floats, std::move(uv));
        desc.indices = packet.own(packet.indices, std::move(index));
        if (data.instanced) {
            // USD row-vector matrices into UE: S^-1 * M * S, S = the USD -> UE basis and units.
            std::vector<double> matrices(data.instances.size() * 16);
            for (size_t i = 0; i < data.instances.size(); ++i) rowMajor(inverse * data.instances[i] * basis, &matrices[i * 16]);
            desc.instance_count = uint32_t(data.instances.size());
            std::vector<float> instanceColors;
            for (const auto& c : data.instanceColors) instanceColors.insert(instanceColors.end(), {c[0], c[1], c[2]});
            if (instanceColors.size() == size_t(desc.instance_count) * 3 && desc.instance_count) desc.instance_colors = packet.own(packet.floats, std::move(instanceColors));
            if (data.instanceIds.size() == desc.instance_count && desc.instance_count)
                desc.instance_ids = packet.own(packet.ids, std::vector<int32_t>(data.instanceIds.begin(), data.instanceIds.end()));
            static const double none = 0;
            desc.instance_transforms = matrices.empty() ? &none : packet.own(packet.doubles, std::move(matrices));
        }
        desc.flags = requireNanite && !nativeAsset.empty() ? HDUE_MESH_REQUIRE_NANITE : 0;
        // Surface effects edit vertex colours after upload, so their meshes stay editable (dynamic) in UE.
        if (state.wetBindings && !HdUEWetSettings(*state.wetBindings, data).empty()) desc.flags |= HDUE_MESH_EDITABLE;
        for (int i = 0; i < 4; ++i) desc.transform[i * 4 + i] = 1;
        // Native geometry keeps its own dimensions; only its placement transform comes from Hydra.
        if (!nativeAsset.empty()) rowMajor(inverse * data.transform * basis, desc.transform);
        else packet.genericMeshes.push_back(id);
        packet.meshes.push_back(desc);
        if (TfGetenvBool("HDUE_TRACE_SCENE", false)) TF_STATUS("hdUE mesh %s material=%s vertices=%u", desc.id, desc.material_id, desc.vertex_count);
    }
}
void prepareLights(HdUEState& state, ScenePacket& packet) {
    const double viewerIntensity = state.setting<double>("viewerLightIntensity", state.setting<float>("viewerLightIntensity", 4.f));
    if (!std::isfinite(viewerIntensity) || viewerIntensity < 0 || viewerIntensity > 100)
        throw std::runtime_error("viewerLightIntensity must be between 0 and 100");
    int sceneLights = 0;
    for (const auto& [id, light] : state.lights) if (active(light)) {
        if (light.viewer) ++packet.cameraLights;
        else if (state.setting<bool>("sceneLightsEnabled", true)) ++sceneLights;
    }
    packet.autoLight = state.setting<bool>("autoViewerLight", false) && !sceneLights && !packet.cameraLights && !state.nativeAssets.lights;
    const double units = state.setting<double>("metersPerUnit", .01) * 100;
    const auto domeResolution = uint32_t(state.setting<int>("domeResolution", 512));
    const auto extend = [&](const HdueLightDesc& legacy, const HdUELightData* source) {
        HdueLight light{}; light.struct_size = sizeof(light); light.type = legacy.type; light.id = legacy.id;
        light.visible = legacy.visible; light.intensity = legacy.intensity;
        light.flags = source ? source->flags : HDUE_LIGHT_SHADOWS | HDUE_LIGHT_MEGALIGHTS;
        light.diffuse = source ? source->diffuse : 1; light.specular = source ? source->specular : 1;
        light.cone_angle = source ? source->coneAngle : 180; light.cone_softness = source ? source->coneSoftness : 0;
        light.angle_degrees = source ? source->angle : .53f;
        light.cube_resolution = domeResolution;
        light.texture_format = source ? source->textureFormat : HDUE_ENVIRONMENT_AUTOMATIC;
        light.texture_file = packet.own(source ? source->texture : "");
        light.ies_file = packet.own(source ? source->ies : ""); light.ies_angle_scale = source ? source->iesAngleScale : 0;
        light.ies_normalize = source && source->iesNormalize;
        for (int c = 0; c < 3; ++c) { light.position_cm[c] = legacy.position[c]; light.color[c] = legacy.color[c]; }
        for (int axis = 0; axis < 3; ++axis) {
            GfVec3d direction(0); direction[axis] = 1;
            direction = source ? state.convert(source->transform.TransformDir(direction), true).GetNormalized() : direction;
            for (int c = 0; c < 3; ++c) light.axes[axis * 3 + c] = direction[c];
        }
        light.width_cm = legacy.width_cm; light.height_cm = legacy.height_cm;
        if (source) {
            light.length_cm = float(source->length * source->transform.TransformDir(GfVec3d(1, 0, 0)).GetLength() * units);
            if (source->type == HDUE_CYLINDER_LIGHT || source->type == HDUE_SPHERE_LIGHT) {
                const double x = source->transform.TransformDir(GfVec3d(1, 0, 0)).GetLength();
                const double y = source->transform.TransformDir(GfVec3d(0, 1, 0)).GetLength();
                const double z = source->transform.TransformDir(GfVec3d(0, 0, 1)).GetLength();
                light.width_cm = float(source->width * (source->type == HDUE_CYLINDER_LIGHT ? std::max(y, z) : std::max({x, y, z})) * units);
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
        HdueLightDesc desc{}; desc.id = packet.own(data.id); desc.type = data.type;
        desc.visible = data.visible && (data.viewer || state.setting<bool>("sceneLightsEnabled", true));
        const auto position = state.convert(data.transform.ExtractTranslation());
        const auto direction = state.convert(data.transform.TransformDir(GfVec3d(0, 0, -1)), true).GetNormalized();
        for (int c = 0; c < 3; ++c) { desc.position[c] = position[c]; desc.direction[c] = direction[c]; desc.color[c] = data.color[c]; }
        desc.intensity = data.intensity;
        if (data.viewer) {
            desc.intensity = float(viewerIntensity);
            desc.direction[0] = 1; desc.direction[1] = desc.direction[2] = 0;
        }
        desc.width_cm = float(data.width * data.transform.TransformDir(GfVec3d(1, 0, 0)).GetLength() * units);
        desc.height_cm = float(data.height * data.transform.TransformDir(GfVec3d(0, 1, 0)).GetLength() * units);
        packet.lights.push_back(desc);
        extend(desc, &data);
        if (TfGetenvBool("HDUE_TRACE_SCENE", false)) TF_STATUS("hdUE light %s intensity=%g rgb=(%g,%g,%g)", desc.id, desc.intensity, desc.color[0], desc.color[1], desc.color[2]);
    }
    if (packet.autoLight) {
        for (int i = 0; i < 2; ++i) {
            HdueLightDesc desc{};
            desc.id = i ? AutoFill : AutoKey; desc.type = HDUE_CAMERA_DISTANT_LIGHT; desc.visible = 1;
            desc.color[0] = desc.color[1] = desc.color[2] = 1;
            desc.intensity = float(viewerIntensity * (i ? 0.1875 : 1.0));
            desc.direction[0] = 1; desc.direction[1] = i ? -0.6 : 0.4; desc.direction[2] = i ? -0.2 : -0.5;
            packet.lights.push_back(desc);
            extend(desc, nullptr);
        }
    } else if (state.autoLightActive) {
        packet.removed.push_back(AutoKey); packet.removed.push_back(AutoFill);
    }
    if (!state.backend->lighting_api().upsert) for (const auto& light : packet.extendedLights)
        if (light.type > HDUE_CAMERA_DISTANT_LIGHT || (light.texture_file && *light.texture_file))
            throw std::runtime_error("This scene requires the backend lighting extension; build/cook/stage a current runtime.");
}
void prepareVolumes(HdUEState& state, ScenePacket& packet) {
    if (packet.dirtyVolumes.empty()) return;
    if (!state.backend->volume_api().upsert) throw std::runtime_error("This scene has volumes; build and stage the current backend");
    // Voxel space to UE world: USD placement, then the USD -> UE basis and units (row vectors).
    const auto basis = sceneBasis(state);
    const double perCentimeter = 1 / (state.setting<double>("metersPerUnit", .01) * 100);
    for (const auto& id : packet.dirtyVolumes) {
        const auto it = state.volumes.find(id);
        if (it == state.volumes.end()) continue;
        const auto& volume = it->second;
        HdueVolumeDesc desc{}; desc.struct_size = sizeof(desc); desc.id = packet.own(volume.id); desc.visible = volume.visible;
        desc.width = volume.width; desc.height = volume.height; desc.depth = volume.depth;
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
            throw std::runtime_error(data.bindingPath + ": " + (error != data.maskErrors.end() ? error->second : "Wet mask not synchronized: " + maskName));
        }
        HdueWetBinding binding{}; binding.struct_size = sizeof(binding); binding.object_id = packet.own(data.id);
        binding.variant_asset = packet.own(HdUEWetValue<std::string>(fields, "asset", ""));
        binding.preview_asset = packet.own(HdUEWetValue<std::string>(fields, "previewAsset", ""));
        binding.amount = amount;
        binding.roughness = HdUEWetValue<float>(fields, "roughness", float(HdUEWetValue<double>(fields, "roughness", .08)));
        binding.flags = preview ? HDUE_WET_MASK_PREVIEW : 0;
        if (HdUEWetValue<bool>(fields, "graph", false)) binding.flags |= HDUE_SURFACE_GRAPH;
        if (!maskName.empty()) {
            binding.mask = packet.own(packet.floats, std::vector<float>(mask->second.begin(), mask->second.end()));
            binding.mask_count = uint32_t(mask->second.size());
        }
        // The revision this upload leaves the mesh at: generic meshes advance by one when re-uploaded.
        const bool reuploaded = packet.dirtyMeshes.count(id) && data.sceneAsset.empty() && asset(state, "nativeMeshAssets", id).empty();
        binding.geometry_revision = state.geometryRevisions[data.id] + uint64_t(reuploaded);
        packet.wet.push_back(binding);
    }
    packet.surfaceUpdate = state.backend->surface_api().apply && state.backend->surface_api().get_status &&
        (!packet.wet.empty() || state.surfaceStatus.binding_count);
    if (!packet.wet.empty() && !state.backend->surface_api().apply) throw std::runtime_error("Wet surface extension unavailable; build/cook/stage the current runtime.");
}
// Under the state lock: takes the dirty sets and copies what UE needs.
void prepareScene(HdUEState& state, ScenePacket& packet) {
    packet.take(state);
    packet.revision = state.revision; packet.firstBatch = state.firstBatch;
    packet.nativeLights = state.nativeAssets.lights;
    prepareSurfaceEffects(state, packet);
    prepareMaterials(state, packet);
    prepareMeshes(state, packet);
    prepareLights(state, packet);
    prepareVolumes(state, packet);
    for (const auto& id : packet.removedIds) packet.removed.push_back(packet.own(id));
}
// Without the state lock: only the backend and the packet (the UE thread owns the session).
void sendScene(const HdUEState& state, ScenePacket& packet) {
    const auto& backend = *state.backend;
    const auto session = state.session;
    const auto& lightingApi = backend.lighting_api();
    HdueSceneBatch batch{};
    batch.struct_size = sizeof(batch); batch.reset_scene = packet.firstBatch;
    batch.meshes = packet.meshes.data(); batch.mesh_count = uint32_t(packet.meshes.size());
    batch.materials = packet.materials.data(); batch.material_count = uint32_t(packet.materials.size());
    batch.lights = lightingApi.upsert ? nullptr : packet.lights.data(); batch.light_count = lightingApi.upsert ? 0 : uint32_t(packet.lights.size());
    batch.removed_ids = packet.removed.data(); batch.removed_count = uint32_t(packet.removed.size());
    if (batch.reset_scene || batch.mesh_count || batch.material_count || batch.light_count || batch.removed_count) {
        const auto applyStart = std::chrono::steady_clock::now();
        backend.require(backend.api().apply_scene(session, &batch), "apply_scene");
        trace("hdUE scene submit: UE apply %.1f ms (%u meshes, %u materials, %u lights)\n",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count(), batch.mesh_count, batch.material_count, batch.light_count);
        double syncMs = 0; int syncs = 0; HdUETakeMeshSyncStats(syncMs, syncs);
        if (syncs) trace("hdUE mesh sync since last submit: %d meshes, %.1f ms summed over Hydra threads\n", syncs, syncMs);
    }
    if (lightingApi.upsert && !packet.extendedLights.empty()) {
        HdueLightBatch update{sizeof(HdueLightBatch), uint32_t(packet.extendedLights.size()), packet.extendedLights.data()};
        backend.require(lightingApi.upsert(session, &update), "upsert_lights");
    }
    const auto& assetApi = backend.asset_api();
    if (!packet.sceneAssets.empty() && !assetApi.upsert) throw std::runtime_error("Backend has no native scene asset extension");
    if (assetApi.remove) {
        for (const auto& id : packet.removedIds) backend.require(assetApi.remove(session, id.c_str()), "remove_asset");
        for (const auto* id : packet.removedAssets) backend.require(assetApi.remove(session, id), "remove_asset");
    }
    for (const auto& desc : packet.sceneAssets) backend.require(assetApi.upsert(session, &desc), "upsert_asset");
    for (const auto& desc : packet.volumes) backend.require(backend.volume_api().upsert(session, &desc), "upsert_volume");
    for (const auto& update : packet.materialUpdates) backend.require(backend.material_api().set_parameters(session, &update), "set_material_parameters");
}
// Under the state lock again: records what reached UE.
void finishScene(HdUEState& state, const ScenePacket& packet, const HdueSurfaceStatus& surface) {
    for (const auto& id : packet.removedIds) {
        if (state.meshes.count(id)) { state.dirtyMeshes.insert(id); ++state.revision; continue; } // revived during the send
        state.geometryRevisions.erase(id); state.erasedMeshes.erase(id);
    }
    for (const auto& id : packet.genericMeshes) ++state.geometryRevisions[id];
    state.submittedMeshes += packet.meshes.size(); state.submittedMaterials += packet.materials.size();
    state.materialParameterUpdates += packet.materialUpdates.size();
    state.autoLightActive = packet.autoLight; state.viewerLights = packet.cameraLights + (packet.autoLight ? 2 : 0);
    if (packet.surfaceUpdate) state.surfaceStatus = surface;
    state.submittedNativeLights = packet.nativeLights;
    ++state.sceneSubmissions;
    state.submittedRevision = packet.revision; state.firstBatch = false;
}
// Uploads the scene changes since the last call. Runs on the UE thread; holds the state lock only to copy and record.
void submit(HdUEState& state, std::map<std::string, std::vector<GfVec3f>> points = {}) {
    const auto start = std::chrono::steady_clock::now();
    ScenePacket packet;
    packet.points = std::move(points);
    {
        std::lock_guard lock(state.mutex);
        try { prepareScene(state, packet); } catch (...) { packet.giveBack(state); throw; }
    }
    trace("hdUE scene prepare: %.1f ms under the state lock\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    HdueSurfaceStatus surface{sizeof(HdueSurfaceStatus)};
    try {
        sendScene(state, packet);
        if (packet.surfaceUpdate) {
            const auto& surfaceApi = state.backend->surface_api();
            HdueSurfaceBatch batch{sizeof(batch), uint32_t(packet.wet.size()), packet.wet.data()};
            state.backend->require(surfaceApi.apply(state.session, &batch), "apply_surface_effects");
            state.backend->require(surfaceApi.get_status(state.session, &surface), "surface_status");
        }
    } catch (...) {
        std::lock_guard lock(state.mutex); // not uploaded: dirty again, for the next attempt
        packet.giveBack(state);
        throw;
    }
    std::lock_guard lock(state.mutex);
    finishScene(state, packet, surface);
}
class Pass final : public HdRenderPass {
    std::shared_ptr<HdUEState> state_;
    GfMatrix4d view_{0.0}, projection_{0.0};
    GfVec4f lens_{0.f}; // fStop, focus distance, focal length, linear exposure scale of the requested camera
    // Motion blur: the beauty averages subframes_ converged renders at times spread over the shutter.
    int subframe_ = 0, subframes_ = 1;
    uint32_t subframeSamples_ = 0; // Path Tracer samples of the finished sub-frames
    bool subframeDue_ = false, motionApplied_ = false;
    std::vector<float> accum_;
    // Moves every moving mesh to frame-relative time t through hdue/motion_api.h; t = 0 restores the submitted pose.
    void applyMotion(HdUEState& s, float t) {
        if (!s.backend->motion_api().set_transforms || (!t && !motionApplied_)) return;
        const auto basis = sceneBasis(s);
        const auto inverse = basis.GetInverse();
        std::unique_lock lock(s.mutex);
        std::vector<std::vector<double>> storage;
        std::vector<const std::string*> ids;
        for (const auto& [id, mesh] : s.meshes) {
            if (!mesh.moving()) continue;
            const auto left = mesh.motionLeft.at(t), right = mesh.motionRight.at(t);
            const auto place = [&](GfMatrix4d m, const GfVec3f* velocity) {
                m = left * m * right;
                if (velocity) for (int c = 0; c < 3; ++c) m[3][c] += (*velocity)[c] * t;
                return m;
            };
            auto& out = storage.emplace_back();
            const auto push = [&](const GfMatrix4d& usd) { const auto m = inverse * usd * basis; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out.push_back(m[r][c]); };
            if (mesh.instanced) {
                for (size_t i = 0; i < mesh.instances.size(); ++i) push(place(mesh.instances[i], mesh.velocities.size() == mesh.instances.size() ? &mesh.velocities[i] : nullptr));
            } else if (!asset(s, "nativeMeshAssets", id).empty()) push(place(mesh.transform, nullptr));
            else push(mesh.transform.GetInverse() * place(mesh.transform, nullptr)); // generic meshes are submitted in world space
            ids.push_back(&id);
        }
        // Deforming meshes are resubmitted with their points at t, before set_transforms moves them.
        std::map<std::string, std::vector<GfVec3f>> deformed;
        for (const auto& [id, mesh] : s.meshes) if (!mesh.deformTimes.empty() && mesh.sceneAsset.empty()) {
            deformed[id] = mesh.pointsAt(t);
            s.dirtyMeshes.insert(id);
        }
        std::vector<std::string> names; names.reserve(ids.size());
        for (const auto* id : ids) names.push_back(*id);
        lock.unlock();
        if (!deformed.empty()) submit(s, std::move(deformed));
        std::vector<HdueMeshTransform> moves;
        for (size_t i = 0; i < names.size(); ++i) moves.push_back({names[i].c_str(), uint32_t(storage[i].size() / 16), storage[i].data()});
        if (!moves.empty()) s.backend->require(s.backend->motion_api().set_transforms(s.session, moves.data(), uint32_t(moves.size())), "set_transforms");
        motionApplied_ = t != 0;
    }
    uint64_t settingsVersion_ = ~uint64_t(0), lastFrame_ = 0;
    double centimetersPerUnit_ = 1;
    bool depthEnabled_ = false;
    std::atomic<bool> converged_{false};
    std::atomic<bool> executeQueued_{false};
    uint32_t skippedIdSize_[2] = {0, 0}; // size of the ID buffers last filled with -1 (viewport ID AOVs skipped) // Houdini's UI: an Execute posted to the UE thread and not yet started
    std::chrono::steady_clock::time_point lastRequestAt_{};
    // GBuffer AOVs render one by one after the beauty converges, from its camera.
    std::unique_ptr<HdUERenderBuffer> internalColor_; // beauty target when no color AOV is bound
    HdueRenderRequest beautyRequest_{};
    std::vector<std::pair<HdUERenderBuffer*, uint32_t>> aovs_;
    // hdUE:denoiser = oidn: the converged Path Tracer beauty waits for albedo and normal guides, then Open Image
    // Denoise (Houdini's, GPU when available) replaces it. Guides render as AOVs, internally when not bound.
    std::unique_ptr<HdUERenderBuffer> guideAlbedo_, guideNormal_;
    std::vector<float> beautyPixels_; // top-down RGBA of the converged beauty, before denoising
    HdueFrameInfo beautyFrame_{};
    bool denoisePending_ = false;
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
        for (const auto& [target, id] : aovs_) { if (id == HDUE_AOV_ALBEDO) albedo = target; if (id == HDUE_AOV_NORMAL) normal = target; }
        const auto albedoPixels = albedo ? topDown(*albedo, 3) : std::vector<float>();
        const auto normalPixels = normal ? topDown(*normal, 3) : std::vector<float>();
        const size_t width = beautyFrame_.width, height = beautyFrame_.height, count = width * height;
        static oidn::DeviceRef device = [] { auto d = oidn::newDevice(); d.commit(); return d; }();
        auto color = device.newBuffer(count * 4 * sizeof(float)), output = device.newBuffer(count * 4 * sizeof(float));
        color.write(0, count * 4 * sizeof(float), beautyPixels_.data());
        auto filter = device.newFilter("RT");
        filter.setImage("color", color, oidn::Format::Float3, width, height, 0, 4 * sizeof(float));
        filter.setImage("output", output, oidn::Format::Float3, width, height, 0, 4 * sizeof(float));
        oidn::BufferRef albedoBuffer, normalBuffer;
        if (albedoPixels.size() == count * 3) {
            albedoBuffer = device.newBuffer(count * 3 * sizeof(float)); albedoBuffer.write(0, count * 3 * sizeof(float), albedoPixels.data());
            filter.setImage("albedo", albedoBuffer, oidn::Format::Float3, width, height);
            if (normalPixels.size() == count * 3) {
                normalBuffer = device.newBuffer(count * 3 * sizeof(float)); normalBuffer.write(0, count * 3 * sizeof(float), normalPixels.data());
                filter.setImage("normal", normalBuffer, oidn::Format::Float3, width, height);
            }
            filter.set("cleanAux", true); // GBuffer guides are noise-free
        }
        filter.set("hdr", true);
        filter.commit();
        filter.execute();
        const char* message = nullptr;
        if (device.getError(message) != oidn::Error::None) throw std::runtime_error(std::string("Open Image Denoise: ") + (message ? message : "error"));
        std::vector<float> pixels(count * 4);
        output.read(0, count * 4 * sizeof(float), pixels.data());
        for (size_t i = 0; i < count; ++i) pixels[i * 4 + 3] = beautyPixels_[i * 4 + 3];
        HdUEToSceneLinear(pixels); // OCIO scene_linear (ACEScg in ACES pipelines); the tonemapper is never on here
        beauty.publish(beautyFrame_, std::move(pixels));
#else
        beauty.publish(beautyFrame_, beautyPixels_); // the standalone build has no denoiser
#endif
    }
    size_t aovSlot_ = 0;
    bool aovInFlight_ = false, aovsDone_ = true;
    void publishAov(HdUERenderBuffer& target, uint32_t id, const HdueFrameInfo& frame, const std::vector<float>& rgba) const {
        const size_t count = size_t(frame.width) * frame.height;
        if (id == HDUE_AOV_PRIM_ID || id == HDUE_AOV_INSTANCE_ID) {
            // Int32 buffers hold the integer bit patterns in the float storage.
            std::vector<float> out(count);
            for (size_t i = 0; i < count; ++i) {
                const float* p = rgba.empty() ? nullptr : &rgba[i * 4];
                const int32_t value = p && p[2] > .5f ? int32_t(std::lround(p[0])) * 2048 + int32_t(std::lround(p[1])) : -1;
                std::memcpy(&out[i], &value, sizeof(value));
            }
            target.publish(frame, std::move(out));
            return;
        }
        const bool vector = id == HDUE_AOV_ALBEDO || id == HDUE_AOV_NORMAL;
        const bool zUp = state_->setting<std::string>("upAxis", "Y") == "Z";
        std::vector<float> out(count * (vector ? 3 : 1));
        for (size_t i = 0; i < count; ++i) {
            const float* p = &rgba[i * 4];
            if (!vector) { out[i] = p[0]; continue; }
            float* o = &out[i * 3];
            // UE world normals back to USD world: the inverse of HdUEState::convert.
            if (id == HDUE_AOV_NORMAL && zUp) { o[0] = p[0]; o[1] = -p[1]; o[2] = p[2]; }
            else if (id == HDUE_AOV_NORMAL) { o[0] = p[1]; o[1] = p[2]; o[2] = -p[0]; }
            else { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
            // SceneTexture WorldNormal is not normalized; background pixels stay zero.
            if (const float length = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]); id == HDUE_AOV_NORMAL && length > 1e-6f)
                for (int c = 0; c < 3; ++c) o[c] /= length;
        }
        target.publish(frame, std::move(out));
    }
public:
    Pass(HdRenderIndex* index, const HdRprimCollection& collection, std::shared_ptr<HdUEState> state)
        : HdRenderPass(index, collection), state_(std::move(state)) {}
    // A posted Execute must not outlive the pass: wait for the UE thread to drain its queue.
    ~Pass() override {
        if (selfHost().continuous && selfHost().worker.joinable()) selfHost().run([this] { selfHost().pumps.erase(this); });
    }
    bool IsConverged() const override {
        if (const auto error=state_->setting<std::string>("wetValidationError", ""); !error.empty())
            return converged_ && state_->error==error;
        if (!state_->setting<bool>("materialGraphValid",true)) return true;
        return converged_ && (aovs_.empty() || aovsDone_) && !denoisePending_ &&
            settingsVersion_==state_->requestSettingsRevision && state_->submittedRevision==state_->revision;
    }
    // What Execute reads from Hydra's render pass state, copied on the calling thread: Houdini reuses that object for
    // its other drawing once Execute returns, so a posted Execute must not read it later.
    struct PassSnapshot {
        HdRenderPassAovBindingVector aovs; GfMatrix4d view, projection; const HdCamera* camera = nullptr;
        const HdRenderPassAovBindingVector& GetAovBindings() const { return aovs; }
        const GfMatrix4d& GetWorldToViewMatrix() const { return view; }
        const GfMatrix4d& GetProjectionMatrix() const { return projection; }
        const HdCamera* GetCamera() const { return camera; }
    };
    void _Execute(const HdRenderPassStateSharedPtr& state, const TfTokenVector& tags) override {
        const auto pass = std::make_shared<const PassSnapshot>(PassSnapshot{state->GetAovBindings(), state->GetWorldToViewMatrix(),
            state->GetProjectionMatrix(), state->GetCamera()});
        // husk polls Execute at its own pace and UE advances one tick per call; when this process owns UE,
        // keep ticking for a while so a realtime frame takes its GPU time rather than husk's polling interval.
        auto& host = selfHost();
        if (!host.worker.joinable()) { ueRun([&] { executeOnce(pass, tags); }); return; }
        if (host.continuous) {
            if (const auto error = host.error(); !error.empty()) {
                std::lock_guard lock(state_->mutex); state_->error = error; converged_ = true; return;
            }
            // Houdini's UI: post and return; Hydra calls again while not converged. A UE tick can take hundreds of ms
            // (scene uploads, ID-pass material swaps), which froze the UI while it waited.
            const auto eye = pass->view.GetInverse().ExtractTranslation();
            const bool skipped = executeQueued_.exchange(true);
            trace("hdUE execute posted=%d eye=(%.2f, %.2f, %.2f)\n", int(!skipped), eye[0], eye[1], eye[2]);
            if (skipped) return;
            host.post([this, pass, tags] {
                executeQueued_ = false; executeOnce(pass, tags);
                // Between Executes, keep publishing frames UE finishes for the last camera.
                selfHost().pumps[this] = [this, pass, tags] { if (!executeQueued_ && !IsConverged()) executeOnce(pass, tags); };
            });
            return;
        }
        const auto waitStart = std::chrono::steady_clock::now();
        const auto ran = std::make_shared<std::chrono::steady_clock::time_point>();
        struct Report { std::chrono::steady_clock::time_point start; std::shared_ptr<std::chrono::steady_clock::time_point> ran;
            ~Report() { using namespace std::chrono; const auto end = steady_clock::now();
                if (ran->time_since_epoch().count()) trace("hdUE execute call: waited %.1f ms for the UE thread, %.1f ms total\n",
                    duration<double, std::milli>(*ran - start).count(), duration<double, std::milli>(end - start).count()); } } report{waitStart, ran};
        host.run([&] {
            *ran = std::chrono::steady_clock::now();
            // Houdini's UI: UE ticks between calls on its thread, so submit and poll without holding the UI thread.
            if (host.continuous) { executeOnce(pass, tags); return; }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            do executeOnce(pass, tags);
            while (!IsConverged() && state_->error.empty() && std::chrono::steady_clock::now() < deadline);
        });
    }
    void executeOnce(const std::shared_ptr<const PassSnapshot>& pass, const TfTokenVector&) {
        // HDUE_TRACE_TIMING=1: one line per Execute that did work, with where its time went.
        static const bool timing = TfGetenvBool("HDUE_TRACE_TIMING", false);
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        double copyMs = 0, publishMs = 0, aovMs = 0;
        const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        HdueFrameInfo traced{};
        bool requested = false;
        try {
            auto& s = *state_;
            if (const auto error=s.setting<std::string>("wetValidationError", ""); !error.empty()) {
                // Preparation/undo must change the graph before rendering can
                // resume. Stop Hydra's update loop, retaining the displayed AOV
                // while keeping the invalid request out of completed-frame APIs.
                s.setError(error); s.lastFrame={}; converged_=true; return;
            }
#if defined(HDUE_HOUDINI)
            s.executeThread=GetCurrentThreadId();
#endif
            if (!selfHost().pumping) selfHost().tick(); // a second tick per loop doubled each frame's engine cost
            // Keep the last valid AOV while a prepared material graph is
            // invalid. Dirty USD data stays queued until preparation/undo.
            if (!s.setting<bool>("materialGraphValid", true)) { converged_ = true; return; }
            HdUERenderBuffer* buffer = nullptr;
            HdUERenderBuffer* depth = nullptr;
            std::vector<std::pair<HdUERenderBuffer*, uint32_t>> aovs;
            for (const auto& aov : pass->GetAovBindings()) {
                auto* renderBuffer=aov.renderBuffer;
                if (!renderBuffer && !aov.renderBufferId.IsEmpty())
                    renderBuffer=static_cast<HdRenderBuffer*>(GetRenderIndex()->GetBprim(HdPrimTypeTokens->renderBuffer,aov.renderBufferId));
                // husk names bindings after the render var's AOV name; a raw var's sourceName says what to render.
                const auto setting=[&](const char* key) {
                    const auto it=aov.aovSettings.find(TfToken(key));
                    if(it==aov.aovSettings.end()) return TfToken();
                    return it->second.IsHolding<TfToken>() ? it->second.UncheckedGet<TfToken>()
                        : it->second.IsHolding<std::string>() ? TfToken(it->second.UncheckedGet<std::string>()) : TfToken();
                };
                TfToken name=aov.aovName;
                const auto sourceType=setting("sourceType"), sourceName=setting("sourceName");
                // ponytail: every light path expression renders the full beauty; split lobes if LPE AOVs are needed.
                if(sourceType==TfToken("lpe")) name=HdAovTokens->color;
                else if(!sourceName.IsEmpty() && (sourceType.IsEmpty() || sourceType==TfToken("raw"))) name=sourceName;
                // Karma and RenderMan name the beauty Ci or C; treat them like Hydra's color.
                if(name==TfToken("Ci") || name==TfToken("C") || name==TfToken("beauty")) name=HdAovTokens->color;
                if(name==HdAovTokens->color) buffer=dynamic_cast<HdUERenderBuffer*>(renderBuffer);
                if(name==HdAovTokens->depth) depth=dynamic_cast<HdUERenderBuffer*>(renderBuffer);
                // Houdini's viewport binds primId/instanceId for its selection highlight. Rendering them swaps every
                // material twice per converged view (hundreds of ms each on large scenes), so the viewport skips them
                // unless HDUE_VIEWPORT_ID_AOVS=1; husk renders what its render vars ask for.
                static const bool viewportIds = TfGetenvBool("HDUE_VIEWPORT_ID_AOVS", false);
                const auto id = HdUEAovId(name);
                const bool skipped = selfHost().continuous && !viewportIds && (id == HDUE_AOV_PRIM_ID || id == HDUE_AOV_INSTANCE_ID);
                if(id && !skipped && dynamic_cast<HdUERenderBuffer*>(renderBuffer))
                    aovs.emplace_back(dynamic_cast<HdUERenderBuffer*>(renderBuffer),id);
                if(skipped) if(auto* target=dynamic_cast<HdUERenderBuffer*>(renderBuffer); target && target->GetWidth() &&
                    (target->GetWidth()!=skippedIdSize_[0] || target->GetHeight()!=skippedIdSize_[1])) {
                    // "No prim" everywhere, once per size, so Houdini never reads stale IDs.
                    HdueFrameInfo blank{}; blank.struct_size=sizeof(blank); blank.width=target->GetWidth(); blank.height=target->GetHeight(); blank.converged=1;
                    std::vector<float> ids(size_t(blank.width)*blank.height); const int32_t none=-1;
                    for(auto& value:ids) std::memcpy(&value,&none,sizeof(none));
                    target->publish(blank,std::move(ids));
                    skippedIdSize_[0]=target->GetWidth(); skippedIdSize_[1]=target->GetHeight();
                }
            }
            const bool denoising = s.setting<std::string>("renderMode", "realtime") == "pathtraced" &&
                TfStringToLower(s.setting<std::string>("denoiser", "off")) == "oidn";
            if (denoising && buffer && buffer->GetWidth()) {
                const auto guide = [&](std::unique_ptr<HdUERenderBuffer>& owned, uint32_t id, const char* path) {
                    if (std::any_of(aovs.begin(), aovs.end(), [&](const auto& aov) { return aov.second == id; })) return;
                    if (!owned || owned->GetWidth() != buffer->GetWidth() || owned->GetHeight() != buffer->GetHeight()) {
                        owned = std::make_unique<HdUERenderBuffer>(SdfPath(path));
                        owned->Allocate(GfVec3i(int(buffer->GetWidth()), int(buffer->GetHeight()), 1), HdFormatFloat32Vec3, false);
                    }
                    aovs.emplace_back(owned.get(), id);
                };
                guide(guideAlbedo_, HDUE_AOV_ALBEDO, "/__hdUE/denoiseAlbedo");
                guide(guideNormal_, HDUE_AOV_NORMAL, "/__hdUE/denoiseNormal");
            }
            if(aovs!=aovs_) { aovs_=aovs; aovSlot_=0; aovInFlight_=false; aovsDone_=aovs_.empty(); }
            if(!buffer && !aovs_.empty() && aovs_.front().first->GetWidth()) {
                const auto* first=aovs_.front().first;
                if(!internalColor_ || internalColor_->GetWidth()!=first->GetWidth() || internalColor_->GetHeight()!=first->GetHeight()) {
                    internalColor_=std::make_unique<HdUERenderBuffer>(SdfPath("/__hdUE/beauty"));
                    internalColor_->Allocate(GfVec3i(int(first->GetWidth()),int(first->GetHeight()),1),HdFormatFloat32Vec4,false);
                }
                buffer=internalColor_.get();
            }
            if (!buffer && std::any_of(pass->GetAovBindings().begin(), pass->GetAovBindings().end(), [&](const HdRenderPassAovBinding& aov) {
                    const auto* b = aov.renderBuffer ? aov.renderBuffer : static_cast<HdRenderBuffer*>(GetRenderIndex()->GetBprim(HdPrimTypeTokens->renderBuffer, aov.renderBufferId));
                    return b && b->GetWidth(); })) {
                // Allocated outputs, none of them renderable: fail the frame instead of leaving husk waiting.
                std::string names;
                for (const auto& aov : pass->GetAovBindings()) names += " " + aov.aovName.GetString();
                if (s.error.empty()) TF_RUNTIME_ERROR("hdUE renders none of the requested render vars:%s", names.c_str());
                s.setError("hdUE renders none of the requested render vars:" + names); converged_ = true; aovsDone_ = true; return;
            }
            if (!buffer || !buffer->GetWidth() || !buffer->GetHeight()) {
                std::string bindings;
                for (const auto& aov:pass->GetAovBindings()) bindings+=" "+aov.aovName.GetString()+":"+aov.renderBufferId.GetString();
                throw std::runtime_error("hdUE requires an allocated float32 RGBA color AOV; bindings:"+bindings+
                    (buffer?" allocation: "+buffer->allocationError():" (missing buffer)"));
            }
            bool changed = s.locked([&] { return s.submittedRevision != s.revision; });
            // A new size keeps the session and its scene when the runtime can resize in place (leaving a camera in
            // Houdini changes the size); recreating it re-sent every mesh, material and texture.
            if (s.session && (s.width != buffer->GetWidth() || s.height != buffer->GetHeight())) {
                const auto resized = s.backend->resize_session(s.session, buffer->GetWidth(), buffer->GetHeight());
                trace("hdUE resize %ux%u -> %ux%u: result %d\n", s.width, s.height, buffer->GetWidth(), buffer->GetHeight(), int(resized));
                if (resized != HDUE_OK) goto recreate;
                s.width = buffer->GetWidth(); s.height = buffer->GetHeight();
                s.lastFrame = {}; lastFrame_ = 0; settingsVersion_ = ~uint64_t(0); // render the new size from scratch
            }
            recreate:
            if (!s.session || s.width != buffer->GetWidth() || s.height != buffer->GetHeight()) {
                trace("hdUE create session %ux%u (had session %d)\n", buffer->GetWidth(), buffer->GetHeight(), int(s.session != 0));
                if (s.session) s.backend->require(s.backend->api().destroy_session(s.session), "destroy_session");
                HdueSessionDesc desc{}; desc.struct_size = sizeof(desc); desc.width = buffer->GetWidth(); desc.height = buffer->GetHeight();
                s.backend->require(s.backend->api().create_session(&desc, &s.session), "create_session");
                s.width = desc.width; s.height = desc.height; s.firstBatch = true;
                s.geometryRevisions.clear(); s.surfaceStatus={sizeof(HdueSurfaceStatus)};
                depthEnabled_=false;
                for (const auto& [id, data] : s.meshes) s.dirtyMeshes.insert(id);
                for (const auto& [id, data] : s.materials) s.dirtyMaterials.insert(id);
                for (const auto& [id, data] : s.lights) s.dirtyLights.insert(id);
                changed = true;
            }
            if(depth && !depthEnabled_) {
                if(!s.backend->depth_api().set_enabled) throw std::runtime_error("Solaris requires the updated backend depth extension; run Update-HoudiniRuntime.ps1");
                s.backend->require(s.backend->depth_api().set_enabled(s.session,1),"enable_depth");
                depthEnabled_=true; changed=true;
            }
            if (changed) { HdUERefreshWetMasks(s,GetRenderIndex()); submit(s); }
            if (s.backend->asset_api().get_scene_status) {
                HdueAssetSceneStatus assets{}; assets.struct_size=sizeof(assets);
                s.backend->require(s.backend->asset_api().get_scene_status(s.session,&assets),"asset_scene_status");
                const bool lightsChanged=assets.lights != s.submittedNativeLights;
                s.nativeAssets=assets;
                if (assets.loading) { s.error.clear(); s.assetRequestPending=true; converged_=false; return; }
                if (assets.failed) { s.setError(assets.message); s.assetRequestPending=true; converged_=true; return; }
                if (lightsChanged) { s.locked([&] { ++s.revision; }); submit(s); changed=true; }
            }
            if (s.assetRequestPending) { changed=true; s.assetRequestPending=false; }
            const auto view = pass->GetWorldToViewMatrix();
            const auto projection = pass->GetProjectionMatrix();
            const HdCamera* hdCamera = pass->GetCamera();
            const GfVec4f lens = hdCamera ? GfVec4f(hdCamera->GetFStop(), hdCamera->GetFocusDistance(), hdCamera->GetFocalLength(), hdCamera->GetLinearExposureScale())
                                          : GfVec4f(0.f, 0.f, 0.f, 1.f);
            // Sub-frame times are spread over the camera shutter; the camera moves with its own samples.
            std::optional<HdUEMotionSamples> cameraMotion;
            if (hdCamera) {
                std::lock_guard cameraLock(s.cameraMutex);
                if (const auto it = s.cameraMotion.find(hdCamera->GetId().GetString()); it != s.cameraMotion.end()) cameraMotion = it->second;
            }
            const auto subframeTime = [&](int k) {
                return float(hdCamera->GetShutterOpen() + (k + .5) / subframes_ * (hdCamera->GetShutterClose() - hdCamera->GetShutterOpen()));
            };
            const auto viewAt = [&](float t) {
                if (subframes_ < 2 || !cameraMotion) return view;
                return view * cameraMotion->at(0) * cameraMotion->at(t).GetInverse();
            };
            const auto settingsVersion = s.requestSettingsRevision;
            // Consume a completed image before changing its generation. Otherwise
            // continuous camera input can discard every image before presentation.
            HdueFrameInfo frame{}; frame.struct_size = sizeof(frame);
            const auto result = s.backend->api().poll_frame(s.session, &frame);
            if (result == HDUE_OK && frame.generation == s.generation && frame.frame_id != lastFrame_) {
                const auto copyStart = Clock::now();
                std::vector<float> pixels = buffer->takeSpare(size_t(frame.required_bytes / sizeof(float)));
                const auto copy = s.backend->api().copy_frame(s.session, frame.frame_id, pixels.data(), frame.required_bytes);
                copyMs = ms(copyStart, Clock::now()); traced = frame;
                std::vector<float> depthPixels;
                HdueResult depthCopy=HDUE_OK;
                if(copy==HDUE_OK && depth) {
                    depthPixels=depth->takeSpare(size_t(frame.width)*frame.height);
                    depthCopy=s.backend->depth_api().copy_depth(s.session,frame.frame_id,depthPixels.data(),depthPixels.size()*sizeof(float));
                }
                if (copy == HDUE_STALE_FRAME || depthCopy==HDUE_STALE_FRAME) {
                    // The Render Thread may publish another completed frame
                    // between poll and copy. Retry on the next Execute without
                    // blocking or publishing bytes from the wrong frame.
                    ++s.staleFrameRetries;
                } else {
                    s.backend->require(copy, "copy_frame");
                    if(depth) {
                        s.backend->require(depthCopy,"copy_depth");
                        WorkParallelForN(depthPixels.size(), [&](size_t begin, size_t end) {
                            for (size_t i = begin; i < end; ++i) {
                                float& value = depthPixels[i];
#if defined(HDUE_HOUDINI)
                                value=value>0 && std::isfinite(value) ? float(value/centimetersPerUnit_) : std::numeric_limits<float>::max();
#else
                                value=HdUEProjectDepth(value,projection_,centimetersPerUnit_);
#endif
                            }
                        });
                        depth->publish(frame,std::move(depthPixels));
                    }
                    if (subframes_ > 1 && frame.converged) {
                        // One converged sub-frame: add it and publish the running average.
                        if (accum_.size() != pixels.size()) accum_.assign(pixels.size(), 0.f);
                        for (size_t i = 0; i < pixels.size(); ++i) accum_[i] += pixels[i];
                        ++subframe_;
                        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = accum_[i] / float(subframe_);
                        subframeSamples_ += frame.completed_samples; frame.completed_samples = subframeSamples_;
                        frame.converged = subframe_ >= subframes_;
                        subframeDue_ = !frame.converged;
                        if (frame.converged) applyMotion(s, 0);
                    } else if (subframes_ > 1) lastFrame_ = frame.frame_id; // an unfinished sub-frame stays out of the average
                    if (frame.frame_id != lastFrame_) {
                        if (lens_[3] != 1.f && !s.setting<bool>("tonemapper", false)) // camera exposure, as Karma applies it to the beauty only
                            for (size_t i = 0; i + 3 < pixels.size(); i += 4) { pixels[i] *= lens_[3]; pixels[i + 1] *= lens_[3]; pixels[i + 2] *= lens_[3]; }
                        if (denoising && frame.converged) { beautyPixels_ = pixels; beautyFrame_ = frame; denoisePending_ = true; }
                        const auto publishStart = Clock::now();
                        // UE renders linear Rec.709; hosts expect the OCIO scene_linear role. Tonemapped output is display-referred.
                        if (!s.setting<bool>("tonemapper", false)) HdUEToSceneLinear(pixels);
                        buffer->publish(frame, std::move(pixels));
                        publishMs = ms(publishStart, Clock::now());
                        ++s.publishedFrames;
                        s.lastFrame = frame; lastFrame_ = frame.frame_id;
                    }
                }
            } else if (result != HDUE_PENDING && result != HDUE_OK) s.backend->require(result, "poll_frame");
            if (aovInFlight_) {
                // AOV frames have their own slot (hdue/aov_api.h); the beauty above is untouched.
                HdueFrameInfo aovFrame{}; aovFrame.struct_size = sizeof(aovFrame);
                const auto polled = s.backend->aov_api().poll_aov(s.session, &aovFrame);
                if (polled == HDUE_OK && aovFrame.generation == s.aovGeneration && aovFrame.converged && aovSlot_ < aovs_.size()) {
                    const auto aovStart = Clock::now();
                    std::vector<float> pixels(size_t(aovFrame.required_bytes / sizeof(float)));
                    const auto copied = s.backend->api().copy_frame(s.session, aovFrame.frame_id, pixels.data(), aovFrame.required_bytes);
                    aovMs = ms(aovStart, Clock::now());
                    if (copied == HDUE_STALE_FRAME) ++s.staleFrameRetries;
                    else {
                        s.backend->require(copied, "copy_frame");
                        publishAov(*aovs_[aovSlot_].first, aovs_[aovSlot_].second, aovFrame, pixels);
                        aovInFlight_ = false; ++aovSlot_;
                    }
                } else if (polled != HDUE_PENDING && polled != HDUE_OK) s.backend->require(polled, "poll_aov");
            }
            converged_ = s.lastFrame.frame_id && s.lastFrame.converged;
            const bool settingsChanged = settingsVersion != settingsVersion_;
            const bool cameraChanged = view != view_ || projection != projection_ || lens != lens_;
            const bool needsRequest = changed || settingsChanged || cameraChanged;
            if (TfGetenvBool("HDUE_TRACE_PASS", false)) {
                std::fprintf(stderr, "hdUE pass changed=%d settings=%d camera=%d converged=%d frame=%llu gen=%llu buffer=%ux%u aovs=%zu slot=%zu inflight=%d done=%d error=%s\n",
                    changed, settingsChanged, cameraChanged, int(converged_.load()), (unsigned long long)s.lastFrame.frame_id, (unsigned long long)s.generation,
                    buffer->GetWidth(), buffer->GetHeight(), aovs_.size(), aovSlot_, aovInFlight_, aovsDone_, s.error.c_str());
                std::fflush(stderr);
            }
            const bool deferCamera = needsRequest && !changed && !settingsChanged && s.generation && !s.lastFrame.frame_id;
            if (deferCamera) {
                // Read the newest matrices on the next Execute; never queue every
                // mouse event or invalidate an in-flight camera image repeatedly.
                ++s.coalescedCameraUpdates;
                converged_ = false;
            } else if (needsRequest || subframeDue_) {
              requested = true; lastRequestAt_ = Clock::now();
              const auto build = [&](const GfMatrix4d& view) {
                HdueRenderRequest request{};
                request.struct_size = sizeof(request); request.generation = std::max(s.generation, s.aovGeneration)+1;
                request.mode = s.setting<std::string>("renderMode", "realtime") == "pathtraced" ? HDUE_PATH_TRACED : HDUE_REALTIME;
                request.samples_per_pixel = uint32_t(std::max(1, s.setting<int>("samplesPerPixel", 64)));
                request.warmup_frames = uint32_t(std::max(1, s.setting<int>("warmupFrames", 16)));
                request.lumen_enabled = s.setting<bool>("lumenEnabled", true);
                request.time_seconds = s.setting<double>("timeSeconds", 0);
                const auto camera = view.GetInverse();
                const auto eye = s.convert(camera.ExtractTranslation());
                const auto target = s.convert(camera.Transform(GfVec3d(0, 0, -1)));
                const auto up = s.convert(camera.TransformDir(GfVec3d(0, 1, 0)), true);
                for (int c = 0; c < 3; ++c) { request.eye[c] = eye[c]; request.target[c] = target[c]; request.up[c] = up[c]; }
                request.horizontal_fov_degrees = float(2 * std::atan(1.0 / projection[0][0]) * 180 / 3.14159265358979323846);
                if (TfGetenvBool("HDUE_TRACE_SCENE", false))
                    TF_STATUS("hdUE camera %s eye=(%g,%g,%g) target=(%g,%g,%g) fov=%g size=%ux%u units=%g axis=%s",
                        pass->GetCamera() ? pass->GetCamera()->GetId().GetText() : "none",
                        eye[0], eye[1], eye[2], target[0], target[1], target[2], request.horizontal_fov_degrees,
                        s.width, s.height, s.setting<double>("metersPerUnit", .01), s.setting<std::string>("upAxis", "Y").c_str());
                const double cmPerUnit = s.setting<double>("metersPerUnit", 0.01) * 100;
                request.near_clip_cm = hdCamera ? float(hdCamera->GetClippingRange().GetMin() * cmPerUnit) : 0.1f;
                // Hydra's projection already holds the conformed window and aperture offsets (GfFrustum layout).
                if (projection[3][3] == 1) {
                    request.orthographic = 1;
                    request.projection_scale[0] = projection[0][0] / cmPerUnit; request.projection_scale[1] = projection[1][1] / cmPerUnit;
                    request.projection_offset[0] = projection[3][0]; request.projection_offset[1] = projection[3][1];
                    request.near_clip_cm = float((projection[3][2] + 1) / projection[2][2] * cmPerUnit);
                    request.far_clip_cm = float((projection[3][2] - 1) / projection[2][2] * cmPerUnit);
                    request.horizontal_fov_degrees = 90;
                } else {
                    request.projection_scale[0] = projection[0][0]; request.projection_scale[1] = projection[1][1];
                    request.projection_offset[0] = -projection[2][0]; request.projection_offset[1] = -projection[2][1];
                }
                if (lens[0] > 0 && lens[1] > 0) {
                    request.fstop = lens[0]; request.focus_distance_cm = float(lens[1] * cmPerUnit); request.focal_length_cm = float(lens[2] * cmPerUnit); // world units, as Hydra and Karma read USD lens attributes
                }
                return request;
              };
                if (needsRequest && s.backend->postprocess_api().set_post_process) {
                    // hdUE:postProcess:<FPostProcessSettings property>, hdUE:postProcessMaterials, hdUE:tonemapper.
                    // With the tonemapper on, the camera exposure goes in before it rather than onto display values.
                    std::vector<std::string> names, values, materials;
                    const bool tonemapper = s.setting<bool>("tonemapper", false);
                    const auto settings = s.settingsSnapshot();
                    for (const auto& [key, value] : settings) {
                        if (!TfStringStartsWith(key.GetString(), "postProcess:")) continue;
                        names.push_back(key.GetString().substr(12));
                        values.push_back(value.IsHolding<bool>() ? (value.UncheckedGet<bool>() ? "True" : "False") : TfStringify(value));
                    }
                    if (tonemapper) {
                        // The USD camera exposure adds to any authored AutoExposureBias; UE's own physical camera
                        // exposure (ISO, shutter, aperture) stays off unless the scene asks for it.
                        const float stops = std::log2(std::max(lens[3], 1e-6f));
                        const auto bias = std::find(names.begin(), names.end(), "AutoExposureBias");
                        if (bias == names.end()) { names.push_back("AutoExposureBias"); values.push_back(TfStringify(stops)); }
                        else values[bias - names.begin()] = TfStringify(std::strtof(values[bias - names.begin()].c_str(), nullptr) + stops);
                        if (std::find(names.begin(), names.end(), "AutoExposureApplyPhysicalCameraExposure") == names.end()) {
                            names.push_back("AutoExposureApplyPhysicalCameraExposure"); values.push_back("False");
                        }
                    }
                    const auto list = settings.find(TfToken("postProcessMaterials"));
                    if (list != settings.end()) {
                        if (list->second.IsHolding<VtStringArray>()) for (const auto& m : list->second.UncheckedGet<VtStringArray>()) materials.push_back(m);
                        else if (list->second.IsHolding<VtTokenArray>()) for (const auto& m : list->second.UncheckedGet<VtTokenArray>()) materials.push_back(m.GetString());
                        else for (const auto& m : TfStringTokenize(s.setting<std::string>("postProcessMaterials", ""), " ,;")) materials.push_back(m);
                    }
                    std::vector<const char*> namePtrs, valuePtrs, materialPtrs;
                    for (size_t i = 0; i < names.size(); ++i) { namePtrs.push_back(names[i].c_str()); valuePtrs.push_back(values[i].c_str()); }
                    for (const auto& m : materials) materialPtrs.push_back(m.c_str());
                    const HduePostProcessDesc desc{sizeof(HduePostProcessDesc), namePtrs.data(), valuePtrs.data(), uint32_t(names.size()),
                        materialPtrs.data(), uint32_t(materials.size()), tonemapper ? 1 : 0};
                    s.backend->require(s.backend->postprocess_api().set_post_process(s.session, &desc), "set_post_process");
                }
                if (needsRequest) {
                    applyMotion(s, 0);
                    // Motion blur needs a shutter, sub-frames and something moving.
                    const int samples = std::max(1, s.setting<int>("motionSamples", 16));
                    const bool shutter = hdCamera && hdCamera->GetShutterClose() > hdCamera->GetShutterOpen();
                    const bool moving = cameraMotion.has_value() ||
                        s.locked([&] { return std::any_of(s.meshes.begin(), s.meshes.end(), [](const auto& entry) { return entry.second.moving(); }); });
                    subframes_ = s.setting<bool>("motionBlur", true) && shutter && moving && samples > 1 && s.backend->motion_api().set_transforms ? samples : 1;
                    subframe_ = 0; subframeSamples_ = 0; accum_.clear();
                }
                subframeDue_ = false;
                auto request = build(viewAt(subframes_ > 1 ? subframeTime(subframe_) : 0.f));
                if (subframes_ > 1) {
                    applyMotion(s, subframeTime(subframe_));
                    request.samples_per_pixel = std::max(1u, (request.samples_per_pixel + uint32_t(subframes_) - 1) / uint32_t(subframes_));
                    // Lumen keeps its history between sub-frames, so the realtime warmup is shared too.
                    request.warmup_frames = std::max(2u, (request.warmup_frames + uint32_t(subframes_) - 1) / uint32_t(subframes_));
                }
                const auto requestStart = std::chrono::steady_clock::now();
                const auto requested=s.backend->api().request_render(s.session, &request);
                if(requested==HDUE_PENDING) {
                    if (!needsRequest) { subframeDue_ = true; converged_ = false; s.error.clear(); return; } // retry the sub-frame
                    s.assetRequestPending=true; converged_=false; s.lastFrame={};
                    buffer->invalidate(); if(depth) depth->invalidate(); s.error.clear(); return;
                }
                s.backend->require(requested, "request_render");
                s.generation=request.generation;
                s.requestCpuMsTotal += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requestStart).count();
                converged_ = false;
                if (needsRequest) {
                    view_ = view; projection_ = projection; lens_ = lens; settingsVersion_ = settingsVersion;
                    s.submittedRequestSettingsRevision=settingsVersion;
                    centimetersPerUnit_=s.setting<double>("metersPerUnit",.01)*100.;
                    s.lastFrame = {}; buffer->invalidate();
                    if(depth) depth->invalidate();
                    beautyRequest_ = build(view); // AOVs render the unblurred pose at the current time
                    aovSlot_ = 0; aovInFlight_ = false; aovsDone_ = aovs_.empty(); denoisePending_ = false;
                    for (const auto& [aovBuffer, id] : aovs_) aovBuffer->invalidate();
                }
            }
            // In Houdini's UI the AOV passes (Solaris' primId/instanceId swap every material, ~300 ms each) wait until the
            // camera has been still for 1 s, so navigation never pays for them.
            const bool still = !selfHost().continuous || Clock::now() - lastRequestAt_ > std::chrono::milliseconds(1000);
            if (!needsRequest && converged_ && !aovsDone_ && !aovInFlight_ && still) {
                if (aovSlot_ >= aovs_.size()) aovsDone_ = true;
                else {
                    // Same camera and time as the beauty; GBuffer channels converge after a couple of draws.
                    auto request = beautyRequest_;
                    if (!s.backend->aov_api().poll_aov) throw std::runtime_error("This runtime has no AOV extension; build and stage the current backend");
                    request.generation = std::max(s.generation, s.aovGeneration) + 1; request.mode = HDUE_REALTIME; request.lumen_enabled = 0;
                    request.warmup_frames = 2; request.aov = aovs_[aovSlot_].second;
                    const auto requested = s.backend->api().request_render(s.session, &request);
                    if (requested != HDUE_PENDING) {
                        s.backend->require(requested, "request_render");
                        s.aovGeneration = request.generation; aovInFlight_ = true;
                    }
                }
            }
            if (denoisePending_ && aovsDone_) { denoise(*buffer); denoisePending_ = false; }
            { std::lock_guard lock(s.mutex); s.error.clear(); }
            const double total = ms(start, Clock::now());
            if (timing && (traced.frame_id || requested || aovMs > 0 || total > 5)) {
                trace("hdUE timing: execute %.1f ms | request %d | frame %llu %ux%u converged %u (UE request->frame %.1f ms, readback %.1f ms) "
                    "copy %.1f ms publish %.1f ms | aov slot %zu/%zu copy+publish %.1f ms\n", total, int(requested),
                    (unsigned long long)traced.frame_id, traced.width, traced.height, traced.converged, traced.request_to_frame_ms, traced.readback_ms,
                    copyMs, publishMs, aovSlot_, aovs_.size(), aovMs);
                std::fflush(stderr);
            }
        } catch (const std::exception& error) {
            if (TfGetenvBool("HDUE_TRACE_PASS", false)) { std::fprintf(stderr, "hdUE pass error: %s\n", error.what()); std::fflush(stderr); }
            std::lock_guard lock(state_->mutex); state_->error = error.what(); converged_ = false;
        }
    }
};
}

HdUERenderDelegate::HdUERenderDelegate(const HdRenderSettingsMap& settings)
    : HdRenderDelegate(settings), state_(std::make_shared<HdUEState>()), registry_(std::make_shared<HdResourceRegistry>()) {
    // Before the backend DLL loads: UE changes the working directory while it loads and starts.
    const auto hostDirectory = std::filesystem::current_path();
    state_->settings = settings;
    for(const auto& [key,value]:settings) if(key.GetString().starts_with("hdUE:"))
        state_->settings[TfToken(key.GetString().substr(5))]=value;
    state_->wetBindings=std::make_shared<const VtDictionary>(state_->setting<VtDictionary>("surfaceEffects",{}));
    const auto stageUnits=settings.find(TfToken("stageMetersPerUnit"));
    if (stageUnits!=settings.end() && stageUnits->second.IsHolding<double>())
        state_->settings[TfToken("metersPerUnit")]=stageUnits->second;
    auto dll = state_->setting<std::string>("backendLibrary", TfGetenv("HDUE_BACKEND_DLL"));
    if (dll.empty()) dll = runtimeConfig("backend_dll");
    if (dll.empty()) throw std::runtime_error("hdUE requires backendLibrary, HDUE_BACKEND_DLL or HDUE_RUNTIME_CONFIG");
    state_->backend = std::make_shared<hdue::Backend>(std::filesystem::u8path(dll));
    auto& host = selfHost();
    const auto rendererInfo = [state = state_] {
        if (state->backend->material_api().get_renderer_info)
            state->backend->require(state->backend->material_api().get_renderer_info(&state->rendererInfo), "get_renderer_info");
    };
    if (host.worker.joinable()) {
        // UE runs (or boots) on hdUE's thread.
        if (const auto error = host.error(); !error.empty()) throw std::runtime_error(error);
        ++host.delegates; selfHosted_ = true;
        if (host.continuous) host.post(rendererInfo); else host.run(rendererInfo);
    } else {
        HdueStatus status{}; status.struct_size = sizeof(status);
        ueRun([&] { state_->backend->require(state_->backend->api().get_status(&status), "get_status"); });
        if (status.state == HDUE_STATE_COLD) {
            // This process owns UE. Houdini's UI does not wait for the boot: Execute's work queues behind it, and
            // the viewport shows Hydra's progress until frames arrive.
            host.directory = hostDirectory;
            const bool wait = !(TfGetenvBool("HDUE_HOUDINI_UI", false) || houdiniUi());
            startSelfHost(dll, state_->setting<std::string>("runtimeProject", runtimeConfig("runtime_project")), wait);
            std::error_code ignored; std::filesystem::current_path(host.directory, ignored);
            ++host.delegates; selfHosted_ = true;
            if (host.continuous) host.post(rendererInfo); else host.run(rendererInfo);
        } else {
            // A host (usdview, or a test) started UE and ticks it.
            if (status.state != HDUE_STATE_READY || !status.world_ready || !status.rhi_ready)
                throw std::runtime_error("hdUE requires a ready runtime; launch usdview with scripts/Launch-Usdview.ps1");
            ueRun(rendererInfo);
        }
    }
#if defined(HDUE_HOUDINI)
    state_->createThread=GetCurrentThreadId();
    if (!ownerQueue().owner || ownerQueue().owner == state_->createThread) HdUERegisterViewport(state_,this);
#endif
    for (const auto& desc : GetRenderSettingDescriptors()) {
        if (!state_->settings.count(desc.key)) state_->settings[desc.key]=desc.defaultValue;
        HdRenderDelegate::SetRenderSetting(desc.key,state_->settings[desc.key]);
    }
}
HdUERenderDelegate::~HdUERenderDelegate() {
#if defined(HDUE_HOUDINI)
    HdUEUnregisterViewport(this);
#endif
    auto& host = selfHost();
    if (!selfHosted_) {
        try {
            ueRun([&] {
                std::lock_guard lock(state_->mutex);
                if (state_->session) { state_->backend->api().destroy_session(state_->session); state_->session = 0; }
            });
        } catch (const std::exception& e) { TF_RUNTIME_ERROR("hdUE: %s", e.what()); }
        return;
    }
    if (--host.delegates > 0) return;
    if (host.continuous) {
        // Houdini's UI keeps UE for the next viewport (one UE lifetime per process); hdue_shutdown_ue ends it.
        host.run([&] {
            std::lock_guard lock(state_->mutex);
            if (state_->session) { state_->backend->api().destroy_session(state_->session); state_->session = 0; }
        });
        return;
    }
    // The session must end while UE still runs; UE allows one lifetime per process.
    host.run([&] {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->session) { state_->backend->api().destroy_session(state_->session); state_->session = 0; }
        }
        try { host.tick(); host.backend->shutdown(); } catch (const std::exception& e) { TF_RUNTIME_ERROR("hdUE shutdown: %s", e.what()); }
        if (host.window) DestroyWindow(host.window);
        host.window = nullptr;
    });
    host.stop();
    host.backend.reset();
    // husk may write its images after releasing the delegate.
    std::error_code ignored; std::filesystem::current_path(host.directory, ignored);
}
const TfTokenVector& HdUERenderDelegate::GetSupportedRprimTypes() const {
#if defined(HDUE_HOUDINI)
    static TfTokenVector t{HdPrimTypeTokens->mesh, HdPrimTypeTokens->points, HdPrimTypeTokens->basisCurves, HdPrimTypeTokens->volume};
#else
    static TfTokenVector t{HdPrimTypeTokens->mesh, HdPrimTypeTokens->points, HdPrimTypeTokens->basisCurves};
#endif
    return t;
}
const TfTokenVector& HdUERenderDelegate::GetSupportedSprimTypes() const { static TfTokenVector t{HdPrimTypeTokens->camera, HdPrimTypeTokens->material,
    HdPrimTypeTokens->distantLight, HdPrimTypeTokens->sphereLight, HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->domeLight,HdPrimTypeTokens->diskLight,HdPrimTypeTokens->cylinderLight}; return t; }
const TfTokenVector& HdUERenderDelegate::GetSupportedBprimTypes() const {
#if defined(HDUE_HOUDINI)
    static TfTokenVector t{HdPrimTypeTokens->renderBuffer, TfToken("openvdbAsset"), TfToken("houdiniFieldAsset")};
#else
    static TfTokenVector t{HdPrimTypeTokens->renderBuffer};
#endif
    return t;
}
HdRenderPassSharedPtr HdUERenderDelegate::CreateRenderPass(HdRenderIndex* index, const HdRprimCollection& collection) {
    state_->renderIndex=index; return std::make_shared<Pass>(index, collection, state_);
}
HdInstancer* HdUERenderDelegate::CreateInstancer(HdSceneDelegate* delegate, const SdfPath& id) { return HdUECreateInstancer(delegate, id); }
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
    if (type == HdPrimTypeTokens->distantLight) return HdUECreateLight(id, HDUE_DISTANT_LIGHT);
    if (type == HdPrimTypeTokens->sphereLight) return HdUECreateLight(id, HDUE_SPHERE_LIGHT);
    if (type == HdPrimTypeTokens->rectLight) return HdUECreateLight(id, HDUE_RECT_LIGHT);
    if (type == HdPrimTypeTokens->domeLight) return HdUECreateLight(id,HDUE_DOME_LIGHT);
    if (type == HdPrimTypeTokens->diskLight) return HdUECreateLight(id,HDUE_DISK_LIGHT);
    if (type == HdPrimTypeTokens->cylinderLight) return HdUECreateLight(id,HDUE_CYLINDER_LIGHT);
    return nullptr;
}
HdSprim* HdUERenderDelegate::CreateFallbackSprim(const TfToken& type) { return CreateSprim(type, SdfPath::EmptyPath()); }
void HdUERenderDelegate::DestroySprim(HdSprim* p) { delete p; }
HdBprim* HdUERenderDelegate::CreateBprim(const TfToken& type, const SdfPath& id) {
#if defined(HDUE_HOUDINI)
    if (type == TfToken("openvdbAsset") || type == TfToken("houdiniFieldAsset")) return HdUECreateField(id);
#endif
    return type == HdPrimTypeTokens->renderBuffer ? new HdUERenderBuffer(id) : nullptr;
}
HdBprim* HdUERenderDelegate::CreateFallbackBprim(const TfToken& type) { return CreateBprim(type, SdfPath::EmptyPath()); }
void HdUERenderDelegate::DestroyBprim(HdBprim* p) { delete p; }
HdAovDescriptor HdUERenderDelegate::GetDefaultAovDescriptor(const TfToken& name) const {
    if(name==HdAovTokens->color) return HdAovDescriptor(HdFormatFloat32Vec4,false,VtValue(GfVec4f(0)));
    if(const auto aov=HdUEAovId(name); aov==HDUE_AOV_PRIM_ID || aov==HDUE_AOV_INSTANCE_ID)
        return HdAovDescriptor(HdFormatInt32,false,VtValue(-1));
    if(const auto aov=HdUEAovId(name)) return aov==HDUE_AOV_ALBEDO || aov==HDUE_AOV_NORMAL
        ? HdAovDescriptor(HdFormatFloat32Vec3,false,VtValue(GfVec3f(0))) : HdAovDescriptor(HdFormatFloat32,false,VtValue(0.f));
#if defined(HDUE_HOUDINI)
    if(name==HdAovTokens->depth) return HdAovDescriptor(HdFormatFloat32,false,VtValue(std::numeric_limits<float>::max()));
#endif
    return HdAovDescriptor();
}
HdRenderSettingDescriptorList HdUERenderDelegate::GetRenderSettingDescriptors() const { return {
    {"Render mode", TfToken("renderMode"), VtValue(std::string("realtime"))},
    {"Samples per pixel", TfToken("samplesPerPixel"), VtValue(64)},
    {"Warmup frames", TfToken("warmupFrames"), VtValue(16)}, {"Lumen", TfToken("lumenEnabled"), VtValue(true)},
    {"Viewer light intensity", TfToken("viewerLightIntensity"), VtValue(4.f)},
    {"Environment cube resolution",TfToken("domeResolution"),VtValue(512)} }; }
void HdUERenderDelegate::SetRenderSetting(const TfToken& key, const VtValue& value) {
    if (TfGetenvBool("HDUE_TRACE_SETTINGS", false)) { std::fprintf(stderr, "hdUE setting %s = %s\n", key.GetText(), TfStringify(value).c_str()); std::fflush(stderr); }
    if(key.GetString().starts_with("hdUE:")) { SetRenderSetting(TfToken(key.GetString().substr(5)),value); return; }
    if (key==TfToken("stageMetersPerUnit")) {
        SetRenderSetting(TfToken("metersPerUnit"),value); return;
    }
    // Houdini repeats settings on redraws: an unchanged value returns without the state lock (held while an upload is
    // prepared). The state lock is taken first, as everywhere else (setting() takes settingsMutex inside it).
    {
        std::lock_guard settingsLock(state_->settingsMutex);
        if (const auto old=state_->settings.find(key); old!=state_->settings.end() && old->second==value) return;
    }
    std::lock_guard lock(state_->mutex);
    std::lock_guard settingsLock(state_->settingsMutex);
    if (const auto old=state_->settings.find(key); old!=state_->settings.end() && old->second==value) return;
    HdRenderDelegate::SetRenderSetting(key, value);
    if (key == TfToken("nativeMaterialParameters")) {
        const auto previous = state_->setting<VtDictionary>("nativeMaterialParameters", {});
        const auto current = value.IsHolding<VtDictionary>() ? value.UncheckedGet<VtDictionary>() : VtDictionary();
        for (const auto& [id, fields] : previous) if (state_->materials.count(id) && (!current.count(id) || current.find(id)->second != fields)) state_->dirtyMaterials.insert(id);
        for (const auto& [id, fields] : current) if (state_->materials.count(id) && (!previous.count(id) || previous.find(id)->second != fields)) state_->dirtyMaterials.insert(id);
        ++state_->revision;
    }
    state_->settings[key] = value;
    if (key==TfToken("surfaceEffects")) state_->wetBindings=std::make_shared<const VtDictionary>(state_->setting<VtDictionary>("surfaceEffects",{}));
    if (key==TfToken("surfaceEffects") || key==TfToken("wetMaskPreview") || key==TfToken("wetValidationError")) {
        ++state_->revision;
        if (key==TfToken("surfaceEffects") && state_->renderIndex)
            for (const auto& id:state_->renderIndex->GetRprimIds()) state_->renderIndex->GetChangeTracker().MarkRprimDirty(id,HdChangeTracker::DirtyPrimvar);
        if (key==TfToken("surfaceEffects") && TfGetenvBool("HDUE_TRACE_SCENE",false))
            TF_STATUS("hdUE wet settings: renderIndex=%p rprims=%zu",state_->renderIndex,state_->renderIndex?state_->renderIndex->GetRprimIds().size():0);
    }
    if (key==TfToken("renderMode") || key==TfToken("samplesPerPixel") || key==TfToken("warmupFrames") ||
        key==TfToken("lumenEnabled") || key==TfToken("timeSeconds") || key==TfToken("motionBlur") || key==TfToken("motionSamples") ||
        key==TfToken("framesPerSecond") || key==TfToken("tonemapper") || key==TfToken("postProcessMaterials") || key==TfToken("denoiser") ||
        TfStringStartsWith(key.GetString(), "postProcess:")) ++state_->requestSettingsRevision;
    if (key==TfToken("houdini:frame") || key==TfToken("houdini:fps")) {
        const double frame=state_->setting<double>("houdini:frame",1.);
        const double fps=state_->setting<double>("houdini:fps",24.);
        if (std::isfinite(frame) && std::isfinite(fps) && fps>0) {
            state_->settings[TfToken("timeSeconds")]=VtValue(frame/fps);
            ++state_->requestSettingsRevision;
        }
    }
    if (key == TfToken("subdivisionLevel") && state_->renderIndex) {
        for (const auto& id : state_->renderIndex->GetRprimIds()) state_->renderIndex->GetChangeTracker().MarkRprimDirty(id, HdChangeTracker::DirtyTopology);
        ++state_->revision;
    }
    if (key == TfToken("autoViewerLight") || key == TfToken("viewerLightIntensity")) {
        for (const auto& [id, light] : state_->lights) if (light.viewer) state_->dirtyLights.insert(id);
        ++state_->revision;
    }
    if (key == TfToken("sceneLightsEnabled") || key==TfToken("domeResolution")) {
        for (const auto& [id, light] : state_->lights) if (!light.viewer) state_->dirtyLights.insert(id);
        ++state_->revision;
    }
    if (key == TfToken("nativeMaterialAssets")) {
        for (const auto& [id, v] : state_->materials) state_->dirtyMaterials.insert(id);
        ++state_->revision;
    }
    if (key == TfToken("nativeMeshAssets") || key == TfToken("requireNanite") || key == TfToken("metersPerUnit") || key == TfToken("upAxis")) {
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
                try { fresh = renderStats(*state); } catch (const std::exception& e) { fresh["error"] = VtValue(std::string(e.what())); }
                catch (...) { fresh["error"] = VtValue(std::string("unknown error")); }
                std::lock_guard lock(state->statsMutex); state->stats = std::move(fresh); state->statsPending = false;
            });
        }
        std::lock_guard lock(state_->statsMutex);
        return state_->stats;
    }
    VtDictionary stats;
    try { ueRun([&] { stats = renderStats(*state_); }); } catch (const std::exception& e) { stats["error"] = VtValue(std::string(e.what())); }
    return stats;
}
VtDictionary HdUERenderDelegate::renderStats(HdUEState& state) {
    std::lock_guard lock(state.mutex);
    HdueResidencyStatus residency{}; residency.struct_size = sizeof(residency);
    const auto& residencyApi = state.backend->residency_api();
    if (state.session && state.backend->surface_api().get_status)
        state.backend->surface_api().get_status(state.session,&state.surfaceStatus);
    if (state.session && residencyApi.get_status) residencyApi.get_status(state.session, &residency);
    VtDictionary nativeAssetIds;
    int triangles = 0;
    for (const auto& [id, mesh] : state.meshes) {
        triangles += int(mesh.points.size() / 3);
        if (!mesh.sceneAsset.empty()) nativeAssetIds[mesh.scenePath]=VtValue(id);
    }
    int translucentMaterials = 0;
    for (const auto& [id, material] : state.materials) if (material.opacity < 1.f && asset(state, "nativeMaterialAssets", id).empty()) ++translucentMaterials;
    int sceneLights = int(state.nativeAssets.lights);
    HdueLightingStatus lighting{}; lighting.struct_size=sizeof(lighting);
    const auto& lightingApi=state.backend->lighting_api();
    if(lightingApi.get_status && state.session) state.backend->require(lightingApi.get_status(state.session,&lighting),"lighting_status");
    if (state.setting<bool>("sceneLightsEnabled", true))
        for (const auto& [id, light] : state.lights) if (!light.viewer && active(light)) ++sceneLights;
    return { {"error", VtValue(residency.phase == HDUE_RESIDENCY_FAILED ? std::string(residency.message) : state.error)}, {"generation", VtValue(state.generation)},
        {"wetBindings",VtValue(int(state.surfaceStatus.binding_count))}, {"wetUploadPending",VtValue(bool(state.surfaceStatus.upload_pending))},
        {"wetMaskUploads",VtValue(state.surfaceStatus.mask_uploads)}, {"wetMaskBytes",VtValue(state.surfaceStatus.mask_bytes)},
        {"wetGeometryUploads",VtValue(state.surfaceStatus.geometry_uploads)}, {"wetUpdates",VtValue(state.surfaceStatus.updates)},
        {"vtResidencyPhase", VtValue(int(residency.phase))}, {"vtResidencyGeneration", VtValue(residency.generation)},
        {"vtRequiredPages", VtValue(residency.required_pages)}, {"vtResidentPages", VtValue(residency.resident_pages)},
        {"vtLockedTiles", VtValue(residency.locked_tiles)}, {"vtRequiredMappings", VtValue(residency.required_mappings)},
        {"vtReadyMappings", VtValue(residency.ready_mappings)}, {"vtTextures", VtValue(int(residency.textures))},
        {"vtPreparationFrames", VtValue(int(residency.preparation_frames))}, {"vtPreparationMs", VtValue(residency.preparation_ms)},
        {"materialGraphBlocked", VtValue(!state.setting<bool>("materialGraphValid", true))},
        {"translucentMaterials", VtValue(translucentMaterials)},
        {"megaLightsProject",VtValue(bool(lighting.mega_lights_project))}, {"megaLightsAllowed",VtValue(bool(lighting.mega_lights_allowed))},
        {"megaLightsActive",VtValue(bool(lighting.mega_lights_active))}, {"domeLights",VtValue(int(lighting.dome_count))},
        {"environmentPending",VtValue(bool(lighting.environment_pending))}, {"environmentCubeResolution",VtValue(int(lighting.cube_resolution))},
        {"environmentMaxRadiance",VtValue(lighting.environment_max_radiance)},
        {"nativeAssetInstances", VtValue(int(state.nativeAssets.instances))},
        {"nativeAssetIds", VtValue(nativeAssetIds)},
        {"nativeAssetLoading", VtValue(int(state.nativeAssets.loading))},
        {"nativeAssetFailed", VtValue(int(state.nativeAssets.failed))},
        {"nativeActors", VtValue(int(state.nativeAssets.actors))},
        {"nativeMeshes", VtValue(int(state.nativeAssets.meshes))},
        {"nativeLights", VtValue(int(state.nativeAssets.lights))},
        {"sessionHandle", VtValue(uint64_t(state.session))},
        {"submittedMeshes", VtValue(state.submittedMeshes)}, {"submittedMaterials", VtValue(state.submittedMaterials)},
        {"gpuLocalMemoryBytes", VtValue(state.gpuLocalMemoryBytes)}, {"gpuMemoryAvailable", VtValue(state.gpuMemoryAvailable)},
        {"materialParameterUpdates", VtValue(state.materialParameterUpdates)},
        {"substrateEnabled", VtValue(bool(state.rendererInfo.substrate_enabled))},
        {"adaptiveGBuffer", VtValue(bool(state.rendererInfo.adaptive_gbuffer))},
        {"substrateClosureBudget", VtValue(int(state.rendererInfo.project_closures_per_pixel))},
        {"substrateBytesPerPixel", VtValue(int(state.rendererInfo.bytes_per_pixel))},
        {"pick", VtValue(state.pickResult)},
        {"publishedFrames", VtValue(state.publishedFrames)}, {"sceneSubmissions", VtValue(state.sceneSubmissions)},
        {"staleFrameRetries", VtValue(state.staleFrameRetries)},
        {"coalescedCameraUpdates", VtValue(state.coalescedCameraUpdates)}, {"requestCpuMsTotal", VtValue(state.requestCpuMsTotal)},
        {"meshes", VtValue(int(state.meshes.size()))}, {"materials", VtValue(int(state.materials.size()))}, {"lights", VtValue(int(state.lights.size()))},
        {"sceneLights", VtValue(sceneLights)}, {"viewerLights", VtValue(state.viewerLights)}, {"autoLightActive", VtValue(state.autoLightActive)},
        {"triangles", VtValue(triangles)}, {"frameId", VtValue(state.lastFrame.frame_id)}, {"completedSamples", VtValue(int(state.lastFrame.completed_samples))},
        {"percentDone", VtValue(state.lastFrame.converged ? 100.f : state.lastFrame.frame_id && state.setting<std::string>("renderMode", "realtime") == "pathtraced"
            ? std::min(99.f, 100.f * float(state.lastFrame.completed_samples) / float(std::max(1, state.setting<int>("samplesPerPixel", 64)))) : 0.f)},
        {"totalClockTime", VtValue(state.lastFrame.request_to_frame_ms / 1000.)},
        {"requestToFrameMs", VtValue(state.lastFrame.request_to_frame_ms)}, {"readbackMs", VtValue(state.lastFrame.readback_ms)} };
}
bool HdUERenderDelegate::InvokeCommand(const TfToken& command, const HdCommandArgs& args) {
    if (command == TfToken("hdue:sampleMemory")) {
        const auto memory = hdue::gpu_memory_bytes();
        std::lock_guard lock(state_->mutex);
        state_->gpuMemoryAvailable = memory.has_value(); state_->gpuLocalMemoryBytes = memory.value_or(0);
        return true;
    }
    if (command != TfToken("hdue:pickRay")) return false;
    std::lock_guard lock(state_->mutex);
    auto& result = state_->pickResult;
    result = {{"hit", VtValue(false)}, {"path", VtValue(std::string())}, {"error", VtValue(std::string())}};
    const auto argument = [&](const char* key) { const auto it = args.find(key); return it == args.end() ? VtValue() : it->second; };
    const auto originValue = argument("origin"), directionValue = argument("direction");
    const auto minValue = argument("minimum"), maxValue = argument("maximum");
    if (!originValue.IsHolding<GfVec3d>() || !directionValue.IsHolding<GfVec3d>() ||
        !minValue.IsHolding<double>() || !maxValue.IsHolding<double>()) return false;
    const auto origin = originValue.UncheckedGet<GfVec3d>();
    auto direction = directionValue.UncheckedGet<GfVec3d>();
    double minimum = minValue.UncheckedGet<double>(), maximum = maxValue.UncheckedGet<double>();
    for (int i = 0; i < 3; ++i) if (!std::isfinite(origin[i]) || !std::isfinite(direction[i])) return false;
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < 0 || maximum < minimum || direction.GetLengthSq() < 1e-20) return false;
    direction.Normalize();
    bool nativeScene = false;
    for (const auto& [id, mesh] : state_->meshes) {
        if (mesh.visible && (!asset(*state_, "nativeMeshAssets", id).empty() ||
                             !asset(*state_, "nativeMaterialAssets", mesh.material).empty())) nativeScene = true;
    }
    const auto store = [&](const std::string& path, const GfVec3d& point, const GfVec3d& normal, double distance,
                           const char* geometry, bool naniteFallback = false) {
        result["hit"] = VtValue(true); result["path"] = VtValue(path);
        result["px"] = VtValue(point[0]); result["py"] = VtValue(point[1]); result["pz"] = VtValue(point[2]);
        result["nx"] = VtValue(normal[0]); result["ny"] = VtValue(normal[1]); result["nz"] = VtValue(normal[2]);
        result["distance"] = VtValue(distance); result["geometry"] = VtValue(std::string(geometry));
        result["naniteFallback"] = VtValue(naniteFallback);
    };
    if (nativeScene) {
        if (!state_->backend || !state_->session || state_->firstBatch || state_->submittedRevision != state_->revision) {
            result["error"] = VtValue(std::string("Native picking requires the current scene to be submitted to UE"));
            return true;
        }
        const auto& api = state_->backend->native_pick_api();
        if (!api.pick_native) {
            result["error"] = VtValue(std::string("Backend has no native picking extension"));
            return true;
        }
        // Native materials can change culling or deform even a generic USD
        // mesh. Validate them before returning any hit in the scene.
        for (const auto& [id, mesh] : state_->meshes) {
            if (!mesh.visible) continue;
            const auto path = asset(*state_, "nativeMaterialAssets", mesh.material);
            if (path.empty() || state_->pickMaterialFlags.count(path)) continue;
            HduePickMaterialInfo info{}; info.struct_size = sizeof(info);
            const auto code = api.get_material_info(path.c_str(), &info);
            if (code != HDUE_OK || !(info.flags & HDUE_PICK_MATERIAL_OPAQUE) || (info.flags & HDUE_PICK_MATERIAL_DEFORMED)) {
                result["error"] = VtValue(code == HDUE_OK ? std::string("Picking requires opaque native materials without deformation") : state_->backend->status_message());
                return true;
            }
            state_->pickMaterialFlags[path] = info.flags;
        }
        const double scale = state_->setting<double>("metersPerUnit", 0.01) * 100;
        if (!(scale > 0) || !std::isfinite(scale)) return false;
        HdueNativeRay ray{}; ray.struct_size = sizeof(ray);
        const auto ueOrigin = state_->convert(origin), ueDirection = state_->convert(direction, true);
        for (int i = 0; i < 3; ++i) { ray.origin[i] = ueOrigin[i]; ray.direction[i] = ueDirection[i]; }
        ray.minimum_cm = minimum * scale; ray.maximum_cm = maximum * scale;
        HdueNativeHit hit{}; hit.struct_size = sizeof(hit);
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
            if (mesh == state_->meshes.end()) { result["error"] = VtValue(std::string("UE returned an unknown pick object")); return true; }
            const auto fromUE = [&](const double* v) {
                return state_->setting<std::string>("upAxis", "Y") == "Z" ? GfVec3d(v[0], -v[1], v[2]) : GfVec3d(v[1], v[2], -v[0]);
            };
            maximum = hit.distance_cm / scale;
            store(mesh->second.scenePath, fromUE(hit.point) / scale, fromUE(hit.normal).GetNormalized(), maximum,
                  "ue-lod0", (hit.flags & HDUE_NATIVE_PICK_NANITE_FALLBACK) != 0);
        }
    }
    for (auto& [id, mesh] : state_->meshes) {
        if (!mesh.visible || mesh.points.empty() || !asset(*state_, "nativeMeshAssets", id).empty() || std::abs(mesh.transform.GetDeterminant()) < 1e-20) continue;
        if (!mesh.pickMesh) mesh.pickMesh = std::make_shared<HdUEPickMesh>(mesh.points); // lazily, on the first pick
        const auto material = asset(*state_, "nativeMaterialAssets", mesh.material);
        const bool twoSided = !material.empty() && (state_->pickMaterialFlags.at(material) & HDUE_PICK_MATERIAL_TWO_SIDED);
        const auto inverse = mesh.transform.GetInverse();
        // Preserve the ray parameter through nonuniform/negative scale.
        const GfRay local(inverse.Transform(origin), inverse.TransformDir(direction));
        HdUEPickHit hit;
        if (!mesh.pickMesh->intersect(mesh.points, local, minimum, maximum, &hit, twoSided)) continue;
        maximum = hit.distance;
        const GfVec3d point = origin + direction * hit.distance;
        GfVec3d normal(0);
        for (int c = 0; c < 3; ++c) normal += GfVec3d(mesh.normals[size_t(hit.triangle) * 3 + c]) * hit.barycentric[c];
        normal = inverse.GetTranspose().TransformDir(normal).GetNormalized();
        if (twoSided && GfDot(normal, direction) > 0) normal = -normal;
        store(mesh.scenePath, point, normal, hit.distance, mesh.sceneAsset.empty() ? "usd" : "ue-asset-bounds");
    }
    return true;
}
PXR_NAMESPACE_CLOSE_SCOPE

#if defined(HDUE_HOUDINI)
// Ends UE that hdUE runs on its own thread in Houdini's UI (hdue_houdini calls this when Houdini quits).
extern "C" HDUE_VIEWPORT_EXPORT void HDUE_CALL hdue_shutdown_ue() {
    auto& host = PXR_NS::selfHost();
    if (!host.backend) return;
    host.run([&] {
        try { host.tick(); host.backend->shutdown(); } catch (const std::exception& e) { std::fprintf(stderr, "hdUE shutdown: %s\n", e.what()); }
        if (host.window) DestroyWindow(host.window);
        host.window = nullptr;
    });
    host.stop();
    host.backend.reset();
}
extern "C" HDUE_VIEWPORT_EXPORT void HDUE_CALL hdue_run_ui_tasks() {
    auto& queue = PXR_NS::ownerQueue();
    queue.owner = GetCurrentThreadId();
    for (;;) {
        std::shared_ptr<PXR_NS::HdUEOwnerQueue::Task> task;
        {
            std::lock_guard lock(queue.mutex);
            if (queue.tasks.empty()) return;
            task = queue.tasks.front(); queue.tasks.pop_front(); task->state = 1;
        }
        try { task->run(); } catch (...) { task->error = std::current_exception(); }
        { std::lock_guard lock(queue.mutex); task->state = 2; }
        queue.done.notify_all();
    }
}
#endif

