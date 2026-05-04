// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
using FHandlerFn = bool (*)(const FInsightCliRequest&, const FTraceContext&, FInsightCliResponse&);

constexpr int32 ExpectedDataHandlerCount = 8;
constexpr FHandlerFn DataHandlers[] =
{
	&HandleSymbolsCommands,
	&HandleCountersCommands,
	&HandleMemoryCommands,
	&HandleMarksCommands,
	&HandleLoadTimeCommands,
	&HandleGcCommands,
	&HandleIoCommands,
	&HandleNetCommands,
};

static_assert(UE_ARRAY_COUNT(DataHandlers) == ExpectedDataHandlerCount, "Update data handler mapping when adding/removing data subcommands.");
}

bool HandleDataCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	for (FHandlerFn Handler : DataHandlers)
	{
		if (Handler(Request, Context, OutResponse))
		{
			return true;
		}
	}

	return false;
}
}
