#pragma once
#include "CoreMinimal.h"
#include "hdue/bridge.h"
#include "ImageCore.h"
#include "RenderingThread.h"
#include "RenderCommandFence.h"
#include "RHIResources.h"
#include "UObject/StrongObjectPtr.h"
class UWorld;
class AActor;
class USkyLightComponent;
class UTextureCube;
class UTexture2D;
class UTextureLightProfile;
class UPrimitiveComponent;
class FProperty;

// Sets one property of Container from text, as set_post_process reads its values (Bridge.cpp).
bool ImportPropertyValue(FProperty* Property, const FString& Text, void* Container);

class FHdueLighting {
public:
    explicit FHdueLighting(UWorld* InWorld);
    ~FHdueLighting();
    bool Apply(const HdueLightBatch& Batch, FString& Error);
    void Remove(const FString& Id);
    void Clear();
    void Tick();
    void UpdateCamera(const FVector& Eye, const FVector& Target, const FVector& Up);
    bool Ready() const;
    bool BackgroundVisible() const;
    bool HasDome() const { return DomeCount > 0; }
    FTextureRHIRef EnvironmentTexture() const;
    float EnvironmentScale() const { return RadianceScale; }
    void Status(HdueLightingStatus& Status) const;
    const FString& GetError() const { return LastError; }
    // Applies an object's light and shadow links (HdueLightBatch object table) to its mesh component.
    void Link(const FString& Id, UPrimitiveComponent* Mesh) const;

private:
    struct FDefinition {
        HdueLight Data{};
        FString Id, File, Ies, LightFunction;
        TArray<TPair<FString, FString>> Properties;  // unreal:light:<Property>, by name
    };
    struct FImageEntry {
        FDateTime Modified;
        int64 Size = 0;
        TSharedPtr<FImage> Image;
    };
    UWorld* World;
    TMap<FString, FDefinition> Definitions;
    TMap<FString, AActor*> Actors;
    TMap<FString, FImageEntry> Images;
    TMap<FString, TStrongObjectPtr<UTexture2D>> LightTextures;
    TMap<FString, TStrongObjectPtr<class UMaterialInstanceDynamic>> LightFunctions;  // by light ID
    FString LoadImageFile(const FString& File);     // into Images; the error, empty on success
    UTexture2D* ImageTexture(const FString& File);  // a loaded image as a texture, kept in LightTextures
    struct FProfileEntry {
        FDateTime Modified;
        int64 Size = 0;
        TStrongObjectPtr<UTextureLightProfile> Texture;
        float Mean = 0;  // of the max-normalized profile over the sphere
    };
    TMap<FString, FProfileEntry> Profiles;  // IES files by path (and angle scale)
    const FProfileEntry* Profile(const FString& File, float AngleScale, FString& Error);
    TMap<FString, uint32> Objects;  // HDUE_LINK_* by mesh ID; absent: every channel, casts shadows
    AActor* SkyActor = nullptr;
    USkyLightComponent* Sky = nullptr;
    TStrongObjectPtr<UTextureCube> Environment;
    TStrongObjectPtr<UTexture2D> DiskTexture;
    FRenderCommandFence CaptureFence;
    bool EnvironmentDirty = false;
    bool ShowBackground = false;
    uint32 DomeCount = 0, Resolution = 0;
    float RadianceScale = 1, MaximumRadiance = 0;
    FString LastError;
    bool RebuildEnvironment(FString& Error);
    bool UpdateLocal(const FDefinition& Definition, FString& Error);
};
