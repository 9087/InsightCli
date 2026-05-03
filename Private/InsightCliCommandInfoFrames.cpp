// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Misc/Paths.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Channel.h"
#include "TraceServices/Model/Threads.h"

namespace UE::InsightCli::Internal
{
void ApplyTimeWindowFilter(TArray<FFrameSample>& Frames, const FResolvedTimeWindowMs& TimeWindow)
{
	if (!TimeWindow.IsSet())
	{
		return;
	}

	const double EffectiveStart = TimeWindow.StartMs.IsSet() ? TimeWindow.StartMs.GetValue() : 0.0;
	const double EffectiveEnd = TimeWindow.EndMs.IsSet() ? TimeWindow.EndMs.GetValue() : TNumericLimits<double>::Max();
	Frames = Frames.FilterByPredicate([EffectiveStart, EffectiveEnd](const FFrameSample& Sample)
	{
		return Sample.FrameStartMs >= EffectiveStart && Sample.FrameStartMs < EffectiveEnd;
	});
}

TSharedRef<FJsonObject> MakeInfoSummaryData(const FTraceContext& Context)
{
	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	const double DurationMs = (Context.TraceDurationMs > 0.0) ? Context.TraceDurationMs : (!Frames.IsEmpty() ? Frames.Last().FrameEndMs : 0.0);
	const FString StartTimestamp = Context.TimeStamp.ToIso8601();
	const FString EndTimestamp = (DurationMs > 0.0)
		? (Context.TimeStamp + FTimespan::FromMilliseconds(DurationMs)).ToIso8601()
		: TEXT("unavailable");

	FString ThreadCount = TEXT("unavailable");
	FString ThreadCountReason;
	{
		TSharedPtr<const TraceServices::IAnalysisSession> Session;
		FString FailureStage;
		FString FailureReason;
		if (AcquireAnalysisSession(Context, Session, FailureStage, FailureReason))
		{
			int32 ThreadTotal = 0;
			bool bHasThreadProvider = false;
			{
				TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
				const TraceServices::IThreadProvider* ThreadProvider = Session->ReadProvider<TraceServices::IThreadProvider>(TraceServices::GetThreadProviderName());
				if (ThreadProvider != nullptr)
				{
					bHasThreadProvider = true;
					ThreadProvider->EnumerateThreads([&ThreadTotal](const TraceServices::FThreadInfo&)
					{
						++ThreadTotal;
					});
				}
			}

			if (bHasThreadProvider)
			{
				ThreadCount = FString::FromInt(ThreadTotal);
			}
			else
			{
				ThreadCountReason = TEXT("thread_provider_not_available");
			}
		}
		else
		{
			ThreadCountReason = FString::Printf(TEXT("analysis_session_unavailable:%s:%s"), *FailureStage, *FailureReason);
		}
	}

	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("trace_name"), FPaths::GetCleanFilename(Context.FullPath));
	Data->SetStringField(TEXT("trace_path"), Context.FullPath);
	Data->SetStringField(TEXT("trace_size_bytes"), FString::Printf(TEXT("%lld"), Context.FileSize));
	Data->SetStringField(TEXT("start_timestamp"), StartTimestamp);
	Data->SetStringField(TEXT("start_timestamp_source"), TEXT("recorded_at_file_mtime"));
	Data->SetStringField(TEXT("end_timestamp"), EndTimestamp);
	Data->SetStringField(TEXT("duration_ms"), FString::Printf(TEXT("%.3f"), DurationMs));
	Data->SetStringField(TEXT("thread_count"), ThreadCount);
	if (!ThreadCountReason.IsEmpty())
	{
		Data->SetStringField(TEXT("thread_count_reason"), ThreadCountReason);
	}
	Data->SetStringField(TEXT("event_count"), TEXT("unavailable"));
	Data->SetStringField(TEXT("event_count_reason"), TEXT("trace_event_count_not_exposed"));
	Data->SetStringField(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
	Data->SetNumberField(TEXT("game_frame_count"), Context.TraceGameFrameCount);
	Data->SetNumberField(TEXT("rendering_frame_count"), Context.TraceRenderingFrameCount);
	if (Context.bFrameSamplesFailed)
	{
		Data->SetStringField(TEXT("trace_parse_failure_stage"), Context.FrameSamplesFailureStage);
		Data->SetStringField(TEXT("trace_parse_failure_reason"), Context.FrameSamplesFailureReason);
	}
	Data->SetStringField(TEXT("platform"), FPlatformProperties::IniPlatformName());
	Data->SetStringField(TEXT("build_version"), TEXT("unavailable"));
	Data->SetStringField(TEXT("build_version_reason"), TEXT("trace_build_version_not_exposed"));
	return Data;
}

TSharedRef<FJsonObject> MakeInfoChannelsData(const FTraceContext& Context, TMap<FString, FString>& OutMeta)
{
	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ChannelsArray;

	OutMeta.Add(TEXT("data_source"), TEXT("unavailable"));
	OutMeta.Add(TEXT("channel_count"), TEXT("0"));

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	FString FailureStage;
	FString FailureReason;
	if (!AcquireAnalysisSession(Context, Session, FailureStage, FailureReason))
	{
		OutMeta.Add(TEXT("channel_provider_reason"), FString::Printf(TEXT("analysis_session_unavailable:%s:%s"), *FailureStage, *FailureReason));
		Data->SetArrayField(TEXT("channels"), ChannelsArray);
		return Data;
	}

	bool bHasChannelProvider = false;
	FDateTime ChannelTimeStamp;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IChannelProvider* ChannelProvider = TraceServices::ReadChannelProvider(*Session.Get());
		if (ChannelProvider != nullptr)
		{
			bHasChannelProvider = true;
			ChannelTimeStamp = ChannelProvider->GetTimeStamp();

			const TArray<TraceServices::FChannelEntry>& Channels = ChannelProvider->GetChannels();
			ChannelsArray.Reserve(Channels.Num());
			for (const TraceServices::FChannelEntry& Channel : Channels)
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("channel_id"), Channel.Id);
				Item->SetStringField(TEXT("name"), Channel.Name);
				Item->SetBoolField(TEXT("enabled"), Channel.bIsEnabled);
				Item->SetBoolField(TEXT("read_only"), Channel.bReadOnly);
				Item->SetStringField(TEXT("provider"), TEXT("ChannelProvider"));
				Item->SetStringField(TEXT("event_count"), TEXT("unavailable"));
				Item->SetStringField(TEXT("event_count_reason"), TEXT("channel_event_count_not_exposed"));
				Item->SetStringField(TEXT("first_ts"), TEXT("unavailable"));
				Item->SetStringField(TEXT("last_ts"), TEXT("unavailable"));
				ChannelsArray.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
	}

	if (!bHasChannelProvider)
	{
		OutMeta.Add(TEXT("channel_provider_reason"), TEXT("channel_provider_not_available"));
		Data->SetArrayField(TEXT("channels"), ChannelsArray);
		return Data;
	}

	OutMeta.Add(TEXT("data_source"), TEXT("trace"));
	OutMeta.Add(TEXT("channel_count"), FString::FromInt(ChannelsArray.Num()));
	OutMeta.Add(TEXT("channel_timestamp"), ChannelTimeStamp.ToIso8601());
	Data->SetArrayField(TEXT("channels"), ChannelsArray);
	return Data;
}

TSharedRef<FJsonObject> MakeFramesSummaryData(const TArray<FFrameSample>& Frames)
{
	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	if (Frames.IsEmpty())
	{
		return Data;
	}

	TArray<double> SortedTimes;
	SortedTimes.Reserve(Frames.Num());
	double Sum = 0.0;
	double MinValue = TNumericLimits<double>::Max();
	double MaxValue = 0.0;

	for (const FFrameSample& Sample : Frames)
	{
		SortedTimes.Add(Sample.FrameTimeMs);
		Sum += Sample.FrameTimeMs;
		MinValue = FMath::Min(MinValue, Sample.FrameTimeMs);
		MaxValue = FMath::Max(MaxValue, Sample.FrameTimeMs);
	}

	SortedTimes.Sort();
	const auto PercentileValue = [&SortedTimes](const double Ratio)
	{
		const int32 Index = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(Ratio * (SortedTimes.Num() - 1))), 0, SortedTimes.Num() - 1);
		return SortedTimes[Index];
	};

	Data->SetNumberField(TEXT("frame_count"), Frames.Num());
	Data->SetNumberField(TEXT("avg_frame_ms"), Sum / Frames.Num());
	Data->SetNumberField(TEXT("max_frame_ms"), MaxValue);
	Data->SetNumberField(TEXT("min_frame_ms"), MinValue);
	Data->SetNumberField(TEXT("p50"), PercentileValue(0.50));
	Data->SetNumberField(TEXT("p90"), PercentileValue(0.90));
	Data->SetNumberField(TEXT("p95"), PercentileValue(0.95));
	Data->SetNumberField(TEXT("p99"), PercentileValue(0.99));
	return Data;
}

TSharedRef<FJsonObject> MakeFrameObject(const FFrameSample& Sample)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), Sample.FrameIndex);
	Item->SetNumberField(TEXT("frame_start_ms"), Sample.FrameStartMs);
	Item->SetNumberField(TEXT("frame_end_ms"), Sample.FrameEndMs);
	Item->SetNumberField(TEXT("frame_time_ms"), Sample.FrameTimeMs);
	Item->SetNumberField(TEXT("game_thread_ms"), Sample.GameThreadMs);
	Item->SetNumberField(TEXT("render_thread_ms"), Sample.RenderThreadMs);
	Item->SetNumberField(TEXT("rhi_thread_ms"), Sample.RhiThreadMs);
	Item->SetNumberField(TEXT("gpu_ms"), Sample.GpuMs);
	Item->SetNumberField(TEXT("game_frame_index"), Sample.GameFrameIndex);
	Item->SetNumberField(TEXT("rendering_frame_index"), Sample.RenderingFrameIndex);
	Item->SetStringField(TEXT("data_source"), Sample.bTraceBacked ? TEXT("trace") : TEXT("unavailable"));
	return Item;
}
}
