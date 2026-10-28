#include "delegate.h"
#include "depth.h"
#include "surface_settings.h"
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
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/js/json.h"
#include <cmath>
#include <chrono>
#include <cstdio>
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
#include <windows.h>
#if defined(HDUE_HOUDINI)
#define HDUE_VIEWPORT_EXPORTS 1 // hdue_run_ui_tasks is defined below
#include "../houdini/diagnostics.h"
#include "hdue/viewport_api.h"
#endif

PXR_NAMESPACE_OPEN_SCOPE
namespace {
// Hosts such as husk never start UE. The first delegate created while the
// runtime is cold starts it on its own thread and ticks it from Execute, as
// hdue_usd_render does; the last delegate shuts it down.
struct HdUESelfHost {
    std::unique_ptr<hdue::Backend> backend;
    HWND window = nullptr;
    DWORD thread = 0;
    int delegates = 0;
    std::filesystem::path directory; // the host's working directory, which UE moves to its Binaries
    // UE runs on its own thread: husk calls from one thread, but Live Render creates the delegate on one
    // thread and executes it on others, and UE only accepts calls from the thread that started it.
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
    template<class F> auto run(F&& f) -> decltype(f()) {
        if (!worker.joinable() || std::this_thread::get_id() == worker.get_id()) return f();
        std::packaged_task<decltype(f())()> task(std::forward<F>(f));
        auto result = task.get_future();
        { std::lock_guard lock(queueMutex); queue.emplace_back([&task] { task(); }); }
        queueReady.notify_one();
        return result.get();
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
LRESULT CALLBACK selfHostWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto& host = selfHost();
    if (host.backend && host.backend->initialized()) {
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
void startSelfHost(const std::string& dll, const std::string& project) {
    auto& host = selfHost();
    if (project.empty()) throw std::runtime_error("hdUE cannot start UE without runtime_project in HDUE_RUNTIME_CONFIG");
    std::promise<void> started;
    auto ready = started.get_future();
    host.stopping = false;
    host.worker = std::thread([&host, dll, project, &started] {
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
            host.thread = GetCurrentThreadId();
            host.backend->initialize(init);
            started.set_value();
        } catch (...) { host.backend.reset(); started.set_exception(std::current_exception()); return; }
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(host.queueMutex);
                host.queueReady.wait(lock, [&host] { return host.stopping || !host.queue.empty(); });
                if (host.queue.empty()) return;
                task = std::move(host.queue.front()); host.queue.pop_front();
            }
            task();
        }
    });
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
    meshes.erase(id); materials.erase(id); lights.erase(id); volumes.erase(id); dirtyVolumes.erase(id);
    geometryRevisions.erase(id);
    dirtyMeshes.erase(id); dirtyMaterials.erase(id); dirtyLights.erase(id);
    removed.insert(id); ++revision;
}
GfVec3d HdUEState::convert(const GfVec3d& p, bool direction) const {
    const double scale = direction ? 1 : setting<double>("metersPerUnit", 0.01) * 100;
    return (setting<std::string>("upAxis", "Y") == "Z" ? GfVec3d(p[0], -p[1], p[2]) : GfVec3d(-p[2], p[0], p[1])) * scale;
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
void submit(HdUEState& state) {
    std::vector<HdueMeshDesc> meshes;
    std::vector<HdueAssetDesc> sceneAssets;
    std::vector<std::vector<float>> positions, normals, uvs, colors, instanceColors;
    std::vector<std::vector<double>> instances; // row-major UE world matrices per instanced mesh
    std::vector<std::vector<uint32_t>> indices;
    std::vector<std::string> nativeMeshes, nativeMaterials;
    std::vector<HdueMaterialDesc> materials;
    std::vector<HdueLightDesc> lights;
    std::vector<HdueLight> extendedLights;
    std::vector<const char*> removed;
    // Sync has joined before Execute. The lock also protects stats/settings access.
    std::lock_guard lock(state.mutex);
    const auto effects=state.setting<VtDictionary>("surfaceEffects",{});
    std::vector<HdueWetBinding> wet;
    std::vector<std::string> wetAssets, wetPreviews;
    wetAssets.reserve(state.meshes.size()); wetPreviews.reserve(state.meshes.size());
    for (const auto& [id, data] : state.meshes) {
        const auto fields=HdUEWetSettings(effects,data);
        if (fields.empty()) continue;
        const float amount=HdUEWetValue<float>(fields,"amount",float(HdUEWetValue<double>(fields,"amount",1.)));
        if (amount==0.f && !state.setting<bool>("wetMaskPreview",false)) continue;
        const auto maskName=HdUEWetValue<std::string>(fields,"maskPrimvar","");
        const auto mask=data.masks.find(maskName);
        if (!maskName.empty() && mask==data.masks.end()) {
            const auto error=data.maskErrors.find(maskName);
            throw std::runtime_error(data.bindingPath+": "+(error!=data.maskErrors.end()?error->second:"Wet mask not synchronized: "+maskName));
        }
        wetAssets.push_back(HdUEWetValue<std::string>(fields,"asset",""));
        wetPreviews.push_back(HdUEWetValue<std::string>(fields,"previewAsset",""));
        HdueWetBinding binding{}; binding.struct_size=sizeof(binding); binding.object_id=data.id.c_str();
        binding.variant_asset=wetAssets.back().c_str(); binding.preview_asset=wetPreviews.back().c_str();
        binding.amount=amount;
        binding.roughness=HdUEWetValue<float>(fields,"roughness",float(HdUEWetValue<double>(fields,"roughness",.08)));
        binding.flags=state.setting<bool>("wetMaskPreview",false)?HDUE_WET_MASK_PREVIEW:0;
        if (HdUEWetValue<bool>(fields,"graph",false)) binding.flags|=HDUE_SURFACE_GRAPH;
        if (!maskName.empty()) { binding.mask=mask->second.data(); binding.mask_count=uint32_t(mask->second.size()); }
        wet.push_back(binding);
    }
    const auto& surfaceApi=state.backend->surface_api();
    if (!wet.empty() && !surfaceApi.apply) throw std::runtime_error("Wet surface extension unavailable; build/cook/stage the current runtime.");
    const auto count = state.dirtyMeshes.size();
    positions.reserve(count); normals.reserve(count); uvs.reserve(count); indices.reserve(count); nativeMeshes.reserve(count); instances.reserve(count);
    colors.reserve(count); instanceColors.reserve(count);
    nativeMaterials.reserve(state.dirtyMaterials.size());
    for (const auto& id : state.dirtyMaterials) {
        const auto& data = state.materials.at(id);
        nativeMaterials.push_back(asset(state, "nativeMaterialAssets", id));
        HdueMaterialDesc desc{};
        desc.id = data.id.c_str(); desc.native_asset = nativeMaterials.back().c_str();
        for (int i = 0; i < 3; ++i) { desc.base_color[i] = data.color[i]; desc.emissive_color[i] = data.emissive[i]; }
        desc.base_color[3] = data.opacity; desc.metallic = data.metallic; desc.roughness = data.roughness;
        desc.opacity_threshold = data.opacityThreshold;
        desc.flags = data.vertexColor ? HDUE_MATERIAL_VERTEX_COLOR : 0;
        std::copy(data.uvTransform.begin(), data.uvTransform.end(), desc.uv_transform);
        for (int slot = 0; slot < HDUE_TEXTURE_SLOT_COUNT; ++slot) {
            const auto& texture = data.textures[slot];
            auto& out = desc.textures[slot];
            out.file = texture.file.c_str();
            for (int i = 0; i < 4; ++i) { out.scale[i] = texture.scale[i]; out.bias[i] = texture.bias[i]; out.channel_mask[i] = texture.mask[i]; }
        }
        materials.push_back(desc);
        if (TfGetenvBool("HDUE_TRACE_SCENE", false)) TF_STATUS("hdUE material %s rgb=(%g,%g,%g)", desc.id, desc.base_color[0], desc.base_color[1], desc.base_color[2]);
    }
    for (const auto& id : state.dirtyMeshes) {
        const auto& data = state.meshes.at(id);
        if (!data.sceneAsset.empty()) {
            HdueAssetDesc d{}; d.struct_size=sizeof(d); d.id=data.id.c_str(); d.asset_path=data.sceneAsset.c_str(); d.visible=data.visible;
            GfMatrix4d basis(1);
            for (int r=0; r<3; ++r) { GfVec3d e(0); e[r]=1; const auto v=state.convert(e); for (int c=0; c<3; ++c) basis[r][c]=v[c]; }
            const auto t=basis.GetInverse()*data.transform*basis;
            for (int r=0; r<4; ++r) for (int c=0; c<4; ++c) d.transform[r*4+c]=t[r][c];
            sceneAssets.push_back(d); removed.push_back(data.id.c_str());
            continue;
        }
        nativeMeshes.push_back(asset(state, "nativeMeshAssets", id));
        positions.emplace_back(); normals.emplace_back(); uvs.emplace_back(); indices.emplace_back();
        auto& p = positions.back(); auto& n = normals.back(); auto& uv = uvs.back(); auto& index = indices.back();
        // Instanced prototypes keep local vertices; each instance matrix carries the placement.
        const GfMatrix4d bake = data.instanced ? GfMatrix4d(1) : data.transform;
        const auto normalTransform = bake.GetInverse().GetTranspose();
        for (size_t i = 0; i < data.points.size(); ++i) {
            const auto point = state.convert(bake.Transform(GfVec3d(data.points[i])));
            for (int c = 0; c < 3; ++c) p.push_back(float(point[c]));
            if (data.normals.size() == data.points.size()) {
                auto normal = state.convert(normalTransform.TransformDir(GfVec3d(data.normals[i])), true).GetNormalized();
                for (int c = 0; c < 3; ++c) n.push_back(float(normal[c]));
            }
            if (data.uv.size() == data.points.size()) { uv.push_back(data.uv[i][0]); uv.push_back(1 - data.uv[i][1]); }
        }
        index.resize(data.points.size()); std::iota(index.begin(), index.end(), 0u);
        // World transforms are baked into dynamic vertices. Preserve front
        // faces when an authored transform adds another handedness reversal.
        if (bake.GetDeterminant() < 0)
            for (size_t i = 0; i + 2 < index.size(); i += 3) std::swap(index[i + 1], index[i + 2]);
        HdueMeshDesc desc{};
        desc.id = data.id.c_str(); desc.material_id = data.material.c_str(); desc.native_asset = nativeMeshes.back().c_str();
        desc.positions = p.data(); desc.normals = n.empty() ? nullptr : n.data(); desc.uv = uv.empty() ? nullptr : uv.data();
        desc.indices = index.data(); desc.vertex_count = uint32_t(data.points.size()); desc.index_count = uint32_t(index.size()); desc.visible = data.visible;
        desc.prim_id = data.primId;
        colors.emplace_back(); instanceColors.emplace_back();
        for (const auto& c : data.colors) colors.back().insert(colors.back().end(), {c[0], c[1], c[2]});
        if (colors.back().size() == p.size()) desc.colors = colors.back().data();
        instances.emplace_back();
        if (data.instanced) {
            // USD row-vector matrices into UE: S^-1 * M * S, S = the USD -> UE basis and units.
            GfMatrix4d basis(1);
            for (int r = 0; r < 3; ++r) { GfVec3d e(0); e[r] = 1; const auto v = state.convert(e); for (int c = 0; c < 3; ++c) basis[r][c] = v[c]; }
            const auto inverse = basis.GetInverse();
            auto& matrices = instances.back(); matrices.reserve(data.instances.size() * 16);
            for (const auto& instance : data.instances) {
                const auto m = inverse * instance * basis;
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) matrices.push_back(m[r][c]);
            }
            desc.instance_count = uint32_t(data.instances.size());
            for (const auto& c : data.instanceColors) instanceColors.back().insert(instanceColors.back().end(), {c[0], c[1], c[2]});
            if (instanceColors.back().size() == size_t(desc.instance_count) * 3 && desc.instance_count) desc.instance_colors = instanceColors.back().data();
            if (data.instanceIds.size() == desc.instance_count && desc.instance_count) desc.instance_ids = data.instanceIds.data();
            static const double none = 0;
            desc.instance_transforms = matrices.empty() ? &none : matrices.data();
        }
        desc.flags = state.setting<bool>("requireNanite", false) && !nativeMeshes.back().empty() ? HDUE_MESH_REQUIRE_NANITE : 0;
        for (int i = 0; i < 4; ++i) desc.transform[i * 4 + i] = 1;
        if (!nativeMeshes.back().empty()) {
            // The cooked sample cube has side 100 cm. Native geometry keeps its
            // own dimensions; only its placement transform comes from Hydra.
            GfMatrix4d basis(1);
            for (int r = 0; r < 3; ++r) {
                GfVec3d e(0); e[r] = 1;
                const auto v = state.convert(e);
                for (int c = 0; c < 3; ++c) basis[r][c] = v[c];
            }
            const auto transform = basis.GetInverse() * data.transform * basis;
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) desc.transform[r * 4 + c] = transform[r][c];
        }
        meshes.push_back(desc);
        if (TfGetenvBool("HDUE_TRACE_SCENE", false)) TF_STATUS("hdUE mesh %s material=%s first=(%g,%g,%g)", desc.id, desc.material_id, p.empty()?0:p[0], p.empty()?0:p[1], p.empty()?0:p[2]);
    }
    const double viewerIntensity = state.setting<double>("viewerLightIntensity", state.setting<float>("viewerLightIntensity", 4.f));
    if (!std::isfinite(viewerIntensity) || viewerIntensity < 0 || viewerIntensity > 100)
        throw std::runtime_error("viewerLightIntensity must be between 0 and 100");
    int sceneLights = 0, cameraLights = 0;
    for (const auto& [id, light] : state.lights) if (active(light)) {
        if (light.viewer) ++cameraLights;
        else if (state.setting<bool>("sceneLightsEnabled", true)) ++sceneLights;
    }
    const bool autoLight = state.setting<bool>("autoViewerLight", false) && !sceneLights && !cameraLights && !state.nativeAssets.lights;
    const auto extend=[&](const HdueLightDesc& legacy,const HdUELightData* source) {
        HdueLight light{}; light.struct_size=sizeof(light); light.type=legacy.type; light.id=legacy.id;
        light.visible=legacy.visible; light.intensity=legacy.intensity;
        light.flags=source?source->flags:HDUE_LIGHT_SHADOWS|HDUE_LIGHT_MEGALIGHTS;
        light.diffuse=source?source->diffuse:1; light.specular=source?source->specular:1;
        light.cone_angle=source?source->coneAngle:180; light.cone_softness=source?source->coneSoftness:0;
        light.angle_degrees=source?source->angle:.53f;
        light.cube_resolution=uint32_t(state.setting<int>("domeResolution",512));
        light.texture_format=source?source->textureFormat:HDUE_ENVIRONMENT_AUTOMATIC;
        light.texture_file=source?source->texture.c_str():"";
        for(int c=0;c<3;++c) {light.position_cm[c]=legacy.position[c]; light.color[c]=legacy.color[c];}
        for(int axis=0;axis<3;++axis) {
            GfVec3d direction(0); direction[axis]=1;
            direction=source?state.convert(source->transform.TransformDir(direction),true).GetNormalized():direction;
            for(int c=0;c<3;++c) light.axes[axis*3+c]=direction[c];
        }
        light.width_cm=legacy.width_cm; light.height_cm=legacy.height_cm;
        const double units=state.setting<double>("metersPerUnit",.01)*100;
        if(source) {
            light.length_cm=float(source->length*source->transform.TransformDir(GfVec3d(1,0,0)).GetLength()*units);
            if(source->type==HDUE_CYLINDER_LIGHT || source->type==HDUE_SPHERE_LIGHT) {
                const double x=source->transform.TransformDir(GfVec3d(1,0,0)).GetLength();
                const double y=source->transform.TransformDir(GfVec3d(0,1,0)).GetLength();
                const double z=source->transform.TransformDir(GfVec3d(0,0,1)).GetLength();
                light.width_cm=float(source->width*(source->type==HDUE_CYLINDER_LIGHT?std::max(y,z):std::max({x,y,z}))*units);
                if(light.width_cm==0 && source->type==HDUE_SPHERE_LIGHT) light.flags|=HDUE_LIGHT_POINT;
            }
        }
        if(legacy.type==HDUE_CAMERA_DISTANT_LIGHT) {
            for(int i=0;i<9;++i) light.axes[i]=(i%4)==0?1.:0.;
            for(int c=0;c<3;++c) light.axes[c]=legacy.direction[c];
        }
        extendedLights.push_back(light);
    };
    for (const auto& id : state.dirtyLights) {
        const auto& data = state.lights.at(id);
        HdueLightDesc desc{}; desc.id = data.id.c_str(); desc.type = data.type;
        desc.visible = data.visible && (data.viewer || state.setting<bool>("sceneLightsEnabled", true));
        const auto position = state.convert(data.transform.ExtractTranslation());
        const auto direction = state.convert(data.transform.TransformDir(GfVec3d(0, 0, -1)), true).GetNormalized();
        for (int c = 0; c < 3; ++c) { desc.position[c] = position[c]; desc.direction[c] = direction[c]; desc.color[c] = data.color[c]; }
        desc.intensity = data.intensity;
        if (data.viewer) {
            desc.intensity = float(viewerIntensity);
            desc.direction[0] = 1; desc.direction[1] = desc.direction[2] = 0;
        }
        const double widthScale=data.transform.TransformDir(GfVec3d(1,0,0)).GetLength();
        const double heightScale=data.transform.TransformDir(GfVec3d(0,1,0)).GetLength();
        desc.width_cm = float(data.width * widthScale * state.setting<double>("metersPerUnit", 0.01) * 100);
        desc.height_cm = float(data.height * heightScale * state.setting<double>("metersPerUnit", 0.01) * 100);
        lights.push_back(desc);
        extend(desc,&data);
        if (TfGetenvBool("HDUE_TRACE_SCENE", false)) TF_STATUS("hdUE light %s intensity=%g rgb=(%g,%g,%g)", desc.id, desc.intensity, desc.color[0], desc.color[1], desc.color[2]);
    }
    for (const auto& id : state.removed) removed.push_back(id.c_str());
    if (autoLight) {
        for (int i = 0; i < 2; ++i) {
            HdueLightDesc desc{};
            desc.id = i ? AutoFill : AutoKey; desc.type = HDUE_CAMERA_DISTANT_LIGHT; desc.visible = 1;
            desc.color[0] = desc.color[1] = desc.color[2] = 1;
            desc.intensity = float(viewerIntensity * (i ? 0.1875 : 1.0));
            desc.direction[0] = 1; desc.direction[1] = i ? -0.6 : 0.4; desc.direction[2] = i ? -0.2 : -0.5;
            lights.push_back(desc);
            extend(desc,nullptr);
        }
    } else if (state.autoLightActive) {
        removed.push_back(AutoKey); removed.push_back(AutoFill);
    }
    HdueSceneBatch batch{};
    batch.struct_size = sizeof(batch); batch.reset_scene = state.firstBatch;
    batch.meshes = meshes.data(); batch.mesh_count = uint32_t(meshes.size());
    batch.materials = materials.data(); batch.material_count = uint32_t(materials.size());
    const auto& lightingApi=state.backend->lighting_api();
    if(!lightingApi.upsert) for(const auto& light:extendedLights)
        if(light.type>HDUE_CAMERA_DISTANT_LIGHT || (light.texture_file && *light.texture_file))
            throw std::runtime_error("This scene requires the backend lighting extension; build/cook/stage a current runtime.");
    batch.lights = lightingApi.upsert?nullptr:lights.data(); batch.light_count = lightingApi.upsert?0:uint32_t(lights.size());
    batch.removed_ids = removed.data(); batch.removed_count = uint32_t(removed.size());
    if (batch.reset_scene || batch.mesh_count || batch.material_count || batch.light_count || batch.removed_count) {
        state.backend->require(state.backend->api().apply_scene(state.session, &batch), "apply_scene");
        for (const auto& id:state.removed) state.geometryRevisions.erase(id);
        for (const auto& mesh:meshes) if (!mesh.native_asset || !*mesh.native_asset) ++state.geometryRevisions[mesh.id];
    }
    if(lightingApi.upsert && !extendedLights.empty()) {
        HdueLightBatch update{sizeof(HdueLightBatch),uint32_t(extendedLights.size()),extendedLights.data()};
        state.backend->require(lightingApi.upsert(state.session,&update),"upsert_lights");
    }
    const auto& assetApi=state.backend->asset_api();
    if (!sceneAssets.empty() && !assetApi.upsert) throw std::runtime_error("Backend has no native scene asset extension");
    if (assetApi.remove) {
        for (const auto& id : state.removed) state.backend->require(assetApi.remove(state.session,id.c_str()),"remove_asset");
        for (const auto& id : state.dirtyMeshes) if (state.meshes.at(id).sceneAsset.empty())
            state.backend->require(assetApi.remove(state.session,id.c_str()),"remove_asset");
    }
    for (const auto& desc : sceneAssets) state.backend->require(assetApi.upsert(state.session,&desc),"upsert_asset");
    if (!state.dirtyVolumes.empty()) {
        const auto& volumeApi = state.backend->volume_api();
        if (!volumeApi.upsert) throw std::runtime_error("This scene has volumes; build and stage the current backend");
        // Voxel space to UE world: USD placement, then the USD -> UE basis and units (row vectors).
        GfMatrix4d basis(1);
        for (int r = 0; r < 3; ++r) { GfVec3d e(0); e[r] = 1; const auto v = state.convert(e); for (int c = 0; c < 3; ++c) basis[r][c] = v[c]; }
        const double perCentimeter = 1 / (state.setting<double>("metersPerUnit", .01) * 100);
        for (const auto& id : state.dirtyVolumes) {
            const auto it = state.volumes.find(id);
            if (it == state.volumes.end()) continue;
            const auto& volume = it->second;
            HdueVolumeDesc desc{}; desc.struct_size = sizeof(desc); desc.id = volume.id.c_str(); desc.visible = volume.visible;
            desc.width = volume.width; desc.height = volume.height; desc.depth = volume.depth;
            desc.density = volume.density.empty() ? nullptr : volume.density.data();
            const auto m = volume.localToWorld * basis;
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) desc.transform[r * 4 + c] = m[r][c];
            for (int c = 0; c < 3; ++c) desc.albedo[c] = volume.albedo[c];
            // Density is extinction per scene unit; UE's ray marcher steps in voxel units (measured: halving
            // the resolution halves optical depth), so scale by the voxel's edge length in cm.
            const double voxelCm = std::cbrt(std::abs(m.ExtractRotationMatrix().GetDeterminant()));
            desc.extinction_scale = float(perCentimeter * voxelCm);
            state.backend->require(volumeApi.upsert(state.session, &desc), "upsert_volume");
        }
    }
    state.submittedMeshes += meshes.size(); state.submittedMaterials += materials.size();
    const auto overrides = state.setting<VtDictionary>("nativeMaterialParameters", {});
    for (const auto& id : state.dirtyMaterials) {
        if (state.materials.find(id) == state.materials.end()) continue;
        const auto entry = overrides.find(id);
        if (entry == overrides.end()) continue;
        if (asset(state, "nativeMaterialAssets", id).empty() || !entry->second.IsHolding<VtDictionary>())
            throw std::runtime_error("Native material parameters require a mapped cooked material and a dictionary");
        const auto& api = state.backend->material_api();
        if (!api.set_parameters) throw std::runtime_error("Backend has no material parameter extension");
        const auto& fields = entry->second.UncheckedGet<VtDictionary>();
        std::vector<HdueMaterialParameter> parameters;
        for (const auto& [name, value] : fields) {
            HdueMaterialParameter p{}; p.name = name.c_str();
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
                p.type = HDUE_PARAMETER_TEXTURE; p.texture_asset = value.UncheckedGet<std::string>().c_str();
            } else throw std::runtime_error("Unsupported native material parameter value: " + name);
            parameters.push_back(p);
        }
        HdueMaterialUpdate update{}; update.struct_size = sizeof(update); update.material_id = id.c_str();
        update.parameter_count = uint32_t(parameters.size()); update.parameters = parameters.data();
        state.backend->require(api.set_parameters(state.session, &update), "set_material_parameters");
        ++state.materialParameterUpdates;
    }
    state.autoLightActive = autoLight; state.viewerLights = cameraLights + (autoLight ? 2 : 0);
    if (surfaceApi.apply && (!wet.empty() || state.surfaceStatus.binding_count)) {
        for (auto& binding:wet) binding.geometry_revision=state.geometryRevisions[binding.object_id];
        HdueSurfaceBatch surface{sizeof(surface),uint32_t(wet.size()),wet.data()};
        state.backend->require(surfaceApi.apply(state.session,&surface),"apply_surface_effects");
        state.backend->require(surfaceApi.get_status(state.session,&state.surfaceStatus),"surface_status");
    }
    state.submittedNativeLights = state.nativeAssets.lights;
    ++state.sceneSubmissions;
    state.dirtyMeshes.clear(); state.dirtyMaterials.clear(); state.dirtyLights.clear(); state.dirtyVolumes.clear(); state.removed.clear();
    state.submittedRevision = state.revision; state.firstBatch = false;
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
        GfMatrix4d basis(1);
        for (int r = 0; r < 3; ++r) { GfVec3d e(0); e[r] = 1; const auto v = s.convert(e); for (int c = 0; c < 3; ++c) basis[r][c] = v[c]; }
        const auto inverse = basis.GetInverse();
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
        std::vector<std::pair<HdUEMeshData*, std::vector<GfVec3f>>> rest;
        for (auto& [id, mesh] : s.meshes) if (!mesh.deformTimes.empty() && mesh.sceneAsset.empty()) {
            rest.emplace_back(&mesh, mesh.points);
            mesh.points = mesh.pointsAt(t);
            s.dirtyMeshes.insert(id);
        }
        if (!rest.empty()) {
            submit(s);
            for (auto& [mesh, points] : rest) mesh->points = std::move(points);
        }
        std::vector<HdueMeshTransform> moves;
        for (size_t i = 0; i < ids.size(); ++i) moves.push_back({ids[i]->c_str(), uint32_t(storage[i].size() / 16), storage[i].data()});
        if (!moves.empty()) s.backend->require(s.backend->motion_api().set_transforms(s.session, moves.data(), uint32_t(moves.size())), "set_transforms");
        motionApplied_ = t != 0;
    }
    uint64_t settingsVersion_ = ~uint64_t(0), lastFrame_ = 0;
    double centimetersPerUnit_ = 1;
    bool depthEnabled_ = false;
    bool converged_ = false;
    // GBuffer AOVs render one by one after the beauty converges, from its camera.
    std::unique_ptr<HdUERenderBuffer> internalColor_; // beauty target when no color AOV is bound
    HdueRenderRequest beautyRequest_{};
    std::vector<std::pair<HdUERenderBuffer*, uint32_t>> aovs_;
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
    bool IsConverged() const override {
        if (const auto error=state_->setting<std::string>("wetValidationError", ""); !error.empty())
            return converged_ && state_->error==error;
        if (!state_->setting<bool>("materialGraphValid",true)) return true;
        return converged_ && (aovs_.empty() || aovsDone_) &&
            settingsVersion_==state_->requestSettingsRevision && state_->submittedRevision==state_->revision;
    }
    void _Execute(const HdRenderPassStateSharedPtr& pass, const TfTokenVector& tags) override {
        // husk polls Execute at its own pace and UE advances one tick per call; when this process owns UE,
        // keep ticking for a while so a realtime frame takes its GPU time rather than husk's polling interval.
        auto& host = selfHost();
        if (!host.backend) { ueRun([&] { executeOnce(pass, tags); }); return; }
        host.run([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            do executeOnce(pass, tags);
            while (!IsConverged() && state_->error.empty() && std::chrono::steady_clock::now() < deadline);
        });
    }
    void executeOnce(const HdRenderPassStateSharedPtr& pass, const TfTokenVector&) {
        try {
            auto& s = *state_;
            if (const auto error=s.setting<std::string>("wetValidationError", ""); !error.empty()) {
                // Preparation/undo must change the graph before rendering can
                // resume. Stop Hydra's update loop, retaining the displayed AOV
                // while keeping the invalid request out of completed-frame APIs.
                s.error=error; s.lastFrame={}; converged_=true; return;
            }
#if defined(HDUE_HOUDINI)
            s.executeThread=GetCurrentThreadId();
#endif
            selfHost().tick();
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
                if(const auto id=HdUEAovId(name); id && dynamic_cast<HdUERenderBuffer*>(renderBuffer))
                    aovs.emplace_back(dynamic_cast<HdUERenderBuffer*>(renderBuffer),id);
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
                s.error = "hdUE renders none of the requested render vars:" + names; converged_ = true; aovsDone_ = true; return;
            }
            if (!buffer || !buffer->GetWidth() || !buffer->GetHeight()) {
                std::string bindings;
                for (const auto& aov:pass->GetAovBindings()) bindings+=" "+aov.aovName.GetString()+":"+aov.renderBufferId.GetString();
                throw std::runtime_error("hdUE requires an allocated float32 RGBA color AOV; bindings:"+bindings+
                    (buffer?" allocation: "+buffer->allocationError():" (missing buffer)"));
            }
            bool changed = s.submittedRevision != s.revision;
            if (!s.session || s.width != buffer->GetWidth() || s.height != buffer->GetHeight()) {
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
                if (assets.failed) { s.error=assets.message; s.assetRequestPending=true; converged_=true; return; }
                if (lightsChanged) { ++s.revision; submit(s); changed=true; }
            }
            if (s.assetRequestPending) { changed=true; s.assetRequestPending=false; }
            const auto view = pass->GetWorldToViewMatrix();
            const auto projection = pass->GetProjectionMatrix();
            const HdCamera* hdCamera = pass->GetCamera();
            const GfVec4f lens = hdCamera ? GfVec4f(hdCamera->GetFStop(), hdCamera->GetFocusDistance(), hdCamera->GetFocalLength(), hdCamera->GetLinearExposureScale())
                                          : GfVec4f(0.f, 0.f, 0.f, 1.f);
            // Sub-frame times are spread over the camera shutter; the camera moves with its own samples.
            const auto cameraMotion = hdCamera ? s.cameraMotion.find(hdCamera->GetId().GetString()) : s.cameraMotion.end();
            const auto subframeTime = [&](int k) {
                return float(hdCamera->GetShutterOpen() + (k + .5) / subframes_ * (hdCamera->GetShutterClose() - hdCamera->GetShutterOpen()));
            };
            const auto viewAt = [&](float t) {
                if (subframes_ < 2 || cameraMotion == s.cameraMotion.end()) return view;
                return view * cameraMotion->second.at(0) * cameraMotion->second.at(t).GetInverse();
            };
            const auto settingsVersion = s.requestSettingsRevision;
            // Consume a completed image before changing its generation. Otherwise
            // continuous camera input can discard every image before presentation.
            HdueFrameInfo frame{}; frame.struct_size = sizeof(frame);
            const auto result = s.backend->api().poll_frame(s.session, &frame);
            if (result == HDUE_OK && frame.generation == s.generation && frame.frame_id != lastFrame_) {
                std::vector<float> pixels(size_t(frame.required_bytes / sizeof(float)));
                const auto copy = s.backend->api().copy_frame(s.session, frame.frame_id, pixels.data(), frame.required_bytes);
                std::vector<float> depthPixels;
                HdueResult depthCopy=HDUE_OK;
                if(copy==HDUE_OK && depth) {
                    depthPixels.resize(size_t(frame.width)*frame.height);
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
                        for(auto& value:depthPixels) {
#if defined(HDUE_HOUDINI)
                            value=value>0 && std::isfinite(value) ? float(value/centimetersPerUnit_) : std::numeric_limits<float>::max();
#else
                            value=HdUEProjectDepth(value,projection_,centimetersPerUnit_);
#endif
                        }
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
                        buffer->publish(frame, std::move(pixels));
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
                    std::vector<float> pixels(size_t(aovFrame.required_bytes / sizeof(float)));
                    const auto copied = s.backend->api().copy_frame(s.session, aovFrame.frame_id, pixels.data(), aovFrame.required_bytes);
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
                    changed, settingsChanged, cameraChanged, converged_, (unsigned long long)s.lastFrame.frame_id, (unsigned long long)s.generation,
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
                    for (const auto& [key, value] : s.settings) {
                        if (!TfStringStartsWith(key.GetString(), "postProcess:")) continue;
                        names.push_back(key.GetString().substr(12));
                        values.push_back(value.IsHolding<bool>() ? (value.UncheckedGet<bool>() ? "True" : "False") : TfStringify(value));
                    }
                    if (tonemapper && std::find(names.begin(), names.end(), "AutoExposureBias") == names.end()) {
                        names.push_back("AutoExposureBias"); values.push_back(TfStringify(std::log2(std::max(lens[3], 1e-6f))));
                        names.push_back("AutoExposureApplyPhysicalCameraExposure"); values.push_back("False");
                    }
                    const auto list = s.settings.find(TfToken("postProcessMaterials"));
                    if (list != s.settings.end()) {
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
                    const bool moving = cameraMotion != s.cameraMotion.end() ||
                        std::any_of(s.meshes.begin(), s.meshes.end(), [](const auto& entry) { return entry.second.moving(); });
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
                    aovSlot_ = 0; aovInFlight_ = false; aovsDone_ = aovs_.empty();
                    for (const auto& [aovBuffer, id] : aovs_) aovBuffer->invalidate();
                }
            }
            if (!needsRequest && converged_ && !aovsDone_ && !aovInFlight_) {
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
            { std::lock_guard lock(s.mutex); s.error.clear(); }
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
    HdueStatus status{}; status.struct_size = sizeof(status);
    ueRun([&] { state_->backend->require(state_->backend->api().get_status(&status), "get_status"); });
    if (status.state == HDUE_STATE_COLD && !selfHost().backend) {
        selfHost().directory = hostDirectory;
        startSelfHost(dll, state_->setting<std::string>("runtimeProject", runtimeConfig("runtime_project")));
        std::error_code ignored; std::filesystem::current_path(selfHost().directory, ignored);
        // UE reports ready only after its first ticks.
        selfHost().run([&] {
            for (int i = 0; i < 600 && !(status.state == HDUE_STATE_READY && status.world_ready && status.rhi_ready); ++i) {
                selfHost().tick();
                state_->backend->require(state_->backend->api().get_status(&status), "get_status");
                if (status.state == HDUE_STATE_FAILED) break;
                Sleep(10);
            }
        });
    }
    if (selfHost().backend) { ++selfHost().delegates; selfHosted_ = true; }
    if (status.state != HDUE_STATE_READY || !status.world_ready || !status.rhi_ready)
        throw std::runtime_error("hdUE requires a ready runtime; launch usdview with scripts/Launch-Usdview.ps1");
    if (state_->backend->material_api().get_renderer_info)
        ueRun([&] { state_->backend->require(state_->backend->material_api().get_renderer_info(&state_->rendererInfo), "get_renderer_info"); });
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
    std::lock_guard lock(state_->mutex);
    const auto old=state_->settings.find(key);
    if (old!=state_->settings.end() && old->second==value) return;
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
        key==TfToken("framesPerSecond") || key==TfToken("tonemapper") || key==TfToken("postProcessMaterials") ||
        TfStringStartsWith(key.GetString(), "postProcess:")) ++state_->requestSettingsRevision;
    if (key==TfToken("houdini:frame") || key==TfToken("houdini:fps")) {
        const double frame=state_->setting<double>("houdini:frame",1.);
        const double fps=state_->setting<double>("houdini:fps",24.);
        if (std::isfinite(frame) && std::isfinite(fps) && fps>0) {
            state_->settings[TfToken("timeSeconds")]=VtValue(frame/fps);
            ++state_->requestSettingsRevision;
        }
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
    VtDictionary stats;
    try { ueRun([&] { stats = renderStats(); }); } catch (const std::exception& e) { stats["error"] = VtValue(std::string(e.what())); }
    return stats;
}
VtDictionary HdUERenderDelegate::renderStats() const {
    std::lock_guard lock(state_->mutex);
    HdueResidencyStatus residency{}; residency.struct_size = sizeof(residency);
    const auto& residencyApi = state_->backend->residency_api();
    if (state_->session && state_->backend->surface_api().get_status)
        state_->backend->surface_api().get_status(state_->session,&state_->surfaceStatus);
    if (state_->session && residencyApi.get_status) residencyApi.get_status(state_->session, &residency);
    VtDictionary nativeAssetIds;
    int triangles = 0;
    for (const auto& [id, mesh] : state_->meshes) {
        triangles += int(mesh.points.size() / 3);
        if (!mesh.sceneAsset.empty()) nativeAssetIds[mesh.scenePath]=VtValue(id);
    }
    int translucentMaterials = 0;
    for (const auto& [id, material] : state_->materials) if (material.opacity < 1.f && asset(*state_, "nativeMaterialAssets", id).empty()) ++translucentMaterials;
    int sceneLights = int(state_->nativeAssets.lights);
    HdueLightingStatus lighting{}; lighting.struct_size=sizeof(lighting);
    const auto& lightingApi=state_->backend->lighting_api();
    if(lightingApi.get_status && state_->session) state_->backend->require(lightingApi.get_status(state_->session,&lighting),"lighting_status");
    if (state_->setting<bool>("sceneLightsEnabled", true))
        for (const auto& [id, light] : state_->lights) if (!light.viewer && active(light)) ++sceneLights;
    return { {"error", VtValue(residency.phase == HDUE_RESIDENCY_FAILED ? std::string(residency.message) : state_->error)}, {"generation", VtValue(state_->generation)},
        {"wetBindings",VtValue(int(state_->surfaceStatus.binding_count))}, {"wetUploadPending",VtValue(bool(state_->surfaceStatus.upload_pending))},
        {"wetMaskUploads",VtValue(state_->surfaceStatus.mask_uploads)}, {"wetMaskBytes",VtValue(state_->surfaceStatus.mask_bytes)},
        {"wetGeometryUploads",VtValue(state_->surfaceStatus.geometry_uploads)}, {"wetUpdates",VtValue(state_->surfaceStatus.updates)},
        {"vtResidencyPhase", VtValue(int(residency.phase))}, {"vtResidencyGeneration", VtValue(residency.generation)},
        {"vtRequiredPages", VtValue(residency.required_pages)}, {"vtResidentPages", VtValue(residency.resident_pages)},
        {"vtLockedTiles", VtValue(residency.locked_tiles)}, {"vtRequiredMappings", VtValue(residency.required_mappings)},
        {"vtReadyMappings", VtValue(residency.ready_mappings)}, {"vtTextures", VtValue(int(residency.textures))},
        {"vtPreparationFrames", VtValue(int(residency.preparation_frames))}, {"vtPreparationMs", VtValue(residency.preparation_ms)},
        {"materialGraphBlocked", VtValue(!state_->setting<bool>("materialGraphValid", true))},
        {"translucentMaterials", VtValue(translucentMaterials)},
        {"megaLightsProject",VtValue(bool(lighting.mega_lights_project))}, {"megaLightsAllowed",VtValue(bool(lighting.mega_lights_allowed))},
        {"megaLightsActive",VtValue(bool(lighting.mega_lights_active))}, {"domeLights",VtValue(int(lighting.dome_count))},
        {"environmentPending",VtValue(bool(lighting.environment_pending))}, {"environmentCubeResolution",VtValue(int(lighting.cube_resolution))},
        {"environmentMaxRadiance",VtValue(lighting.environment_max_radiance)},
        {"nativeAssetInstances", VtValue(int(state_->nativeAssets.instances))},
        {"nativeAssetIds", VtValue(nativeAssetIds)},
        {"nativeAssetLoading", VtValue(int(state_->nativeAssets.loading))},
        {"nativeAssetFailed", VtValue(int(state_->nativeAssets.failed))},
        {"nativeActors", VtValue(int(state_->nativeAssets.actors))},
        {"nativeMeshes", VtValue(int(state_->nativeAssets.meshes))},
        {"nativeLights", VtValue(int(state_->nativeAssets.lights))},
        {"sessionHandle", VtValue(uint64_t(state_->session))},
        {"submittedMeshes", VtValue(state_->submittedMeshes)}, {"submittedMaterials", VtValue(state_->submittedMaterials)},
        {"gpuLocalMemoryBytes", VtValue(state_->gpuLocalMemoryBytes)}, {"gpuMemoryAvailable", VtValue(state_->gpuMemoryAvailable)},
        {"materialParameterUpdates", VtValue(state_->materialParameterUpdates)},
        {"substrateEnabled", VtValue(bool(state_->rendererInfo.substrate_enabled))},
        {"adaptiveGBuffer", VtValue(bool(state_->rendererInfo.adaptive_gbuffer))},
        {"substrateClosureBudget", VtValue(int(state_->rendererInfo.project_closures_per_pixel))},
        {"substrateBytesPerPixel", VtValue(int(state_->rendererInfo.bytes_per_pixel))},
        {"pick", VtValue(state_->pickResult)},
        {"publishedFrames", VtValue(state_->publishedFrames)}, {"sceneSubmissions", VtValue(state_->sceneSubmissions)},
        {"staleFrameRetries", VtValue(state_->staleFrameRetries)},
        {"coalescedCameraUpdates", VtValue(state_->coalescedCameraUpdates)}, {"requestCpuMsTotal", VtValue(state_->requestCpuMsTotal)},
        {"meshes", VtValue(int(state_->meshes.size()))}, {"materials", VtValue(int(state_->materials.size()))}, {"lights", VtValue(int(state_->lights.size()))},
        {"sceneLights", VtValue(sceneLights)}, {"viewerLights", VtValue(state_->viewerLights)}, {"autoLightActive", VtValue(state_->autoLightActive)},
        {"triangles", VtValue(triangles)}, {"frameId", VtValue(state_->lastFrame.frame_id)}, {"completedSamples", VtValue(int(state_->lastFrame.completed_samples))},
        {"percentDone", VtValue(state_->lastFrame.converged ? 100.f : state_->lastFrame.frame_id && state_->setting<std::string>("renderMode", "realtime") == "pathtraced"
            ? std::min(99.f, 100.f * float(state_->lastFrame.completed_samples) / float(std::max(1, state_->setting<int>("samplesPerPixel", 64)))) : 0.f)},
        {"totalClockTime", VtValue(state_->lastFrame.request_to_frame_ms / 1000.)},
        {"requestToFrameMs", VtValue(state_->lastFrame.request_to_frame_ms)}, {"readbackMs", VtValue(state_->lastFrame.readback_ms)} };
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
    for (const auto& [id, mesh] : state_->meshes) {
        if (!mesh.visible || !mesh.pickMesh || !asset(*state_, "nativeMeshAssets", id).empty() || std::abs(mesh.transform.GetDeterminant()) < 1e-20) continue;
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

