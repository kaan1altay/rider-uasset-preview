#include "AssetPreviewSession.h"
#include "WidgetPreviewContent.h"
#include "WidgetBlueprint.h"
#include "Engine/World.h"

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
#include "DiffUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PreviewScene.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "Misc/PackagePath.h"
#include "UObject/Linker.h"
#include "UObject/UObjectHash.h"
#include "RenderingThread.h"
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
    Widget.Reset();
    // Preview world owns all the actor/components. Release render state before destroying it.
    if (Capture) Capture->UnregisterComponent();
    if (Host && Scene) Scene->GetWorld()->DestroyActor(Host);
    Scene.Reset();
    Components.Reset(); Asset = nullptr; DetailsObject = nullptr; Host = nullptr; Capture = nullptr; RenderTarget = nullptr;
    // Never use PackageTools::UnloadPackages here: it clears the user's selection
    // and can reset undo history. Only our /Temp revision exports lose standalone
    // retention; the editor's normal garbage collection can then reclaim them.
    UPackage* OwnedPackage = RevisionPackage;
    RevisionPackage = nullptr;
    if (OwnedPackage && OwnedPackage->HasAnyPackageFlags(PKG_ForDiffing) && FPackageName::IsTempPackage(OwnedPackage->GetName()))
    {
        // Revision loading can initialize world subsystems even without gameplay.
        // Balance that initialization before GC releases the isolated map exports.
        if (UWorld* OwnedWorld = UWorld::FindWorldInPackage(OwnedPackage))
            OwnedWorld->DestroyWorld(false);
        FlushRenderingCommands();
        ResetLoaders(OwnedPackage);
        TArray<UObject*> Objects;
        GetObjectsWithPackage(OwnedPackage, Objects, EGetObjectsFlags::IncludeNestedObjects);
        for (UObject* Object : Objects)
        {
            if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
            {
                Blueprint->ClearEditorReferences();
                for (const auto& Dependency : Blueprint->CachedDependencies)
                    if (UBlueprint* Other = Dependency.Get()) Other->CachedDependents.Remove(Blueprint);
                for (const auto& Dependent : Blueprint->CachedDependents)
                    if (UBlueprint* Other = Dependent.Get()) Other->CachedDependencies.Remove(Blueprint);
                Blueprint->CachedDependencies.Reset(); Blueprint->CachedDependents.Reset();
            }
            Object->ClearFlags(RF_Standalone);
            Object->SetFlags(RF_Transient);
        }
        OwnedPackage->ClearFlags(RF_Standalone);
        OwnedPackage->SetFlags(RF_Transient);
    }
    if (!LocalSnapshotDirectory.IsEmpty())
        IFileManager::Get().DeleteDirectory(*LocalSnapshotDirectory, false, true);
}

void FAssetPreviewSession::Touch() { LastAccess = FPlatformTime::Seconds(); }

#if WITH_DEV_AUTOMATION_TESTS
UUserWidget* FAssetPreviewSession::GetWidgetForTesting() const { return Widget ? Widget->GetInstance() : nullptr; }
#endif

void FAssetPreviewSession::AddReferencedObjects(FReferenceCollector& Collector)
{
    Collector.AddReferencedObject(Asset);
    Collector.AddReferencedObject(DetailsObject);
    Collector.AddReferencedObject(Host);
    Collector.AddReferencedObject(Capture);
    Collector.AddReferencedObject(RenderTarget);
    Collector.AddReferencedObject(RevisionPackage);
    if (Widget) Widget->AddReferencedObjects(Collector);
    for (FAssetPreviewComponent& Component : Components)
    {
        Collector.AddReferencedObject(Component.Source);
        Collector.AddReferencedObject(Component.Preview);
    }
}

bool FAssetPreviewSession::Open(const FString& Filename, FString& Error, const FString& Snapshot)
{
    check(IsInGameThread());
    FString LongPackage;
    const FString Extension = FPaths::GetExtension(Filename).ToLower();
    if (Filename.Len() > 4096 || (Extension != TEXT("uasset") && Extension != TEXT("umap"))
        || (Snapshot.IsEmpty() && !FPaths::FileExists(Filename))
        || !FPackageName::TryConvertFilenameToLongPackageName(Filename, LongPackage)
        || !FPackageName::IsValidLongPackageName(LongPackage))
    {
        Error = TEXT("Select an existing .uasset or .umap inside this project's registered Content mounts.");
        return false;
    }
    OriginalPackage = LongPackage;
    FString RevisionFile = Snapshot;
    if (Extension == TEXT("umap") && RevisionFile.IsEmpty())
    {
        // Stage saved bytes rather than exposing the user's live editor UWorld.
        if (IFileManager::Get().FileSize(*Filename) <= 0 || IFileManager::Get().FileSize(*Filename) > 128ll * 1024 * 1024)
        { Error = TEXT("Map previews are limited to 128 MB saved packages."); return false; }
        LocalSnapshotDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/Diff/") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
        IFileManager::Get().MakeDirectory(*LocalSnapshotDirectory, true);
        RevisionFile = LocalSnapshotDirectory / FPaths::GetCleanFilename(Filename);
        if (IFileManager::Get().Copy(*RevisionFile, *Filename, false) != COPY_OK)
        { Error = TEXT("Could not stage the saved map for isolated preview."); return false; }
    }
    UPackage* Package = nullptr;
    if (RevisionFile.IsEmpty()) Package = LoadPackage(nullptr, *LongPackage, LOAD_None);
    else
    {
        // Use Unreal's own revision loader: PKG_ForDiffing and an instancing context
        // keep old generated classes separate from the project's live package.
        const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/Diff/"));
        FString Full = FPaths::ConvertRelativePathToFull(RevisionFile);
        FPaths::NormalizeFilename(Full);
        FString NormalizedRoot = Root;
        FPaths::NormalizeDirectoryName(NormalizedRoot);
        NormalizedRoot += TEXT("/");
        if (!Full.StartsWith(NormalizedRoot, ESearchCase::IgnoreCase) || Full.Contains(TEXT("/../"))
            || !FPaths::GetExtension(Full).Equals(Extension, ESearchCase::IgnoreCase)
            || IFileManager::Get().FileSize(*Full) <= 0 || IFileManager::Get().FileSize(*Full) > 128ll * 1024 * 1024)
        {
            Error = TEXT("Revision snapshots must match the original package extension and be below this project's Saved/RiderAssetPreview/Diff folder (128 MB maximum).");
            return false;
        }
        Package = DiffUtils::LoadPackageForDiff(FPackagePath::FromLocalPath(Full), FPackagePath::FromLocalPath(Filename));
        RevisionPackage = Package;
    }
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

    if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset))
    {
        if (Snapshot.IsEmpty())
        {
            if (IFileManager::Get().FileSize(*Filename) <= 0 || IFileManager::Get().FileSize(*Filename) > 128ll * 1024 * 1024)
            { Error = TEXT("Widget previews are limited to 128 MB saved packages."); return false; }
            LocalSnapshotDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RiderAssetPreview/Diff/") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
            IFileManager::Get().MakeDirectory(*LocalSnapshotDirectory, true);
            const FString Copy = LocalSnapshotDirectory / FPaths::GetCleanFilename(Filename);
            if (IFileManager::Get().Copy(*Copy, *Filename, false) != COPY_OK)
            { Error = TEXT("Could not stage the saved widget design."); return false; }
            return Open(Filename, Error, Copy);
        }
        return OpenWidget(WidgetBlueprint, Error);
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

    if (UWorld* World = Cast<UWorld>(Asset))
    {
        if (!OpenMap(World, Error)) return false;
    }
    else if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        if (!OpenBlueprint(Blueprint, Error)) return false;
    }
    else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
    {
        UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(GetTransientPackage());
        Component->SetFlags(RF_Transient);
        Component->SetStaticMesh(StaticMesh);
        Components.Add({TEXT("mesh"), TEXT("asset"), StaticMesh->GetName(), Component, nullptr, NAME_None, TEXT("mesh")});
        Notice = TEXT("Static Mesh · Unreal renderer · properties are read-only");
    }
    else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
    {
        USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(GetTransientPackage());
        Component->SetFlags(RF_Transient);
        Component->SetSkeletalMesh(SkeletalMesh);
        Components.Add({TEXT("mesh"), TEXT("asset"), SkeletalMesh->GetName(), Component, nullptr, NAME_None, TEXT("mesh")});
        Notice = TEXT("Skeletal Mesh reference pose · no animation playback · properties are read-only");
    }
    else
    {
        Notice = TEXT("Settings inspection · this asset has no 3D preview · values are read-only");
    }

    if (Components.IsEmpty()) return true;

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
    if (bMap && !Components.ContainsByPredicate([](const FAssetPreviewComponent& Item) { return Cast<UPrimitiveComponent>(Item.Preview) != nullptr; }))
    {
        Notice += TEXT(" · no supported mesh geometry in this inspected level");
        return true;
    }
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
    if (!Generated)
    {
        Error = TEXT("The preview currently supports compiled Actor Blueprints. Compile it in Unreal Editor first.");
        return false;
    }
    if (!Generated->IsChildOf(AActor::StaticClass()))
    {
        DetailsObject = Generated->GetDefaultObject();
        Notice = TEXT("Blueprint class defaults · no Actor geometry · scripts are not executed");
        return DetailsObject != nullptr;
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
            Components.Add({*Key, Parent, Node->GetVariableName().ToString(), Node->GetActualComponentTemplate(Generated), nullptr, Node->AttachToName,
                Node->VariableGuid.IsValid() ? TEXT("scs:") + Node->VariableGuid.ToString(EGuidFormats::Digits) : TEXT("scs-name:") + Node->GetVariableName().ToString()});
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
        UActorComponent* SourceComponent = Cast<UActorComponent>(Component.Source);
        if (!SourceComponent || SourceComponent->IsA<UChildActorComponent>()) continue;
        if (bMap && !SourceComponent->IsA<UStaticMeshComponent>() && !SourceComponent->IsA<USkeletalMeshComponent>()) continue;
        Component.Preview = DuplicateObject<UActorComponent>(SourceComponent, Host, MakeUniqueObjectName(Host, SourceComponent->GetClass()));
        Component.Preview->ClearFlags(RF_ArchetypeObject | RF_DefaultSubObject | RF_Public | RF_Standalone | RF_Transactional);
        Component.Preview->SetFlags(RF_Transient);
        Component.Preview->CreationMethod = EComponentCreationMethod::Instance;
        Component.Preview->SetComponentTickEnabled(false);
        Host->AddInstanceComponent(Component.Preview);
        if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component.Preview))
        {
            // Remove template attachment pointers before registering the duplicate in the preview world.
            const FTransform Relative = Component.bWorldSpace ? Component.SavedTransform : CastChecked<USceneComponent>(Component.Source)->GetRelativeTransform();
            SceneComponent->SetupAttachment(nullptr);
            SceneComponent->SetRelativeTransform(Relative);
            SceneComponents.Add(Component.Id, SceneComponent);
        }
    }
    for (FAssetPreviewComponent& Component : Components)
    {
        if (USceneComponent* Child = Cast<USceneComponent>(Component.Preview))
        {
            if (!Component.bWorldSpace)
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
    Result->SetBoolField(TEXT("visual"), !TexturePng.IsEmpty() || Capture != nullptr || Widget != nullptr);
    Result->SetStringField(TEXT("previewKind"), Widget ? TEXT("widget") : !TexturePng.IsEmpty() ? TEXT("image") : bMap ? TEXT("map") : TEXT("scene"));
    Result->SetBoolField(TEXT("hierarchyTruncated"), bHierarchyLimited);
    auto EncodeBounds = [](const FBox& Box)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (double Value : {Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z})
            Values.Add(MakeShared<FJsonValueNumber>(Value));
        return Values;
    };
    Result->SetArrayField(TEXT("bounds"), EncodeBounds(Bounds(TEXT("asset"))));
    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const FAssetPreviewComponent& Component : Components)
    {
        TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("id"), Component.Id);
        Node->SetStringField(TEXT("parent"), Component.Parent);
        Node->SetStringField(TEXT("socket"), Component.Socket.ToString());
        Node->SetStringField(TEXT("name"), Component.Name);
        Node->SetStringField(TEXT("type"), Component.Source->GetClass()->GetName());
        Node->SetStringField(TEXT("key"), Component.StableKey.IsEmpty() ? TEXT("native:") + Component.Source->GetName() : Component.StableKey);
        Node->SetArrayField(TEXT("bounds"), EncodeBounds(Bounds(Component.Id)));
        Nodes.Add(MakeShared<FJsonValueObject>(Node));
    }
    Result->SetArrayField(TEXT("components"), Nodes);
    return Result;
}

TSharedRef<FJsonObject> FAssetPreviewSession::Inspect() const
{
    TSharedRef<FJsonObject> Result = Describe();
    const FString PackageName = Asset->GetOutermost()->GetName();
    TSharedRef<FJsonObject> Objects = MakeShared<FJsonObject>();
    int32 RemainingRows = 3000;
    auto InspectObject = [this, &RemainingRows](const FString& ComponentId)
    {
        TSharedRef<FJsonObject> Data = Details(ComponentId, true);
        TArray<TSharedPtr<FJsonValue>> Rows = Data->GetArrayField(TEXT("properties"));
        if (Rows.Num() > RemainingRows) { Rows.SetNum(RemainingRows); Data->SetBoolField(TEXT("truncated"), true); }
        RemainingRows -= Rows.Num();
        for (const auto& Value : Rows)
        {
            const auto Row = Value->AsObject();
            const TPair<FString, int32> Limits[] = {{TEXT("value"), 512}, {TEXT("diffValue"), 1024}, {TEXT("tooltip"), 512}};
            for (const auto& Limit : Limits)
            {
                const FString Full = Row->GetStringField(Limit.Key);
                if (Full.Len() > Limit.Value) { Row->SetStringField(Limit.Key, Full.Left(Limit.Value)); Row->SetBoolField(TEXT("truncated"), true); }
            }
        }
        Data->SetArrayField(TEXT("properties"), Rows);
        return Data;
    };
    Objects->SetObjectField(TEXT("asset"), InspectObject(TEXT("asset")));
    // Derived asset facts belong to the review, rather than the editable Details
    // panel. Counts reveal useful changes without claiming a full geometry diff.
    TArray<TSharedPtr<FJsonValue>> Facts = Objects->GetObjectField(TEXT("asset"))->GetArrayField(TEXT("properties"));
    auto Fact = [&Facts](const FString& Key, const FString& Label, const FString& Value, const FString& Tooltip)
    {
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("key"), TEXT("fact:") + Key);
        Row->SetStringField(TEXT("property"), Key);
        Row->SetStringField(TEXT("category"), TEXT("Asset information"));
        Row->SetStringField(TEXT("name"), Label);
        Row->SetStringField(TEXT("value"), Value);
        Row->SetStringField(TEXT("diffValue"), Value);
        Row->SetStringField(TEXT("tooltip"), Tooltip);
        Facts.Add(MakeShared<FJsonValueObject>(Row));
    };
    if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->GetSuperClass())
            Fact(TEXT("ParentClass"), TEXT("Parent class"), Blueprint->GeneratedClass->GetSuperClass()->GetName(),
                TEXT("The class this Blueprint inherits its starting behavior and settings from. Changing a parent can affect inherited components and defaults."));
    }
    if (const UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
    {
        Fact(TEXT("LODCount"), TEXT("Level-of-detail count"), FString::FromInt(Mesh->GetNumLODs()), TEXT("The number of mesh detail levels available for rendering at different distances."));
        if (Mesh->GetNumLODs() > 0)
        {
            Fact(TEXT("Vertices"), TEXT("Vertices at highest detail"), FString::FromInt(Mesh->GetNumVertices(0)), TEXT("The number of points in the highest-detail render mesh. Equal counts do not guarantee identical geometry."));
            Fact(TEXT("Triangles"), TEXT("Triangles at highest detail"), FString::FromInt(Mesh->GetNumTriangles(0)), TEXT("The number of triangles in the highest-detail render mesh. More triangles generally mean more geometry to render."));
        }
    }
    if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
    {
        Fact(TEXT("LODCount"), TEXT("Level-of-detail count"), FString::FromInt(Mesh->GetLODNum()), TEXT("The number of available mesh detail levels."));
        Fact(TEXT("BoneCount"), TEXT("Skeleton bone count"), FString::FromInt(Mesh->GetRefSkeleton().GetNum()), TEXT("The number of bones in the reference skeleton used to deform and animate this mesh."));
    }
    if (const UTexture2D* Texture = Cast<UTexture2D>(Asset))
        Fact(TEXT("SourceDimensions"), TEXT("Source image dimensions"), FString::Printf(TEXT("%lld × %lld px"), Texture->Source.GetSizeX(), Texture->Source.GetSizeY()),
            TEXT("The source image width and height in pixels. Preview images may be downscaled; pixel differences are shown at a shared preview resolution."));
    Objects->GetObjectField(TEXT("asset"))->SetArrayField(TEXT("properties"), Facts);
    int32 Count = 0;
    for (const FAssetPreviewComponent& Component : Components)
    {
        if (++Count > 128 || RemainingRows <= 0) break;
        Objects->SetObjectField(Component.Id, InspectObject(Component.Id));
    }
    Result->SetObjectField(TEXT("details"), Objects);
    Result->SetBoolField(TEXT("componentsTruncated"), bHierarchyLimited || Components.Num() > 128 || RemainingRows <= 0);
    TArray<TSharedPtr<FJsonValue>> GraphRows;
    static const TSet<FName> LayoutFields = {TEXT("NodePosX"), TEXT("NodePosY"), TEXT("NodeWidth"), TEXT("NodeHeight"), TEXT("NodeGuid"),
        TEXT("Pins"), TEXT("NodeComment"), TEXT("bCommentBubblePinned"), TEXT("bCommentBubbleVisible"), TEXT("bHasCompilerMessage"),
        TEXT("ErrorType"), TEXT("ErrorMsg"), TEXT("NodeUpgradeMessage"), TEXT("CommentColor"), TEXT("bCanResizeNode")};
    if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph) continue;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node || GraphRows.Num() >= 512) continue;
                TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("key"), Graph->GraphGuid.ToString(EGuidFormats::Digits) + TEXT(":") + Node->NodeGuid.ToString(EGuidFormats::Digits));
                Row->SetStringField(TEXT("graph"), Graph->GetName());
                Row->SetStringField(TEXT("name"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                Row->SetStringField(TEXT("type"), Node->GetClass()->GetName());
                Row->SetStringField(TEXT("tooltip"), Node->GetTooltipText().ToString().Left(2048));
                TArray<FString> PinData;
                TArray<FString> PinDisplay;
                // A function/member reference can change without changing the
                // displayed title or pin defaults. Compare serialized node settings
                // too, excluding layout and compiler diagnostics.
                for (TFieldIterator<FProperty> Property(Node->GetClass()); Property; ++Property)
                {
                    if (LayoutFields.Contains(Property->GetFName()) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient)) continue;
                    FString Value;
                    Property->ExportText_InContainer(0, Value, Node, nullptr, const_cast<UEdGraphNode*>(Node), PPF_None);
                    PinData.Add(TEXT("setting:") + Property->GetName() + TEXT("=") + Value);
                }
                if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
                    if (const UFunction* Function = Call->GetTargetFunction()) PinDisplay.Add(TEXT("Function: ") + Function->GetOuter()->GetName() + TEXT(" / ") + Function->GetName());
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin) continue;
                    FString Entry = Pin->PinName.ToString() + TEXT("=") + Pin->DefaultValue + TEXT("|") + Pin->DefaultTextValue.ToString();
                    FString TypeText;
                    FEdGraphPinType::StaticStruct()->ExportText(TypeText, &Pin->PinType, nullptr, nullptr, PPF_None, nullptr);
                    Entry += TEXT("|") + TypeText;
                    if (Pin->DefaultObject) Entry += TEXT("|") + Pin->DefaultObject->GetPathName();
                    TArray<FString> Links;
                    TArray<FString> FriendlyLinks;
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                        if (Linked && Linked->GetOwningNode())
                        {
                            Links.Add(Linked->GetOwningNode()->NodeGuid.ToString(EGuidFormats::Digits) + TEXT("/") + Linked->PinName.ToString());
                            FriendlyLinks.Add(Linked->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString() + TEXT(" / ") + Linked->GetDisplayName().ToString());
                        }
                    Links.Sort();
                    Entry += TEXT(" -> ") + FString::Join(Links, TEXT(","));
                    PinData.Add(Entry);
                    FriendlyLinks.Sort();
                    FString Display = Pin->GetDisplayName().ToString();
                    if (Display.IsEmpty()) Display = FName::NameToDisplayString(Pin->PinName.ToString(), false);
                    if (!FriendlyLinks.IsEmpty()) Display += TEXT(" -> ") + FString::Join(FriendlyLinks, TEXT(", "));
                    else if (Pin->DefaultObject) Display += TEXT(": ") + Pin->DefaultObject->GetName();
                    else if (!Pin->DefaultValue.IsEmpty()) Display += TEXT(": ") + Pin->DefaultValue;
                    else if (!Pin->DefaultTextValue.IsEmpty()) Display += TEXT(": ") + Pin->DefaultTextValue.ToString();
                    else continue;
                    PinDisplay.Add(Display);
                }
                PinData.Sort();
                const FString FullValue = FString::Join(PinData, TEXT("\n")).Replace(*PackageName, TEXT("$ASSET"), ESearchCase::CaseSensitive);
                const FTCHARToUTF8 Bytes(*FullValue);
                uint8 Digest[FSHA1::DigestSize];
                FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Digest);
                Row->SetStringField(TEXT("fingerprint"), BytesToHex(Digest, FSHA1::DigestSize));
                const FString DisplayValue = FString::Join(PinDisplay, TEXT("\n"));
                Row->SetStringField(TEXT("value"), DisplayValue.Left(4096));
                Row->SetBoolField(TEXT("truncated"), DisplayValue.Len() > 4096);
                GraphRows.Add(MakeShared<FJsonValueObject>(Row));
            }
        }
    }
    Result->SetArrayField(TEXT("logic"), GraphRows);
    Result->SetBoolField(TEXT("logicTruncated"), GraphRows.Num() >= 512);
    // Keep protocol IDs unchanged; normalize only values containing our temporary
    // package instance, so equivalent self-references compare equal across revisions.
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Objects->Values)
    {
        for (const TSharedPtr<FJsonValue>& Value : Pair.Value->AsObject()->GetArrayField(TEXT("properties")))
        {
            TSharedPtr<FJsonObject> Row = Value->AsObject();
            for (const FString Field : {TEXT("value"), TEXT("diffValue")})
            {
                FString Text;
                if (Row->TryGetStringField(Field, Text)) Row->SetStringField(Field, Text.Replace(*PackageName, TEXT("$ASSET"), ESearchCase::CaseSensitive));
            }
        }
    }
    for (const TSharedPtr<FJsonValue>& Value : GraphRows)
    {
        TSharedPtr<FJsonObject> Row = Value->AsObject();
        Row->SetStringField(TEXT("value"), Row->GetStringField(TEXT("value")).Replace(*PackageName, TEXT("$ASSET"), ESearchCase::CaseSensitive));
    }
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
    return ExportBlueprintDetails(Object, bShowAdvanced, Asset->GetOutermost()->GetName());
}

FBox FAssetPreviewSession::Bounds(const FString& ComponentId) const
{
    FBox Box(ForceInit);
    TMap<FString, FString> Parents;
    for (const auto& Component : Components) Parents.Add(Component.Id, Component.Parent);
    for (const FAssetPreviewComponent& Component : Components)
    {
        bool bIncluded = ComponentId == TEXT("asset");
        FString Parent = Component.Id;
        for (int32 Depth = 0; !bIncluded && Depth < 64 && !Parent.IsEmpty(); ++Depth)
        {
            bIncluded = Parent == ComponentId;
            Parent = Parents.FindRef(Parent);
        }
        if (!bIncluded) continue;
        if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component.Preview))
            if (Primitive->IsRegistered()) Box += Primitive->Bounds.GetBox();
    }
    if (!Box.IsValid) Box = FBox(FVector(-50), FVector(50));
    return Box;
}

bool FAssetPreviewSession::Render(double Yaw, double Pitch, double Zoom, const FString& Focus,
    TArray<uint8>& OutPng, FString& Error, const FBox* SharedBounds)
{
    check(IsInGameThread());
    if (Widget) return Widget->Render(OutPng, Error);
    if (!TexturePng.IsEmpty()) { OutPng = TexturePng; return true; }
    if (!Capture || !RenderTarget || !FApp::CanEverRender())
    {
        Error = TEXT("Unreal rendering is unavailable. Run the Editor with a graphics device, without -nullrhi.");
        return false;
    }
    const FBox Box = SharedBounds ? *SharedBounds : Bounds(Focus);
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
