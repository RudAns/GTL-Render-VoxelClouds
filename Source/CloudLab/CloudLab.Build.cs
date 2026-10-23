using UnrealBuildTool;

public class CloudLab : ModuleRules
{
	public CloudLab(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "UMG", "Slate", "SlateCore",
			"RenderCore", "RHI", "ProceduralMeshComponent", "CloudLabShaders"
		});
	}
}
