#include "AssetPreviewSession.h"

#include "Components/ActorComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "DetailsExport.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr int32 Width = 960;
    constexpr int32 Height = 640;

    FString ComponentId(const UActorComponent* Component)
    {
        return Component ? Component->GetPathName() : TEXT("asset");
    }

}

FAssetPreviewSession::FAssetPreviewSession()
{
    Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Touch();
}

FAssetPreviewSession::~FAssetPreviewSession()
{
    // Preview world owns all the actor/components. Release render state before destroying it.
    if (Capture) Capture->UnregisterComponent();
    if (Host && Scene) Scene->GetWorld()->DestroyActor(Host);
    Scene.Reset();
}

void FAssetPreviewSession::Touch() { LastAccess = FPlatformTime::Seconds(); }

void FAssetPreviewSession::AddReferencedObjects(FReferenceCollector& Collector)
{
    Collector.AddReferencedObject(Asset);
    Collector.AddReferencedObject(DetailsObject);
    Collector.AddReferencedObject(Host);
    Collector.AddReferencedObject(Capture);
    Collector.AddReferencedObject(RenderTarget);
    for (FAssetPreviewComponent& Component : Components)
    {
        Collector.AddReferencedObject(Component.Source);
        Collector.AddReferencedObject(Component.Preview);
    }
}

bool FAssetPreviewSession::Open(const FString& Filename, FString& Error)
{
    check(IsInGameThread());
    FString LongPackage;
    if (Filename.Len() > 4096 || !FPaths::GetExtension(Filename).Equals(TEXT("uasset"), ESearchCase::IgnoreCase)
        || !FPaths::FileExists(Filename)
        || !FPackageName::TryConvertFilenameToLongPackageName(Filename, LongPackage)
        || !FPackageName::IsValidLongPackageName(LongPackage))
    {
        Error = TEXT("Select an existing .uasset inside this project's registered Content mounts.");
        return false;
    }
    UPackage* Package = LoadPackage(nullptr, *LongPackage, LOAD_None);
    if (!Package)
    {
        Error = TEXT("Unreal could not load the asset package. Check the Editor Output Log.");
        return false;
    }
    Asset = FindObject<UObject>(Package, *FPackageName::GetLongPackageAssetName(LongPackage));
    DetailsObject = Asset;
    if (!Asset)
    {
        Error = TEXT("No primary asset found in the package.");
        return false;
    }

    if (UTexture2D* Texture = Cast<UTexture2D>(Asset))
    {
        FImage Image;
        TArray64<uint8> Png;
        if (!FImageUtils::GetTexture2DSourceImage(Texture, Image))
        {
            Error = TEXT("This Texture2D has no readable editor source image.");
            return false;
        }
        // Bound transfer/memory cost; this is an inspection preview, not an export operation.
        if (Image.SizeX > 2048 || Image.SizeY > 2048)
        {
            const float Scale = 2048.f / FMath::Max(Image.SizeX, Image.SizeY);
            FImage Resized;
            Image.ResizeTo(Resized, FMath::Max(1, FMath::RoundToInt(Image.SizeX * Scale)),
                FMath::Max(1, FMath::RoundToInt(Image.SizeY * Scale)), ERawImageFormat::BGRA8, EGammaSpace::sRGB);
            Image = MoveTemp(Resized);
        }
        if (!FImageUtils::CompressImage(Png, TEXT(".png"), Image) || Png.Num() > 16 * 1024 * 1024)
        {
            Error = TEXT("Could not encode the texture preview within the 16 MB limit.");
            return false;
        }
        TexturePng.Append(Png.GetData(), static_cast<int32>(Png.Num()));
        Notice = TEXT("Texture2D source preview · up to 2048 px · properties are read-only");
        return true;
    }

    if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        if (!OpenBlueprint(Blueprint, Error)) return false;
    }
    else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
    {
        UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage());
        Component->SetFlags(RF_Transient);
        Component->SetStaticMesh(StaticMesh);
        Components.Add({TEXT("mesh"), TEXT("asset"), StaticMesh->GetName(), Component, nullptr});
        Notice = TEXT("Static Mesh · Unreal renderer · properties are read-only");
    }
    else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
    {
        USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(GetTransientPackage());
        Component->SetFlags(RF_Transient);
        Component->SetSkeletalMesh(SkeletalMesh);
        Components.Add({TEXT("mesh"), TEXT("asset"), SkeletalMesh->GetName(), Component, nullptr});
        Notice = TEXT("Skeletal Mesh reference pose · no animation playback · properties are read-only");
    }
    else
    {
        Error = FString::Printf(TEXT("%s is not supported yet. Use an Actor Blueprint, Static Mesh, Skeletal Mesh or Texture2D."), *Asset->GetClass()->GetName());
        return false;
    }

    Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
        .SetCreatePhysicsScene(false).ShouldSimulatePhysics(false).SetTransactional(false)
        .AllowAudioPlayback(false).SetForceMipsResident(false));
    // Match the engine's neutral asset-viewer lighting without changing shared preview profiles.
    if (UTextureCube* Environment = LoadObject<UTextureCube>(nullptr,
        TEXT("/Engine/EditorMaterials/AssetViewer/T_GreyAmbient.T_GreyAmbient")))
    {
        Scene->SetSkyCubemap(Environment);
        Scene->SetSkyBrightness(2.f);
    }
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags = RF_Transient;
    Spawn.bTemporaryEditorActor = true;
    Host = Scene->GetWorld()->SpawnActor<AActor>(Spawn);
    if (!Host)
    {
        Error = TEXT("Could not create the isolated preview actor.");
        return false;
    }
    BuildPreviewComponents();
    RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
    RenderTarget->SetFlags(RF_Transient);
    RenderTarget->ClearColor = FLinearColor(0.035f, 0.04f, 0.05f);
    RenderTarget->InitCustomFormat(Width, Height, PF_B8G8R8A8, false);
    RenderTarget->UpdateResourceImmediate(true);
    Capture = NewObject<USceneCaptureComponent2D>(Host);
    Capture->SetFlags(RF_Transient);
    Capture->TextureTarget = RenderTarget;
    Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->FOVAngle = 50.f;
    Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
    Capture->PostProcessSettings.AutoExposureBias = 0.f;
    Host->AddInstanceComponent(Capture);
    Capture->RegisterComponentWithWorld(Scene->GetWorld());
    Scene->UpdateCaptureContents();
    return true;
}

bool FAssetPreviewSession::OpenBlueprint(UBlueprint* Blueprint, FString& Error)
{
    UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
    if (!Generated || !Generated->IsChildOf(AActor::StaticClass()))
    {
        Error = TEXT("The preview currently supports compiled Actor Blueprints. Compile it in Unreal Editor first.");
        return false;
    }
    AActor* Defaults = Cast<AActor>(Generated->GetDefaultObject());
    if (!Defaults)
    {
        Error = TEXT("The Blueprint has no valid actor defaults.");
        return false;
    }
    DetailsObject = Defaults;
    TInlineComponentArray<UActorComponent*> Native;
    Defaults->GetComponents(Native);
    for (UActorComponent* Component : Native)
    {
        if (!Component) continue;
        const USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
        const FString Parent = SceneComponent ? ComponentId(SceneComponent->GetAttachParent()) : TEXT("asset");
        Components.Add({ComponentId(Component), Parent, Component->GetName(), Component, nullptr,
            SceneComponent ? SceneComponent->GetAttachSocketName() : NAME_None});
    }

    TArray<UBlueprintGeneratedClass*> Classes;
    for (UClass* Class = Generated; Class; Class = Class->GetSuperClass())
        if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Class)) Classes.Insert(BPClass, 0);

    TMap<USCS_Node*, FString> NodeIds;
    TMap<FName, FString> Names;
    TMap<USCS_Node*, FString> ChildParents;
    for (const FAssetPreviewComponent& Component : Components) Names.Add(Component.Source->GetFName(), Component.Id);
    for (UBlueprintGeneratedClass* Class : Classes)
    {
        if (!Class->SimpleConstructionScript) continue;
        for (USCS_Node* Node : Class->SimpleConstructionScript->GetAllNodes())
        {
            if (!Node) continue;
            UActorComponent* Template = Node->GetActualComponentTemplate(Generated);
            if (!Template) continue;
            const FString Key = ComponentId(Template);
            NodeIds.Add(Node, Key);
            Names.Add(Node->GetVariableName(), Key);
            Names.Add(Template->GetFName(), Key);
        }
    }
    for (const TPair<USCS_Node*, FString>& Pair : NodeIds)
        for (USCS_Node* Child : Pair.Key->GetChildNodes()) ChildParents.Add(Child, Pair.Value);

    // Walk base-to-derived order, while resolving overrides using the actual generated class.
    TSet<FString> Added;
    for (const FAssetPreviewComponent& Component : Components) Added.Add(Component.Id);
    for (UBlueprintGeneratedClass* Class : Classes)
    {
        if (!Class->SimpleConstructionScript) continue;
        for (USCS_Node* Node : Class->SimpleConstructionScript->GetAllNodes())
        {
            const FString* Key = NodeIds.Find(Node);
            if (!Key || Added.Contains(*Key)) continue;
            FString Parent = TEXT("asset");
            if (const FString* LocalParent = ChildParents.Find(Node)) Parent = *LocalParent;
            else if (const FString* ExternalParent = Names.Find(Node->ParentComponentOrVariableName)) Parent = *ExternalParent;
            Components.Add({*Key, Parent, Node->GetVariableName().ToString(), Node->GetActualComponentTemplate(Generated), nullptr, Node->AttachToName});
            Added.Add(*Key);
        }
    }
    Notice = TEXT("Blueprint component defaults · Construction Script, BeginPlay, child actors and simulation are not executed");
    return true;
}

void FAssetPreviewSession::BuildPreviewComponents()
{
    TMap<FString, USceneComponent*> SceneComponents;
    for (FAssetPreviewComponent& Component : Components)
    {
        // ChildActor registration creates another game actor. Inspect its template, but do not spawn it.
        if (!Component.Source || Component.Source->IsA<UChildActorComponent>()) continue;
        Component.Preview = DuplicateObject<UActorComponent>(Component.Source, Host, MakeUniqueObjectName(Host, Component.Source->GetClass()));
        Component.Preview->ClearFlags(RF_ArchetypeObject | RF_DefaultSubObject | RF_Public | RF_Standalone | RF_Transactional);
        Component.Preview->SetFlags(RF_Transient);
        Component.Preview->CreationMethod = EComponentCreationMethod::Instance;
        Component.Preview->SetComponentTickEnabled(false);
        Host->AddInstanceComponent(Component.Preview);
        if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component.Preview))
        {
            // Remove template attachment pointers before registering the duplicate in the preview world.
            const FTransform Relative = CastChecked<USceneComponent>(Component.Source)->GetRelativeTransform();
            SceneComponent->SetupAttachment(nullptr);
            SceneComponent->SetRelativeTransform(Relative);
            SceneComponents.Add(Component.Id, SceneComponent);
        }
    }
    for (FAssetPreviewComponent& Component : Components)
    {
        if (USceneComponent* Child = Cast<USceneComponent>(Component.Preview))
        {
            if (USceneComponent** Parent = SceneComponents.Find(Component.Parent))
                if (*Parent != Child && !(*Parent)->IsAttachedTo(Child)) Child->SetupAttachment(*Parent, Component.Socket);
        }
    }
    for (FAssetPreviewComponent& Component : Components)
    {
        // Non-scene components are inspectable, but do not need gameplay registration.
        if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component.Preview))
        {
            if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(SceneComponent))
            {
                Primitive->SetSimulatePhysics(false);
                Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            }
            SceneComponent->RegisterComponentWithWorld(Scene->GetWorld());
            SceneComponent->UpdateComponentToWorld();
        }
    }
}

TSharedRef<FJsonObject> FAssetPreviewSession::Describe() const
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("session"), Id);
    Result->SetStringField(TEXT("name"), Asset->GetName());
    Result->SetStringField(TEXT("type"), Asset->GetClass()->GetName());
    Result->SetStringField(TEXT("notice"), Notice);
    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const FAssetPreviewComponent& Component : Components)
    {
        TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), Component.Id);
        Node->SetStringField(TEXT("parent"), Component.Parent);
        Node->SetStringField(TEXT("name"), Component.Name);
        Node->SetStringField(TEXT("type"), Component.Source->GetClass()->GetName());
        Nodes.Add(MakeShared<FJsonValueObject>(Node));
    }
    Result->SetArrayField(TEXT("components"), Nodes);
    return Result;
}

bool FAssetPreviewSession::HasComponent(const FString& ComponentId) const
{
    return ComponentId == TEXT("asset") || Components.ContainsByPredicate(
        [&ComponentId](const FAssetPreviewComponent& Component) { return Component.Id == ComponentId; });
}

TSharedRef<FJsonObject> FAssetPreviewSession::Details(const FString& ComponentId, bool bShowAdvanced) const
{
    UObject* Object = DetailsObject;
    for (const FAssetPreviewComponent& Component : Components)
        if (Component.Id == ComponentId) { Object = Component.Source; break; }
    return ExportBlueprintDetails(Object, bShowAdvanced);
}

FBox FAssetPreviewSession::Bounds(const FString& ComponentId) const
{
    FBox Box(ForceInit);
    for (const FAssetPreviewComponent& Component : Components)
    {
        if (ComponentId != TEXT("asset") && Component.Id != ComponentId) continue;
        if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component.Preview))
            if (Primitive->IsRegistered()) Box += Primitive->Bounds.GetBox();
    }
    if (!Box.IsValid) Box = FBox(FVector(-50), FVector(50));
    return Box;
}

bool FAssetPreviewSession::Render(double Yaw, double Pitch, double Zoom, const FString& Focus,
    TArray<uint8>& OutPng, FString& Error)
{
    check(IsInGameThread());
    if (!TexturePng.IsEmpty()) { OutPng = TexturePng; return true; }
    if (!Capture || !RenderTarget || !FApp::CanEverRender())
    {
        Error = TEXT("Unreal rendering is unavailable. Run the Editor with a graphics device, without -nullrhi.");
        return false;
    }
    const FBox Box = Bounds(Focus);
    const double Radius = FMath::Max(25.0, Box.GetExtent().Size());
    const FRotator Orbit(-FMath::Clamp(Pitch, -85.0, 85.0), FMath::Fmod(Yaw, 360.0), 0);
    const FVector Location = Box.GetCenter() - Orbit.Vector() * Radius * 3.2 * FMath::Clamp(Zoom, 0.08, 20.0);
    Capture->SetWorldLocationAndRotation(Location, (Box.GetCenter() - Location).Rotation());
    Capture->CaptureScene();
    TArray<FColor> Pixels;
    FReadSurfaceDataFlags Flags(RCM_UNorm);
    Flags.SetLinearToGamma(false);
    if (!RenderTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels, Flags) || Pixels.Num() != Width * Height)
    {
        Error = TEXT("Could not read the Unreal preview render target.");
        return false;
    }
    // LDR render-target alpha can be zero. The viewport is intentionally opaque.
    for (FColor& Pixel : Pixels) Pixel.A = 255;
    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(Width, Height, MakeArrayView(Pixels), Png);
    if (Png.IsEmpty()) { Error = TEXT("Could not encode the preview frame."); return false; }
    OutPng.Append(Png.GetData(), static_cast<int32>(Png.Num()));
    return true;
}
