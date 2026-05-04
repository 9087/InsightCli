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
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
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
				OutResponse = MakeTraceUnavailableError(
					Context,
					TEXT("cpu.top"),
					FailureStage,
					FailureReason,
					TEXT("thread_filter"),
					TEXT("failed to resolve cpu thread filter"),
					TEXT("Trace-backed CPU timing is unavailable for this trace."),
					{{TEXT("thread_filter"), ThreadFilter}});
				return true;
			}

			CpuThreadId = ResolvedThreadId;
		}

		TArray<FCpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, CpuThreadId, Samples, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("cpu.top"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build cpu aggregation"),
				TEXT("Trace-backed CPU timing is unavailable for this trace."));
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
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("thread"), TEXT("limit"), TEXT("view") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 FrameIndex = -1;
		if (!TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			OutResponse = MakeOptionError(TEXT("--frame-index is required for cpu stack."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		FString View = TEXT("top-down");
		if (TryGetStringOption(Request.Args, TEXT("--view"), View))
		{
			if (!View.Equals(TEXT("top-down"), ESearchCase::IgnoreCase)
				&& !View.Equals(TEXT("bottom-up"), ESearchCase::IgnoreCase)
				&& !View.Equals(TEXT("leaf"), ESearchCase::IgnoreCase))
			{
				OutResponse = MakeOptionError(TEXT("view must be one of: top-down, bottom-up, leaf."));
				return true;
			}
			View = View.ToLower();
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
				OutResponse = MakeTraceUnavailableError(
					Context,
					TEXT("cpu.stack"),
					ThreadFailureStage,
					ThreadFailureReason,
					TEXT("thread_filter"),
					TEXT("failed to resolve cpu thread filter"),
					TEXT("Trace-backed CPU stack is unavailable for this trace."),
					{{TEXT("thread_filter"), ThreadFilter}});
				return true;
			}

			CpuThreadId = ResolvedThreadId;
		}

		TSharedPtr<FJsonObject> StackObject;
		bool bFound = false;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuStackObject(Context, FrameIndex, CpuThreadId, Limit, View, StackObject, bFound, FailureStage, FailureReason))
		{
			TMap<FString, FString> ExtraDetails;
			ExtraDetails.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
			if (bHasThreadFilter)
			{
				ExtraDetails.Add(TEXT("thread_filter"), ThreadFilter);
			}
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("cpu.stack"),
				FailureStage,
				FailureReason,
				TEXT("stack_extraction"),
				TEXT("failed to build cpu stack"),
				TEXT("Trace-backed CPU stack is unavailable for this trace."),
				ExtraDetails);
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
		if (HasOption(Request.Args, TEXT("--limit")))
		{
			Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		}
		Meta.Add(TEXT("view"), View);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("cpu") && Request.Action == TEXT("hot-functions"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("thread") }, UnknownOptionError))
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

		FString ThreadFilter;
		const bool bHasThreadFilter = TryGetStringOption(Request.Args, TEXT("--thread"), ThreadFilter);
		TOptional<uint32> CpuThreadId;
		FString NormalizedThread;
		if (bHasThreadFilter)
		{
			if (!ThreadFilter.Equals(TEXT("GameThread"), ESearchCase::IgnoreCase)
				&& !ThreadFilter.Equals(TEXT("RenderThread"), ESearchCase::IgnoreCase)
				&& !ThreadFilter.Equals(TEXT("RHIThread"), ESearchCase::IgnoreCase)
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
				OutResponse = MakeTraceUnavailableError(
					Context,
					TEXT("cpu.hot-functions"),
					FailureStage,
					FailureReason,
					TEXT("thread_filter"),
					TEXT("failed to resolve cpu thread filter"),
					TEXT("Trace-backed CPU function hot list is unavailable for this trace."),
					{{TEXT("thread_filter"), ThreadFilter}});
				return true;
			}

			CpuThreadId = ResolvedThreadId;
		}

		TArray<FCpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, CpuThreadId, Samples, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("cpu.hot-functions"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build cpu hot functions"),
				TEXT("Trace-backed CPU function hot list is unavailable for this trace."));
			return true;
		}

		Samples.Sort([](const FCpuScopeSample& A, const FCpuScopeSample& B)
		{
			if (A.SelfMs == B.SelfMs)
			{
				return A.ScopeName < B.ScopeName;
			}
			return A.SelfMs > B.SelfMs;
		});

		const int32 TakeCount = FMath::Min(Limit, Samples.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeCpuTopObject(Samples[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("sort_by"), TEXT("self_ms_desc"));
		if (bHasThreadFilter)
		{
			Meta.Add(TEXT("thread"), NormalizedThread.IsEmpty() ? ThreadFilter : NormalizedThread);
		}
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
