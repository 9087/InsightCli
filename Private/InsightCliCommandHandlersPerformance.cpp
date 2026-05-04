// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
using FHandlerFn = bool (*)(const FInsightCliRequest&, const FTraceContext&, FInsightCliResponse&);

constexpr int32 ExpectedPerformanceHandlerCount = 7;
constexpr FHandlerFn PerformanceHandlers[] =
{
	&HandleCpuCommands,
	&HandleGpuCommands,
	&HandleAnimCommands,
	&HandleNiagaraCommands,
	&HandleSlateCommands,
	&HandleThreadsCommands,
	&HandleTasksCommands,
};

static_assert(UE_ARRAY_COUNT(PerformanceHandlers) == ExpectedPerformanceHandlerCount, "Update performance handler mapping when adding/removing performance subcommands.");
}

bool HandlePerformanceCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	for (FHandlerFn Handler : PerformanceHandlers)
	{
		if (Handler(Request, Context, OutResponse))
		{
			return true;
		}
	}

	return false;
}
}
