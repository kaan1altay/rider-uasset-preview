#include "WidgetPreviewContent.h"

#include "Blueprint/UserWidget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"

namespace { constexpr int32 WidgetWidth = 1280; constexpr int32 WidgetHeight = 720; }

FWidgetPreviewContent::~FWidgetPreviewContent()
{
    SlateWidget.Reset();
    if (Instance) Instance->ReleaseSlateResources(true);
    if (Renderer) BeginCleanup(Renderer);
    Renderer = nullptr;
    Instance = nullptr;
    Target = nullptr;
}

bool FWidgetPreviewContent::Open(UClass* WidgetClass, FString& Error)
{
    if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass()) || WidgetClass->HasAnyClassFlags(CLASS_Abstract))
    {
        Error = TEXT("Compile this Widget Blueprint in Unreal before previewing it.");
        return false;
    }
    Instance = NewObject<UUserWidget>(GetTransientPackage(), WidgetClass, NAME_None, RF_Transient);
    // Designing intentionally omits ExecutePreConstruct. Initialize and TakeWidget
    // then skip Blueprint OnInitialized, PreConstruct and Construct game events.
    Instance->SetDesignerFlags(EWidgetDesignFlags::Designing);
    if (!Instance->Initialize()) { Error = TEXT("Could not initialize the widget design."); return false; }
    SlateWidget = Instance->TakeWidget();
    Renderer = new FWidgetRenderer(true, true);
    Target = FWidgetRenderer::CreateTargetFor(FVector2D(WidgetWidth, WidgetHeight), TF_Bilinear, true);
    if (!Target) { Error = TEXT("Could not create the widget preview render target."); return false; }
    Target->SetFlags(RF_Transient);
    Target->ClearColor = FLinearColor(0.025f, 0.03f, 0.04f, 1.f);
    return true;
}

bool FWidgetPreviewContent::Render(TArray<uint8>& OutPng, FString& Error)
{
    if (!FApp::CanEverRender() || !Renderer || !Target || !SlateWidget)
    {
        Error = TEXT("Widget rendering requires Unreal Editor with a graphics device.");
        return false;
    }
    Renderer->DrawWidget(Target, SlateWidget.ToSharedRef(), FVector2D(WidgetWidth, WidgetHeight), 0.f);
    FlushRenderingCommands();
    TArray<FColor> Pixels;
    FReadSurfaceDataFlags Flags(RCM_UNorm);
    Flags.SetLinearToGamma(false);
    if (!Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels, Flags) || Pixels.Num() != WidgetWidth * WidgetHeight)
    {
        Error = TEXT("Could not read the widget preview frame.");
        return false;
    }
    for (FColor& Pixel : Pixels) Pixel.A = 255;
    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(WidgetWidth, WidgetHeight, MakeArrayView(Pixels), Png);
    if (Png.IsEmpty()) { Error = TEXT("Could not encode the widget preview."); return false; }
    OutPng.Append(Png.GetData(), static_cast<int32>(Png.Num()));
    return true;
}

void FWidgetPreviewContent::AddReferencedObjects(FReferenceCollector& Collector)
{
    Collector.AddReferencedObject(Instance);
    Collector.AddReferencedObject(Target);
}
