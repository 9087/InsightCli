// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "InsightCliTypes.h"

#include "Containers/Array.h"
#include "Templates/Function.h"

namespace UE::InsightCli::Internal
{
struct FTraceContext;
}

namespace UE::InsightCli
{
enum class EInsightCliCommandDataQuality : uint8
{
	TraceBacked,
	ApproxOrTraceBacked,
	Approx,
};

struct FInsightCliCommandCatalogEntry
{
	FString Group;
	FString Action;
	TArray<FString> RequiredOptions;
	TArray<FString> OptionalOptions;
	TArray<FString> RequiredChannels;
	TArray<FString> OptionalChannels;
	EInsightCliCommandDataQuality DataQuality = EInsightCliCommandDataQuality::TraceBacked;
	bool bMayBeEmpty = true;
};

FInsightCliResponse ExecuteCommand(const FInsightCliRequest& Request);
FInsightCliResponse ExecuteCommandWithSharedContext(const FInsightCliRequest& Request, const Internal::FTraceContext& SharedContext);
void EnumerateCommandCatalog(TFunctionRef<void(const FInsightCliCommandCatalogEntry&)> Visitor);
}
