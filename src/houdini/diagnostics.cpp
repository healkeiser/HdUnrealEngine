#define HDUE_VIEWPORT_EXPORTS 1
#include "hdue/viewport_api.h"
#include "delegate.h"
#include "diagnostics.h"
#include <windows.h>
#include <cstring>
#include <mutex>
#include <cmath>
#include "pxr/base/gf/vec4f.h"
namespace {
std::weak_ptr<PXR_NS::HdUEState> Viewport;
PXR_NS::HdUERenderDelegate* Delegate = nullptr;
}  // namespace
PXR_NAMESPACE_OPEN_SCOPE
void HdUERegisterViewport(const std::shared_ptr<HdUEState>& state, HdUERenderDelegate* delegate) {
    Viewport = state;
    Delegate = delegate;
}
void HdUEUnregisterViewport(HdUERenderDelegate* delegate) {
    if (Delegate == delegate) {
        Delegate = nullptr;
        Viewport.reset();
    }
}
PXR_NAMESPACE_CLOSE_SCOPE
namespace {
// The hdUE host (Houdini's UI thread) calls these. Its first call claims the viewport state; calls from any other
// thread are refused, as Houdini owns its UI objects there.
bool onHostThread(PXR_NS::HdUEState& state) {
    std::lock_guard lock(state.mutex);
    if (!state.createThread) state.createThread = GetCurrentThreadId();
    return GetCurrentThreadId() == state.createThread;
}
}  // namespace
extern "C" HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_surfaces(const HdueViewportSurfaces* update) {
    if (!update || update->struct_size != sizeof(*update) || update->abi_version != 2 || update->mask_preview > 1 ||
        update->binding_count > 100000 || (update->binding_count && !update->bindings))
        return HDUE_INVALID_ARGUMENT;
    const auto state = Viewport.lock();
    if (!state || !Delegate) return HDUE_PENDING;
    if (!onHostThread(*state)) return HDUE_WRONG_THREAD;
    if (!update->session || update->session != state->session) return HDUE_INVALID_STATE;
    using namespace PXR_NS;
    VtDictionary bindings;
    for (uint32_t i = 0; i < update->binding_count; ++i) {
        const auto& b = update->bindings[i];
        if (!b.prim_path || !SdfPath::IsValidPathString(b.prim_path) || !SdfPath(b.prim_path).IsAbsolutePath() ||
            !SdfPath(b.prim_path).IsPrimPath() || !b.variant_asset || !b.preview_asset || !b.mask_primvar ||
            !std::isfinite(b.amount) || !std::isfinite(b.roughness) || bindings.count(b.prim_path) || b.reserved ||
            (b.flags & ~2u))
            return HDUE_INVALID_ARGUMENT;
        bindings[b.prim_path] = VtValue(VtDictionary{{"asset", VtValue(std::string(b.variant_asset))},
                                                     {"previewAsset", VtValue(std::string(b.preview_asset))},
                                                     {"maskPrimvar", VtValue(std::string(b.mask_primvar))},
                                                     {"amount", VtValue(b.amount)},
                                                     {"roughness", VtValue(b.roughness)}});
        auto fields = bindings[b.prim_path].UncheckedGet<VtDictionary>();
        fields["graph"] = VtValue(bool(b.flags & 2));
        bindings[b.prim_path] = VtValue(fields);
    }
    if (!update->validation_error || !*update->validation_error)
        Delegate->SetRenderSetting(TfToken("surfaceEffects"), VtValue(bindings));
    Delegate->SetRenderSetting(TfToken("wetValidationError"),
                               VtValue(std::string(update->validation_error ? update->validation_error : "")));
    Delegate->SetRenderSetting(TfToken("wetMaskPreview"), VtValue(bool(update->mask_preview)));
    return HDUE_OK;
}
extern "C" HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_materials(const HdueViewportMaterials* update) {
    if (!update || update->struct_size != sizeof(*update) || update->abi_version != 1 || update->reserved ||
        update->material_count > 65536 || (update->material_count && !update->materials))
        return HDUE_INVALID_ARGUMENT;
    const auto state = Viewport.lock();
    if (!state || !Delegate) return HDUE_PENDING;
    if (!onHostThread(*state)) return HDUE_WRONG_THREAD;
    if (!update->session || update->session != state->session) return HDUE_INVALID_STATE;
    using namespace PXR_NS;
    VtDictionary assets, overrides;
    const auto text = [](const char* value) { return value && value[0] && strnlen(value, 4097) <= 4096; };
    for (uint32_t i = 0; i < update->material_count; ++i) {
        const auto& material = update->materials[i];
        if (!text(material.material_id) || !SdfPath::IsValidPathString(material.material_id) ||
            !SdfPath(material.material_id).IsAbsolutePath() || !SdfPath(material.material_id).IsPrimPath() ||
            !text(material.native_asset) || material.native_asset[0] != '/' || material.reserved ||
            material.parameter_count > 4096 || (material.parameter_count && !material.parameters) ||
            assets.count(material.material_id))
            return HDUE_INVALID_ARGUMENT;
        assets[material.material_id] = VtValue(std::string(material.native_asset));
        VtDictionary fields;
        for (uint32_t p = 0; p < material.parameter_count; ++p) {
            const auto& parameter = material.parameters[p];
            if (!text(parameter.name) || parameter.reserved || fields.count(parameter.name))
                return HDUE_INVALID_ARGUMENT;
            if (parameter.type == HDUE_PARAMETER_TEXTURE) {
                if (!text(parameter.texture_asset) || parameter.texture_asset[0] != '/') return HDUE_INVALID_ARGUMENT;
                fields[parameter.name] = VtValue(std::string(parameter.texture_asset));
            } else if (parameter.type == HDUE_PARAMETER_SCALAR || parameter.type == HDUE_PARAMETER_VECTOR) {
                for (float value : parameter.value)
                    if (!std::isfinite(value)) return HDUE_INVALID_ARGUMENT;
                fields[parameter.name] = parameter.type == HDUE_PARAMETER_SCALAR
                                             ? VtValue(parameter.value[0])
                                             : VtValue(GfVec4f(parameter.value[0], parameter.value[1],
                                                               parameter.value[2], parameter.value[3]));
            } else
                return HDUE_INVALID_ARGUMENT;
        }
        overrides[material.material_id] = VtValue(fields);
    }
    Delegate->SetRenderSetting(TfToken("nativeMaterialAssets"), VtValue(assets));
    Delegate->SetRenderSetting(TfToken("nativeMaterialParameters"), VtValue(overrides));
    return HDUE_OK;
}
extern "C" HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_textures(const HdueViewportTextures* update) {
    if (!update || update->struct_size != sizeof(*update) || update->abi_version != 1 || update->reserved ||
        update->texture_count > 65536 || (update->texture_count && !update->textures))
        return HDUE_INVALID_ARGUMENT;
    const auto state = Viewport.lock();
    if (!state || !Delegate) return HDUE_PENDING;
    if (!onHostThread(*state)) return HDUE_WRONG_THREAD;
    if (!update->session || update->session != state->session) return HDUE_INVALID_STATE;
    using namespace PXR_NS;
    VtDictionary files;
    const auto text = [](const char* value) { return value && value[0] && strnlen(value, 4097) <= 4096; };
    for (uint32_t i = 0; i < update->texture_count; ++i) {
        const auto& t = update->textures[i];
        if (!text(t.uri) || !text(t.file) || files.count(t.uri) ||
            (std::strncmp(t.uri, "op:", 3) && std::strncmp(t.uri, "opdef:", 6)))
            return HDUE_INVALID_ARGUMENT;
        files[t.uri] = VtValue(std::string(t.file));
    }
    Delegate->SetRenderSetting(TfToken("opTextures"), VtValue(files));
    return HDUE_OK;
}
extern "C" HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_set_viewport_up_axis(uint32_t z_up) {
    if (z_up > 1) return HDUE_INVALID_ARGUMENT;
    const auto state = Viewport.lock();
    if (!state || !Delegate) return HDUE_PENDING;
    if (!onHostThread(*state)) return HDUE_WRONG_THREAD;
    using namespace PXR_NS;
    // Like Houdini's stageMetersPerUnit: an authored unreal:upAxis overrides it.
    Delegate->SetRenderSetting(TfToken("stageUpAxis"), VtValue(std::string(z_up ? "Z" : "Y")));
    return HDUE_OK;
}
extern "C" HDUE_VIEWPORT_EXPORT HdueResult HDUE_CALL hdue_get_viewport_status(HdueViewportStatus* status) {
    if (!status || status->struct_size != sizeof(*status)) return HDUE_INVALID_ARGUMENT;
    HdueViewportStatus out{};
    out.struct_size = sizeof(out);
    if (const auto s = Viewport.lock()) {
        // Houdini's UI thread polls this: while an upload is being prepared under the state lock, answer with the last
        // status.
        std::unique_lock lock(s->mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            std::lock_guard cached(s->statusMutex);
            if (const auto last = std::static_pointer_cast<HdueViewportStatus>(s->viewportStatus)) {
                *status = *last;
                return HDUE_OK;
            }
            lock.lock();
        }
        out.active = 1;
        out.create_thread = s->createThread;
        out.execute_thread = s->executeThread;
        out.session = s->session;
        out.generation = s->generation;
        out.published_frames = s->publishedFrames;
        out.scene_submissions = s->sceneSubmissions;
        out.mode = s->setting<std::string>("renderMode", "realtime") == "pathtraced" ? HDUE_PATH_TRACED : HDUE_REALTIME;
        out.meshes = uint32_t(s->meshes.size());
        out.materials = uint32_t(s->materials.size());
        out.lights = uint32_t(s->lights.size());
        // Settings can change before Hydra gets its next Execute. Do not label
        // the previous mode/scene's completed image as the new request's result.
        if (s->submittedRequestSettingsRevision == s->requestSettingsRevision && s->submittedRevision == s->revision)
            out.frame = s->lastFrame;
        std::memcpy(out.error, s->error.c_str(), std::min(s->error.size(), sizeof(out.error) - 1));
        std::lock_guard cached(s->statusMutex);
        s->viewportStatus = std::make_shared<HdueViewportStatus>(out);
    }
    *status = out;
    return HDUE_OK;
}
