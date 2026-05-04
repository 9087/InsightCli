// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
enum class ESlateMetric
{
	Paint,
	Tick,
	Invalidation,
};

bool MatchesSlateMetric(const FString& ScopeName, ESlateMetric Metric)
{
	const FString Lower = ScopeName.ToLower();
	const bool bIsSlateScope =
		Lower.Contains(TEXT("slate"))
		|| Lower.Contains(TEXT("umg"))
		|| Lower.Contains(TEXT("widget"));

	if (!bIsSlateScope)
	{
		return false;
	}

	switch (Metric)
	{
	case ESlateMetric::Paint:
		return Lower.Contains(TEXT("paint")) || Lower.Contains(TEXT("draw"));
	case ESlateMetric::Tick:
		return Lower.Contains(TEXT("tick")) || Lower.Contains(TEXT("update"));
	case ESlateMetric::Invalidation:
		return Lower.Contains(TEXT("invalid"));
	default:
		return false;
	}
}

bool TryParseSlateMetric(const FString& InBy, ESlateMetric& OutMetric)
{
	if (InBy.Equals(TEXT("paint"), ESearchCase::IgnoreCase))
	{
		OutMetric = ESlateMetric::Paint;
		return true;
	}
	if (InBy.Equals(TEXT("tick"), ESearchCase::IgnoreCase))
	{
		OutMetric = ESlateMetric::Tick;
		return true;
	}
	if (InBy.Equals(TEXT("invalidation"), ESearchCase::IgnoreCase))
	{
		OutMetric = ESlateMetric::Invalidation;
		return true;
	}

	return false;
}

bool BuildSlateScopeRows(
	const FTraceContext& Context,
	ESlateMetric Metric,
	TArray<FCpuScopeSample>& OutRows,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutRows.Reset();

	TArray<FCpuScopeSample> AllCpuScopes;
	if (!BuildCpuTopSamples(Context, {}, AllCpuScopes, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	for (const FCpuScopeSample& Scope : AllCpuScopes)
	{
		if (MatchesSlateMetric(Scope.ScopeName, Metric))
		{
			OutRows.Add(Scope);
		}
	}

	OutRows.Sort([](const FCpuScopeSample& A, const FCpuScopeSample& B)
	{
		if (A.TotalMs == B.TotalMs)
		{
			return A.ScopeName < B.ScopeName;
		}
		return A.TotalMs > B.TotalMs;
	});

	return true;
}

double ResolveWindowDurationMs(const FTraceContext& Context, const FResolvedTimeWindowMs& TimeWindow)
{
	if (TimeWindow.StartMs.IsSet() && TimeWindow.EndMs.IsSet())
	{
		return FMath::Max(0.0, TimeWindow.EndMs.GetValue() - TimeWindow.StartMs.GetValue());
	}

	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	if (Frames.IsEmpty())
	{
		return 0.0;
	}

	const double StartMs = Frames[0].FrameStartMs;
	const double EndMs = Frames.Last().FrameEndMs;
	return FMath::Max(0.0, EndMs - StartMs);
}
}

bool HandleSlateCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("slate") && Request.Action == TEXT("top-widgets"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("by"), TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 20;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 20, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		FString By = TEXT("paint");
		if (TryGetStringOption(Request.Args, TEXT("--by"), By))
		{
			By = By.ToLower();
		}

		ESlateMetric Metric = ESlateMetric::Paint;
		if (!TryParseSlateMetric(By, Metric))
		{
			OutResponse = MakeOptionError(TEXT("--by must be one of: paint, tick, invalidation."));
			return true;
		}

		TArray<FCpuScopeSample> Rows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildSlateScopeRows(Context, Metric, Rows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("slate.top-widgets"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build slate approximation"),
				TEXT("Trace-backed Slate data is unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FCpuScopeSample& Row = Rows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("widget"), Row.ScopeName);
			Item->SetNumberField(TEXT("call_count"), Row.CallCount);
			Item->SetNumberField(TEXT("total_ms"), Row.TotalMs);
			Item->SetNumberField(TEXT("avg_ms"), Row.AvgMs);
			Item->SetNumberField(TEXT("max_ms"), Row.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("by"), By);
		Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern"));
		if (Rows.IsEmpty())
		{
			Meta.Add(TEXT("warning"), TEXT("Slate channel unavailable or no matching scope samples."));
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("slate") && Request.Action == TEXT("paint-cost"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 FrameIndex = -1;
		if (!TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			OutResponse = MakeOptionError(TEXT("--frame-index is required for slate paint-cost."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("slate.paint-cost")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		const FFrameSample* Found = Frames.FindByPredicate([FrameIndex](const FFrameSample& Sample)
		{
			return Sample.FrameIndex == FrameIndex;
		});

		if (Found == nullptr)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern"));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		TArray<FCpuScopeSample> Rows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildSlateScopeRows(Context, ESlateMetric::Paint, Rows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("slate.paint-cost"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build slate paint approximation"),
				TEXT("Trace-backed Slate paint cost is unavailable for this trace."));
			return true;
		}

		double PaintMs = 0.0;
		for (const FCpuScopeSample& Row : Rows)
		{
			PaintMs += Row.TotalMs;
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("frame_index"), FrameIndex);
		Data->SetNumberField(TEXT("paint_ms"), PaintMs);
		Data->SetNumberField(TEXT("approx_widget_count"), Rows.Num());

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
		Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern"));
		Meta.Add(TEXT("warning"), TEXT("Approximation from CPU scope pattern; frame-local Slate channel data unavailable."));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("slate") && Request.Action == TEXT("invalidation-rate"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, false, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		TArray<FCpuScopeSample> Rows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildSlateScopeRows(Context, ESlateMetric::Invalidation, Rows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("slate.invalidation-rate"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build slate invalidation approximation"),
				TEXT("Trace-backed Slate invalidation data is unavailable for this trace."));
			return true;
		}

		int64 InvalidationEvents = 0;
		for (const FCpuScopeSample& Row : Rows)
		{
			InvalidationEvents += FMath::Max(0, Row.CallCount);
		}

		const double WindowMs = ResolveWindowDurationMs(Context, TimeWindow);
		const double InvalidationsPerSec = (WindowMs > 0.0) ? (static_cast<double>(InvalidationEvents) * 1000.0 / WindowMs) : 0.0;

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("invalidation_events"), static_cast<double>(InvalidationEvents));
		Data->SetNumberField(TEXT("window_ms"), WindowMs);
		Data->SetNumberField(TEXT("invalidations_per_sec"), InvalidationsPerSec);

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern"));
		Meta.Add(TEXT("warning"), TEXT("Slate invalidation-rate is approximated by CPU scope pattern because Slate trace channel may be unavailable."));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	return false;
}
}