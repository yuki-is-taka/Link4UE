// Fill out your copyright notice in the Description page of Project Settings.

using System.IO;
using UnrealBuildTool;

public class LinkLibrary : ModuleRules
{
	public LinkLibrary(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "link", "include"));
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "link", "modules", "asio-standalone", "asio", "include"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("LINK_PLATFORM_WINDOWS=1");
		}
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
			PublicDefinitions.Add("LINK_PLATFORM_MACOSX=1");
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
			PublicDefinitions.Add("LINK_PLATFORM_LINUX=1");
		}
		PublicDefinitions.Add("BOOST_VERSION");
	}
}
