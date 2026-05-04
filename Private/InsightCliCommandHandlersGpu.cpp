// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleGpuCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles gpu/top, gpu/passes and gpu/pass-detail subcommands; returns false for non-gpu requests.
	if (Request.Group == TEXT("gpu") && Request.Action == TEXT("top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
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

	if (Request.Group == TEXT("gpu") && Request.Action == TEXT("passes"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		const bool bHasTimeStart = HasOption(Request.Args, TEXT("--time-start"));
		const bool bHasTimeEnd = HasOption(Request.Args, TEXT("--time-end"));

		int32 FrameIndex = -1;
		const bool bHasFrameIndex = TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex);
		if (bHasFrameIndex && FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}
		if (bHasFrameIndex && (bHasTimeStart || bHasTimeEnd))
		{
			OutResponse = MakeOptionError(TEXT("--frame-index cannot be combined with --time-start/--time-end."));
			return true;
		}

		TOptional<double> IntervalStartSec;
		TOptional<double> IntervalEndSec;
		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));

		if (bHasFrameIndex)
		{
			FInsightCliResponse FrameGuardError;
			if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("gpu.passes")))
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
				TMap<FString, FString> NotFoundMeta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
				NotFoundMeta.Add(TEXT("data_source"), TEXT("trace"));
				OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, NotFoundMeta));
				return true;
			}

			IntervalStartSec = Found->FrameStartMs / 1000.0;
			IntervalEndSec = Found->FrameEndMs / 1000.0;
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
		}
		else
		{
			FResolvedTimeWindowMs TimeWindow;
			FInsightCliResponse TimeWindowError;
			if (!TryResolveTimeWindowMs(Context, Request.Args, false, TimeWindow, TimeWindowError))
			{
				OutResponse = TimeWindowError;
				return true;
			}

			if (TimeWindow.StartMs.IsSet())
			{
				IntervalStartSec = TimeWindow.StartMs.GetValue() / 1000.0;
			}
			if (TimeWindow.EndMs.IsSet())
			{
				IntervalEndSec = TimeWindow.EndMs.GetValue() / 1000.0;
			}
			AppendTimeWindowMeta(TimeWindow, Meta);
		}

		TArray<FGpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(Context, Samples, FailureStage, FailureReason, IntervalStartSec, IntervalEndSec))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gpu.passes"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build gpu pass aggregation"),
				TEXT("Trace-backed GPU pass data is unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Samples.Num());
		for (const FGpuScopeSample& Sample : Samples)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeGpuPassesObject(Sample)));
		}

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
			OutResponse = MakeOptionError(TEXT("--frame-index is required for gpu pass-detail."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}

		FString PassName;
		FInsightCliResponse RequiredOptionError;
		if (!RequireStringOption(Request.Args, TEXT("--pass"), TEXT("gpu pass-detail"), PassName, RequiredOptionError))
		{
			OutResponse = RequiredOptionError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
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
