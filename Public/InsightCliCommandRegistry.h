// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "InsightCliTypes.h"

namespace UE::InsightCli::Internal
{
struct FTraceContext;
}

namespace UE::InsightCli
{
FInsightCliResponse ExecuteCommand(const FInsightCliRequest& Request);
FInsightCliResponse ExecuteCommandWithSharedContext(const FInsightCliRequest& Request, const Internal::FTraceContext& SharedContext);
}
