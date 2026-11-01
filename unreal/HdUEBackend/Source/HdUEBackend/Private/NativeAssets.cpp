#include "NativeAssets.h"
#include "Message.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/HeterogeneousVolumeComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"

FHdueNativeAssets::~FHdueNativeAssets() { Reset(); }
void FHdueNativeAssets::Actors(const FEntry& E, TArray<AActor*>& Out) {
    if (E.Actor.IsValid()) {
        Out.Add(E.Actor.Get());
        TArray<AActor*> Children;
        E.Actor->GetAllChildActors(Children, true);
        Out.Append(Children);
    }
    if (E.Level.IsValid() && E.Level->GetLoadedLevel())
        for (const auto& A : E.Level->GetLoadedLevel()->Actors)
            if (IsValid(A.Get())) Out.Add(A.Get());
}
TArray<AActor*> FHdueNativeAssets::GetActors() const {
    TArray<AActor*> Result;
    for (const auto& Pair : Entries)
        if (Pair.Value->Visible && Pair.Value->State == HDUE_ASSET_READY) Actors(*Pair.Value, Result);
    return Result;
}
void FHdueNativeAssets::Remove(const FString& Id) {
    auto* Found = Entries.Find(Id);
    if (!Found) return;
    auto& E = **Found;
    if (E.Load) E.Load->CancelHandle();
    if (E.Actor.IsValid()) World->DestroyActor(E.Actor.Get());
    if (E.Level.IsValid()) {
        E.Level->SetShouldBeVisible(false);
        E.Level->SetShouldBeLoaded(false);
        E.Level->SetIsRequestingUnloadAndRemoval(true);
        Retiring.Emplace(E.Level.Get());
    }
    Entries.Remove(Id);
}
void FHdueNativeAssets::Reset() {
    TArray<FString> Keys;
    Entries.GetKeys(Keys);
    for (const auto& K : Keys) Remove(K);
    // Drain removals before session destruction/recreation; UE itself stays alive.
    if (World) World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
    Retiring.Empty();
}
HdueResult FHdueNativeAssets::Upsert(const HdueAssetDesc& D, FString& Error) {
    if (!D.id || !*D.id || !D.asset_path || !*D.asset_path) return HDUE_INVALID_ARGUMENT;
    FMatrix Matrix;
    for (int R = 0; R < 4; ++R)
        for (int C = 0; C < 4; ++C) {
            if (!FMath::IsFinite(D.transform[R * 4 + C])) return HDUE_INVALID_ARGUMENT;
            Matrix.M[R][C] = D.transform[R * 4 + C];
        }
    const FString Id = UTF8_TO_TCHAR(D.id), Path = UTF8_TO_TCHAR(D.asset_path);
    const FSoftObjectPath ObjectPath(Path);
    if (!ObjectPath.IsValid() || !Path.StartsWith(TEXT("/")) || Path.Contains(TEXT("'"))) {
        Error = TEXT("Use a cooked UE object path such as /Game/HdUE/SM_Nanite.SM_Nanite.");
        return HDUE_INVALID_ARGUMENT;
    }
    if (!FMath::IsFinite(D.frame)) return HDUE_INVALID_ARGUMENT;
    const FTransform Transform(Matrix);
    if (auto* Existing = Entries.Find(Id)) {
        auto& E = **Existing;
        if (E.Path == Path && (E.Kind != HDUE_ASSET_LEVEL || E.Transform.Equals(Transform))) {
            const bool VisibilityChanged = E.Visible != (D.visible != 0);
            E.Transform = Transform;
            E.Visible = D.visible != 0;
            if (E.Actor.IsValid()) {
                E.Actor->SetActorTransform(Transform);
                E.Actor->SetActorHiddenInGame(!E.Visible);
            }
            // Another frame of an animated SVT: the component streams it on its next tick (blocking requests), so the
            // asset reads as loading until it settles, as a Level made visible does.
            if (E.Frame != D.frame) {
                E.Frame = D.frame;
                if (auto* Volume =
                        E.Actor.IsValid() ? E.Actor->FindComponentByClass<UHeterogeneousVolumeComponent>() : nullptr) {
                    Volume->SetFrame(E.Frame);
                    if (E.State == HDUE_ASSET_READY) {
                        E.State = HDUE_ASSET_LOADING;
                        E.Started = FPlatformTime::Seconds();
                        E.ReadyFrame = 0;
                    }
                }
            }
            if (E.Level.IsValid() && VisibilityChanged) {
                E.Level->SetShouldBeVisible(E.Visible);
                if (E.State != HDUE_ASSET_FAILED) {
                    E.State = HDUE_ASSET_LOADING;
                    E.Started = FPlatformTime::Seconds();
                    E.ReadyFrame = 0;
                }
            }
            return HDUE_OK;
        }
        Remove(Id);
    }
    auto E = MakeUnique<FEntry>();
    E->Path = Path;
    E->Transform = Transform;
    E->Visible = D.visible != 0;
    E->Frame = D.frame;
    E->Started = FPlatformTime::Seconds();
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const FAssetData Data = Registry.GetAssetByObjectPath(ObjectPath);
    const FString Class = Data.AssetClassPath.GetAssetName().ToString();
    if (Class == TEXT("World"))
        E->Kind = HDUE_ASSET_LEVEL;
    else if (Class == TEXT("StaticMesh"))
        E->Kind = HDUE_ASSET_STATIC_MESH;
    else if (Class == TEXT("SkeletalMesh"))
        E->Kind = HDUE_ASSET_SKELETAL_MESH;
    else if (Class == TEXT("Blueprint") || Class == TEXT("BlueprintGeneratedClass"))
        E->Kind = HDUE_ASSET_ACTOR;
    else if (Class == TEXT("StaticSparseVolumeTexture") || Class == TEXT("AnimatedSparseVolumeTexture"))
        E->Kind = HDUE_ASSET_SPARSE_VOLUME;
    else {
        E->State = HDUE_ASSET_FAILED;
        E->Kind = Data.IsValid() ? HDUE_ASSET_RESOURCE : HDUE_ASSET_UNKNOWN;
        E->Error =
            Data.IsValid()
                ? FString::Printf(TEXT("%s is a resource (%s), not a placeable Actor/mesh/Level."), *Path, *Class)
                : FString::Printf(TEXT("Asset is absent from this runtime's cooked registry: %s"), *Path);
    }
    if (E->Kind == HDUE_ASSET_LEVEL) {
        bool Success = false;
        ULevelStreamingDynamic::FLoadLevelInstanceParams P(World, ObjectPath.GetLongPackageName(), Transform);
        P.bInitiallyVisible = E->Visible;
        E->Level.Reset(ULevelStreamingDynamic::LoadLevelInstance(P, Success));
        if (!Success || !E->Level.IsValid()) {
            E->State = HDUE_ASSET_FAILED;
            E->Error = TEXT("Level instance loading failed; include the map in the cook.");
        }
    } else if (E->State != HDUE_ASSET_FAILED) {
        FString LoadPath = Path;
        if (E->Kind == HDUE_ASSET_ACTOR && Class == TEXT("Blueprint")) LoadPath += TEXT("_C");
        E->Load = Streamable.RequestAsyncLoad(FSoftObjectPath(LoadPath));
        if (!E->Load) {
            E->State = HDUE_ASSET_FAILED;
            E->Error = TEXT("Cannot request the cooked asset.");
        }
    }
    Entries.Add(Id, MoveTemp(E));
    return HDUE_OK;
}
void FHdueNativeAssets::Tick() {
    Retiring.RemoveAll([](const auto& L) { return !L.IsValid() || (!L->IsLevelVisible() && !L->IsLevelLoaded()); });
    for (auto& Pair : Entries) {
        auto& E = *Pair.Value;
        if (E.State != HDUE_ASSET_LOADING) continue;
        if (FPlatformTime::Seconds() - E.Started > 120) {
            E.State = HDUE_ASSET_FAILED;
            E.Error = TEXT("Native asset preparation timed out; check cooked dependencies and streaming errors.");
            continue;
        }
        const auto Settle = [&E]() {
            // Allow world initialization/sky capture/component ticks to run
            // after streaming visibility or BP construction, before PT freezes time.
            if (!E.ReadyFrame) E.ReadyFrame = GFrameCounter + 2;
            if (GFrameCounter >= E.ReadyFrame) E.State = HDUE_ASSET_READY;
        };
        if (E.Actor.IsValid()) {
            Settle();
            continue;
        }
        if (E.Level.IsValid()) {
            if (E.Level->IsLevelLoaded() && (E.Visible ? E.Level->IsLevelVisible() : !E.Level->IsLevelVisible()))
                Settle();
            continue;
        }
        if (!E.Load || !E.Load->HasLoadCompleted()) continue;
        UObject* Asset = E.Load->GetLoadedAsset();
        E.Asset.Reset(Asset);
        AActor* A = nullptr;
        if (E.Kind == HDUE_ASSET_ACTOR) {
            auto* Class = Cast<UClass>(Asset);
            if (Class && Class->IsChildOf(AActor::StaticClass()) &&
                !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) {
                FActorSpawnParameters P;
                P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                A = World->SpawnActor<AActor>(Class, E.Transform, P);
            }
        } else if ((E.Kind == HDUE_ASSET_STATIC_MESH && Cast<UStaticMesh>(Asset)) ||
                   (E.Kind == HDUE_ASSET_SKELETAL_MESH && Cast<USkeletalMesh>(Asset))) {
            A = World->SpawnActor<AActor>();
            UMeshComponent* C = nullptr;
            if (auto* Mesh = Cast<UStaticMesh>(Asset)) {
                auto* SM = NewObject<UStaticMeshComponent>(A);
                SM->SetStaticMesh(Mesh);
                C = SM;
            } else {
                auto* SK = NewObject<USkeletalMeshComponent>(A);
                SK->SetSkeletalMeshAsset(Cast<USkeletalMesh>(Asset));
                C = SK;
            }
            C->SetMobility(EComponentMobility::Movable);
            A->SetRootComponent(C);
            A->AddInstanceComponent(C);
            C->RegisterComponent();
            A->SetActorTransform(E.Transform);
        } else if (auto* Texture = Cast<USparseVolumeTexture>(Asset); E.Kind == HDUE_ASSET_SPARSE_VOLUME && Texture) {
            // A Heterogeneous Volume with UE's default SVT material, as the editor's SVT material instance factory
            // makes it (USparseVolumeTextureMaterialInstanceFactoryNew: SparseVolumeMaterial's SparseVolumeTexture
            // parameter); the prepared MI_HdUESparseVolume is that instance. The SVT keeps its VDB's transform, whose
            // units UE reads as centimetres.
            auto* Parent = LoadObject<UMaterialInterface>(
                nullptr, TEXT("/Game/HdUE/MI_HdUESparseVolume.MI_HdUESparseVolume"), nullptr, LOAD_NoWarn | LOAD_Quiet);
            if (Parent) {
                A = World->SpawnActor<AActor>();
                auto* C = NewObject<UHeterogeneousVolumeComponent>(A);
                auto* Material = UMaterialInstanceDynamic::Create(Parent, C);
                Material->SetSparseVolumeTextureParameterValue(TEXT("SparseVolumeTexture"), Texture);
                C->SetMaterial(0, Material);
                C->SetVolumeResolution(Texture->GetVolumeResolution());
                // The frame comes from primvars:unreal:volume:frame, as Sequencer keys UE's Frame; no playback.
                C->SetEndFrame(FMath::Max(Texture->GetNumFrames() - 1, 0));
                C->SetFrame(E.Frame);
                // Renders wait for the streamed frame, as an offline render must (the component's own option).
                C->bIssueBlockingRequests = true;
                C->SetMobility(EComponentMobility::Movable);
                A->SetRootComponent(C);
                A->AddInstanceComponent(C);
                C->RegisterComponent();
                A->SetActorTransform(E.Transform);
            }
        }
        if (!A) {
            E.State = HDUE_ASSET_FAILED;
            E.Error = E.Kind == HDUE_ASSET_SPARSE_VOLUME
                          ? TEXT("This runtime has no /Game/HdUE/MI_HdUESparseVolume; prepare and cook its content.")
                          : TEXT("Asset could not be instantiated (Blueprint must derive from a concrete Actor).");
        } else {
            // A placed BP may author static components. Hydra edits its placement
            // after construction, so those owned components must be movable.
            if (E.Kind == HDUE_ASSET_ACTOR) {
                TArray<USceneComponent*> Components;
                A->GetComponents(Components);
                for (auto* C : Components) C->SetMobility(EComponentMobility::Movable);
            }
            E.Actor = A;
            A->SetActorHiddenInGame(!E.Visible);
            E.ReadyFrame = GFrameCounter + 2;
        }
        E.Load.Reset();
    }
}
void FHdueNativeAssets::Stats(const FEntry& E, HdueAssetStatus& S) {
    S = {};
    S.struct_size = sizeof(S);
    S.kind = E.Kind;
    S.state = E.State;
    HdueCopyMessage(S.message, E.Error);
    FBox Box(ForceInit);
    TArray<AActor*> List;
    Actors(E, List);
    S.actors = List.Num();
    for (auto* A : List) {
        TArray<UMeshComponent*> Meshes;
        A->GetComponents(Meshes);
        S.meshes += Meshes.Num();
        if (E.Visible && !A->IsHidden()) {
            for (auto* C : Meshes)  // a Heterogeneous Volume is a mesh component too
                if (C->IsVisible()) Box += C->Bounds.GetBox();
            TArray<ULightComponentBase*> Lights;
            A->GetComponents(Lights);
            for (auto* C : Lights)
                if (C->IsVisible() && C->Intensity > 0) ++S.lights;
        }
    }
    if (Box.IsValid)
        for (int I = 0; I < 3; ++I) {
            S.bounds_min[I] = Box.Min[I];
            S.bounds_max[I] = Box.Max[I];
        }
}
HdueResult FHdueNativeAssets::GetStatus(const FString& Id, HdueAssetStatus& S) const {
    const auto* E = Entries.Find(Id);
    if (!E) return HDUE_INVALID_ARGUMENT;
    Stats(**E, S);
    return HDUE_OK;
}
void FHdueNativeAssets::GetSceneStatus(HdueAssetSceneStatus& S) const {
    S = {};
    S.struct_size = sizeof(S);
    S.instances = Entries.Num();
    S.loading = Retiring.Num();
    for (const auto& Pair : Entries) {
        HdueAssetStatus E;
        Stats(*Pair.Value, E);
        S.loading += E.state == HDUE_ASSET_LOADING;
        S.failed += E.state == HDUE_ASSET_FAILED;
        S.actors += E.actors;
        S.meshes += E.meshes;
        S.lights += E.lights;
        if (E.state == HDUE_ASSET_FAILED && !S.message[0])
            HdueCopyMessage(S.message, Pair.Key + TEXT(": ") + Pair.Value->Error);
    }
}
