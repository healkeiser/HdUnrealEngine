#pragma once
#include "hdue/backend.hpp"
#include "picking.h"
#include "placement.h"
#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/getenv.h"
#include <algorithm>
#include <type_traits>
#include <array>
#include <functional>
#include <map>
#include <atomic>
#include <set>
#include <mutex>
#include <vector>

#ifdef HDUE_HYDRA_EXPORTS
#define HDUE_HYDRA_API __declspec(dllexport)
#else
#define HDUE_HYDRA_API __declspec(dllimport)
#endif

PXR_NAMESPACE_OPEN_SCOPE
class HdUEGpuFrames;
// Diagnostic switches, read once (they were read per prim and per Execute).
inline bool HdUETraceScene() {
    static const bool on = TfGetenvBool("HDUE_TRACE_SCENE", false);
    return on;
}
inline bool HdUETracePass() {
    static const bool on = TfGetenvBool("HDUE_TRACE_PASS", false);
    return on;
}
inline bool HdUETraceSettings() {
    static const bool on = TfGetenvBool("HDUE_TRACE_SETTINGS", false);
    return on;
}
// Matrix samples at frame-relative times around the current frame (motion blur).
struct HdUEMotionSamples {
    std::vector<float> times;
    std::vector<GfMatrix4d> values;
    bool moving() const {
        for (const auto& v : values)
            if (!GfIsClose(v, values.front(), 1e-9)) return true;
        return false;
    }
    // Piecewise linear per element, clamped to the end samples; identity without samples.
    GfMatrix4d at(float t) const {
        if (values.empty()) return GfMatrix4d(1);
        if (t <= times.front()) return values.front();
        for (size_t i = 1; i < times.size(); ++i)
            if (t <= times[i]) {
                const double u = (t - times[i - 1]) / std::max(1e-6f, times[i] - times[i - 1]);
                return values[i - 1] * (1 - u) + values[i] * u;
            }
        return values.back();
    }
};
struct HdUEMeshData {
    std::string id, material, scenePath, sceneAsset, bindingPath, sourceId;
    // Indexed triangles: three indices per triangle into the per-vertex arrays (points, normals, uv, colors, masks).
    std::vector<GfVec3f> points, normals;
    std::vector<GfVec2f> uv;
    std::vector<std::vector<GfVec2f>> uvSets;  // the material's other UV primvars (HdueMeshDesc uv sets 1..), like uv
    std::vector<GfVec3f> colors;               // empty unless displayColor varies
    std::vector<uint32_t> indices;
    std::map<std::string, std::vector<float>> masks;
    std::map<std::string, std::string> maskErrors;
    std::vector<uint32_t>
        sourceCorners;  // per vertex: a triangulated corner (3 * triangle + corner) of the source mesh
    GfMatrix4d transform{1};
    bool visible = true;
    TfToken renderTag;                    // purpose; hidden while the render pass does not draw it
    int primId = -1;                      // HdRprim::GetPrimId, written by the ID pass
    VtArray<TfToken> categories;          // light/shadow link collections that include the prim (GetCategories)
    bool doubleSided = true;              // UsdGeomGprim doubleSided; hdUE:cullBackface culls single-sided backs
    bool instanced = false;               // prototype of a Hydra instancer: drawn once per entry of instances
    std::vector<GfMatrix4d> instances;    // USD world matrices (prototype transform included)
    std::vector<GfVec3f> instanceColors;  // per instance: an instance-rate displayColor (or material colour primvar)
    std::vector<int32_t> instanceIds;     // Hydra instanceId per instance; empty means the instance index
    // Motion blur: the matrices at time t are motionLeft.at(t) * M * motionRight.at(t) + velocity * t +
    // acceleration * t^2 / 2, for M the transform or each instance. Both factors are relative to the current frame
    // (identity at t = 0).
    HdUEMotionSamples motionLeft, motionRight;
    std::vector<GfVec3f> velocities;     // per instance, world units per frame
    std::vector<GfVec3f> accelerations;  // per instance, world units per frame^2
    // Deformation blur: per-vertex point offsets at frame-relative times (piecewise linear); empty unless points move.
    std::vector<float> deformTimes;
    std::vector<std::vector<GfVec3f>> deformOffsets;
    bool moving() const {
        return motionLeft.moving() || motionRight.moving() || !velocities.empty() || !accelerations.empty() ||
               !deformTimes.empty();
    }
    std::vector<GfVec3f> pointsAt(float t) const {
        auto out = points;
        if (deformTimes.empty() || !t) return out;
        size_t i = 1;
        while (i + 1 < deformTimes.size() && t > deformTimes[i]) ++i;
        const float span = std::max(1e-6f, deformTimes[i] - deformTimes[i - 1]);
        const float u = std::clamp((t - deformTimes[i - 1]) / span, 0.f, 1.f);
        for (size_t c = 0; c < out.size(); ++c) out[c] += deformOffsets[i - 1][c] * (1 - u) + deformOffsets[i][c] * u;
        return out;
    }
    std::shared_ptr<const HdUEPickMesh> pickMesh;
};
// Whether placing an uploaded mesh at transform needs no new geometry: a non-instanced mesh uploaded in local space,
// moved without shear (HdUEShears). The motion extension then moves its UE actor.
inline bool HdUEPlacedOnly(const HdUEMeshData& mesh, const GfMatrix4d& transform) {
    return mesh.transform != transform && !mesh.instanced && mesh.sceneAsset.empty() && !HdUEShears(mesh.transform) &&
           !HdUEShears(transform);
}
// A dense density grid; local voxel (i, j, k) covers [i, i+1]^3 and localToWorld maps it to USD world.
struct HdUEVolumeData {
    std::string id;
    bool visible = true;
    TfToken renderTag;
    uint32_t width = 0, height = 0, depth = 0;
    VtFloatArray density;  // shared by the volumes of one prim's instances
    GfMatrix4d localToWorld{1};
    GfVec3f albedo{.8f};
};
struct HdUETextureData {
    std::string file;
    GfVec4f scale{1}, bias{0}, mask{1, 0, 0, 0};
    // A colour image, converted with OCIO from colorSpace (its authored colour space; empty: scene_linear for float
    // images) to linear Rec.709 (HdUELinearRec709Texture).
    bool convert = false;
    std::string colorSpace;
    uint32_t wrap[2] = {HDUE_WRAP_REPEAT, HDUE_WRAP_REPEAT};
    uint32_t encoding = HDUE_TEXTURE_ENCODING_AUTO;
    uint32_t uvSet = 0;  // index into the material's UV primvars (HdUEMaterialPrimvars)
};
// op: (Houdini COP) and opdef: (embedded HDA) images exist only inside the host, which exports them and maps each
// path to its file (render setting opTextures). Materials and lights keep these paths; they are resolved when packed.
inline bool HdUEHostTexture(const std::string& path) { return !path.rfind("op:", 0) || !path.rfind("opdef:", 0); }
struct HdUEMaterialData {
    std::string id;
    GfVec3f color{0.7f}, emissive{0};
    float metallic = 0, roughness = 0.5f, opacity = 1, opacityThreshold = 0,
          displacement = 0;  // displacement in scene units
    float occlusion = 1, clearcoat = 0, clearcoatRoughness = 0.01f, ior = 1.5f;
    GfVec3f specularColor{0};
    bool specularWorkflow = false;
    // MaterialX standard_surface / OpenPBR; subsurfaceRadius is the mean free path in scene units.
    float subsurface = 0, sheen = 0, sheenRoughness = .3f;
    GfVec3f subsurfaceColor{1}, subsurfaceRadius{1}, sheenColor{1}, coatColor{1};
    bool thinWalled = false, subsurfaceBaseTexture = false, sheenBaseTexture = false;  // HDUE_MATERIAL_*
    std::array<float, 6> uvTransform{1, 0, 0, 0, 1, 0};
    std::array<HdUETextureData, HDUE_TEXTURE_SLOT_COUNT> textures;
    bool vertexColor = false;  // HDUE_MATERIAL_VERTEX_COLOR
};
struct HdUELightData {
    std::string id;
    std::string texture;
    GfMatrix4d transform{1};
    GfVec3f color{1};
    uint32_t type = HDUE_DISTANT_LIGHT;
    float intensity = 1, width = 1, height = 1, length = 1, angle = .53f;
    float diffuse = 1, specular = 1, coneAngle = 180, coneSoftness = 0;
    std::string ies;  // shaping:ies:file
    float iesAngleScale = 0;
    bool iesNormalize = false;
    TfToken lightLink, shadowLink;  // collection ids; empty when the collection includes everything
    uint32_t flags = HDUE_LIGHT_SHADOWS | HDUE_LIGHT_BACKGROUND | HDUE_LIGHT_MEGALIGHTS,
             textureFormat = HDUE_ENVIRONMENT_AUTOMATIC;
    bool visible = true, viewer = false;
};
struct HdUEState : HdRenderParam {
    std::mutex mutex;
    // Short reads/writes of shared state from the UE thread (Sync runs concurrently on Houdini's threads).
    template <class F>
    auto locked(F&& f) {
        std::lock_guard lock(mutex);
        return f();
    }
    void setError(std::string message) {
        std::lock_guard lock(mutex);
        error = std::move(message);
    }
    std::map<std::string, HdUEMeshData> meshes;
    std::map<std::string, HdUEMaterialData> materials;
    std::map<std::string, HdUELightData> lights;
    std::map<std::string, HdUEVolumeData> volumes;
    std::map<std::string, HdUEMotionSamples> cameraMotion;  // absolute camera transforms by camera id
    std::atomic<bool> paused{false};  // Pause()/houdini:render_pause: no frames are requested or drawn
    // What the renderer is doing, for Houdini's viewport (rendererStage). Its own lock: the UI reads it while the UE
    // thread is busy uploading, when the stats refresh queued on that thread cannot run.
    void setStage(std::string text) {
        std::lock_guard lock(stageMutex);
        stage = std::move(text);
    }
    std::string currentStage() const {
        std::lock_guard lock(stageMutex);
        return stage;
    }
    mutable std::mutex stageMutex;
    std::string stage = "Starting Unreal Engine";
    std::mutex cameraMutex;  // guards cameraMotion (camera Sync runs on Houdini's thread, without the state lock)
    std::set<std::string> dirtyVolumes;
    std::map<std::string, uint64_t> geometryRevisions;
    HdRenderIndex* renderIndex = nullptr;
    std::set<std::string> dirtyMeshes, dirtyMaterials, dirtyLights, removed;
    // Meshes whose transform alone changed: placed through the motion extension, their geometry stays in UE.
    std::set<std::string> movedMeshes;
    TfTokenVector renderTags;  // sorted; the render pass's last tags (empty: everything)
    static bool tagShown(const TfTokenVector& tags, const TfToken& tag) {
        return tags.empty() || std::binary_search(tags.begin(), tags.end(), tag);
    }
    bool shown(const TfToken& tag) const { return tagShown(renderTags, tag); }
    // Cached viewport status for Houdini's UI while an upload holds the lock (diagnostics.cpp).
    std::mutex statusMutex;
    std::shared_ptr<void> viewportStatus;
    // Meshes erased since the last submit, kept so a re-created identical prim cancels its removal (HdUEState::erase).
    std::map<std::string, HdUEMeshData> erasedMeshes;
    HdRenderSettingsMap settings;
    // Guards settings: Houdini's UI thread writes them (SetRenderSetting) while hdUE's UE thread reads them.
    mutable std::recursive_mutex settingsMutex;
    std::set<TfToken> namespaced;  // settings set as hdUE:<name>, which the bare <name> no longer overrides
    // GetRenderStats' last result; the UE thread refreshes it (Houdini's UI asks often and must not wait on UE).
    std::mutex statsMutex;
    VtDictionary stats;
    std::atomic<bool> statsPending{false};
    HdRenderSettingsMap settingsSnapshot() const {
        std::lock_guard lock(settingsMutex);
        return settings;
    }
    std::shared_ptr<const VtDictionary> wetBindings = std::make_shared<const VtDictionary>();
    std::shared_ptr<hdue::Backend> backend;
    HdueSession session = 0;
    std::shared_ptr<HdUEGpuFrames> gpuFrames;  // Houdini's GL viewport (gpu_frames.h); set under mutex by SetDrivers
    uint64_t aovGeneration = 0;                // AOV requests (hdue/aov_api.h); generation stays the beauty's
    uint32_t width = 0, height = 0;
    // Revisions are also read without the lock by IsConverged on the host's thread.
    std::atomic<uint64_t> revision{1}, submittedRevision{0}, requestSettingsRevision{1};
    uint64_t generation = 0;
    uint64_t submittedRequestSettingsRevision = 0;
    uint64_t publishedFrames = 0, sceneSubmissions = 0, coalescedCameraUpdates = 0;
    uint64_t staleFrameRetries = 0;
    uint64_t submittedMeshes = 0, submittedMaterials = 0, materialParameterUpdates = 0;
    uint64_t gpuLocalMemoryBytes = 0;
    bool gpuMemoryAvailable = false;
    int viewerLights = 0;
    bool autoLightActive = false;
    double requestCpuMsTotal = 0;
    std::string error;
    HdueFrameInfo lastFrame{};
    HdueRendererInfo rendererInfo{sizeof(HdueRendererInfo)};
    uint32_t createThread = 0, executeThread = 0;
    VtDictionary pickResult;
    std::map<std::string, uint32_t> pickMaterialFlags;
    bool firstBatch = true;
    bool assetRequestPending = false;
    HdueAssetSceneStatus nativeAssets{sizeof(HdueAssetSceneStatus)};
    HdueSurfaceStatus surfaceStatus{sizeof(HdueSurfaceStatus)};
    uint32_t submittedNativeLights = 0;
    // Light linking as UE last received it: channels per light (0: all) and the object table (prepareLinks).
    std::map<std::string, uint32_t> linkedLights, linkedObjects;
    ~HdUEState() override;
    void erase(const std::string& id);
    template <class T>
    T setting(const char* key, T fallback) const {
        std::lock_guard lock(settingsMutex);
        const auto it = settings.find(TfToken(key));
        if (it == settings.end()) return fallback;
        if (it->second.IsHolding<T>()) return it->second.UncheckedGet<T>();
        // USD authors these as string or token, int or int64, float or double: accept either.
        if constexpr (std::is_same_v<T, std::string>) {
            if (it->second.IsHolding<TfToken>()) return it->second.UncheckedGet<TfToken>().GetString();
        } else if constexpr (std::is_arithmetic_v<T>) {
            const VtValue cast = VtValue::Cast<T>(it->second);
            if (!cast.IsEmpty()) return cast.UncheckedGet<T>();
        }
        return fallback;
    }
    // USD scene space to UE (centimetres, Z up). Converter reads the settings once, for loops over many points.
    struct Converter {
        double scale = 1;
        bool zUp = false;
        GfVec3d operator()(const GfVec3d& p, bool direction = false) const {
            return (zUp ? GfVec3d(p[0], -p[1], p[2]) : GfVec3d(-p[2], p[0], p[1])) * (direction ? 1 : scale);
        }
    };
    // houdini:interactive: Houdini's viewport is tumbling, scrubbing or playing. Not "normal" nor "flipbooking" (a
    // flipbook, or a screenshot, waits for full quality).
    bool interactive() const {
        const auto purpose = setting<std::string>("houdini:interactive", "normal");
        return purpose == "tumbling" || purpose == "scrubbing" || purpose == "playing";
    }
    // Path Tracer samples, capped while interactive so tumbling and playback keep up (all samples once still).
    int samplesPerPixel() const {
        const int samples = std::max(1, setting<int>("samplesPerPixel", 64));
        return interactive() ? std::min(samples, 8) : samples;  // ponytail: fixed cap; a setting if users ask
    }
    // hdUE:debugView, which replaces the realtime beauty only.
    std::string debugView() const {
        return setting<std::string>("renderMode", "realtime") == "realtime" ? setting<std::string>("debugView", "off")
                                                                            : "off";
    }
    // UE's tonemapper draws the beauty (hdUE:tonemapper, or a debug view drawn for display as UE's viewport does): the
    // camera exposure goes in before it. Buffer debug views stay linear data.
    bool toneMapped() const {
        const auto view = debugView();
        return setting<bool>("tonemapper", false) || (view != "off" && !view.starts_with("buffer:"));
    }
    // The camera exposure scales the beauty (not a debug view's data).
    bool exposed() const { return !toneMapped() && debugView() == "off"; }
    Converter converter() const {
        return {setting<double>("metersPerUnit", 0.01) * 100, setting<std::string>("upAxis", "Y") == "Z"};
    }
    GfVec3d convert(const GfVec3d& point, bool direction = false) const { return converter()(point, direction); }
};

class HDUE_HYDRA_API HdUERenderBuffer final : public HdRenderBuffer {
public:
    explicit HdUERenderBuffer(const SdfPath& id);
    ~HdUERenderBuffer() override;
    // Whether b is a render buffer that has not been deleted (a posted Execute holds pointers Hydra may have released).
    static bool alive(const HdRenderBuffer* b);
    bool Allocate(const GfVec3i&, HdFormat, bool) override;
    unsigned int GetWidth() const override;
    unsigned int GetHeight() const override;
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override {
        std::lock_guard lock(mutex_);
        return format_;
    }
    bool IsMultiSampled() const override {
        std::lock_guard lock(mutex_);
        return multiSampled_;
    }
    void* Map() override;
    void Unmap() override;
    bool IsMapped() const override;
    void Resolve() override {}
    bool IsConverged() const override;
    // Publishes a top-down float image with sourceChannels per pixel (0: the format's), stored bottom-up in the
    // buffer's format; channels the buffer has and the image lacks are filled (alpha 1).
    void publish(const HdueFrameInfo&, std::vector<float> pixels, size_t sourceChannels = 0);
    std::vector<float> takeSpare(size_t size);  // a previous frame's float allocation, sized for the next publish
    void invalidate();
    std::string allocationError() const {
        std::lock_guard lock(mutex_);
        return allocationError_;
    }
    // HdxAovInputTask composites a GPU texture from GetResource instead of mapping and uploading the CPU pixels.
    // resource runs on the host's graphics thread; empty (or an empty result) keeps the CPU path.
    VtValue GetResource(bool) const override;
    void setResource(std::function<VtValue()> resource) {
        std::lock_guard lock(mutex_);
        resource_ = std::move(resource);
    }

protected:
    void _Deallocate() override;

private:
    mutable std::mutex mutex_;
    uint32_t width_ = 0, height_ = 0, maps_ = 0;
    HdFormat format_ = HdFormatFloat32Vec4;
    bool converged_ = false, multiSampled_ = false, sizeWarned_ = false;
    std::string allocationError_;
    std::shared_ptr<std::vector<uint8_t>> pixels_, pinned_;  // in format_
    std::vector<uint8_t> spareBytes_;  // the replaced frame's storage, reused unless a Map still holds it
    std::vector<float> spare_;         // the last published float image, handed back by takeSpare
    std::function<VtValue()> resource_;
};

class HDUE_HYDRA_API HdUERenderDelegate final : public HdRenderDelegate {
public:
    explicit HdUERenderDelegate(const HdRenderSettingsMap& settings);
    ~HdUERenderDelegate() override;
    const TfTokenVector& GetSupportedRprimTypes() const override;
    const TfTokenVector& GetSupportedSprimTypes() const override;
    const TfTokenVector& GetSupportedBprimTypes() const override;
    // MaterialX surfaces first (materials.cpp maps them), then the universal UsdPreviewSurface.
    TfTokenVector GetMaterialRenderContexts() const override { return {TfToken("mtlx"), TfToken()}; }
    TfToken GetMaterialBindingPurpose() const override;
    HdRenderParam* GetRenderParam() const override { return state_.get(); }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override { return registry_; }
    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex*, const HdRprimCollection&) override;
    // Houdini pauses a deactivated viewport renderer (and on houdini:render_pause) instead of destroying it.
    bool IsPauseSupported() const override { return true; }
    bool Pause() override;
    bool Resume() override;
    bool IsStopSupported() const override { return true; }
    bool IsStopped() const override;
    bool Stop(bool blocking = true) override;
    bool Restart() override;

    HdInstancer* CreateInstancer(HdSceneDelegate*, const SdfPath&) override;
    void DestroyInstancer(HdInstancer*) override;
    HdRprim* CreateRprim(const TfToken&, const SdfPath&) override;
    void DestroyRprim(HdRprim*) override;
    HdSprim* CreateSprim(const TfToken&, const SdfPath&) override;
    HdSprim* CreateFallbackSprim(const TfToken&) override;
    void DestroySprim(HdSprim*) override;
    HdBprim* CreateBprim(const TfToken&, const SdfPath&) override;
    HdBprim* CreateFallbackBprim(const TfToken&) override;
    void DestroyBprim(HdBprim*) override;
    void CommitResources(HdChangeTracker*) override;
    void SetDrivers(const HdDriverVector& drivers) override;
    HdAovDescriptor GetDefaultAovDescriptor(const TfToken&) const override;
    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
    void SetRenderSetting(const TfToken&, const VtValue&) override;
    void SetRenderSetting(const TfToken&, const VtValue&, bool fromNamespace);
    VtDictionary GetRenderStats() const override;
    bool InvokeCommand(const TfToken&, const HdCommandArgs&) override;

private:
    std::shared_ptr<HdUEState> state_;
    HdResourceRegistrySharedPtr registry_;
    bool setPaused(bool paused);
    static VtDictionary renderStats(HdUEState& state);
    bool selfHosted_ = false;  // this delegate counts towards the UE runtime it started (husk)
};
PXR_NAMESPACE_CLOSE_SCOPE
