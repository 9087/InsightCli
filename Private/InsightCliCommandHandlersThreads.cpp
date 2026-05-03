// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleThreadsCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles threads/waits and optional frame filtering; ignores unrelated command groups.
	if (Request.Group == TEXT("threads") && Request.Action == TEXT("waits"))
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

		TArray<FThreadWaitSample> Waits;
		FString FailureStage;
		FString FailureReason;
		bool bFrameFound = true;
		if (!BuildThreadWaitSamplesTrace(Context, bHasFrameIndex ? TOptional<int32>(FrameIndexFilter) : TOptional<int32>(), Waits, FailureStage, FailureReason, bFrameFound))
		{
			TMap<FString, FString> ExtraDetails;
			if (bHasFrameIndex)
			{
				ExtraDetails.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
			}
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("threads.waits"),
				FailureStage,
				FailureReason,
				TEXT("waits_extraction"),
				TEXT("failed to build thread waits"),
				TEXT("Trace-backed thread wait causality is unavailable for this trace."),
				ExtraDetails);
			return true;
		}

		if (bHasFrameIndex && !bFrameFound)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndexFilter));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Waits.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeThreadWaitObject(Waits[Index])));
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
