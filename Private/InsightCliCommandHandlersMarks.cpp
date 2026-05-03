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
		if (!TryGetStringOption(Request.Args, TEXT("--keyword"), Keyword))
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("--keyword is required for marks search."));
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

		double TimeStartMs = 0.0;
		double TimeEndMs = 0.0;
		const bool bHasTimeStart = TryGetDoubleOption(Request.Args, TEXT("--time-start"), TimeStartMs);
		const bool bHasTimeEnd = TryGetDoubleOption(Request.Args, TEXT("--time-end"), TimeEndMs);
		if (bHasTimeStart && bHasTimeEnd && TimeStartMs > TimeEndMs)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
			return true;
		}

		TArray<FMarkSample> AllMarks;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMarks(
			Context,
			AllMarks,
			FailureStage,
			FailureReason,
			bHasTimeStart ? TOptional<double>(TimeStartMs) : TOptional<double>(),
			bHasTimeEnd ? TOptional<double>(TimeEndMs) : TOptional<double>()))
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
			if (bHasCategoryFilter && !Mark.Category.Equals(CategoryFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (bHasChannelFilter && !Mark.Channel.Equals(ChannelFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (bHasThreadIdFilter && Mark.ThreadId != ThreadIdFilter)
			{
				continue;
			}

			if (bHasTimeStart && Mark.TimestampMs < TimeStartMs)
			{
				continue;
			}

			if (bHasTimeEnd && Mark.TimestampMs >= TimeEndMs)
			{
				continue;
			}

			const FString Haystack = Mark.Message;
			bool bMatched = false;
			if (bExact)
			{
				bMatched = Haystack.Equals(Keyword, bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
			}
			else
			{
				bMatched = Haystack.Contains(Keyword, bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
			}

			if (bMatched)
			{
				Data.Add(MakeShared<FJsonValueObject>(MakeMarkObject(Mark)));
			}
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
		if (bHasTimeStart)
		{
			Meta.Add(TEXT("filter_time_start"), ToNumberString(TimeStartMs));
		}
		if (bHasTimeEnd)
		{
			Meta.Add(TEXT("filter_time_end"), ToNumberString(TimeEndMs));
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

		double TimeStartMs = 0.0;
		double TimeEndMs = 0.0;
		const bool bHasTimeStart = TryGetDoubleOption(Request.Args, TEXT("--time-start"), TimeStartMs);
		const bool bHasTimeEnd = TryGetDoubleOption(Request.Args, TEXT("--time-end"), TimeEndMs);
		if (bHasTimeStart && bHasTimeEnd && TimeStartMs > TimeEndMs)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
			return true;
		}

		const double Start = TimestampMs - WindowMs;
		const double End = TimestampMs + WindowMs;

		const TOptional<double> RequestStart = bHasTimeStart ? TOptional<double>(FMath::Max(Start, TimeStartMs)) : TOptional<double>(Start);
		const TOptional<double> RequestEnd = bHasTimeEnd ? TOptional<double>(FMath::Min(End, TimeEndMs)) : TOptional<double>(End);

		TArray<FMarkSample> AllMarks;
		FString FailureStage;
		FString FailureReason;
		if (!BuildMarks(
			Context,
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
			if (Mark.TimestampMs < Start || Mark.TimestampMs >= End)
			{
				continue;
			}

			if (bHasCategoryFilter && !Mark.Category.Equals(CategoryFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (bHasChannelFilter && !Mark.Channel.Equals(ChannelFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (bHasThreadIdFilter && Mark.ThreadId != ThreadIdFilter)
			{
				continue;
			}

			if (bHasTimeStart && Mark.TimestampMs < TimeStartMs)
			{
				continue;
			}

			if (bHasTimeEnd && Mark.TimestampMs >= TimeEndMs)
			{
				continue;
			}

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
		if (bHasTimeStart)
			{
				Meta.Add(TEXT("filter_time_start"), ToNumberString(TimeStartMs));
			}
		if (bHasTimeEnd)
		{
			Meta.Add(TEXT("filter_time_end"), ToNumberString(TimeEndMs));
		}
		Meta.Add(TEXT("data_source"), TEXT("trace"));

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
