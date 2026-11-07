#if WITH_DEV_AUTOMATION_TESTS
#include "AssetPreviewSession.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "K2Node_Event.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FRiderSpatialPreviewTest, "RiderAssetPreview.Spatial.IsolatedContent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FRiderSpatialPreviewTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) == TEXT("AssetPreviewHost"))
    { Names.Add(TEXT("RiderAssetPreview.Spatial.IsolatedContent")); Commands.Add(FString()); }
}

bool FRiderSpatialPreviewTest::RunTest(const FString& Parameters)
{
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("AssetPreviewHost"))
    { AddError(TEXT("Spatial fixtures are restricted to the disposable build host.")); return false; }
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Evidence = FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/SpatialEvidence");
    const FString RevisionRoot = FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/Diff/") / Suffix;
    IFileManager::Get().MakeDirectory(*Evidence, true);
    auto RevisionPath = [&RevisionRoot](const FString& Side, const FString& Name, const FString& Extension)
    {
        const FString Directory = RevisionRoot / Side;
        IFileManager::Get().MakeDirectory(*Directory, true);
        return Directory / Name + Extension;
    };
    auto Save = [this](UPackage* Package, UObject* Object, const FString& Filename)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
        return TestTrue(TEXT("Saved real fixture package"), UPackage::SavePackage(Package, Object, *Filename, Args));
    };
    auto Record = [this, &Evidence](FAssetPreviewSession& Session, const FString& Name)
    {
        FString Json, Error;
        const auto Writer = TJsonWriterFactory<>::Create(&Json);
        FJsonSerializer::Serialize(Session.Inspect(), Writer);
        FFileHelper::SaveStringToFile(Json, *(Evidence / Name + TEXT(".json")));
        TArray<uint8> Png;
        if (!TestTrue(TEXT("Rendered fixture"), Session.Render(40, 20, 1, TEXT("asset"), Png, Error))) { AddError(Error); return false; }
        TestTrue(TEXT("PNG has rendered content"), Png.Num() > 4000);
        FFileHelper::SaveArrayToFile(Png, *(Evidence / Name + TEXT(".png")));
        return true;
    };

    const FString MapName = TEXT("Map_Review_") + Suffix;
    const FString MapPackageName = TEXT("/Game/RiderSpatialFixtures/") + MapName;
    UPackage* MapPackage = CreatePackage(*MapPackageName);
    MapPackage->SetPackageFlags(PKG_ContainsMap);
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false, *MapName, MapPackage, false);
    if (!TestNotNull(TEXT("Created disposable map"), World)) return false;
    World->SetFlags(RF_Public | RF_Standalone);
    AStaticMeshActor* Body = World->SpawnActor<AStaticMeshActor>();
    Body->SetActorLabel(TEXT("Review platform"));
    Body->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Body->SetActorLocation(FVector(100, 0, 0));
    UStaticMeshComponent* Absolute = NewObject<UStaticMeshComponent>(Body, TEXT("AbsoluteMesh"));
    Body->AddInstanceComponent(Absolute);
    Absolute->SetStaticMesh(Body->GetStaticMeshComponent()->GetStaticMesh());
    Absolute->SetupAttachment(Body->GetRootComponent());
    Absolute->SetAbsolute(true, false, false);
    Absolute->SetRelativeLocation(FVector(20, -200, 0));
    Absolute->RegisterComponent();
    const FString MapFile = FPackageName::LongPackageNameToFilename(MapPackageName, TEXT(".umap"));
    if (!Save(MapPackage, World, MapFile)) return false;
    const FString MapBefore = RevisionPath(TEXT("map-before"), MapName, TEXT(".umap"));
    TestEqual(TEXT("Copied map before"), IFileManager::Get().Copy(*MapBefore, *MapFile), COPY_OK);
    Body->SetActorLocation(FVector(500, 0, 0));
    AStaticMeshActor* Added = World->SpawnActor<AStaticMeshActor>();
    Added->SetActorLabel(TEXT("Added obstacle"));
    Added->GetStaticMeshComponent()->SetStaticMesh(Body->GetStaticMeshComponent()->GetStaticMesh());
    Added->SetActorLocation(FVector(0, 250, 0));
    if (!Save(MapPackage, World, MapFile)) return false;
    const FString MapAfter = RevisionPath(TEXT("map-after"), MapName, TEXT(".umap"));
    TestEqual(TEXT("Copied map after"), IFileManager::Get().Copy(*MapAfter, *MapFile), COPY_OK);
    TWeakObjectPtr<UPackage> ReleasedMap;
    {
        FString Error;
        FAssetPreviewSession Before, After, Ordinary;
        if (!TestTrue(TEXT("Map before loads"), Before.Open(MapFile, Error, MapBefore))) { AddError(Error); return false; }
        if (!TestTrue(TEXT("Map after loads"), After.Open(MapFile, Error, MapAfter))) { AddError(Error); return false; }
        if (!TestTrue(TEXT("Ordinary map uses isolated saved copy"), Ordinary.Open(MapFile, Error))) { AddError(Error); return false; }
        TestNotEqual(TEXT("Normal map did not reuse the live package"), Ordinary.GetRevisionPackageForTesting(), MapPackage);
        ReleasedMap = Ordinary.GetRevisionPackageForTesting();
        bool bFoundAbsolute = false;
        const auto AfterDescription = After.Describe();
        for (const auto& Value : AfterDescription->GetArrayField(TEXT("components")))
        {
            const auto Node = Value->AsObject();
            if (Node->GetStringField(TEXT("name")) != TEXT("AbsoluteMesh")) continue;
            const auto& Bounds = Node->GetArrayField(TEXT("bounds"));
            TestTrue(TEXT("Absolute child ignores parent translation"), FMath::IsNearlyEqual((Bounds[0]->AsNumber() + Bounds[3]->AsNumber()) * 0.5, 20., 0.1));
            bFoundAbsolute = true;
        }
        TestTrue(TEXT("Absolute child present in hierarchy"), bFoundAbsolute);
        TestEqual(TEXT("Active editor level unchanged"), GEditor->GetEditorWorldContext().World(), EditorWorld);
        TestTrue(TEXT("Actor and component were added"), After.Describe()->GetArrayField(TEXT("components")).Num() > Before.Describe()->GetArrayField(TEXT("components")).Num());
        if (!Record(Before, TEXT("map-before")) || !Record(After, TEXT("map-after"))) return false;
    }
    World->DestroyWorld(false);

    const FString WidgetName = TEXT("WBP_Review_") + Suffix;
    const FString WidgetPackageName = TEXT("/Game/RiderSpatialFixtures/") + WidgetName;
    UPackage* WidgetPackage = CreatePackage(*WidgetPackageName);
    UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), WidgetPackage,
        *WidgetName, BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
    if (!Blueprint->WidgetTree) Blueprint->WidgetTree = NewObject<UWidgetTree>(Blueprint, TEXT("WidgetTree"), RF_Transactional);
    UCanvasPanel* Canvas = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas"));
    Blueprint->WidgetTree->RootWidget = Canvas;
    Blueprint->WidgetVariableNameToGuidMap.Add(TEXT("Canvas"), FGuid::NewGuid());
    UTextBlock* Text = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Heading"));
    Text->SetText(FText::FromString(TEXT("Before preview")));
    FSlateFontInfo Font = Text->GetFont(); Font.Size = 48; Text->SetFont(Font);
    UCanvasPanelSlot* Slot = Canvas->AddChildToCanvas(Text);
    Slot->SetPosition(FVector2D(80, 100)); Slot->SetSize(FVector2D(900, 150));
    Blueprint->WidgetVariableNameToGuidMap.Add(TEXT("Heading"), FGuid::NewGuid());
    FEdGraphPinType BoolType; BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("PreviewExecuted"), BoolType);
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
    UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
    Event->EventReference.SetExternalMember(TEXT("PreConstruct"), UUserWidget::StaticClass());
    Event->bOverrideFunction = true; Graph->AddNode(Event); Event->CreateNewGuid(); Event->AllocateDefaultPins();
    UK2Node_VariableSet* Setter = NewObject<UK2Node_VariableSet>(Graph);
    Setter->VariableReference.SetSelfMember(TEXT("PreviewExecuted"));
    Graph->AddNode(Setter); Setter->CreateNewGuid(); Setter->AllocateDefaultPins();
    Setter->FindPinChecked(TEXT("PreviewExecuted"))->DefaultValue = TEXT("true");
    Graph->GetSchema()->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), Setter->FindPin(UEdGraphSchema_K2::PN_Execute));
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    const FString WidgetFile = FPackageName::LongPackageNameToFilename(WidgetPackageName, TEXT(".uasset"));
    if (!Save(WidgetPackage, Blueprint, WidgetFile)) return false;
    const FString WidgetBefore = RevisionPath(TEXT("widget-before"), WidgetName, TEXT(".uasset"));
    TestEqual(TEXT("Copied widget before"), IFileManager::Get().Copy(*WidgetBefore, *WidgetFile), COPY_OK);
    Text->SetText(FText::FromString(TEXT("After preview")));
    Slot->SetPosition(FVector2D(180, 100));
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (!Save(WidgetPackage, Blueprint, WidgetFile)) return false;
    const FString WidgetAfter = RevisionPath(TEXT("widget-after"), WidgetName, TEXT(".uasset"));
    TestEqual(TEXT("Copied widget after"), IFileManager::Get().Copy(*WidgetAfter, *WidgetFile), COPY_OK);
    TWeakObjectPtr<UPackage> ReleasedWidget;
    {
        FString Error;
        FAssetPreviewSession Before, After, Ordinary;
        if (!TestTrue(TEXT("Widget before loads"), Before.Open(WidgetFile, Error, WidgetBefore))) { AddError(Error); return false; }
        if (!TestTrue(TEXT("Widget after loads"), After.Open(WidgetFile, Error, WidgetAfter))) { AddError(Error); return false; }
        if (!TestTrue(TEXT("Ordinary widget loads saved design"), Ordinary.Open(WidgetFile, Error))) { AddError(Error); return false; }
        TestNotEqual(TEXT("Ordinary widget does not reuse the live package"), Ordinary.GetRevisionPackageForTesting(), WidgetPackage);
        ReleasedWidget = Ordinary.GetRevisionPackageForTesting();
        if (!Record(Before, TEXT("widget-before")) || !Record(After, TEXT("widget-after"))) return false;
        TestEqual(TEXT("Widget viewports are 2D"), Before.Describe()->GetStringField(TEXT("previewKind")), FString(TEXT("widget")));
        TestTrue(TEXT("Widget is in design mode"), Before.GetWidgetForTesting()->IsDesignTime());
        FBoolProperty* Flag = FindFProperty<FBoolProperty>(Before.GetWidgetForTesting()->GetClass(), TEXT("PreviewExecuted"));
        if (TestNotNull(TEXT("PreConstruct test flag exists"), Flag))
            TestFalse(TEXT("Blueprint PreConstruct did not run"), Flag->GetPropertyValue_InContainer(Before.GetWidgetForTesting()));
        TestEqual(TEXT("Current text template unchanged"), Text->GetText().ToString(), FString(TEXT("After preview")));
    }
    TestEqual(TEXT("Editor level still unchanged"), GEditor->GetEditorWorldContext().World(), EditorWorld);
    FString Paths;
    const auto Writer = TJsonWriterFactory<>::Create(&Paths);
    TSharedRef<FJsonObject> Locations = MakeShared<FJsonObject>();
    Locations->SetStringField(TEXT("map"), FPaths::ConvertRelativePathToFull(MapFile));
    Locations->SetStringField(TEXT("mapBefore"), FPaths::ConvertRelativePathToFull(MapBefore));
    Locations->SetStringField(TEXT("mapAfter"), FPaths::ConvertRelativePathToFull(MapAfter));
    Locations->SetStringField(TEXT("widget"), FPaths::ConvertRelativePathToFull(WidgetFile));
    Locations->SetStringField(TEXT("widgetBefore"), FPaths::ConvertRelativePathToFull(WidgetBefore));
    Locations->SetStringField(TEXT("widgetAfter"), FPaths::ConvertRelativePathToFull(WidgetAfter));
    FJsonSerializer::Serialize(Locations, Writer);
    FFileHelper::SaveStringToFile(Paths, *(Evidence / TEXT("fixture-paths.json")));
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Closed map revision is reclaimed"), ReleasedMap.IsValid());
    TestFalse(TEXT("Closed widget revision is reclaimed"), ReleasedWidget.IsValid());
    return true;
}
#endif
