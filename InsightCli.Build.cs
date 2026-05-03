// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class InsightCli : ModuleRules
{
	public InsightCli(ReadOnlyTargetRules Target) : base(Target)
	{
		// Single-module layout: app entry, contracts, and command handlers.
		PublicIncludePathModuleNames.Add("Launch");

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Runtime and file/path helpers.
				"Core",
				"Projects",
				// Trace analysis providers for raw frame extraction.
				"TraceServices",
				// JSON envelope serialization and parsing.
				"Json"
			}
		);
	}
}
