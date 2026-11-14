#pragma once
// The delegate's scene-upload bookkeeping, header-only so tests/scene_bookkeeping.cpp can run it without UE: which
// meshes an upload takes and gives back (ScenePacket in delegate.cpp), when a mesh's arrays are freed, and how Mesh
// Sync (prims.cpp) stores a new part against the previous one.
#include "delegate.h"
#include "surface_settings.h"
#include <cstdint>
#include <set>
#include <string>
#include <vector>
PXR_NAMESPACE_OPEN_SCOPE
// Whether a generic mesh's geometry arrays can be freed once UE has it (finishScene): change detection compares its
// geometryHash, update_meshes carries material, visibility and sidedness changes, and an upload that needs the arrays
// again re-syncs the mesh from Hydra. Kept where something reads them after the upload: deformation blur (pointsAt),
// wet masks and surfaces, displacement dicing and usdview's ray picks. A mesh that becomes wet later is re-pulled
// (prepareSurfaceEffects). hostOwnsUe: hdUE runs UE on its own thread (selfHost's worker; not usdview).
inline bool HdUEReleasableMesh(const HdUEState& state, const HdUEMeshData& m, bool hostOwnsUe) {
    // A moving sheared-instance companion keeps its arrays: motion blur bakes it again per sub-frame.
    if (!hostOwnsUe || !m.deformTimes.empty() || !m.masks.empty() || (m.bakedInstances && m.moving()) ||
        (state.wetBindings && !HdUEWetSettings(*state.wetBindings, m).empty()))
        return false;
    const auto material = state.materials.find(m.material);
    return material == state.materials.end() ||
           (material->second.textures[HDUE_TEXTURE_DISPLACEMENT].file.empty() && material->second.displacement == 0);
}
// Frees a mesh's geometry arrays, keeping what change detection and update_meshes read; returns the bytes freed.
inline uint64_t HdUEReleaseGeometry(HdUEMeshData& m) {
    const uint64_t bytes = HdUEMeshBytes(m);
    m.releasedTriangles = uint32_t(m.indices.size() / 3);
    // Swapped with empty vectors: `v = {}` is assign(initializer_list), which keeps the capacity, so released meshes
    // kept all their memory (NVIDIA's Da Vinci Workshop: about 30 GB).
    const auto drop = [](auto& v) { std::decay_t<decltype(v)>().swap(v); };
    drop(m.points);
    drop(m.normals);
    drop(m.uv);
    drop(m.uvSets);
    drop(m.colors);
    drop(m.indices);
    drop(m.sourceCorners);
    m.pickMesh = nullptr;
    m.released = true;
    return bytes;
}
// The dirty sets one scene upload covers (ScenePacket's); Sync may dirty more meanwhile, for the next upload.
struct HdUESceneSets {
    std::set<std::string> dirtyMeshes, movedMeshes, dirtyMaterials, dirtyLights, dirtyVolumes, dirtyActors, dirtyGrooms,
        removedIds;
    std::set<std::string> volumeParameters;  // volumes whose bound material's parameters alone changed
    // Meshes in dirtyMeshes only because their material changed: they go through update_meshes, released or not.
    std::set<std::string> materialMeshes;
    uint64_t bytes = 0;  // stored geometry of the meshes it takes
    // Takes the state's dirty sets (state lock held): the dirty meshes up to meshBudget bytes of stored geometry (at
    // least one), the other sets whole.
    void take(HdUEState& state, uint64_t meshBudget) {
        uint64_t bytes = 0;
        for (auto it = state.dirtyMeshes.begin();
             it != state.dirtyMeshes.end() && (dirtyMeshes.empty() || bytes < meshBudget);) {
            if (const auto mesh = state.meshes.find(*it); mesh != state.meshes.end())
                bytes += HdUEMeshBytes(mesh->second);
            dirtyMeshes.insert(*it);
            it = state.dirtyMeshes.erase(it);
        }
        this->bytes = bytes;
        movedMeshes.swap(state.movedMeshes);
        dirtyMaterials.swap(state.dirtyMaterials);
        // A material changed in place can change blend mode, and with it the backend's Nanite choice for the static
        // meshes drawn with it: its meshes go through update_meshes, which asks for the geometry of those built for
        // the other blend mode (FHdueScene::UpdateMeshes). Meshes Sync dirtied upload in full anyway.
        if (!dirtyMaterials.empty())
            for (const auto& [id, mesh] : state.meshes)
                if (dirtyMaterials.count(mesh.material) && mesh.sceneAsset.empty() && !dirtyMeshes.count(id) &&
                    !state.dirtyMeshes.count(id)) {
                    dirtyMeshes.insert(id);
                    materialMeshes.insert(id);
                }
        dirtyLights.swap(state.dirtyLights);
        dirtyVolumes.swap(state.dirtyVolumes);
        volumeParameters.swap(state.volumeParameters);
        dirtyActors.swap(state.dirtyActors);
        dirtyGrooms.swap(state.dirtyGrooms);
        // A groom takes its bound material's colour and roughness: edits to that material rebuild it.
        if (!dirtyMaterials.empty())
            for (const auto& [id, groom] : state.grooms)
                if (dirtyMaterials.count(groom.material)) dirtyGrooms.insert(id);
        removedIds.swap(state.removed);
    }
    // Gives them back after a failed upload (state lock held). Sync may have erased or revived prims meanwhile:
    // only prims that still exist stay dirty, and only prims still gone stay removed. taken: the meshes the upload
    // released when it took them (ScenePacket's deferred).
    void giveBack(HdUEState& state, const std::set<std::string>& taken) const {
        const auto restore = [](const std::set<std::string>& from, std::set<std::string>& to, const auto& prims) {
            for (const auto& id : from)
                if (prims.count(id)) to.insert(id);
        };
        for (const auto& id : taken) {
            // Released when taken but never reached UE: its arrays come back with the next Sync, uploaded in full.
            if (const auto mesh = state.meshes.find(id); mesh != state.meshes.end() && mesh->second.released) {
                state.fullUploads.insert(mesh->first);
                state.repull.insert(mesh->second.sourceId);
            }
        }
        for (const auto& id : dirtyMeshes)
            if (state.meshes.count(id) && !taken.count(id)) state.dirtyMeshes.insert(id);
        restore(movedMeshes, state.movedMeshes, state.meshes);
        restore(dirtyMaterials, state.dirtyMaterials, state.materials);
        restore(dirtyLights, state.dirtyLights, state.lights);
        restore(dirtyVolumes, state.dirtyVolumes, state.volumes);
        restore(volumeParameters, state.volumeParameters, state.volumes);
        restore(dirtyActors, state.dirtyActors, state.actors);
        restore(dirtyGrooms, state.dirtyGrooms, state.grooms);
        for (const auto& id : removedIds)
            if (!state.meshes.count(id) && !state.materials.count(id) && !state.lights.count(id) &&
                !state.volumes.count(id) && !state.actors.count(id) && !state.grooms.count(id))
                state.removed.insert(id);
    }
};
// Stores one part of a synced mesh (Mesh::Sync, state lock held, after HdUEStoreShearCompanion) against its previous
// or erased version: what changed decides whether it uploads in full (dirtyMeshes), is only placed (movedMeshes), or
// stays as UE has it. Adds its id to current.
inline void HdUEStoreMeshPart(HdUEState& state, HdUEMeshData&& part, std::set<std::string>& current) {
    // A prim deleted and re-created with the same data (Houdini rebuilds the Hydra scene when its LOP chain
    // changes) is compared with its last data, so an unchanged mesh is neither removed nor re-uploaded.
    auto previous = state.meshes.find(part.id);
    const bool revived = previous == state.meshes.end() && state.erasedMeshes.count(part.id);
    if (revived) previous = state.erasedMeshes.find(part.id);
    const auto missing = revived ? state.erasedMeshes.end() : state.meshes.end();
    if (previous != missing && previous->second.geometryHash == part.geometryHash)
        part.pickMesh = previous->second.pickMesh;
    // Pick structures are built on the first pick (HdUERenderDelegate pick): building a BVH for every mesh
    // here, under the state mutex, serialized scene loads (tens of seconds on Sponza).
    const bool onlyNativeBounds =
        previous != missing && !part.sceneAsset.empty() && previous->second.sceneAsset == part.sceneAsset &&
        previous->second.transform == part.transform && previous->second.visible == part.visible &&
        previous->second.assetFrame == part.assetFrame && previous->second.effectStart == part.effectStart;
    const bool moved = previous != missing && HdUEPlacedOnly(previous->second, part.transform);
    // A mesh UE asked for in full (fullUploads) uploads even when nothing changed since it was released.
    const bool meshChanged =
        !onlyNativeBounds &&
        (previous == missing || state.fullUploads.count(part.id) ||
         previous->second.geometryHash != part.geometryHash ||
         (previous->second.transform != part.transform && !moved) || previous->second.material != part.material ||
         previous->second.visible != part.visible || previous->second.renderTag != part.renderTag ||
         previous->second.sceneAsset != part.sceneAsset || previous->second.assetFrame != part.assetFrame ||
         previous->second.effectStart != part.effectStart || previous->second.instanced != part.instanced ||
         previous->second.instances != part.instances || previous->second.instanceColors != part.instanceColors ||
         previous->second.doubleSided != part.doubleSided);
    const auto id = part.id;
    // Link categories change no geometry: the next upload recomputes the light-link table (prepareLinks).
    const bool maskChanged = previous == missing || previous->second.masks != part.masks ||
                             previous->second.maskErrors != part.maskErrors ||
                             previous->second.categories != part.categories;
    // A released mesh Hydra resent with the same geometry stays released: UE has it, and update_meshes (or a
    // move) carries what changed, unless UE asked for it in full (fullUploads). A revived mesh whose removal
    // is still queued (not taken into an upload) is one UE still holds.
    if ((!revived || state.removed.count(id)) && previous != missing && previous->second.released &&
        !state.fullUploads.count(id) && previous->second.geometryHash == part.geometryHash && part.sceneAsset.empty() &&
        previous->second.sceneAsset.empty() && previous->second.instanced == part.instanced &&
        previous->second.instances == part.instances && previous->second.instanceColors == part.instanceColors &&
        (previous->second.transform == part.transform || moved) && HdUEReleasable(state, part))
        HdUEReleaseGeometry(part);
    // A released mesh that keeps its arrays now (made wet, displaced, deforming) uploads: UE may hold it in a
    // form that no longer fits (a static mesh where wet needs a dynamic one).
    const bool unreleased = !revived && previous != missing && previous->second.released && !part.released;
    state.meshes[id] = std::move(part);
    state.fullUploads.erase(id);
    state.removed.erase(id);
    state.repull.erase(state.meshes[id].sourceId);
    state.erasedMeshes.erase(id);
    if (meshChanged || unreleased) {
        if (state.dirtyMeshes.insert(id).second) state.pendingBytes += HdUEMeshBytes(state.meshes[id]);
        ++state.revision;
    } else if (moved || maskChanged) {
        if (moved) state.movedMeshes.insert(id);
        ++state.revision;
    }
    current.insert(id);
}
PXR_NAMESPACE_CLOSE_SCOPE
