#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

class UUserWidget;
class UTextureRenderTarget2D;
class FWidgetRenderer;
class SWidget;
class FReferenceCollector;

/** Design-time rendering only; no player, input forwarding or game-world attachment. */
class FWidgetPreviewContent
{
public:
    ~FWidgetPreviewContent();
    bool Open(UClass* WidgetClass, FString& Error);
    bool Render(TArray<uint8>& OutPng, FString& Error);
    void AddReferencedObjects(FReferenceCollector& Collector);
    UUserWidget* GetInstance() const { return Instance; }
private:
    TObjectPtr<UUserWidget> Instance = nullptr;
    TObjectPtr<UTextureRenderTarget2D> Target = nullptr;
    FWidgetRenderer* Renderer = nullptr;
    TSharedPtr<SWidget> SlateWidget;
};
