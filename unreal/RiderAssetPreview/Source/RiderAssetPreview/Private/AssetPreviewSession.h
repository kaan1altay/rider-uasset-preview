#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/GCObject.h"

class FPreviewScene;
class AActor;
class UActorComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UPackage;

struct FAssetPreviewComponent
{
    FString Id;
    FString Parent;
    FString Name;
    TObjectPtr<UActorComponent> Source = nullptr;
    TObjectPtr<UActorComponent> Preview = nullptr;
    FName Socket = NAME_None;
    FString StableKey;
};

/** One isolated world per Rider tab. Never uses the user's level or runs Blueprint UCS. */
class FAssetPreviewSession final : public FGCObject
{
public:
    FAssetPreviewSession();
    virtual ~FAssetPreviewSession() override;
    bool Open(const FString& Filename, FString& Error, const FString& Snapshot = FString());
    TSharedRef<FJsonObject> Describe() const;
    TSharedRef<FJsonObject> Inspect() const;
    TSharedRef<FJsonObject> Details(const FString& ComponentId, bool bShowAdvanced = false) const;
    bool Render(double Yaw, double Pitch, double Zoom, const FString& Focus, TArray<uint8>& OutPng, FString& Error, const FBox* SharedBounds = nullptr);
    bool HasComponent(const FString& Id) const;
    void Touch();
#if WITH_DEV_AUTOMATION_TESTS
    UPackage* GetRevisionPackageForTesting() const { return RevisionPackage; }
#endif
    double LastAccess = 0;
    FString Id;

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("RiderAssetPreviewSession"); }

private:
    bool OpenBlueprint(class UBlueprint* Blueprint, FString& Error);
    void BuildPreviewComponents();
    FBox Bounds(const FString& ComponentId) const;
    TObjectPtr<UObject> Asset = nullptr;
    TObjectPtr<UObject> DetailsObject = nullptr;
    TObjectPtr<AActor> Host = nullptr;
    TObjectPtr<USceneCaptureComponent2D> Capture = nullptr;
    TObjectPtr<UTextureRenderTarget2D> RenderTarget = nullptr;
    TUniquePtr<FPreviewScene> Scene;
    TArray<FAssetPreviewComponent> Components;
    TArray<uint8> TexturePng;
    FString Notice;
    FString OriginalPackage;
    TObjectPtr<UPackage> RevisionPackage = nullptr;
};
