// Implicit gprims (cube, sphere, cone, cylinder, capsule, plane) reach hdUE as meshes, as for hdEmbree.
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"
PXR_NAMESPACE_OPEN_SCOPE
class HdUE_ImplicitSurfaceSceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle&) override {
        const HdDataSourceBaseHandle toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(HdsiImplicitSurfaceSceneIndexTokens->toMesh);
        return HdsiImplicitSurfaceSceneIndex::New(input, HdRetainedContainerDataSource::New(
            HdPrimTypeTokens->sphere, toMesh, HdPrimTypeTokens->cube, toMesh, HdPrimTypeTokens->cone, toMesh,
            HdPrimTypeTokens->cylinder, toMesh, HdPrimTypeTokens->capsule, toMesh, HdPrimTypeTokens->plane, toMesh));
    }
};
TF_REGISTRY_FUNCTION(TfType) { HdSceneIndexPluginRegistry::Define<HdUE_ImplicitSurfaceSceneIndexPlugin>(); }
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    // The renderer display name from plugInfo.json.
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer("Unreal Engine (hdUE)",
        TfToken("HdUE_ImplicitSurfaceSceneIndexPlugin"), nullptr, 0, HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}
PXR_NAMESPACE_CLOSE_SCOPE
