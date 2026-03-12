using UnrealBuildTool;

public class Link4UE : ModuleRules
{
	public Link4UE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Engine",
				"LinkLibrary",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"AudioMixer",
				"AudioExtensions",
				"SignalProcessing",
				"Settings",
			}
		);
	}
}
