using UnrealBuildTool;

public class RiderAssetPreview : ModuleRules
{
    public RiderAssetPreview(ReadOnlyTargetRules Target) : base(Target)
    {
        // Avoid loading UnrealEd's multi-GB shared PCH on memory-constrained hosts.
        PCHUsage = PCHUsageMode.NoPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "HTTPServer", "Json",
            "Sockets", "RenderCore", "RHI", "ImageCore", "ImageWrapper", "PropertyEditor", "Slate", "SlateCore", "BlueprintGraph", "UMG", "UMGEditor"
        });
    }
}
