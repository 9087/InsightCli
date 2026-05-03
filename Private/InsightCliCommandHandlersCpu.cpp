// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleCpuCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles cpu/top and cpu/stack subcommands; returns false when request is outside cpu group.
	if (Request.Group == TEXT("cpu") && Request.Action == TEXT("top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("thread") }, UnknownOptionError))
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

		FString ThreadFilter;
		const bool bHasThreadFilter = TryGetStringOption(Request.Args, TEXT("--thread"), ThreadFilter);
		TOptional<uint32> CpuThreadId;
		FString NormalizedThread;
		if (bHasThreadFilter)
		{
			if (!ThreadFilter.Equals(TEXT("GameThread"), ESearchCase::IgnoreCase)
				&& !ThreadFilter.Equals(TEXT("RenderThread"), ESearchCase::IgnoreCase)
				&& !ThreadFilter.Equals(TEXT("RHIThread"), ESearchCase::IgnoreCase)
				&& ThreadFilter != TEXT("42")
				&& ThreadFilter != TEXT("43")
				&& ThreadFilter != TEXT("44")
				&& !ThreadFilter.IsNumeric())
			{
				TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("unsupported_filter"), TEXT("thread"), ThreadFilter);
				Meta.Add(TEXT("limit"), FString::FromInt(Limit));
				OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
				return true;
			}

			uint32 ResolvedThreadId = 0;
			FString FailureStage;
			FString FailureReason;
			if (!ResolveCpuThreadFilterToTraceId(Context, ThreadFilter, ResolvedThreadId, NormalizedThread, FailureStage, FailureReason))
			{
				TMap<FString, FString> Details;
				Details.Add(TEXT("trace_path"), Context.FullPath);
				Details.Add(TEXT("consumer"), TEXT("cpu.top"));
				Details.Add(TEXT("thread_filter"), ThreadFilter);
				Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("thread_filter") : FailureStage);
				Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to resolve cpu thread filter") : FailureReason);
				Details.Add(TEXT("data_source"), TEXT("unavailable"));
				OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed CPU timing is unavailable for this trace."), Details);
				return true;
			}

			CpuThreadId = ResolvedThreadId;
		}

		TArray<FCpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, CpuThreadId, Samples, FailureStage, FailureReason))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("trace_path"), Context.FullPath);
			Details.Add(TEXT("consumer"), TEXT("cpu.top"));
			Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("aggregation") : FailureStage);
			Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to build cpu aggregation") : FailureReason);
			Details.Add(TEXT("data_source"), TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed CPU timing is unavailable for this trace."), Details);
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Samples.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeCpuTopObject(Samples[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		if (bHasThreadFilter)
		{
			Meta.Add(TEXT("thread"), NormalizedThread.IsEmpty() ? ThreadFilter : NormalizedThread);
		}
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("cpu") && Request.Action == TEXT("stack"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("thread"), TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 FrameIndex = -1;
		if (!TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("--frame-index is required for cpu stack."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("frame-index must be >= 0."));
			return true;
		}

		int32 Limit = 100;
		const bool bHasLimit = TryGetIntOption(Request.Args, TEXT("--limit"), Limit);
		if (bHasLimit && Limit <= 0)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("limit must be > 0."));
			return true;
		}

		FString ThreadFilter;
		const bool bHasThreadFilter = TryGetStringOption(Request.Args, TEXT("--thread"), ThreadFilter);
		TOptional<uint32> CpuThreadId;
		if (bHasThreadFilter)
		{
			uint32 ResolvedThreadId = 0;
			FString NormalizedThread;
			FString ThreadFailureStage;
			FString ThreadFailureReason;
			if (!ResolveCpuThreadFilterToTraceId(Context, ThreadFilter, ResolvedThreadId, NormalizedThread, ThreadFailureStage, ThreadFailureReason))
			{
				TMap<FString, FString> Details;
				Details.Add(TEXT("trace_path"), Context.FullPath);
				Details.Add(TEXT("consumer"), TEXT("cpu.stack"));
				Details.Add(TEXT("thread_filter"), ThreadFilter);
				Details.Add(TEXT("failure_stage"), ThreadFailureStage.IsEmpty() ? TEXT("thread_filter") : ThreadFailureStage);
				Details.Add(TEXT("failure_reason"), ThreadFailureReason.IsEmpty() ? TEXT("failed to resolve cpu thread filter") : ThreadFailureReason);
				Details.Add(TEXT("data_source"), TEXT("unavailable"));
				OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed CPU stack is unavailable for this trace."), Details);
				return true;
			}

			CpuThreadId = ResolvedThreadId;
		}

		TSharedPtr<FJsonObject> StackObject;
		bool bFound = false;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuStackObject(Context, FrameIndex, CpuThreadId, Limit, StackObject, bFound, FailureStage, FailureReason))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("trace_path"), Context.FullPath);
			Details.Add(TEXT("consumer"), TEXT("cpu.stack"));
			Details.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
			if (bHasThreadFilter)
			{
				Details.Add(TEXT("thread_filter"), ThreadFilter);
			}
			Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? TEXT("stack_extraction") : FailureStage);
			Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? TEXT("failed to build cpu stack") : FailureReason);
			Details.Add(TEXT("data_source"), TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Error(10, TEXT("E3001"), TEXT("Trace-backed CPU stack is unavailable for this trace."), Details);
			return true;
		}

		if (!bFound || !StackObject.IsValid())
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Add(MakeShared<FJsonValueObject>(StackObject.ToSharedRef()));
		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (bHasThreadFilter)
		{
			Meta.Add(TEXT("thread"), ThreadFilter);
		}
		if (bHasLimit)
		{
			Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
