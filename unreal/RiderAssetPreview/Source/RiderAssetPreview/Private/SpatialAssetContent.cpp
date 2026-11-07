#include "AssetPreviewSession.h"
#include "WidgetPreviewContent.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "WidgetBlueprint.h"

namespace
{
    UWidgetBlueprint* ResolveTree(UWidgetBlueprint* Blueprint)
    {
        TSet<UWidgetBlueprint*> Seen;
        while (Blueprint && (!Blueprint->WidgetTree || !Blueprint->WidgetTree->RootWidget))
        {
            if (Seen.Contains(Blueprint)) return nullptr;
            Seen.Add(Blueprint);
            Blueprint = Blueprint->ParentClass ? Cast<UWidgetBlueprint>(Blueprint->ParentClass->ClassGeneratedBy) : nullptr;
        }
        return Blueprint;
    }

    // GetAllWidgets excludes nested UserWidget trees. Count every instance's
    // template recursively before Initialize/TakeWidget materializes it.
    bool WithinWidgetBudget(UWidgetBlueprint* Blueprint, int32& Count, TSet<UWidgetBlueprint*>& Ancestors)
    {
        UWidgetBlueprint* Owner = ResolveTree(Blueprint);
        if (!Owner) return true;
        if (Ancestors.Num() >= 16 || Ancestors.Contains(Owner)) return false;
        Ancestors.Add(Owner);
        TArray<UWidget*> Widgets;
        Owner->WidgetTree->GetAllWidgets(Widgets);
        for (UWidget* Item : Widgets)
        {
            if (++Count > 512) return false;
            if (Item->IsA<UUserWidget>())
                if (UWidgetBlueprint* Nested = Cast<UWidgetBlueprint>(Item->GetClass()->ClassGeneratedBy))
                    if (!WithinWidgetBudget(Nested, Count, Ancestors)) return false;
        }
        Ancestors.Remove(Owner);
        return true;
    }
}

bool FAssetPreviewSession::OpenMap(UWorld* World, FString& Error)
{
    if (!World->PersistentLevel) { Error = TEXT("This map has no saved persistent level."); return false; }
    bMap = true;
    DetailsObject = World->GetWorldSettings(false, false);
    if (!DetailsObject) DetailsObject = World;
    TMap<AActor*, FString> ActorIds;
    TMap<UActorComponent*, FString> ComponentIds;
    int32 ActorCount = 0;
    for (AActor* Actor : World->PersistentLevel->Actors)
    {
        // World loading spawns transient helpers (physics volume, debug actors).
        // They have new GUIDs on every load and are not saved level changes.
        if (!IsValid(Actor) || Actor->HasAnyFlags(RF_Transient) || !Actor->HasAnyFlags(RF_WasLoaded) || Actor->IsA<AWorldSettings>()) continue;
        if (++ActorCount > 128) { bHierarchyLimited = true; break; }
        const FString Key = Actor->GetActorGuid().IsValid() ? TEXT("actor:") + Actor->GetActorGuid().ToString(EGuidFormats::Digits) : TEXT("actor-name:") + Actor->GetName();
        ActorIds.Add(Actor, Key);
    }
    for (const auto& Pair : ActorIds)
    {
        AActor* Actor = Pair.Key;
        const FString Parent = ActorIds.FindRef(Actor->GetAttachParentActor());
        Components.Add({Pair.Value, Parent.IsEmpty() ? TEXT("asset") : Parent, Actor->GetActorLabel(), Actor, nullptr, NAME_None, Pair.Value});
        for (UActorComponent* Component : Actor->GetComponents())
        {
            if (!Component || Component->HasAnyFlags(RF_Transient)) continue;
            if (Component && ComponentIds.Num() + ActorIds.Num() < 512)
                ComponentIds.Add(Component, Pair.Value + TEXT("/component:") + Component->GetName());
            else bHierarchyLimited = true;
        }
    }
    for (const auto& Pair : ComponentIds)
    {
        if (Components.Num() >= 512) { bHierarchyLimited = true; break; }
        UActorComponent* Component = Pair.Key;
        FString Parent = ActorIds.FindRef(Component->GetOwner());
        FAssetPreviewComponent Entry{Pair.Value, Parent, Component->GetName(), Component, nullptr, NAME_None, Pair.Value};
        if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
        {
            Entry.Socket = SceneComponent->GetAttachSocketName();
            if (SceneComponent->GetAttachParent() && SceneComponent->GetAttachParent()->GetOwner() == Component->GetOwner())
            {
                const FString ComponentParent = ComponentIds.FindRef(SceneComponent->GetAttachParent());
                if (!ComponentParent.IsEmpty()) Entry.Parent = ComponentParent;
            }
            Entry.bWorldSpace = true;
            // Engine evaluation includes socket offsets and absolute transform
            // channels. The source belongs to our isolated revision package.
            SceneComponent->UpdateComponentToWorld();
            Entry.SavedTransform = SceneComponent->GetComponentTransform();
        }
        // Avoid materializing unbounded instance buffers in a review scene.
        if (const UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Component))
            if (Instances->GetInstanceCount() > 10000) { bHierarchyLimited = true; continue; }
        Components.Add(MoveTemp(Entry));
    }
    // Only saved persistent-level exports are read. External actor packages and
    // streamed levels are deliberately not loaded into the user's editor world.
    const bool bExternal = World->GetWorldPartition() != nullptr || !World->GetStreamingLevels().IsEmpty();
    bHierarchyLimited |= bExternal;
    Notice = TEXT("Saved persistent level · neutral lighting · static/skeletal mesh geometry · no gameplay or Construction Script");
    if (bExternal) Notice += TEXT(" · partial: World Partition/external actors and streamed levels are not included");
    if (bHierarchyLimited) Notice += TEXT(" · bounded hierarchy: some objects are not included");
    return true;
}

bool FAssetPreviewSession::OpenWidget(UWidgetBlueprint* Blueprint, FString& Error)
{
    int32 ControlCount = 0;
    TSet<UWidgetBlueprint*> Ancestors;
    if (!WithinWidgetBudget(Blueprint, ControlCount, Ancestors))
    {
        Error = TEXT("Widget preview supports at most 512 controls across nested designs and 16 nesting levels, without cycles. Inspect this design in Unreal.");
        return false;
    }
    UWidgetBlueprint* TreeOwner = ResolveTree(Blueprint);
    DetailsObject = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : static_cast<UObject*>(Blueprint);
    if (TreeOwner && TreeOwner->WidgetTree)
    {
        TArray<UWidget*> Widgets;
        TreeOwner->WidgetTree->GetAllWidgets(Widgets);
        if (Widgets.Num() > 512) { Error = TEXT("Widget preview is limited to 512 controls. Inspect this larger design in Unreal."); return false; }
        TMap<UWidget*, FString> Keys;
        for (UWidget* Item : Widgets)
        {
            const FGuid* Guid = TreeOwner->WidgetVariableNameToGuidMap.Find(Item->GetFName());
            Keys.Add(Item, Guid && Guid->IsValid() ? TEXT("widget:") + Guid->ToString(EGuidFormats::Digits) : TEXT("widget-name:") + Item->GetName());
        }
        for (UWidget* Item : Widgets)
        {
            if (Components.Num() >= 512) { bHierarchyLimited = true; break; }
            const FString Key = Keys.FindChecked(Item);
            FString Parent = Keys.FindRef(Item->GetParent());
            FString Label = Item->GetDisplayLabel();
            if (Label.IsEmpty()) Label = Item->GetName();
            Components.Add({Key, Parent.IsEmpty() ? TEXT("asset") : Parent, Label, Item, nullptr, NAME_None, Key});
            if (Item->Slot && Components.Num() < 512)
                Components.Add({Key + TEXT("/slot"), Key, TEXT("Layout"), Item->Slot, nullptr, NAME_None, Key + TEXT("/slot")});
            else if (Item->Slot) bHierarchyLimited = true;
        }
    }
    Widget = MakeUnique<FWidgetPreviewContent>();
    if (!Widget->Open(Blueprint->GeneratedClass, Error)) return false;
    Notice = TEXT("Widget design · 1280 × 720 · saved layout · PreConstruct, Construct and OnInitialized skipped · paint, bindings and native designer callbacks may run · nested widget internals are not expanded");
    return true;
}
