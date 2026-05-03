// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleMemoryCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles memory/summary, memory/peak, memory/series, and memory/tags.
	if (Request.Group == TEXT("memory") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		TArray<FMemorySample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemorySamplesTrace(Context, Samples, FailureStage, FailureReason, TimeWindow.StartMs, TimeWindow.EndMs))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("memory.summary"),
				FailureStage,
				FailureReason,
				TEXT("memory_extraction"),
				TEXT("failed to build memory summary"),
				TEXT("Trace-backed memory metrics are unavailable for this trace."));
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeMemorySummaryObject(Samples), Meta));
		return true;
	}

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("peak"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		TArray<FMemorySample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemorySamplesTrace(Context, Samples, FailureStage, FailureReason, TimeWindow.StartMs, TimeWindow.EndMs))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("memory.peak"),
				FailureStage,
				FailureReason,
				TEXT("memory_extraction"),
				TEXT("failed to build memory peak"),
				TEXT("Trace-backed memory metrics are unavailable for this trace."));
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeMemoryPeakObject(Samples), Meta));
		return true;
	}

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("series"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") }, UnknownOptionError))
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

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		TArray<FMemorySample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemorySamplesTrace(Context, Samples, FailureStage, FailureReason, TimeWindow.StartMs, TimeWindow.EndMs))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("memory.series"),
				FailureStage,
				FailureReason,
				TEXT("memory_extraction"),
				TEXT("failed to build memory series"),
				TEXT("Trace-backed memory metrics are unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Samples.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FMemorySample& Sample = Samples[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("timestamp_ms"), Sample.TimestampMs);
			Item->SetNumberField(TEXT("bytes"), static_cast<double>(Sample.Bytes));
			Item->SetNumberField(TEXT("frame_index"), Sample.FrameIndex);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("tags"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") }, UnknownOptionError))
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

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		TArray<FMemoryTagSample> Tags;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemoryTagsTrace(Context, Tags, FailureStage, FailureReason, TimeWindow.StartMs, TimeWindow.EndMs))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("memory.tags"),
				FailureStage,
				FailureReason,
				TEXT("memory_tags"),
				TEXT("failed to build memory tags"),
				TEXT("Trace-backed memory metrics are unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Tags.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeMemoryTagObject(Tags[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
