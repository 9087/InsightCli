// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Common/ProviderLock.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Regions.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FMarkRegionRow
{
	FString Name;
	double StartMs = 0.0;
	double EndMs = 0.0;
	bool bIncomplete = false;
};

bool BuildMarkRegions(
	const FTraceContext& Context,
	TArray<FMarkRegionRow>& OutRegions,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutRegions.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IRegionProvider& RegionProvider = TraceServices::ReadRegionProvider(*Session.Get());
		TraceServices::FProviderReadScopeLock RegionProviderScopedLock(RegionProvider);
		const double DurationSec = Session->GetDurationSeconds();
		RegionProvider.EnumerateRegions(0.0, DurationSec, [&OutRegions](const TraceServices::FTimeRegion& Region)
		{
			FMarkRegionRow Row;
			Row.Name = Region.Text != nullptr ? Region.Text : TEXT("");
			if (Row.Name.IsEmpty())
			{
				Row.Name = TEXT("<unnamed>");
			}

			Row.StartMs = Region.BeginTime * 1000.0;
			if (FMath::IsFinite(Region.EndTime))
			{
				Row.EndMs = Region.EndTime * 1000.0;
			}
			else
			{
				Row.EndMs = Row.StartMs;
				Row.bIncomplete = true;
			}

			OutRegions.Add(MoveTemp(Row));
			return true;
		});
	}

	OutRegions.Sort([](const FMarkRegionRow& A, const FMarkRegionRow& B)
	{
		if (A.StartMs == B.StartMs)
		{
			return A.Name < B.Name;
		}
		return A.StartMs < B.StartMs;
	});

	return true;
}
}


bool HandleMarksCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles marks/search and marks/around with strict argument validation.
	if (Request.Group == TEXT("marks") && Request.Action == TEXT("regions"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("name") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString NameFilter;
		const bool bHasNameFilter = TryGetStringOption(Request.Args, TEXT("--name"), NameFilter);

		TArray<FMarkRegionRow> Regions;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMarkRegions(Context, Regions, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("marks.regions"),
				FailureStage,
				FailureReason,
				TEXT("regions"),
				TEXT("failed to build region list"),
				TEXT("Trace-backed mark regions are unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		bool bHasIncomplete = false;
		for (const FMarkRegionRow& Region : Regions)
		{
			if (bHasNameFilter && !Region.Name.Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Region.Name);
			Item->SetNumberField(TEXT("start_ms"), Region.StartMs);
			Item->SetNumberField(TEXT("end_ms"), Region.EndMs);
			Item->SetNumberField(TEXT("duration_ms"), FMath::Max(0.0, Region.EndMs - Region.StartMs));
			Item->SetBoolField(TEXT("incomplete"), Region.bIncomplete);
			Data.Add(MakeShared<FJsonValueObject>(Item));

			if (Region.bIncomplete)
			{
				bHasIncomplete = true;
			}
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (bHasNameFilter)
		{
			Meta.Add(TEXT("filter_name"), NameFilter);
		}
		if (bHasIncomplete)
		{
			AddMetaWarning(
				Meta,
				TEXT("Some regions are incomplete (missing end event)."),
				TEXT("W2201"),
				TEXT("Check incomplete=true rows for open-ended regions."));
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("marks") && Request.Action == TEXT("region-slice"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("name") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString RegionName;
		FInsightCliResponse RequiredOptionError;
		if (!RequireStringOption(Request.Args, TEXT("--name"), TEXT("marks region-slice"), RegionName, RequiredOptionError))
		{
			OutResponse = RequiredOptionError;
			return true;
		}

		TArray<FMarkRegionRow> Regions;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMarkRegions(Context, Regions, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("marks.region-slice"),
				FailureStage,
				FailureReason,
				TEXT("regions"),
				TEXT("failed to build region list"),
				TEXT("Trace-backed mark regions are unavailable for this trace."));
			return true;
		}

		const FMarkRegionRow* FoundRegion = nullptr;
		for (const FMarkRegionRow& Region : Regions)
		{
			if (Region.Name.Equals(RegionName, ESearchCase::IgnoreCase))
			{
				FoundRegion = &Region;
				break;
			}
		}

		if (FoundRegion == nullptr)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("name"), RegionName);
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeShared<FJsonObject>(), Meta));
			return true;
		}

		const double SliceStartMs = FoundRegion->StartMs;
		const double SliceEndMs = FMath::Max(SliceStartMs, FoundRegion->EndMs);

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		double FrameTimeSumMs = 0.0;
		double FrameTimeMaxMs = 0.0;
		int32 FrameCount = 0;
		for (const FFrameSample& Frame : Frames)
		{
			if (Frame.FrameEndMs <= SliceStartMs || Frame.FrameStartMs >= SliceEndMs)
			{
				continue;
			}

			++FrameCount;
			FrameTimeSumMs += Frame.FrameTimeMs;
			FrameTimeMaxMs = FMath::Max(FrameTimeMaxMs, Frame.FrameTimeMs);
		}

		TArray<FCpuScopeSample> CpuRows;
		if (!BuildCpuTopSamples(
			Context,
			{},
			CpuRows,
			FailureStage,
			FailureReason,
			SliceStartMs / 1000.0,
			SliceEndMs / 1000.0))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("marks.region-slice"),
				FailureStage,
				FailureReason,
				TEXT("cpu_aggregation"),
				TEXT("failed to build cpu metrics for region"),
				TEXT("Trace-backed CPU metrics are unavailable for this region."));
			return true;
		}

		double CpuSelfTotalMs = 0.0;
		for (const FCpuScopeSample& Row : CpuRows)
		{
			CpuSelfTotalMs += FMath::Max(0.0, Row.SelfMs);
		}

		TArray<FMemorySample> MemoryRows;
		if (!BuildMemorySamplesTrace(Context, MemoryRows, FailureStage, FailureReason, SliceStartMs, SliceEndMs))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("marks.region-slice"),
				FailureStage,
				FailureReason,
				TEXT("memory_series"),
				TEXT("failed to build memory metrics for region"),
				TEXT("Trace-backed memory metrics are unavailable for this region."));
			return true;
		}

		int64 MemoryDeltaBytes = 0;
		if (MemoryRows.Num() >= 2)
		{
			MemoryDeltaBytes = MemoryRows.Last().Bytes - MemoryRows[0].Bytes;
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("name"), FoundRegion->Name);
		Data->SetNumberField(TEXT("start_ms"), SliceStartMs);
		Data->SetNumberField(TEXT("end_ms"), SliceEndMs);
		Data->SetNumberField(TEXT("duration_ms"), FMath::Max(0.0, SliceEndMs - SliceStartMs));
		Data->SetBoolField(TEXT("incomplete"), FoundRegion->bIncomplete);
		Data->SetNumberField(TEXT("frame_count"), FrameCount);
		Data->SetNumberField(TEXT("frame_time_avg_ms"), FrameCount > 0 ? (FrameTimeSumMs / FrameCount) : 0.0);
		Data->SetNumberField(TEXT("frame_time_max_ms"), FrameTimeMaxMs);
		Data->SetNumberField(TEXT("cpu_total_self_ms"), CpuSelfTotalMs);
		if (CpuRows.Num() > 0)
		{
			Data->SetStringField(TEXT("cpu_top_scope"), CpuRows[0].ScopeName);
			Data->SetNumberField(TEXT("cpu_top_self_ms"), CpuRows[0].SelfMs);
		}
		else
		{
			Data->SetField(TEXT("cpu_top_scope"), MakeShared<FJsonValueNull>());
			Data->SetField(TEXT("cpu_top_self_ms"), MakeShared<FJsonValueNull>());
		}
		Data->SetNumberField(TEXT("memory_delta_bytes"), static_cast<double>(MemoryDeltaBytes));

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("query_name"), RegionName);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (FoundRegion->bIncomplete)
		{
			AddMetaWarning(
				Meta,
				TEXT("Region has no closing event; metrics are bounded to begin timestamp."),
				TEXT("W2202"),
				TEXT("Use marks regions to inspect incomplete region rows."));
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("marks") && Request.Action == TEXT("search"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(
			Request.Args,
			{ TEXT("keyword"), TEXT("case-sensitive"), TEXT("exact"), TEXT("category"), TEXT("channel"), TEXT("thread-id"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") },
			UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString Keyword;
		FInsightCliResponse RequiredOptionError;
		if (!RequireStringOption(Request.Args, TEXT("--keyword"), TEXT("marks search"), Keyword, RequiredOptionError))
		{
			OutResponse = RequiredOptionError;
			return true;
		}

		const bool bCaseSensitive = HasOption(Request.Args, TEXT("--case-sensitive"));
		const bool bExact = HasOption(Request.Args, TEXT("--exact"));

		FString CategoryFilter;
		const bool bHasCategoryFilter = TryGetStringOption(Request.Args, TEXT("--category"), CategoryFilter);

		FString ChannelFilter;
		const bool bHasChannelFilter = TryGetStringOption(Request.Args, TEXT("--channel"), ChannelFilter);

		int32 ThreadIdFilter = -1;
		const bool bHasThreadIdFilter = TryGetIntOption(Request.Args, TEXT("--thread-id"), ThreadIdFilter);
		if (bHasThreadIdFilter && ThreadIdFilter < 0)
		{
			OutResponse = MakeOptionError(TEXT("thread-id must be >= 0."));
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		FMarksFilter Filter;
		if (bHasCategoryFilter)
		{
			Filter.Category = CategoryFilter;
		}
		if (bHasChannelFilter)
		{
			Filter.Channel = ChannelFilter;
		}
		if (bHasThreadIdFilter)
		{
			Filter.ThreadId = ThreadIdFilter;
		}
		Filter.Keyword = Keyword;
		Filter.bCaseSensitive = bCaseSensitive;
		Filter.bExact = bExact;

		TArray<FMarkSample> AllMarks;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMarks(
			Context,
			Filter,
			AllMarks,
			FailureStage,
			FailureReason,
			TimeWindow.StartMs,
			TimeWindow.EndMs))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("marks.search"),
				FailureStage,
				FailureReason,
				TEXT("marks_stream"),
				TEXT("failed to build marks stream"),
				TEXT("Trace-backed marks are unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		for (const FMarkSample& Mark : AllMarks)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeMarkObject(Mark)));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("keyword"), Keyword);
		Meta.Add(TEXT("case_sensitive"), bCaseSensitive ? TEXT("true") : TEXT("false"));
		Meta.Add(TEXT("exact"), bExact ? TEXT("true") : TEXT("false"));
		if (bHasCategoryFilter)
		{
			Meta.Add(TEXT("filter_category"), CategoryFilter);
		}
		if (bHasChannelFilter)
		{
			Meta.Add(TEXT("filter_channel"), ChannelFilter);
		}
		if (bHasThreadIdFilter)
		{
			Meta.Add(TEXT("filter_thread_id"), FString::FromInt(ThreadIdFilter));
		}
		AppendTimeWindowMeta(TimeWindow, Meta);
		Meta.Add(TEXT("data_source"), TEXT("trace"));

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("marks") && Request.Action == TEXT("around"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(
			Request.Args,
			{ TEXT("timestamp"), TEXT("window"), TEXT("category"), TEXT("channel"), TEXT("thread-id"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") },
			UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		double TimestampMs = 0.0;
		double WindowMs = 0.0;
		if (!TryGetDoubleOption(Request.Args, TEXT("--timestamp"), TimestampMs))
		{
			OutResponse = MakeOptionError(TEXT("--timestamp is required for marks around."));
			return true;
		}
		if (!TryGetDoubleOption(Request.Args, TEXT("--window"), WindowMs))
		{
			OutResponse = MakeOptionError(TEXT("--window is required for marks around."));
			return true;
		}
		if (WindowMs < 0.0)
		{
			OutResponse = MakeOptionError(TEXT("window must be >= 0."));
			return true;
		}

		FString CategoryFilter;
		const bool bHasCategoryFilter = TryGetStringOption(Request.Args, TEXT("--category"), CategoryFilter);

		FString ChannelFilter;
		const bool bHasChannelFilter = TryGetStringOption(Request.Args, TEXT("--channel"), ChannelFilter);

		int32 ThreadIdFilter = -1;
		const bool bHasThreadIdFilter = TryGetIntOption(Request.Args, TEXT("--thread-id"), ThreadIdFilter);
		if (bHasThreadIdFilter && ThreadIdFilter < 0)
		{
			OutResponse = MakeOptionError(TEXT("thread-id must be >= 0."));
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		const double Start = TimestampMs - WindowMs;
		const double End = TimestampMs + WindowMs;

		const TOptional<double> RequestStart = TimeWindow.StartMs.IsSet() ? TOptional<double>(FMath::Max(Start, TimeWindow.StartMs.GetValue())) : TOptional<double>(Start);
		const TOptional<double> RequestEnd = TimeWindow.EndMs.IsSet() ? TOptional<double>(FMath::Min(End, TimeWindow.EndMs.GetValue())) : TOptional<double>(End);

		FMarksFilter Filter;
		if (bHasCategoryFilter)
		{
			Filter.Category = CategoryFilter;
		}
		if (bHasChannelFilter)
		{
			Filter.Channel = ChannelFilter;
		}
		if (bHasThreadIdFilter)
		{
			Filter.ThreadId = ThreadIdFilter;
		}

		TArray<FMarkSample> AllMarks;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMarks(
			Context,
			Filter,
			AllMarks,
			FailureStage,
			FailureReason,
			RequestStart,
			RequestEnd))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("marks.around"),
				FailureStage,
				FailureReason,
				TEXT("marks_stream"),
				TEXT("failed to build marks stream"),
				TEXT("Trace-backed marks are unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		for (const FMarkSample& Mark : AllMarks)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeMarkObject(Mark)));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("around_timestamp"), ToNumberString(TimestampMs));
		Meta.Add(TEXT("around_window"), ToNumberString(WindowMs));
		if (bHasCategoryFilter)
		{
			Meta.Add(TEXT("filter_category"), CategoryFilter);
		}
		if (bHasChannelFilter)
		{
			Meta.Add(TEXT("filter_channel"), ChannelFilter);
		}
		if (bHasThreadIdFilter)
		{
			Meta.Add(TEXT("filter_thread_id"), FString::FromInt(ThreadIdFilter));
		}
		AppendTimeWindowMeta(TimeWindow, Meta);
		Meta.Add(TEXT("data_source"), TEXT("trace"));

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
