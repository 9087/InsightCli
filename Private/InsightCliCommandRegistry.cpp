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
	std::initializer_list<const TCHAR*> RequiredOptions;
	std::initializer_list<const TCHAR*> OptionalOptions;
	std::initializer_list<const TCHAR*> RequiredChannels;
	std::initializer_list<const TCHAR*> OptionalChannels;
	EInsightCliCommandDataQuality DataQuality = EInsightCliCommandDataQuality::TraceBacked;
	bool bMayBeEmpty = true;
};

const FCommandCatalogEntry CommandCatalog[] =
{
	{ TEXT("info"), TEXT("summary"), &HandleInfoAndFramesCommands, {}, {}, {}, {}, EInsightCliCommandDataQuality::TraceBacked, false },
	{ TEXT("info"), TEXT("channels"), &HandleInfoAndFramesCommands, {}, {}, {}, {}, EInsightCliCommandDataQuality::TraceBacked, false },
	{ TEXT("info"), TEXT("capabilities"), &HandleInfoAndFramesCommands, {}, {}, {}, {}, EInsightCliCommandDataQuality::TraceBacked, false },
	{ TEXT("frames"), TEXT("summary"), &HandleInfoAndFramesCommands, {}, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("frames"), TEXT("slowest"), &HandleInfoAndFramesCommands, {}, { TEXT("limit"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("frames"), TEXT("detail"), &HandleInfoAndFramesCommands, { TEXT("frame-index") }, { TEXT("breakdown") } },
	{ TEXT("cpu"), TEXT("top"), &HandlePerformanceCommands, {}, { TEXT("thread"), TEXT("limit"), TEXT("stat-group"), TEXT("frame-index") } },
	{ TEXT("cpu"), TEXT("stat-groups"), &HandlePerformanceCommands, {}, {} },
	{ TEXT("cpu"), TEXT("stack"), &HandlePerformanceCommands, { TEXT("frame-index") }, { TEXT("thread"), TEXT("limit"), TEXT("view") } },
	{ TEXT("cpu"), TEXT("hot-functions"), &HandlePerformanceCommands, {}, { TEXT("thread"), TEXT("limit") } },
	{ TEXT("gpu"), TEXT("top"), &HandlePerformanceCommands, {}, { TEXT("limit"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("gpu"), TEXT("passes"), &HandlePerformanceCommands, { TEXT("frame-index") }, {} },
	{ TEXT("gpu"), TEXT("pass-detail"), &HandlePerformanceCommands, { TEXT("frame-index"), TEXT("pass") }, {} },
	{ TEXT("rhi"), TEXT("summary"), &HandlePerformanceCommands, { TEXT("frame-index") }, {} },
	{ TEXT("rhi"), TEXT("drawcalls"), &HandlePerformanceCommands, {}, { TEXT("limit"), TEXT("frame-index") } },
	{ TEXT("rhi"), TEXT("top-materials"), &HandlePerformanceCommands, {}, { TEXT("limit"), TEXT("frame-index") }, { TEXT("RHIDraws") }, { TEXT("RDG") }, EInsightCliCommandDataQuality::TraceBacked, true },
	{ TEXT("rhi"), TEXT("top-meshes"), &HandlePerformanceCommands, {}, { TEXT("limit"), TEXT("frame-index") }, { TEXT("RHIDraws") }, { TEXT("RDG") }, EInsightCliCommandDataQuality::TraceBacked, true },
	{ TEXT("anim"), TEXT("top-actors"), &HandlePerformanceCommands, {}, { TEXT("limit") } },
	{ TEXT("anim"), TEXT("graph"), &HandlePerformanceCommands, { TEXT("actor") }, {} },
	{ TEXT("anim"), TEXT("skinning"), &HandlePerformanceCommands, {}, { TEXT("limit") } },
	{ TEXT("niagara"), TEXT("top-systems"), &HandlePerformanceCommands, {}, { TEXT("limit") } },
	{ TEXT("niagara"), TEXT("emitter-cost"), &HandlePerformanceCommands, {}, { TEXT("system") } },
	{ TEXT("physics"), TEXT("summary"), &HandlePerformanceCommands, {}, { TEXT("frame-index") } },
	{ TEXT("physics"), TEXT("solver-stages"), &HandlePerformanceCommands, {}, {} },
	{ TEXT("physics"), TEXT("top-bodies"), &HandlePerformanceCommands, {}, { TEXT("limit") } },
	{ TEXT("slate"), TEXT("top-widgets"), &HandlePerformanceCommands, {}, { TEXT("by"), TEXT("limit") } },
	{ TEXT("slate"), TEXT("paint-cost"), &HandlePerformanceCommands, {}, { TEXT("frame-index") } },
	{ TEXT("slate"), TEXT("invalidation-rate"), &HandlePerformanceCommands, {}, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("threads"), TEXT("waits"), &HandlePerformanceCommands, {}, { TEXT("frame-index"), TEXT("limit"), TEXT("frame-domain") }, {}, { TEXT("WaitTrace"), TEXT("Mutex"), TEXT("IoStore"), TEXT("RHIFence") }, EInsightCliCommandDataQuality::ApproxOrTraceBacked, true },
	{ TEXT("threads"), TEXT("wait-chain"), &HandlePerformanceCommands, {}, { TEXT("thread"), TEXT("depth"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range"), TEXT("frame-domain") }, {}, { TEXT("WaitTrace"), TEXT("Mutex"), TEXT("IoStore"), TEXT("RHIFence") }, EInsightCliCommandDataQuality::ApproxOrTraceBacked, true },
	{ TEXT("tasks"), TEXT("top"), &HandlePerformanceCommands, {}, { TEXT("frame-index"), TEXT("limit"), TEXT("frame-domain") }, { TEXT("Task"), TEXT("Tasks"), TEXT("TaskGraph") }, {}, EInsightCliCommandDataQuality::TraceBacked, true },
	{ TEXT("tasks"), TEXT("critical-path"), &HandlePerformanceCommands, {}, { TEXT("frame-index"), TEXT("top"), TEXT("frame-domain") }, { TEXT("Task"), TEXT("Tasks"), TEXT("TaskGraph") }, {}, EInsightCliCommandDataQuality::TraceBacked, true },
	{ TEXT("net"), TEXT("summary"), &HandleDataCommands, {}, {} },
	{ TEXT("net"), TEXT("top-actors"), &HandleDataCommands, {}, { TEXT("limit") } },
	{ TEXT("net"), TEXT("top-rpcs"), &HandleDataCommands, {}, { TEXT("limit") } },
	{ TEXT("net"), TEXT("bandwidth-series"), &HandleDataCommands, {}, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("shaders"), TEXT("compile-events"), &HandleDataCommands, {}, { TEXT("limit") } },
	{ TEXT("shaders"), TEXT("pso-cache-misses"), &HandleDataCommands, {}, {} },
	{ TEXT("io"), TEXT("summary"), &HandleDataCommands, {}, {} },
	{ TEXT("io"), TEXT("slowest-reads"), &HandleDataCommands, {}, { TEXT("limit") } },
	{ TEXT("io"), TEXT("top-files"), &HandleDataCommands, {}, { TEXT("limit") } },
	{ TEXT("symbols"), TEXT("resolve"), &HandleDataCommands, { TEXT("name") }, {} },
	{ TEXT("counters"), TEXT("list"), &HandleDataCommands, {}, {} },
	{ TEXT("counters"), TEXT("series"), &HandleDataCommands, { TEXT("name") }, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("counters"), TEXT("stats"), &HandleDataCommands, { TEXT("name") }, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("memory"), TEXT("summary"), &HandleDataCommands, {}, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("memory"), TEXT("peak"), &HandleDataCommands, {}, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("memory"), TEXT("series"), &HandleDataCommands, {}, { TEXT("limit"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("memory"), TEXT("tags"), &HandleDataCommands, {}, { TEXT("limit"), TEXT("at"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("memory"), TEXT("diff"), &HandleDataCommands, { TEXT("t1"), TEXT("t2") }, { TEXT("limit"), TEXT("by") } },
	{ TEXT("memory"), TEXT("alloc-top"), &HandleDataCommands, {}, { TEXT("limit"), TEXT("by") } },
	{ TEXT("memory"), TEXT("leak-suspect"), &HandleDataCommands, {}, { TEXT("limit") } },
	{ TEXT("marks"), TEXT("search"), &HandleDataCommands, { TEXT("keyword") }, { TEXT("limit"), TEXT("category"), TEXT("channel"), TEXT("thread-id"), TEXT("case-sensitive"), TEXT("exact"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") } },
	{ TEXT("marks"), TEXT("around"), &HandleDataCommands, { TEXT("at") }, { TEXT("window-ms"), TEXT("limit") } },
	{ TEXT("marks"), TEXT("regions"), &HandleDataCommands, {}, { TEXT("name") } },
	{ TEXT("marks"), TEXT("region-slice"), &HandleDataCommands, { TEXT("name") }, {} },
	{ TEXT("loadtime"), TEXT("summary"), &HandleDataCommands, {}, {} },
	{ TEXT("loadtime"), TEXT("packages"), &HandleDataCommands, {}, { TEXT("limit"), TEXT("sort-by") } },
	{ TEXT("loadtime"), TEXT("slowest"), &HandleDataCommands, {}, { TEXT("limit") } },
	{ TEXT("loadtime"), TEXT("timeline"), &HandleDataCommands, {}, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range"), TEXT("limit") } },
	{ TEXT("gc"), TEXT("summary"), &HandleDataCommands, {}, {} },
	{ TEXT("gc"), TEXT("events"), &HandleDataCommands, {}, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range"), TEXT("limit") } },
	{ TEXT("gc"), TEXT("longest"), &HandleDataCommands, {}, { TEXT("limit") } },
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

void EnumerateCommandCatalog(TFunctionRef<void(const FInsightCliCommandCatalogEntry&)> Visitor)
{
	for (const FCommandCatalogEntry& Entry : CommandCatalog)
	{
		FInsightCliCommandCatalogEntry PublicEntry;
		PublicEntry.Group = Entry.Group;
		PublicEntry.Action = Entry.Action;
		for (const TCHAR* Option : Entry.RequiredOptions)
		{
			PublicEntry.RequiredOptions.Add(Option);
		}
		for (const TCHAR* Option : Entry.OptionalOptions)
		{
			PublicEntry.OptionalOptions.Add(Option);
		}
		for (const TCHAR* Channel : Entry.RequiredChannels)
		{
			PublicEntry.RequiredChannels.Add(Channel);
		}
		for (const TCHAR* Channel : Entry.OptionalChannels)
		{
			PublicEntry.OptionalChannels.Add(Channel);
		}
		PublicEntry.DataQuality = Entry.DataQuality;
		PublicEntry.bMayBeEmpty = Entry.bMayBeEmpty;

		Visitor(PublicEntry);
	}
}
}
