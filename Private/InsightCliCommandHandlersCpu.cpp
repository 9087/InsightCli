// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
FString ClassifyCpuStatGroup(const FString& ScopeName)
{
	if (ScopeName.Contains(TEXT("World Tick"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("UWorld::Tick"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("Tick"), ESearchCase::IgnoreCase))
	{
		return TEXT("world_tick");
	}
	if (ScopeName.Contains(TEXT("Anim"), ESearchCase::IgnoreCase))
	{
		return TEXT("animation");
	}
	if (ScopeName.Contains(TEXT("Physics"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("Chaos"), ESearchCase::IgnoreCase))
	{
		return TEXT("physics");
	}
	if (ScopeName.Contains(TEXT("Garbage"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("CollectGarbage"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("GC"), ESearchCase::IgnoreCase))
	{
		return TEXT("gc");
	}
	if (ScopeName.Contains(TEXT("Slate"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("UI"), ESearchCase::IgnoreCase))
	{
		return TEXT("slate");
	}

	return TEXT("other");
}

bool TryResolveFrameIntervalSec(const FTraceContext& Context, int32 FrameIndex, double& OutStartSec, double& OutEndSec)
{
	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	for (const FFrameSample& Frame : Frames)
	{
		if (Frame.FrameIndex == FrameIndex)
		{
			OutStartSec = FMath::Max(0.0, Frame.FrameStartMs / 1000.0);
			OutEndSec = FMath::Max(OutStartSec + KINDA_SMALL_NUMBER, Frame.FrameEndMs / 1000.0);
			return true;
		}
	}

	return false;
}
}

bool HandleCpuCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles cpu/top and cpu/stack subcommands; returns false when request is outside cpu group.
	if (Request.Group == TEXT("cpu") && Request.Action == TEXT("stat-groups"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TArray<FCpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, {}, Samples, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("cpu.stat-groups"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build cpu stat-group aggregation"),
				TEXT("Trace-backed CPU timing is unavailable for this trace."));
			return true;
		}

		struct FStatGroupAggregate
		{
			double TotalSelfMs = 0.0;
			TSet<FString> UniqueScopes;
		};

		TMap<FString, FStatGroupAggregate> Groups;
		for (const FCpuScopeSample& Sample : Samples)
		{
			const FString GroupName = ClassifyCpuStatGroup(Sample.ScopeName);
			FStatGroupAggregate& Aggregate = Groups.FindOrAdd(GroupName);
			Aggregate.TotalSelfMs += FMath::Max(0.0, Sample.SelfMs);
			Aggregate.UniqueScopes.Add(Sample.ScopeName);
		}

		struct FRow
		{
			FString Name;
			int32 ScopeCount = 0;
			double TotalSelfMs = 0.0;
		};

		TArray<FRow> Rows;
		Rows.Reserve(Groups.Num());
		for (const TPair<FString, FStatGroupAggregate>& Pair : Groups)
		{
			FRow Row;
			Row.Name = Pair.Key;
			Row.ScopeCount = Pair.Value.UniqueScopes.Num();
			Row.TotalSelfMs = Pair.Value.TotalSelfMs;
			Rows.Add(MoveTemp(Row));
		}

		Rows.Sort([](const FRow& A, const FRow& B)
		{
			if (A.TotalSelfMs == B.TotalSelfMs)
			{
				return A.Name < B.Name;
			}
			return A.TotalSelfMs > B.TotalSelfMs;
		});

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Rows.Num());
		for (const FRow& Row : Rows)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Row.Name);
			Item->SetNumberField(TEXT("scope_count"), Row.ScopeCount);
			Item->SetNumberField(TEXT("total_self_ms"), Row.TotalSelfMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		Meta.Add(TEXT("group_count"), FString::FromInt(Rows.Num()));
		AddMetaWarning(
			Meta,
			TEXT("Stat groups are inferred from scope-name heuristics."),
			TEXT("W2101"),
			TEXT("Use frames detail --breakdown statgroup for per-frame context."));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("cpu") && Request.Action == TEXT("top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("thread"), TEXT("stat-group"), TEXT("frame-index") }, UnknownOptionError))
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

		FString StatGroupFilter;
		const bool bHasStatGroupFilter = TryGetStringOption(Request.Args, TEXT("--stat-group"), StatGroupFilter);
		if (bHasStatGroupFilter)
		{
			StatGroupFilter = StatGroupFilter.ToLower();
		}

		int32 FrameIndexFilter = -1;
		const bool bHasFrameIndexFilter = TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndexFilter);
		if (bHasFrameIndexFilter && FrameIndexFilter < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}

		TOptional<double> IntervalStartSec;
		TOptional<double> IntervalEndSec;
		if (bHasFrameIndexFilter)
		{
			double StartSec = 0.0;
			double EndSec = 0.0;
			if (!TryResolveFrameIntervalSec(Context, FrameIndexFilter, StartSec, EndSec))
			{
				TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndexFilter));
				OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
				return true;
			}

			IntervalStartSec = StartSec;
			IntervalEndSec = EndSec;
		}

		TArray<FCpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, CpuThreadId, Samples, FailureStage, FailureReason, IntervalStartSec, IntervalEndSec))
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

		if (bHasStatGroupFilter)
		{
			Samples = Samples.FilterByPredicate([&StatGroupFilter](const FCpuScopeSample& Sample)
			{
				return ClassifyCpuStatGroup(Sample.ScopeName) == StatGroupFilter;
			});
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
		if (bHasStatGroupFilter)
		{
			Meta.Add(TEXT("stat_group"), StatGroupFilter);
			AddMetaWarning(
				Meta,
				TEXT("stat-group filter uses scope-name heuristics."),
				TEXT("W2101"),
				TEXT("Names may differ from engine stat declarations."));
		}
		if (bHasFrameIndexFilter)
		{
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
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
