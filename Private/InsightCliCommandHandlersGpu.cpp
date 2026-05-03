// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleGpuCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles gpu/top and gpu/pass-detail subcommands; returns false for non-gpu requests.
	if (Request.Group == TEXT("gpu") && Request.Action == TEXT("top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		const bool bHasLimit = TryGetIntOption(Request.Args, TEXT("--limit"), Limit);
		if (bHasLimit && Limit <= 0)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("limit must be > 0."));
			return true;
		}

		TArray<FGpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(Context, Samples, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gpu.top"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build gpu aggregation"),
				TEXT("Trace-backed GPU timing is unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Samples.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeGpuTopObject(Samples[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("gpu") && Request.Action == TEXT("pass-detail"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("pass"), TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 FrameIndex = -1;
		if (!TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("--frame-index is required for gpu pass-detail."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("frame-index must be >= 0."));
			return true;
		}

		FString PassName;
		if (!TryGetStringOption(Request.Args, TEXT("--pass"), PassName))
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("--pass is required for gpu pass-detail."));
			return true;
		}

		int32 Limit = 100;
		const bool bHasLimit = TryGetIntOption(Request.Args, TEXT("--limit"), Limit);
		if (bHasLimit && Limit <= 0)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("limit must be > 0."));
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("gpu.pass-detail")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		const FFrameSample* Found = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
		{
			return Item.FrameIndex == FrameIndex;
		});

		if (Found == nullptr)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("limit"), FString::FromInt(Limit));
			Meta.Add(TEXT("query_pass"), PassName);
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		TArray<FGpuScopeSample> FrameGpuSamples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(
			Context,
			FrameGpuSamples,
			FailureStage,
			FailureReason,
			Found->FrameStartMs / 1000.0,
			Found->FrameEndMs / 1000.0))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gpu.pass-detail"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build frame gpu aggregation"),
				TEXT("Trace-backed GPU timing is unavailable for this trace."),
				{{TEXT("frame_index"), FString::FromInt(FrameIndex)}});
			return true;
		}

		TArray<FGpuScopeSample> Matched;
		for (const FGpuScopeSample& Sample : FrameGpuSamples)
		{
			if (Sample.ScopeName.Equals(PassName, ESearchCase::IgnoreCase))
			{
				Matched.Add(Sample);
			}
		}

		if (Matched.IsEmpty())
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("pass"), PassName);
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("limit"), FString::FromInt(Limit));
			Meta.Add(TEXT("data_source"), TEXT("trace"));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Matched.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeGpuPassDetailObject(*Found, Matched[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
