// Lumen mesh cards for the static meshes hdUE builds at runtime. UE builds cards only in MeshUtilities, which links the
// editor; its card builder (Embree surfel clusters) is compiled here instead. The editor headers MeshUtilitiesPrivate.h
// includes declare nothing the builder uses.
#include "CoreMinimal.h"
#include "StaticMeshResources.h"
#include "Windows/WindowsHWrapper.h"
#if !WITH_EDITOR
#pragma include_alias("IAnimationBlueprintEditor.h", "CoreMinimal.h")
#pragma include_alias("IAnimationBlueprintEditorModule.h", "CoreMinimal.h")
#pragma include_alias("IAnimationEditor.h", "CoreMinimal.h")
#pragma include_alias("IAnimationEditorModule.h", "CoreMinimal.h")
#pragma include_alias("ISkeletalMeshEditor.h", "CoreMinimal.h")
#pragma include_alias("ISkeletalMeshEditorModule.h", "CoreMinimal.h")
#pragma include_alias("ISkeletonEditor.h", "CoreMinimal.h")
#pragma include_alias("ISkeletonEditorModule.h", "CoreMinimal.h")
#pragma include_alias("SkeletalMeshTools.h", "CoreMinimal.h")
class FExtender; class FUICommandList; struct FSoftSkinVertex; struct FSkinnedMeshChunk;
#include "MeshRepresentationCommon.cpp"
#include "MeshCardRepresentationUtilities.cpp"
DEFINE_LOG_CATEGORY(LogMeshUtilities);
#endif

// Any thread, before the LOD's resources are initialized (the builder reads its CPU copies). Null without Embree.
FCardRepresentationData* HdueMeshCards(const FStaticMeshLODResources& LOD, const FBoxSphereBounds& Bounds) {
#if !WITH_EDITOR && USE_EMBREE // USE_EMBREE is only defined where the game target links Embree
    if (!GetModuleHandleW(TEXT("embree4.dll"))) return nullptr; // Bridge's Initialize loads it from this DLL's folder
    FEmbreeScene Embree;
    MeshRepresentation::SetupEmbreeScene(TEXT("hdUE"), false, Embree);
    if (!Embree.Scene) return nullptr;
    const FSignedDistanceFieldBuildSectionData Section; // opaque, one-sided
    FMeshDataForDerivedDataTask Mesh{nullptr, &LOD, MakeArrayView(&Section, 1), {}, {}, FBoxSphereBounds3f(Bounds)};
    FCardRepresentationData* Cards = nullptr;
    if (MeshRepresentation::AddMeshDataToEmbreeScene(Embree, Mesh, true)) {
        Embree.Commit();
        Cards = new FCardRepresentationData;
        BuildMeshCards(Bounds.GetBox(), FGenerateCardMeshContext(Embree.MeshName, Embree, *Cards), 12, *Cards); // UE's default MaxLumenMeshCards
    }
    MeshRepresentation::DeleteEmbreeScene(Embree);
    return Cards;
#else
    return nullptr;
#endif
}
