#pragma once
#include "CoreMinimal.h"
#include "Engine/StreamableManager.h"
#include "UObject/StrongObjectPtr.h"
#include "hdue/bridge.h"
class UWorld;
class ULevelStreamingDynamic;
class AActor;
class FHdueNativeAssets {
public:
    explicit FHdueNativeAssets(UWorld* InWorld) : World(InWorld) {}
    ~FHdueNativeAssets();
    HdueResult Upsert(const HdueAssetDesc& Desc, FString& Error);
    void Remove(const FString& Id);
    void Reset();
    void Tick();
    // The request's scene time: Niagara Systems seek to it (time - their start), as Sequencer's desired age, and read
    // as loading until they get there.
    void SetTime(double Seconds);
    void GetSceneStatus(HdueAssetSceneStatus& Status) const;
    // What a render request waits for, without GetSceneStatus's per-asset bounds: whether an asset still loads, and
    // the first failure (empty when none).
    bool Loading(FString& Failure) const;
    HdueResult GetStatus(const FString& Id, HdueAssetStatus& Status) const;
    TArray<AActor*> GetActors() const;

private:
    struct FEntry {
        FString Path, Error;
        uint32 Kind = HDUE_ASSET_UNKNOWN, State = HDUE_ASSET_LOADING;
        FTransform Transform;
        bool Visible = true;
        float Frame = 0;            // a Sparse Volume Texture's frame
        double Start = 0;           // a Niagara System's start, in scene seconds
        float SecondsPerFrame = 0;  // and its simulation step
        double Started = 0;
        uint64 ReadyFrame = 0;
        TSharedPtr<FStreamableHandle> Load;
        TStrongObjectPtr<UObject> Asset;
        TStrongObjectPtr<ULevelStreamingDynamic> Level;
        TWeakObjectPtr<AActor> Actor;
    };
    UWorld* World;
    FStreamableManager Streamable;
    TMap<FString, TUniquePtr<FEntry>> Entries;
    TArray<TStrongObjectPtr<ULevelStreamingDynamic>> Retiring;
    double Time = 0;
    void Seek(FEntry& Entry);
    static void Actors(const FEntry& Entry, TArray<AActor*>& Out);
    static void Stats(const FEntry& Entry, HdueAssetStatus& Status);
};
