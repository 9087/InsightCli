// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleTasksCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles tasks/top with optional frame filtering; returns false outside tasks group.
	if (Request.Group == TEXT("tasks") && Request.Action == TEXT("top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("frame-index") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		int32 FrameIndexFilter = -1;
		bool bHasFrameIndex = false;
		FInsightCliResponse ValidationError;
		if (!TryGetLimitAndOptionalFrameIndexFilter(Request.Args, Limit, FrameIndexFilter, bHasFrameIndex, ValidationError))
		{
			OutResponse = ValidationError;
			return true;
		}

		TArray<FTaskSample> Tasks;
		FString FailureStage;
		FString FailureReason;
		bool bFrameFound = true;
		if (!BuildTaskTopSamples(Context, bHasFrameIndex ? TOptional<int32>(FrameIndexFilter) : TOptional<int32>(), Tasks, FailureStage, FailureReason, bFrameFound))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("trace_path"), Context.FullPath);
			Details.Add(TEXT("consumer"), TEXT("tasks.top"));
			Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("tasks_extraction") : FailureStage);
			Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to build task diagnostics") : FailureReason);
			if (bHasFrameIndex)
			{
				Details.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
			}
			Details.Add(TEXT("data_source"), TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed task graph diagnostics are unavailable for this trace."), Details);
			return true;
		}

		if (bHasFrameIndex && !bFrameFound)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndexFilter));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Tasks.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeTaskObject(Tasks[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		if (bHasFrameIndex)
		{
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
		}
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
