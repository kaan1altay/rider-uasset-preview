using UnrealBuildTool;

public class RiderAssetPreview : ModuleRules
{
    public RiderAssetPreview(ReadOnlyTargetRules Target) : base(Target)
    {
        // Two small translation units: avoid loading UnrealEd's multi-GB shared PCH on 16 GB hosts.
        PCHUsage = PCHUsageMode.NoPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "HTTPServer", "Json",
            "Sockets", "RenderCore", "RHI", "ImageCore", "ImageWrapper", "PropertyEditor", "Slate", "SlateCore"
        });
    }
}
