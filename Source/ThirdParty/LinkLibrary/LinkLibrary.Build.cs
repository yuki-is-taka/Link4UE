using System.IO;
using UnrealBuildTool;

public class LinkLibrary : ModuleRules
{
	public LinkLibrary(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// Link is header-only. Add include paths for Link and ASIO standalone.
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "link", "include"));
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "link", "modules", "asio-standalone", "asio", "include"));

		// Platform defines (must match Link's AbletonLinkConfig.cmake)
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("LINK_PLATFORM_WINDOWS=1");
			PublicDefinitions.Add("_SCL_SECURE_NO_WARNINGS");

			// Link uses winsock2 and iphlpapi for network interface scanning.
			// These are also pulled in via #pragma comment(lib, ...) in Link headers,
			// but explicit linking is more robust with UBT.
			PublicSystemLibraries.Add("iphlpapi.lib");
			PublicSystemLibraries.Add("ws2_32.lib");
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicDefinitions.Add("LINK_PLATFORM_MACOSX=1");
			PublicDefinitions.Add("LINK_PLATFORM_UNIX=1");
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			PublicDefinitions.Add("LINK_PLATFORM_LINUX=1");
			PublicDefinitions.Add("LINK_PLATFORM_UNIX=1");

			// Link tests link against pthread and atomic on Linux
			PublicSystemLibraries.Add("pthread");
			PublicSystemLibraries.Add("atomic");
		}
	}
}
