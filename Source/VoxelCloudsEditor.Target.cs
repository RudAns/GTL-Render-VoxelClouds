using UnrealBuildTool;

public class VoxelCloudsEditorTarget : TargetRules
{
	public VoxelCloudsEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "CloudLabShaders", "CloudLab" });
	}
}
