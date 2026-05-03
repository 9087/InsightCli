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
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		double Start = 0.0;
		double End = 0.0;
		const bool bHasStart = TryGetDoubleOption(Request.Args, TEXT("--time-start"), Start);
		const bool bHasEnd = TryGetDoubleOption(Request.Args, TEXT("--time-end"), End);
		if (bHasStart && bHasEnd && Start > End)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
			return true;
		}

		TArray<FMemorySample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemorySamplesTrace(Context, Samples, FailureStage, FailureReason, bHasStart ? TOptional<double>(Start) : TOptional<double>(), bHasEnd ? TOptional<double>(End) : TOptional<double>()))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("trace_path"), Context.FullPath);
			Details.Add(TEXT("consumer"), TEXT("memory.summary"));
			Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("memory_extraction") : FailureStage);
			Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to build memory summary") : FailureReason);
			Details.Add(TEXT("data_source"), TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed memory metrics are unavailable for this trace."), Details);
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (bHasStart)
		{
			Meta.Add(TEXT("time_start_ms"), ToNumberString(Start));
		}
		if (bHasEnd)
		{
			Meta.Add(TEXT("time_end_ms"), ToNumberString(End));
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeMemorySummaryObject(Samples), Meta));
		return true;
	}

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("peak"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		double Start = 0.0;
		double End = 0.0;
		const bool bHasStart = TryGetDoubleOption(Request.Args, TEXT("--time-start"), Start);
		const bool bHasEnd = TryGetDoubleOption(Request.Args, TEXT("--time-end"), End);
		if (bHasStart && bHasEnd && Start > End)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
			return true;
		}

		TArray<FMemorySample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemorySamplesTrace(Context, Samples, FailureStage, FailureReason, bHasStart ? TOptional<double>(Start) : TOptional<double>(), bHasEnd ? TOptional<double>(End) : TOptional<double>()))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("trace_path"), Context.FullPath);
			Details.Add(TEXT("consumer"), TEXT("memory.peak"));
			Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("memory_extraction") : FailureStage);
			Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to build memory peak") : FailureReason);
			Details.Add(TEXT("data_source"), TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed memory metrics are unavailable for this trace."), Details);
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (bHasStart)
		{
			Meta.Add(TEXT("time_start_ms"), ToNumberString(Start));
		}
		if (bHasEnd)
		{
			Meta.Add(TEXT("time_end_ms"), ToNumberString(End));
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeMemoryPeakObject(Samples), Meta));
		return true;
	}

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("series"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
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

		double Start = 0.0;
		double End = 0.0;
		const bool bHasStart = TryGetDoubleOption(Request.Args, TEXT("--time-start"), Start);
		const bool bHasEnd = TryGetDoubleOption(Request.Args, TEXT("--time-end"), End);
		if (bHasStart && bHasEnd && Start > End)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
			return true;
		}

		TArray<FMemorySample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemorySamplesTrace(Context, Samples, FailureStage, FailureReason, bHasStart ? TOptional<double>(Start) : TOptional<double>(), bHasEnd ? TOptional<double>(End) : TOptional<double>()))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("trace_path"), Context.FullPath);
			Details.Add(TEXT("consumer"), TEXT("memory.series"));
			Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("memory_extraction") : FailureStage);
			Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to build memory series") : FailureReason);
			Details.Add(TEXT("data_source"), TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed memory metrics are unavailable for this trace."), Details);
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
		if (bHasStart)
		{
			Meta.Add(TEXT("time_start_ms"), ToNumberString(Start));
		}
		if (bHasEnd)
		{
			Meta.Add(TEXT("time_end_ms"), ToNumberString(End));
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("tags"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
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

		double Start = 0.0;
		double End = 0.0;
		const bool bHasStart = TryGetDoubleOption(Request.Args, TEXT("--time-start"), Start);
		const bool bHasEnd = TryGetDoubleOption(Request.Args, TEXT("--time-end"), End);
		if (bHasStart && bHasEnd && Start > End)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
			return true;
		}

		TArray<FMemoryTagSample> Tags;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMemoryTagsTrace(Context, Tags, FailureStage, FailureReason, bHasStart ? TOptional<double>(Start) : TOptional<double>(), bHasEnd ? TOptional<double>(End) : TOptional<double>()))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("trace_path"), Context.FullPath);
			Details.Add(TEXT("consumer"), TEXT("memory.tags"));
			Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("memory_tags") : FailureStage);
			Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to build memory tags") : FailureReason);
			Details.Add(TEXT("data_source"), TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed memory metrics are unavailable for this trace."), Details);
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
		if (bHasStart)
		{
			Meta.Add(TEXT("time_start_ms"), ToNumberString(Start));
		}
		if (bHasEnd)
		{
			Meta.Add(TEXT("time_end_ms"), ToNumberString(End));
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
