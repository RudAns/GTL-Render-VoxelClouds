using UnrealBuildTool;

public class CloudLabShaders : ModuleRules
{
	public CloudLabShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "RenderCore", "RHI", "Renderer" });
		// The ray marcher reads the scene textures handed to scene view extensions (PostProcess/PostProcessInputs.h).
		PrivateIncludePaths.Add(System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Private"));
		PrivateIncludePaths.Add(System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Internal"));
	}
}
