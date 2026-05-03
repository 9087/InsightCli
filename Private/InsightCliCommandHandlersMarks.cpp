// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleMarksCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles marks/search and marks/around with strict argument validation.
	if (Request.Group == TEXT("marks") && Request.Action == TEXT("search"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(
			Request.Args,
			{ TEXT("keyword"), TEXT("case-sensitive"), TEXT("exact"), TEXT("category"), TEXT("channel"), TEXT("thread-id"), TEXT("time-start"), TEXT("time-end") },
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
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("thread-id must be >= 0."));
			return true;
		}

		FTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryGetTimeWindowMs(Request.Args, TimeWindow, TimeWindowError))
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
		if (TimeWindow.StartMs.IsSet())
		{
			Meta.Add(TEXT("filter_time_start"), ToNumberString(TimeWindow.StartMs.GetValue()));
		}
		if (TimeWindow.EndMs.IsSet())
		{
			Meta.Add(TEXT("filter_time_end"), ToNumberString(TimeWindow.EndMs.GetValue()));
		}
		Meta.Add(TEXT("data_source"), TEXT("trace"));

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("marks") && Request.Action == TEXT("around"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(
			Request.Args,
			{ TEXT("timestamp"), TEXT("window"), TEXT("category"), TEXT("channel"), TEXT("thread-id"), TEXT("time-start"), TEXT("time-end") },
			UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		double TimestampMs = 0.0;
		double WindowMs = 0.0;
		if (!TryGetDoubleOption(Request.Args, TEXT("--timestamp"), TimestampMs))
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("--timestamp is required for marks around."));
			return true;
		}
		if (!TryGetDoubleOption(Request.Args, TEXT("--window"), WindowMs))
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("--window is required for marks around."));
			return true;
		}
		if (WindowMs < 0.0)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("window must be >= 0."));
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
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("thread-id must be >= 0."));
			return true;
		}

		FTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryGetTimeWindowMs(Request.Args, TimeWindow, TimeWindowError))
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
		if (TimeWindow.StartMs.IsSet())
		{
			Meta.Add(TEXT("filter_time_start"), ToNumberString(TimeWindow.StartMs.GetValue()));
		}
		if (TimeWindow.EndMs.IsSet())
		{
			Meta.Add(TEXT("filter_time_end"), ToNumberString(TimeWindow.EndMs.GetValue()));
		}
		Meta.Add(TEXT("data_source"), TEXT("trace"));

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
