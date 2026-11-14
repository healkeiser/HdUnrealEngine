// Characterization tests of the delegate's scene-upload bookkeeping (scene_bookkeeping.h): today's behaviour, so a
// change to it shows here. Plans 015 and 016 change this code and update these expectations with it.
//
// Not covered here, because the code lives outside scene_bookkeeping.h (prims.cpp): an error return in Mesh::Sync
// erases the id from repull, and HdUEStoreShape adds a dirtied shape's bytes to pendingBytes (plan 015).
#include "scene_bookkeeping.h"
#include <iostream>
#include <stdexcept>

PXR_NAMESPACE_OPEN_SCOPE
// delegate.cpp's definitions, which a test cannot link (it starts UE): the real destructor only destroys the state's
// UE session, which a test state never has, and HdUEReleasable passes whether hdUE's UE thread runs (husk, Houdini).
HdUEState::~HdUEState() {}
bool HdUEReleasable(const HdUEState& state, const HdUEMeshData& mesh) { return HdUEReleasableMesh(state, mesh, true); }
PXR_NAMESPACE_CLOSE_SCOPE

PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void check(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}
HdUEMeshData mesh(const std::string& id, size_t triangles, float offset = 0) {
    HdUEMeshData m;
    m.id = m.sourceId = id;
    for (size_t i = 0; i < triangles * 3; ++i) {
        m.points.emplace_back(float(i) + offset, 0.f, 0.f);
        m.indices.push_back(uint32_t(i));
    }
    m.geometryHash = HdUEGeometryHash(m);
    return m;
}
void store(HdUEState& state, HdUEMeshData part) {
    std::set<std::string> current;
    HdUEStoreMeshPart(state, std::move(part), current);
}
}  // namespace

int main() {
    try {
        {
            // 1. A small budget takes at least one dirty mesh and leaves the rest; the other sets go whole.
            HdUEState state;
            for (const char* id : {"/a", "/b", "/c"}) {
                state.meshes[id] = mesh(id, 4);
                state.dirtyMeshes.insert(id);
            }
            state.dirtyLights = {"/light"};
            state.removed = {"/gone"};
            HdUESceneSets packet;
            packet.take(state, 1);
            check(packet.dirtyMeshes.size() == 1 && state.dirtyMeshes.size() == 2, "take ignored its mesh budget");
            check(packet.bytes == HdUEMeshBytes(state.meshes["/a"]), "take counted the wrong bytes");
            check(packet.dirtyLights.count("/light") && state.dirtyLights.empty(), "take left the dirty lights");
            check(packet.removedIds.count("/gone") && state.removed.empty(), "take left the removals");
        }
        {
            // 2. The meshes of a dirty material join the upload as materialMeshes.
            HdUEState state;
            state.meshes["/m"] = mesh("/m", 1);
            state.meshes["/m"].material = "/mat";
            state.meshes["/other"] = mesh("/other", 1);
            state.dirtyMaterials = {"/mat"};
            HdUESceneSets packet;
            packet.take(state, 1 << 20);
            check(packet.dirtyMeshes.count("/m") && packet.materialMeshes.count("/m"),
                  "a dirty material's mesh was not taken");
            check(!packet.dirtyMeshes.count("/other"), "a mesh of another material was taken");
        }
        {
            // 3. giveBack restores only prims that still exist, and removals only of prims still gone; a mesh released
            // when taken comes back in full from Hydra.
            HdUEState state;
            for (const char* id : {"/a", "/b", "/released"}) {
                state.meshes[id] = mesh(id, 1);
                state.dirtyMeshes.insert(id);
            }
            state.removed = {"/gone", "/revived"};
            HdUESceneSets packet;
            packet.take(state, 1 << 20);
            HdUEReleaseGeometry(state.meshes["/released"]);
            state.meshes.erase("/a");
            state.meshes["/revived"] = mesh("/revived", 1);
            packet.giveBack(state, {"/released"});
            check(!state.dirtyMeshes.count("/a") && state.dirtyMeshes.count("/b"), "giveBack restored an erased mesh");
            check(!state.dirtyMeshes.count("/released") && state.fullUploads.count("/released") &&
                      state.repull.count("/released"),
                  "a mesh released when taken was not re-pulled");
            check(state.removed.count("/gone") && !state.removed.count("/revived"), "giveBack removed a revived prim");
        }
        {
            // 4. An unchanged part of a released mesh stays released and is not dirty.
            HdUEState state;
            auto released = mesh("/p", 2);
            HdUEReleaseGeometry(released);
            state.meshes["/p"] = released;
            store(state, mesh("/p", 2));
            check(state.meshes["/p"].released && state.meshes["/p"].points.empty(),
                  "an unchanged mesh kept its arrays");
            check(!state.dirtyMeshes.count("/p") && state.pendingBytes == 0, "an unchanged released mesh was dirtied");
        }
        {
            // 5. A new geometryHash dirties the mesh and adds its stored bytes to pendingBytes.
            HdUEState state;
            state.meshes["/p"] = mesh("/p", 2);
            const auto changed = mesh("/p", 2, 1);
            const uint64_t bytes = HdUEMeshBytes(changed);
            const uint64_t revision = state.revision;
            store(state, changed);
            check(state.dirtyMeshes.count("/p") && state.pendingBytes == bytes, "new geometry was not dirtied");
            check(state.revision == revision + 1, "new geometry did not advance the revision");
        }
        {
            // 6. A transform-only change places the mesh (movedMeshes) instead of uploading it.
            HdUEState state;
            state.meshes["/p"] = mesh("/p", 2);
            auto placed = mesh("/p", 2);
            placed.transform.SetTranslate(GfVec3d(1, 2, 3));
            store(state, placed);
            check(state.movedMeshes.count("/p") && !state.dirtyMeshes.count("/p"), "a moved mesh was re-uploaded");
        }
        {
            // 7. A prim erased and re-created with the same data is neither removed nor re-uploaded.
            HdUEState state;
            state.erasedMeshes["/p"] = mesh("/p", 2);
            state.removed = {"/p"};
            store(state, mesh("/p", 2));
            check(!state.dirtyMeshes.count("/p") && !state.removed.count("/p") && !state.erasedMeshes.count("/p"),
                  "a revived identical mesh was re-uploaded");
            check(state.meshes.count("/p"), "a revived mesh was not stored");
        }
        std::cout << "PASS: scene upload bookkeeping (take, giveBack, stored parts)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
