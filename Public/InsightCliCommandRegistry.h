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
struct FInsightCliCommandCatalogEntry
{
	FString Group;
	FString Action;
	TArray<FString> RequiredOptions;
	TArray<FString> OptionalOptions;
};

FInsightCliResponse ExecuteCommand(const FInsightCliRequest& Request);
FInsightCliResponse ExecuteCommandWithSharedContext(const FInsightCliRequest& Request, const Internal::FTraceContext& SharedContext);
void EnumerateCommandCatalog(TFunctionRef<void(const FInsightCliCommandCatalogEntry&)> Visitor);
}
