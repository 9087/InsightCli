// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleInfoAndFramesCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("info") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeInfoSummaryData(Context)));
		return true;
	}

	if (Request.Group == TEXT("frames") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("frames.summary")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		bool bUsedWindow = false;
		FInsightCliResponse WindowError;
		ApplyTimeWindowFilter(Frames, Request.Args, bUsedWindow, WindowError);
		if (WindowError.ExitCode != 0)
		{
			OutResponse = WindowError;
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("game_frame_count"), FString::FromInt(Context.TraceGameFrameCount));
		Meta.Add(TEXT("rendering_frame_count"), FString::FromInt(Context.TraceRenderingFrameCount));
		if (bUsedWindow)
		{
			double Start = 0.0;
			double End = 0.0;
			if (TryGetDoubleOption(Request.Args, TEXT("--time-start"), Start))
			{
				Meta.Add(TEXT("time_start_ms"), ToNumberString(Start));
			}
			if (TryGetDoubleOption(Request.Args, TEXT("--time-end"), End))
			{
				Meta.Add(TEXT("time_end_ms"), ToNumberString(End));
			}
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeFramesSummaryData(Frames), Meta));
		return true;
	}

	if (Request.Group == TEXT("frames") && Request.Action == TEXT("slowest"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
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

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("frames.slowest")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		bool bUsedWindow = false;
		FInsightCliResponse WindowError;
		ApplyTimeWindowFilter(Frames, Request.Args, bUsedWindow, WindowError);
		if (WindowError.ExitCode != 0)
		{
			OutResponse = WindowError;
			return true;
		}

		Frames.Sort([](const FFrameSample& A, const FFrameSample& B)
		{
			if (A.FrameTimeMs == B.FrameTimeMs)
			{
				return A.FrameIndex < B.FrameIndex;
			}
			return A.FrameTimeMs > B.FrameTimeMs;
		});

		const int32 TakeCount = FMath::Min(Limit, Frames.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeFrameObject(Frames[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("game_frame_count"), FString::FromInt(Context.TraceGameFrameCount));
		Meta.Add(TEXT("rendering_frame_count"), FString::FromInt(Context.TraceRenderingFrameCount));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("frames") && Request.Action == TEXT("detail"))
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
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("--frame-index is required for frames detail."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("frame-index must be >= 0."));
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("frames.detail")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		const FFrameSample* Found = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
		{
			return Item.FrameIndex == FrameIndex;
		});

		if (Found == nullptr)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeShared<FJsonObject>(), Meta));
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeFrameObject(*Found), Meta));
		return true;
	}

	return false;
}
}
