#include <stddef.h>
#include "hdue/bridge.h"
#include "hdue/viewport_api.h"
_Static_assert(HDUE_AOV_COUNT == 11 && HDUE_LIGHTING_INDIRECT_VOLUME == 64, "ABI 11 AOV kinds and lighting components");
_Static_assert(sizeof(HdueGpuOptions) == 20, "GPU frame options layout");
_Static_assert(sizeof(HdueGpuFrame) == 136, "GPU frame layout");
_Static_assert(sizeof(HdueLight) == 232, "extended light layout");
_Static_assert(sizeof(HdueLightBatch) == 40, "light batch layout");
_Static_assert(sizeof(HdueLightingStatus) == 56, "lighting status layout");
_Static_assert(sizeof(HdueWetBinding) == 64, "wet binding layout");
_Static_assert(offsetof(HdueWetBinding, mask) == 56, "wet mask pointer alignment");
_Static_assert(sizeof(HdueSurfaceBatch) == 16, "surface snapshot layout");
_Static_assert(sizeof(HdueSurfaceStatus) == 48, "surface status layout");
_Static_assert(sizeof(HdueViewportWet) == 48, "viewport wet layout");
_Static_assert(sizeof(HdueViewportSurfaces) == 40, "viewport surface layout");
_Static_assert(sizeof(HdueViewportMaterial) == 32, "viewport material binding layout");
_Static_assert(sizeof(HdueViewportMaterials) == 32, "viewport materials layout");
_Static_assert(sizeof(HdueViewportTexture) == 16, "viewport texture layout");
_Static_assert(sizeof(HdueViewportTextures) == 32, "viewport textures layout");

_Static_assert(sizeof(HdueSession) == 8, "64-bit session handles");
_Static_assert(HDUE_MESH_SINGLE_SIDED == 4 && HDUE_MESH_EDITABLE == 2, "Mesh flags are bits");
_Static_assert(offsetof(HdueInitDesc, instance_handle) == 8, "C ABI alignment");
_Static_assert(sizeof(HdueInitDesc) == 48, "InitDesc C ABI layout");
_Static_assert(sizeof(HdueStatus) == 552, "Status C ABI layout");
_Static_assert(sizeof(HdueSessionDesc) == 16, "SessionDesc C ABI layout");
_Static_assert(sizeof(HdueRenderRequest) == 208, "RenderRequest C ABI layout");
_Static_assert(HDUE_ABI_VERSION == 16u, "ABI 16: one function table, no extension tables");
_Static_assert(sizeof(HdueFrameInfo) == 72, "FrameInfo C ABI layout");
_Static_assert(sizeof(HdueSceneBatch) == 56, "Scene batch C ABI layout");
_Static_assert(sizeof(HdueTextureDesc) == 72, "Texture C ABI layout (ABI 9)");
_Static_assert(sizeof(HdueMeshDesc) == 248, "Mesh C ABI layout (ABI 10)");
_Static_assert(sizeof(HdueMaterialDesc) == 992, "Material C ABI layout (ABI 16)");
_Static_assert(sizeof(HdueNativeRay) == 72, "Native ray C ABI layout");
_Static_assert(sizeof(HdueNativeHit) == 72, "Native hit C ABI layout");
_Static_assert(sizeof(HduePickMaterialInfo) == 8, "Material info C ABI layout");
_Static_assert(sizeof(HdueRendererInfo) == 20, "Renderer info C ABI layout");
_Static_assert(sizeof(HdueMaterialParameter) == 40, "Material parameter C ABI layout");
_Static_assert(sizeof(HdueMaterialUpdate) == 24, "Material update C ABI layout");
_Static_assert(sizeof(HdueMaterialStats) == 32, "Material stats C ABI layout");
_Static_assert(sizeof(HdueResidencyOptions) == 16, "Residency options C ABI layout");
_Static_assert(sizeof(HdueResidencyStatus) == 584, "Residency status C ABI layout");
_Static_assert(sizeof(HdueAssetDesc) == 152, "Asset descriptor C ABI layout");
_Static_assert(sizeof(HdueAssetStatus) == 584, "Asset status C ABI layout");
_Static_assert(sizeof(HdueAssetSceneStatus) == 544, "Asset scene status C ABI layout");
_Static_assert(sizeof(HdueViewportStatus) == 648, "Houdini viewport status C ABI layout");
_Static_assert(sizeof(HdueMeshUpdate) == 32, "Mesh update C ABI layout (ABI 13)");
_Static_assert(sizeof(HdueLocalFogVolumeDesc) == 168, "Local fog volume C ABI layout (ABI 14)");
_Static_assert(sizeof(HdueGroomDesc) == 48, "Groom C ABI layout (ABI 16)");
_Static_assert(sizeof(HdueApi) == 8 + 42 * 8, "Function table: header and 42 functions");
int main(void) { return 0; }
