using UnrealBuildTool;

public class VoxelCloudsTarget : TargetRules
{
	public VoxelCloudsTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "CloudLabShaders", "CloudLab" });
	}
}
