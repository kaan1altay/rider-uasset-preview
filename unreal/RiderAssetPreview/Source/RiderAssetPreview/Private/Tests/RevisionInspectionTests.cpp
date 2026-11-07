#if WITH_DEV_AUTOMATION_TESTS

#include "AssetPreviewSession.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FRiderRevisionInspectionTest, "RiderAssetPreview.Revisions.IsolatedBlueprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FRiderRevisionInspectionTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    // Do not add a fixture-only test to normal game projects' automation suites.
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) == TEXT("AssetPreviewHost"))
    {
        Names.Add(TEXT("RiderAssetPreview.Revisions.IsolatedBlueprint"));
        Commands.Add(FString());
    }
}

bool FRiderRevisionInspectionTest::RunTest(const FString& Parameters)
{
    // Fixture generation may write assets, so it is deliberately restricted to our
    // disposable build host. This test never runs inside a user's game project.
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("AssetPreviewHost"))
    {
        AddError(TEXT("Revision fixtures can run only in the isolated AssetPreviewHost project."));
        return false;
    }
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = TEXT("BP_Review_") + Suffix;
    const FString PackageName = TEXT("/Game/RiderReviewFixtures/") + AssetName;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    UPackage* Package = CreatePackage(*PackageName);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, *AssetName,
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Fixture Blueprint created"), Blueprint)) return false;
    USCS_Node* MeshNode = Blueprint->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Body"));
    Blueprint->SimpleConstructionScript->AddNode(MeshNode);
    UStaticMeshComponent* Mesh = CastChecked<UStaticMeshComponent>(MeshNode->ComponentTemplate);
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Mesh->SetRelativeScale3D(FVector::OneVector);
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
    UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
    Event->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
    Event->bOverrideFunction = true;
    Event->CreateNewGuid();
    Graph->AddNode(Event, false, false);
    Event->AllocateDefaultPins();
    UK2Node_CallFunction* Print = NewObject<UK2Node_CallFunction>(Graph);
    Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
    Print->CreateNewGuid();
    Graph->AddNode(Print, false, false);
    Print->AllocateDefaultPins();
    UEdGraphPin* Message = Print->FindPin(TEXT("InString"));
    if (!TestNotNull(TEXT("Logic input pin created"), Message)) return false;
    Message->DefaultValue = TEXT("Before review");
    Graph->GetSchema()->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), Print->FindPin(UEdGraphSchema_K2::PN_Execute));
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("Saved before fixture"), UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs))) return false;
    const FString Evidence = FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/TestEvidence");
    const FString BeforeDir = FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/Diff/") / Suffix / TEXT("Before");
    const FString AfterDir = FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/Diff/") / Suffix / TEXT("After");
    IFileManager::Get().MakeDirectory(*BeforeDir, true);
    IFileManager::Get().MakeDirectory(*AfterDir, true);
    IFileManager::Get().MakeDirectory(*Evidence, true);
    const FString BeforeFile = BeforeDir / AssetName + TEXT(".uasset");
    const FString AfterFile = AfterDir / AssetName + TEXT(".uasset");
    if (!TestTrue(TEXT("Copied before revision"), IFileManager::Get().Copy(*BeforeFile, *Filename) == COPY_OK)) return false;

    Mesh->SetRelativeScale3D(FVector(2, 1, 1));
    USCS_Node* Marker = Blueprint->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("NewMarker"));
    MeshNode->AddChildNode(Marker);
    Message->DefaultValue = TEXT("After review");
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (!TestTrue(TEXT("Saved after fixture"), UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs))) return false;
    if (!TestTrue(TEXT("Copied after revision"), IFileManager::Get().Copy(*AfterFile, *Filename) == COPY_OK)) return false;

    FAssetPreviewSession Before, After;
    FString Error;
    if (!TestTrue(TEXT("Historical revision loads without replacing current asset"), Before.Open(Filename, Error, BeforeFile))) { AddError(Error); return false; }
    if (!TestTrue(TEXT("After revision loads independently"), After.Open(Filename, Error, AfterFile))) { AddError(Error); return false; }
    const TSharedRef<FJsonObject> Old = Before.Inspect(), New = After.Inspect();
    TestEqual(TEXT("Before has one component"), Old->GetArrayField(TEXT("components")).Num(), 1);
    TestEqual(TEXT("After has the added component"), New->GetArrayField(TEXT("components")).Num(), 2);
    const FString StableKey = TEXT("scs:") + MeshNode->VariableGuid.ToString(EGuidFormats::Digits);
    auto FindScale = [&StableKey](const TSharedRef<FJsonObject>& Inspection)
    {
        for (const auto& Value : Inspection->GetArrayField(TEXT("components")))
        {
            const auto Component = Value->AsObject();
            if (Component->GetStringField(TEXT("key")) != StableKey) continue;
            for (const auto& Property : Inspection->GetObjectField(TEXT("details"))->GetObjectField(Component->GetStringField(TEXT("id")))->GetArrayField(TEXT("properties")))
                if (Property->AsObject()->GetStringField(TEXT("property")) == TEXT("RelativeScale3D")) return Property->AsObject()->GetStringField(TEXT("diffValue"));
        }
        return FString();
    };
    TestTrue(TEXT("Old scale is preserved"), FindScale(Old).Contains(TEXT("X=1.000000")));
    TestTrue(TEXT("New scale is independent"), FindScale(New).Contains(TEXT("X=2.000000")));
    TestTrue(TEXT("Live asset scale remains unchanged"), Mesh->GetRelativeScale3D().Equals(FVector(2, 1, 1)));
    auto LogicContains = [](const TSharedRef<FJsonObject>& Inspection, const FString& Text)
    {
        for (const auto& Row : Inspection->GetArrayField(TEXT("logic")))
            if (Row->AsObject()->GetStringField(TEXT("value")).Contains(Text)) return true;
        return false;
    };
    TestTrue(TEXT("Old graph input preserved"), LogicContains(Old, TEXT("Before review")));
    TestTrue(TEXT("New graph input preserved"), LogicContains(New, TEXT("After review")));
    const FBox CommonBounds(FVector(-100, -50, -50), FVector(100, 50, 50));
    TArray<uint8> OldPng, NewPng;
    TestTrue(TEXT("Before renders with common camera"), Before.Render(40, 20, 1, TEXT("asset"), OldPng, Error, &CommonBounds));
    TestTrue(TEXT("After renders with common camera"), After.Render(40, 20, 1, TEXT("asset"), NewPng, Error, &CommonBounds));
    TestTrue(TEXT("Scale change is visible"), !OldPng.IsEmpty() && !NewPng.IsEmpty() && OldPng != NewPng);
    FAssetPreviewSession Invalid;
    TestFalse(TEXT("Snapshot outside the allowed directory is rejected"), Invalid.Open(Filename, Error, Filename));
    const FString ProbeDir = FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/Diff/") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*ProbeDir, true);
    const FString ProbeFile = ProbeDir / AssetName + TEXT(".uasset");
    IFileManager::Get().Copy(*ProbeFile, *BeforeFile);
    TWeakObjectPtr<UPackage> ReleasedPackage;
    {
        TUniquePtr<FAssetPreviewSession> Probe = MakeUnique<FAssetPreviewSession>();
        if (TestTrue(TEXT("Cleanup probe revision loads"), Probe->Open(Filename, Error, ProbeFile))) ReleasedPackage = Probe->GetRevisionPackageForTesting();
    }
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Closed revision package is reclaimable by GC"), ReleasedPackage.IsValid());
    auto WriteJson = [&Evidence](const FString& Name, const TSharedRef<FJsonObject>& Object)
    {
        FString Text;
        FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
        FFileHelper::SaveStringToFile(Text, *(Evidence / Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    };
    WriteJson(TEXT("before.json"), Old); WriteJson(TEXT("after.json"), New);
    TSharedRef<FJsonObject> Paths = MakeShared<FJsonObject>();
    Paths->SetStringField(TEXT("original"), FPaths::ConvertRelativePathToFull(Filename));
    Paths->SetStringField(TEXT("before"), FPaths::ConvertRelativePathToFull(BeforeFile));
    Paths->SetStringField(TEXT("after"), FPaths::ConvertRelativePathToFull(AfterFile));
    WriteJson(TEXT("fixture-paths.json"), Paths);
    FFileHelper::SaveArrayToFile(OldPng, *(Evidence / TEXT("before.png")));
    FFileHelper::SaveArrayToFile(NewPng, *(Evidence / TEXT("after.png")));
    return true;
}
#endif
