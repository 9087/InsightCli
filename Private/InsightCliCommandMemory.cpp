// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AllocationsProvider.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Memory.h"

namespace UE::InsightCli::Internal
{
bool BuildMemorySamplesTrace(
	const FTraceContext& Context,
	TArray<FMemorySample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}
	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	const auto GetFrameIndexForTimestampMs = [&Frames](const double TimestampMs) -> int32
	{
		for (const FFrameSample& Frame : Frames)
		{
			if (Frame.FrameStartMs <= TimestampMs && TimestampMs <= Frame.FrameEndMs)
			{
				return Frame.FrameIndex;
			}
		}
		return -1;
	};

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IAllocationsProvider* AllocationsProvider = TraceServices::ReadAllocationsProvider(*Session.Get());
		if (AllocationsProvider == nullptr)
		{
			return true;
		}

		AllocationsProvider->BeginRead();
		if (!AllocationsProvider->IsInitialized())
		{
			AllocationsProvider->EndRead();
			return true;
		}

		const double DurationSec = Session->GetDurationSeconds();
		const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
		const double EndSecRaw = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;
		const double EndSec = FMath::Max(StartSec + KINDA_SMALL_NUMBER, EndSecRaw);

		int32 StartIndex = 0;
		int32 EndIndex = -1;
		AllocationsProvider->GetTimelineIndexRange(StartSec, EndSec, StartIndex, EndIndex);
		if (EndIndex < StartIndex)
		{
			AllocationsProvider->EndRead();
			return true;
		}

		AllocationsProvider->EnumerateMaxTotalAllocatedMemoryTimeline(StartIndex, EndIndex,
			[&OutSamples, &GetFrameIndexForTimestampMs](double Time, double, uint64 Value)
			{
				FMemorySample Sample;
				Sample.TimestampMs = Time * 1000.0;
				Sample.Bytes = static_cast<int64>(Value > static_cast<uint64>(MAX_int64) ? MAX_int64 : Value);
				Sample.FrameIndex = GetFrameIndexForTimestampMs(Sample.TimestampMs);
				OutSamples.Add(MoveTemp(Sample));
			});
		AllocationsProvider->EndRead();
	}

	OutSamples.Sort([](const FMemorySample& A, const FMemorySample& B)
	{
		return A.TimestampMs < B.TimestampMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeMemorySummaryObject(const TArray<FMemorySample>& Samples)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	if (Samples.IsEmpty())
	{
		Item->SetNumberField(TEXT("min_bytes"), 0.0);
		Item->SetNumberField(TEXT("max_bytes"), 0.0);
		Item->SetNumberField(TEXT("avg_bytes"), 0.0);
		Item->SetNumberField(TEXT("end_bytes"), 0.0);
		return Item;
	}

	int64 MinBytes = TNumericLimits<int64>::Max();
	int64 MaxBytes = 0;
	long double SumBytes = 0.0;
	for (const FMemorySample& Sample : Samples)
	{
		MinBytes = FMath::Min(MinBytes, Sample.Bytes);
		MaxBytes = FMath::Max(MaxBytes, Sample.Bytes);
		SumBytes += static_cast<long double>(Sample.Bytes);
	}

	Item->SetNumberField(TEXT("min_bytes"), static_cast<double>(MinBytes));
	Item->SetNumberField(TEXT("max_bytes"), static_cast<double>(MaxBytes));
	Item->SetNumberField(TEXT("avg_bytes"), static_cast<double>(SumBytes / static_cast<long double>(Samples.Num())));
	Item->SetNumberField(TEXT("end_bytes"), static_cast<double>(Samples.Last().Bytes));
	return Item;
}

TSharedRef<FJsonObject> MakeMemoryPeakObject(const TArray<FMemorySample>& Samples)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	if (Samples.IsEmpty())
	{
		Item->SetNumberField(TEXT("peak_bytes"), 0.0);
		Item->SetNumberField(TEXT("peak_timestamp_ms"), 0.0);
		Item->SetNumberField(TEXT("peak_frame_index"), -1);
		Item->SetArrayField(TEXT("peak_context"), {});
		return Item;
	}

	int32 PeakIndex = 0;
	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		if (Samples[Index].Bytes > Samples[PeakIndex].Bytes)
		{
			PeakIndex = Index;
		}
	}

	const FMemorySample& PeakSample = Samples[PeakIndex];
	Item->SetNumberField(TEXT("peak_bytes"), static_cast<double>(PeakSample.Bytes));
	Item->SetNumberField(TEXT("peak_timestamp_ms"), PeakSample.TimestampMs);
	Item->SetNumberField(TEXT("peak_frame_index"), PeakSample.FrameIndex);

	TArray<TSharedPtr<FJsonValue>> PeakContext;
	for (int32 ContextIndex = FMath::Max(0, PeakIndex - 1); ContextIndex <= FMath::Min(PeakIndex + 1, Samples.Num() - 1); ++ContextIndex)
	{
		const FMemorySample& ContextSample = Samples[ContextIndex];
		const TSharedRef<FJsonObject> ContextItem = MakeShared<FJsonObject>();
		ContextItem->SetNumberField(TEXT("timestamp_ms"), ContextSample.TimestampMs);
		ContextItem->SetNumberField(TEXT("bytes"), static_cast<double>(ContextSample.Bytes));
		ContextItem->SetNumberField(TEXT("frame_index"), ContextSample.FrameIndex);
		PeakContext.Add(MakeShared<FJsonValueObject>(ContextItem));
	}
	Item->SetArrayField(TEXT("peak_context"), PeakContext);

	return Item;
}

bool BuildMemoryTagsTrace(
	const FTraceContext& Context,
	TArray<FMemoryTagSample>& OutTags,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs)
{
	OutTags.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}
	TMap<FString, int64> TagBytes;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IMemoryProvider* MemoryProvider = TraceServices::ReadMemoryProvider(*Session.Get());
		if (MemoryProvider == nullptr || MemoryProvider->GetTrackerCount() == 0)
		{
			return true;
		}

		TArray<TraceServices::FMemoryTrackerInfo> Trackers;
		MemoryProvider->EnumerateTrackers([&Trackers](const TraceServices::FMemoryTrackerInfo& Tracker)
		{
			Trackers.Add(Tracker);
		});
		if (Trackers.IsEmpty())
		{
			return true;
		}

		const TraceServices::FMemoryTrackerId TrackerId = Trackers[0].Id;
		const double DurationSec = Session->GetDurationSeconds();
		const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
		const double EndSecRaw = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;
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

			int64 LastValue = 0;
			bool bHasSample = false;
			MemoryProvider->EnumerateTagSamples(TrackerId, TagInfo.Id, StartSec, EndSec, true,
				[&LastValue, &bHasSample](double, double, const TraceServices::FMemoryTagSample& Sample)
				{
					LastValue = Sample.Value;
					bHasSample = true;
				});

			if (bHasSample && LastValue > 0)
			{
				TagBytes.Add(TagInfo.Name, LastValue);
			}
		});
	}

	int64 TotalBytes = 0;
	for (const TPair<FString, int64>& Pair : TagBytes)
	{
		TotalBytes += Pair.Value;
	}

	if (TotalBytes <= 0)
	{
		return true;
	}

	for (const TPair<FString, int64>& Pair : TagBytes)
	{
		FMemoryTagSample Tag;
		Tag.TagName = Pair.Key;
		Tag.Bytes = Pair.Value;
		Tag.PercentRatio = static_cast<double>(Pair.Value) / static_cast<double>(TotalBytes);
		OutTags.Add(MoveTemp(Tag));
	}

	OutTags.Sort([](const FMemoryTagSample& A, const FMemoryTagSample& B)
	{
		if (A.Bytes == B.Bytes)
		{
			return A.TagName < B.TagName;
		}
		return A.Bytes > B.Bytes;
	});

	return true;
}

TSharedRef<FJsonObject> MakeMemoryTagObject(const FMemoryTagSample& Tag)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("tag_name"), Tag.TagName);
	Item->SetNumberField(TEXT("bytes"), static_cast<double>(Tag.Bytes));
	Item->SetNumberField(TEXT("percent_ratio"), Tag.PercentRatio);
	return Item;
}
}
