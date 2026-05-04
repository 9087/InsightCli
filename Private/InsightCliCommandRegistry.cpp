// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandRegistry.h"
#include "InsightCliCommandContext.h"

namespace UE::InsightCli
{
using namespace Internal;

namespace
{
using FGroupHandlerFn = bool (*)(const FInsightCliRequest&, const FTraceContext&, FInsightCliResponse&);

struct FCommandCatalogEntry
{
	const TCHAR* Group;
	const TCHAR* Action;
	FGroupHandlerFn Handler;
};

constexpr FCommandCatalogEntry CommandCatalog[] =
{
	{ TEXT("info"), TEXT("summary"), &HandleInfoAndFramesCommands },
	{ TEXT("info"), TEXT("channels"), &HandleInfoAndFramesCommands },
	{ TEXT("frames"), TEXT("summary"), &HandleInfoAndFramesCommands },
	{ TEXT("frames"), TEXT("slowest"), &HandleInfoAndFramesCommands },
	{ TEXT("frames"), TEXT("detail"), &HandleInfoAndFramesCommands },
	{ TEXT("cpu"), TEXT("top"), &HandlePerformanceCommands },
	{ TEXT("cpu"), TEXT("stack"), &HandlePerformanceCommands },
	{ TEXT("cpu"), TEXT("hot-functions"), &HandlePerformanceCommands },
	{ TEXT("gpu"), TEXT("top"), &HandlePerformanceCommands },
	{ TEXT("gpu"), TEXT("passes"), &HandlePerformanceCommands },
	{ TEXT("gpu"), TEXT("pass-detail"), &HandlePerformanceCommands },
	{ TEXT("threads"), TEXT("waits"), &HandlePerformanceCommands },
	{ TEXT("threads"), TEXT("wait-chain"), &HandlePerformanceCommands },
	{ TEXT("tasks"), TEXT("top"), &HandlePerformanceCommands },
	{ TEXT("tasks"), TEXT("critical-path"), &HandlePerformanceCommands },
	{ TEXT("symbols"), TEXT("resolve"), &HandleDataCommands },
	{ TEXT("counters"), TEXT("list"), &HandleDataCommands },
	{ TEXT("counters"), TEXT("series"), &HandleDataCommands },
	{ TEXT("counters"), TEXT("stats"), &HandleDataCommands },
	{ TEXT("memory"), TEXT("summary"), &HandleDataCommands },
	{ TEXT("memory"), TEXT("peak"), &HandleDataCommands },
	{ TEXT("memory"), TEXT("series"), &HandleDataCommands },
	{ TEXT("memory"), TEXT("tags"), &HandleDataCommands },
	{ TEXT("memory"), TEXT("diff"), &HandleDataCommands },
	{ TEXT("memory"), TEXT("alloc-top"), &HandleDataCommands },
	{ TEXT("memory"), TEXT("leak-suspect"), &HandleDataCommands },
	{ TEXT("marks"), TEXT("search"), &HandleDataCommands },
	{ TEXT("marks"), TEXT("around"), &HandleDataCommands },
	{ TEXT("loadtime"), TEXT("summary"), &HandleDataCommands },
	{ TEXT("loadtime"), TEXT("packages"), &HandleDataCommands },
	{ TEXT("loadtime"), TEXT("slowest"), &HandleDataCommands },
	{ TEXT("loadtime"), TEXT("timeline"), &HandleDataCommands },
	{ TEXT("gc"), TEXT("summary"), &HandleDataCommands },
	{ TEXT("gc"), TEXT("events"), &HandleDataCommands },
	{ TEXT("gc"), TEXT("longest"), &HandleDataCommands },
};

const FCommandCatalogEntry* FindCatalogEntry(const FInsightCliRequest& Request)
{
	for (const FCommandCatalogEntry& Entry : CommandCatalog)
	{
		if (Request.Group == Entry.Group && Request.Action == Entry.Action)
		{
			return &Entry;
		}
	}

	return nullptr;
}

FInsightCliResponse ExecuteResolvedCommand(const FInsightCliRequest& Request, const FTraceContext& Context)
{
	const FCommandCatalogEntry* Entry = FindCatalogEntry(Request);
	if (Entry == nullptr)
	{
		TMap<FString, FString> Details;
		Details.Add(TEXT("group"), Request.Group);
		Details.Add(TEXT("action"), Request.Action);
		return MakeNotFoundError(TEXT("Unknown command group/action."), Details);
	}

	FInsightCliResponse Response;
	if (Entry->Handler(Request, Context, Response))
	{
		return Response;
	}

	TMap<FString, FString> Details;
	Details.Add(TEXT("group"), Request.Group);
	Details.Add(TEXT("action"), Request.Action);
	return FInsightCliResponse::Error(
		10,
		TEXT("E3001"),
		TEXT("Command catalog/handler mismatch."),
		Details);
}
}

FInsightCliResponse ExecuteCommand(const FInsightCliRequest& Request)
{
	FTraceContext Context;
	FInsightCliResponse ValidationError = ValidateTraceAndBuildContext(Request, Context);
	if (ValidationError.ExitCode != 0)
	{
		return ValidationError;
	}

	return ExecuteResolvedCommand(Request, Context);
}

FInsightCliResponse ExecuteCommandWithSharedContext(const FInsightCliRequest& Request, const Internal::FTraceContext& SharedContext)
{
	return ExecuteResolvedCommand(Request, SharedContext);
}
}
