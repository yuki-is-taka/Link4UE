// Copyright Tokyologist. All Rights Reserved.

using UnrealBuildTool;

public class Link4UEEditor : ModuleRules
{
	public Link4UEEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"PropertyEditor",
			"UnrealEd",
			"Link4UE",
		});
	}
}
