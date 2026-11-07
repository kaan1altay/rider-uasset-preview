#include "DetailsExport.h"

#include "Components/MeshComponent.h"
#include "EditorCategoryUtils.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Layout/Children.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/UnrealType.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    FString WidgetLabel(const TSharedPtr<SWidget>& Widget, int32 Depth = 0)
    {
        if (!Widget || Depth > 12) return FString();
        if (Widget->GetType() == TEXT("STextBlock"))
            return StaticCastSharedPtr<STextBlock>(Widget)->GetText().ToString();
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
        {
            FString Label = WidgetLabel(Children->GetChildAt(Index), Depth + 1);
            if (!Label.IsEmpty()) return Label;
        }
        return FString();
    }

    FString CategoryLabel(FName Name)
    {
        // Scene component customization names its native Transform category TransformCommon.
        if (Name == TEXT("TransformCommon")) return TEXT("Transform");
        return FEditorCategoryUtils::GetCategoryDisplayString(Name.ToString());
    }

    void AddRow(TArray<TSharedPtr<FJsonValue>>& Rows, const FString& Category,
        const FString& Label, const FString& Value, const FString& PropertyName = FString())
    {
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("category"), Category.IsEmpty() ? TEXT("General") : Category);
        Row->SetStringField(TEXT("name"), Label);
        Row->SetStringField(TEXT("value"), Value.Left(4096));
        // Stable transport identity for validation; the UI shows the human-readable name.
        Row->SetStringField(TEXT("property"), PropertyName);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    void AppendNode(const TSharedRef<IDetailTreeNode>& Node, FString Category, bool bShowAdvanced,
        TArray<TSharedPtr<FJsonValue>>& Rows, int32 Depth = 0)
    {
        if (Depth > 16 || Rows.Num() >= 500) return;
        if (Node->GetNodeType() == EDetailNodeType::Advanced && !bShowAdvanced) return;
        if (Node->GetNodeType() == EDetailNodeType::Category) Category = CategoryLabel(Node->GetNodeName());
        TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
        TArray<TSharedRef<IDetailTreeNode>> Children;
        Node->GetChildren(Children); // Honors detail customizations and conditional visibility.
        if (Handle && Handle->IsValidHandle() && Node->GetNodeType() == EDetailNodeType::Item)
        {
            const FProperty* Property = Handle->GetProperty();
            if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)
                || (!bShowAdvanced && Property->HasAnyPropertyFlags(CPF_AdvancedDisplay))) return;

            // Customized struct sections expose their visible child rows, not a raw struct blob.
            const FStructProperty* Struct = CastField<FStructProperty>(Property);
            const FString StructName = Struct && Struct->Struct ? Struct->Struct->GetName() : FString();
            const bool bCompactStruct = StructName == TEXT("Vector") || StructName == TEXT("Vector2D")
                || StructName == TEXT("Rotator") || StructName == TEXT("Color") || StructName == TEXT("LinearColor");
            if (Struct && !bCompactStruct && !Children.IsEmpty())
            {
                for (const TSharedRef<IDetailTreeNode>& Child : Children) AppendNode(Child, Category, bShowAdvanced, Rows, Depth + 1);
                return;
            }

            FString Label = WidgetLabel(Node->CreateNodeWidgets().NameWidget);
            if (Label.IsEmpty()) Label = Handle->GetPropertyDisplayName().ToString();
            FString Value;
            if (CastField<FObjectPropertyBase>(Property))
            {
                UObject* Referenced = nullptr;
                if (Handle->GetValue(Referenced) == FPropertyAccess::Success)
                    Value = Referenced ? Referenced->GetName() : TEXT("None");
            }
            if (Value.IsEmpty()) Handle->GetValueAsDisplayString(Value);
            if (CastField<FArrayProperty>(Property) || CastField<FMapProperty>(Property) || CastField<FSetProperty>(Property))
            {
                uint32 Count = 0;
                Handle->GetNumChildren(Count);
                Value = FString::Printf(TEXT("%u elements"), Count);
                AddRow(Rows, Category, Label, Value, Property->GetName());
                int32 Exported = 0;
                for (const TSharedRef<IDetailTreeNode>& Child : Children)
                {
                    if (++Exported > 32) break;
                    AppendNode(Child, Category, bShowAdvanced, Rows, Depth + 1);
                }
                return;
            }
            AddRow(Rows, Category, Label, Value, Property->GetName());
            return;
        }
        for (const TSharedRef<IDetailTreeNode>& Child : Children) AppendNode(Child, Category, bShowAdvanced, Rows, Depth + 1);
    }
}

TSharedRef<FJsonObject> ExportBlueprintDetails(UObject* Object, bool bShowAdvanced)
{
    check(IsInGameThread());
    FPropertyRowGeneratorArgs Args;
    Args.bShouldShowHiddenProperties = false;
    Args.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Show;
    FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
    TSharedRef<IPropertyRowGenerator> Generator = PropertyEditor.CreatePropertyRowGenerator(Args);
    Generator->OnRowsRefreshed().AddLambda([] {});
    Generator->SetObjects({Object});
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const TSharedRef<IDetailTreeNode>& Root : Generator->GetRootTreeNodes()) AppendNode(Root, FString(), bShowAdvanced, Rows);

    // Material slot pickers are custom Slate rows without property handles. Export their
    // read-only values explicitly, while respecting the class's hidden Materials category.
    if (UMeshComponent* Mesh = Cast<UMeshComponent>(Object))
    {
        const bool bHasMaterials = Rows.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Row)
        {
            return Row->AsObject()->GetStringField(TEXT("category")) == TEXT("Materials");
        });
        if (!bHasMaterials && !FEditorCategoryUtils::IsCategoryHiddenFromClass(Object->GetClass(), TEXT("Materials")))
        {
            for (int32 Index = 0; Index < FMath::Min(Mesh->GetNumMaterials(), 32); ++Index)
            {
                UMaterialInterface* Material = Mesh->GetMaterial(Index);
                AddRow(Rows, TEXT("Materials"), FString::Printf(TEXT("Element %d"), Index),
                    Material ? Material->GetName() : TEXT("None"), TEXT("OverrideMaterials"));
            }
        }
    }
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), TEXT("UnrealDetails"));
    Result->SetBoolField(TEXT("advanced"), bShowAdvanced);
    Result->SetArrayField(TEXT("properties"), Rows);
    return Result;
}
