// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Memory.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FTagWindowStat
{
	int64 FirstValue = 0;
	int64 LastValue = 0;
	int32 SampleCount = 0;
	bool bHasSample = false;
};

bool TryBuildTagWindowStats(
	const FTraceContext& Context,
	TOptional<double> StartSecOverride,
	TOptional<double> EndSecOverride,
	TMap<FString, FTagWindowStat>& OutStats,
	double& OutDurationSec,
	FString& OutWarning,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutStats.Reset();
	OutDurationSec = 0.0;
	OutWarning.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
	OutDurationSec = Session->GetDurationSeconds();

	const TraceServices::IMemoryProvider* MemoryProvider = TraceServices::ReadMemoryProvider(*Session.Get());
	if (MemoryProvider == nullptr || MemoryProvider->GetTrackerCount() == 0)
	{
		OutWarning = TEXT("MemAlloc/Memory tag channel unavailable in trace.");
		return true;
	}

	TArray<TraceServices::FMemoryTrackerInfo> Trackers;
	MemoryProvider->EnumerateTrackers([&Trackers](const TraceServices::FMemoryTrackerInfo& Tracker)
	{
		Trackers.Add(Tracker);
	});
	if (Trackers.IsEmpty())
	{
		OutWarning = TEXT("Memory trackers unavailable in trace.");
		return true;
	}

	const TraceServices::FMemoryTrackerId TrackerId = Trackers[0].Id;
	const double StartSec = StartSecOverride.IsSet() ? FMath::Max(0.0, StartSecOverride.GetValue()) : 0.0;
	const double EndSecRaw = EndSecOverride.IsSet() ? FMath::Max(StartSec, EndSecOverride.GetValue()) : OutDurationSec;
	const double EndSec = FMath::Max(StartSec + KINDA_SMALL_NUMBER, EndSecRaw);

	MemoryProvider->EnumerateTags([&](const TraceServices::FMemoryTagInfo& TagInfo)
	{
		if (TagInfo.Name.IsEmpty())
		{
			return;
		}

		if ((TagInfo.Trackers & (1ull << static_cast<uint64>(TrackerId))) == 0)
		{
			return;
		}

		FTagWindowStat Stat;
		MemoryProvider->EnumerateTagSamples(TrackerId, TagInfo.Id, StartSec, EndSec, true,
			[&Stat](double, double, const TraceServices::FMemoryTagSample& Sample)
			{
				if (!Stat.bHasSample)
				{
					Stat.FirstValue = Sample.Value;
					Stat.bHasSample = true;
				}
				Stat.LastValue = Sample.Value;
				++Stat.SampleCount;
			});

		if (Stat.bHasSample)
		{
			OutStats.Add(TagInfo.Name, Stat);
		}
	});

	if (OutStats.IsEmpty())
	{
		OutWarning = TEXT("No memory tag samples found in selected window.");
	}

	return true;
}
}

bool HandleMemoryCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles memory/summary, memory/peak, memory/series, memory/tags, memory/alloc-top, and memory/leak-suspect.
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

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("alloc-top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("by") }, UnknownOptionError))
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

		FString By = TEXT("tag");
		if (TryGetStringOption(Request.Args, TEXT("--by"), By))
		{
			By = By.ToLower();
			if (By != TEXT("tag") && By != TEXT("callstack"))
			{
				TMap<FString, FString> Details;
				Details.Add(TEXT("by"), By);
				OutResponse = MakeOptionError(TEXT("by must be tag or callstack."), Details);
				return true;
			}
		}

		if (By == TEXT("callstack"))
		{
			TMap<FString, FString> Meta;
			Meta.Add(TEXT("limit"), FString::FromInt(Limit));
			Meta.Add(TEXT("by"), TEXT("callstack"));
			Meta.Add(TEXT("data_source"), TEXT("unavailable"));
			Meta.Add(TEXT("warning"), TEXT("callstack mode requires MemAlloc metadata not exposed by current implementation."));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		TMap<FString, FTagWindowStat> TagStats;
		double DurationSec = 0.0;
		FString Warning;
		FString FailureStage;
		FString FailureReason;
		if (!TryBuildTagWindowStats(Context, {}, {}, TagStats, DurationSec, Warning, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("memory.alloc-top"),
				FailureStage,
				FailureReason,
				TEXT("memory_alloc_top"),
				TEXT("failed to build alloc-top tag stats"),
				TEXT("Trace-backed memory allocation diagnostics are unavailable for this trace."));
			return true;
		}

		TArray<TPair<FString, FTagWindowStat>> Rows;
		Rows.Reserve(TagStats.Num());
		for (const TPair<FString, FTagWindowStat>& Pair : TagStats)
		{
			if (Pair.Value.LastValue > 0)
			{
				Rows.Add(Pair);
			}
		}

		Rows.Sort([](const TPair<FString, FTagWindowStat>& A, const TPair<FString, FTagWindowStat>& B)
		{
			if (A.Value.LastValue == B.Value.LastValue)
			{
				return A.Key < B.Key;
			}
			return A.Value.LastValue > B.Value.LastValue;
		});

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("tag_name"), Rows[Index].Key);
			Item->SetNumberField(TEXT("bytes"), static_cast<double>(Rows[Index].Value.LastValue));
			Item->SetNumberField(TEXT("sample_count"), Rows[Index].Value.SampleCount);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("by"), TEXT("tag"));
		Meta.Add(TEXT("data_source"), Warning.IsEmpty() ? TEXT("trace") : TEXT("unavailable"));
		if (!Warning.IsEmpty())
		{
			Meta.Add(TEXT("warning"), Warning);
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("memory") && Request.Action == TEXT("leak-suspect"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("window"), TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		double WindowSec = 0.0;
		if (!TryGetDoubleOption(Request.Args, TEXT("--window"), WindowSec))
		{
			OutResponse = MakeOptionError(TEXT("--window is required for memory leak-suspect."));
			return true;
		}
		if (WindowSec <= 0.0)
		{
			OutResponse = MakeOptionError(TEXT("window must be > 0."));
			return true;
		}

		int32 Limit = 10;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 10, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		TMap<FString, FTagWindowStat> FullStats;
		double DurationSec = 0.0;
		FString Warning;
		FString FailureStage;
		FString FailureReason;
		if (!TryBuildTagWindowStats(Context, {}, {}, FullStats, DurationSec, Warning, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("memory.leak-suspect"),
				FailureStage,
				FailureReason,
				TEXT("memory_leak_suspect"),
				TEXT("failed to build leak-suspect tag stats"),
				TEXT("Trace-backed memory leak diagnostics are unavailable for this trace."));
			return true;
		}

		const double WindowStartSec = FMath::Max(0.0, DurationSec - WindowSec);
		TMap<FString, FTagWindowStat> WindowStats;
		double IgnoredDurationSec = 0.0;
		FString WindowWarning;
		if (!TryBuildTagWindowStats(Context, WindowStartSec, DurationSec, WindowStats, IgnoredDurationSec, WindowWarning, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("memory.leak-suspect"),
				FailureStage,
				FailureReason,
				TEXT("memory_leak_suspect"),
				TEXT("failed to build leak-suspect window stats"),
				TEXT("Trace-backed memory leak diagnostics are unavailable for this trace."));
			return true;
		}

		struct FLeakRow
		{
			FString TagName;
			int64 GrowthBytes = 0;
			int64 StartBytes = 0;
			int64 EndBytes = 0;
			int32 SampleCount = 0;
		};

		TArray<FLeakRow> LeakRows;
		for (const TPair<FString, FTagWindowStat>& Pair : WindowStats)
		{
			const int64 Growth = Pair.Value.LastValue - Pair.Value.FirstValue;
			if (Growth <= 0)
			{
				continue;
			}

			FLeakRow Row;
			Row.TagName = Pair.Key;
			Row.GrowthBytes = Growth;
			Row.StartBytes = Pair.Value.FirstValue;
			Row.EndBytes = Pair.Value.LastValue;
			Row.SampleCount = Pair.Value.SampleCount;
			LeakRows.Add(MoveTemp(Row));
		}

		LeakRows.Sort([](const FLeakRow& A, const FLeakRow& B)
		{
			if (A.GrowthBytes == B.GrowthBytes)
			{
				return A.TagName < B.TagName;
			}
			return A.GrowthBytes > B.GrowthBytes;
		});

		const int32 TakeCount = FMath::Min(Limit, LeakRows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FLeakRow& Row = LeakRows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("tag_name"), Row.TagName);
			Item->SetNumberField(TEXT("growth_bytes"), static_cast<double>(Row.GrowthBytes));
			Item->SetNumberField(TEXT("sample_count"), Row.SampleCount);
			Item->SetNumberField(TEXT("start_bytes"), static_cast<double>(Row.StartBytes));
			Item->SetNumberField(TEXT("end_bytes"), static_cast<double>(Row.EndBytes));
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("window_sec"), ToNumberString(WindowSec));
		Meta.Add(TEXT("data_source"), Warning.IsEmpty() ? TEXT("trace") : TEXT("unavailable"));
		if (!Warning.IsEmpty())
		{
			Meta.Add(TEXT("warning"), Warning);
		}
		else if (!WindowWarning.IsEmpty())
		{
			Meta.Add(TEXT("warning"), WindowWarning);
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
