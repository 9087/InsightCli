// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FGcEventSample
{
	FString ScopeName;
	FString Stage;
	int32 ThreadId = -1;
	FString ThreadName;
	int32 FrameIndex = -1;
	double StartMs = 0.0;
	double EndMs = 0.0;
	double DurationMs = 0.0;
};

bool IsGcScopeName(const FString& ScopeName)
{
	static const TCHAR* GcPatterns[] =
	{
		TEXT("CollectGarbage"),
		TEXT("CollectGarbageInternal"),
		TEXT("IncrementalPurgeGarbage"),
		TEXT("Reachability"),
		TEXT("Sweep"),
		TEXT("MarkObjectsAsUnreachable"),
		TEXT("UnhashUnreachableObjects"),
		TEXT("PurgeGarbage"),
		TEXT("GarbageCollection"),
	};

	for (const TCHAR* Pattern : GcPatterns)
	{
		if (ScopeName.Contains(Pattern, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

FString ClassifyGcStage(const FString& ScopeName)
{
	if (ScopeName.Contains(TEXT("Reachability"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("MarkObjectsAsUnreachable"), ESearchCase::IgnoreCase))
	{
		return TEXT("reachability");
	}
	if (ScopeName.Contains(TEXT("Sweep"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("Purge"), ESearchCase::IgnoreCase)
		|| ScopeName.Contains(TEXT("UnhashUnreachableObjects"), ESearchCase::IgnoreCase))
	{
		return TEXT("sweep");
	}

	return TEXT("other");
}

bool BuildGcEventSamples(
	const FTraceContext& Context,
	const FResolvedTimeWindowMs& TimeWindow,
	TArray<FGcEventSample>& OutEvents,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutEvents.Reset();
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

	struct FTimerSnapshot
	{
		FString Name;
		bool bValid = false;
	};

	TArray<FTimerSnapshot> Timers;
	TMap<uint32, int32> ThreadIdByTimelineIndex;
	TMap<uint32, FString> ThreadNameByTimelineIndex;

	const double StartSec = TimeWindow.StartMs.IsSet() ? FMath::Max(0.0, TimeWindow.StartMs.GetValue() / 1000.0) : 0.0;
	double EndSec = StartSec + KINDA_SMALL_NUMBER;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const double SessionEndSec = Session->GetDurationSeconds();
		const double EndSecRaw = TimeWindow.EndMs.IsSet() ? FMath::Max(StartSec, TimeWindow.EndMs.GetValue() / 1000.0) : SessionEndSec;
		EndSec = FMath::Max(StartSec + KINDA_SMALL_NUMBER, EndSecRaw);

		const TraceServices::ITimingProfilerProvider* TimingProvider = TraceServices::ReadTimingProfilerProvider(*Session.Get());
		if (TimingProvider == nullptr)
		{
			OutFailureStage = TEXT("timing_provider");
			OutFailureReason = TEXT("TimingProfiler provider not available");
			return false;
		}

		TimingProvider->ReadTimers([&Timers](const TraceServices::ITimingProfilerTimerReader& TimerReader)
		{
			const uint32 TimerCount = TimerReader.GetTimerCount();
			Timers.SetNum(static_cast<int32>(TimerCount));
			for (uint32 TimerIndex = 0; TimerIndex < TimerCount; ++TimerIndex)
			{
				const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(TimerIndex);
				if (Timer == nullptr || Timer->Name == nullptr)
				{
					continue;
				}

				FTimerSnapshot& Snapshot = Timers[static_cast<int32>(TimerIndex)];
				Snapshot.Name = Timer->Name;
				Snapshot.bValid = true;
			}
		});

		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session.Get());
		ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
		{
			uint32 TimelineIndex = uint32(-1);
			if (!TimingProvider->GetCpuThreadTimelineIndex(ThreadInfo.Id, TimelineIndex))
			{
				return;
			}

			ThreadIdByTimelineIndex.Add(TimelineIndex, static_cast<int32>(ThreadInfo.Id));
			ThreadNameByTimelineIndex.Add(TimelineIndex, ThreadInfo.Name != nullptr ? ThreadInfo.Name : FString());
		});

		const uint64 TimelineCount = TimingProvider->GetTimelineCount();
		for (uint32 TimelineIndex = 0; TimelineIndex < TimelineCount; ++TimelineIndex)
		{
			struct FStackEntry
			{
				FString ScopeName;
				double StartSec = 0.0;
				bool bIsGc = false;
			};
			TArray<FStackEntry, TInlineAllocator<64>> Stack;

			const int32 ThreadId = ThreadIdByTimelineIndex.FindRef(TimelineIndex);
			const FString ThreadName = ThreadNameByTimelineIndex.FindRef(TimelineIndex);

			TimingProvider->ReadTimeline(TimelineIndex, [&](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
			{
				Timeline.EnumerateEvents(StartSec, EndSec, [&](bool bIsEnter, double Time, const TraceServices::FTimingProfilerEvent& Event)
				{
					FString ScopeName = TEXT("<unknown>");
					if (Timers.IsValidIndex(static_cast<int32>(Event.TimerIndex)) && Timers[static_cast<int32>(Event.TimerIndex)].bValid)
					{
						ScopeName = Timers[static_cast<int32>(Event.TimerIndex)].Name;
					}

					if (bIsEnter)
					{
						FStackEntry& Entry = Stack.AddDefaulted_GetRef();
						Entry.ScopeName = ScopeName;
						Entry.StartSec = Time;
						Entry.bIsGc = IsGcScopeName(ScopeName);
						return TraceServices::EEventEnumerate::Continue;
					}

					if (Stack.IsEmpty())
					{
						return TraceServices::EEventEnumerate::Continue;
					}

					FStackEntry Entry = Stack.Pop(EAllowShrinking::No);
					if (!Entry.bIsGc)
					{
						return TraceServices::EEventEnumerate::Continue;
					}

					const double StartMs = Entry.StartSec * 1000.0;
					const double EndMs = FMath::Max(StartMs, Time * 1000.0);
					FGcEventSample Sample;
					Sample.ScopeName = Entry.ScopeName;
					Sample.Stage = ClassifyGcStage(Entry.ScopeName);
					Sample.ThreadId = ThreadId;
					Sample.ThreadName = ThreadName;
					Sample.StartMs = StartMs;
					Sample.EndMs = EndMs;
					Sample.DurationMs = EndMs - StartMs;
					Sample.FrameIndex = GetFrameIndexForTimestampMs(StartMs);
					OutEvents.Add(MoveTemp(Sample));
					return TraceServices::EEventEnumerate::Continue;
				});
			});
		}
	}

	OutEvents.Sort([](const FGcEventSample& A, const FGcEventSample& B)
	{
		if (A.StartMs == B.StartMs)
		{
			return A.DurationMs > B.DurationMs;
		}
		return A.StartMs < B.StartMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeGcSummaryObject(const TArray<FGcEventSample>& Events)
{
	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	if (Events.IsEmpty())
	{
		Data->SetNumberField(TEXT("gc_count"), 0);
		Data->SetNumberField(TEXT("total_gc_ms"), 0.0);
		Data->SetNumberField(TEXT("avg_gc_ms"), 0.0);
		Data->SetNumberField(TEXT("max_gc_ms"), 0.0);
		Data->SetNumberField(TEXT("reachability_ms"), 0.0);
		Data->SetNumberField(TEXT("sweep_ms"), 0.0);
		Data->SetNumberField(TEXT("other_ms"), 0.0);
		return Data;
	}

	double Total = 0.0;
	double Max = 0.0;
	double Reachability = 0.0;
	double Sweep = 0.0;
	double Other = 0.0;
	for (const FGcEventSample& Event : Events)
	{
		Total += Event.DurationMs;
		Max = FMath::Max(Max, Event.DurationMs);
		if (Event.Stage == TEXT("reachability"))
		{
			Reachability += Event.DurationMs;
		}
		else if (Event.Stage == TEXT("sweep"))
		{
			Sweep += Event.DurationMs;
		}
		else
		{
			Other += Event.DurationMs;
		}
	}

	Data->SetNumberField(TEXT("gc_count"), Events.Num());
	Data->SetNumberField(TEXT("total_gc_ms"), Total);
	Data->SetNumberField(TEXT("avg_gc_ms"), Total / Events.Num());
	Data->SetNumberField(TEXT("max_gc_ms"), Max);
	Data->SetNumberField(TEXT("reachability_ms"), Reachability);
	Data->SetNumberField(TEXT("sweep_ms"), Sweep);
	Data->SetNumberField(TEXT("other_ms"), Other);
	return Data;
}

void AddGcEventObject(TArray<TSharedPtr<FJsonValue>>& OutData, const FGcEventSample& Event)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("scope_name"), Event.ScopeName);
	Item->SetStringField(TEXT("stage"), Event.Stage);
	Item->SetNumberField(TEXT("frame_index"), Event.FrameIndex);
	Item->SetNumberField(TEXT("thread_id"), Event.ThreadId);
	Item->SetStringField(TEXT("thread_name"), Event.ThreadName);
	Item->SetNumberField(TEXT("start_ms"), Event.StartMs);
	Item->SetNumberField(TEXT("end_ms"), Event.EndMs);
	Item->SetNumberField(TEXT("duration_ms"), Event.DurationMs);
	OutData.Add(MakeShared<FJsonValueObject>(Item));
}
}

bool HandleGcCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("gc") && Request.Action == TEXT("summary"))
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

		TArray<FGcEventSample> Events;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGcEventSamples(Context, TimeWindow, Events, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gc.summary"),
				FailureStage,
				FailureReason,
				TEXT("gc_scope_enumeration"),
				TEXT("failed to enumerate cpu scopes for gc"),
				TEXT("Trace-backed GC diagnostics are unavailable for this trace."));
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("source"), TEXT("cpu_scope_pattern"));
		Meta.Add(TEXT("match_pattern"), TEXT("CollectGarbage*,Reachability,Sweep,Purge*"));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeGcSummaryObject(Events), Meta));
		return true;
	}

	if (Request.Group == TEXT("gc") && Request.Action == TEXT("events"))
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

		TArray<FGcEventSample> Events;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGcEventSamples(Context, TimeWindow, Events, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gc.events"),
				FailureStage,
				FailureReason,
				TEXT("gc_scope_enumeration"),
				TEXT("failed to enumerate cpu scopes for gc"),
				TEXT("Trace-backed GC diagnostics are unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Events.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			AddGcEventObject(Data, Events[Index]);
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("source"), TEXT("cpu_scope_pattern"));
		Meta.Add(TEXT("match_pattern"), TEXT("CollectGarbage*,Reachability,Sweep,Purge*"));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("gc") && Request.Action == TEXT("longest"))
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

		TArray<FGcEventSample> Events;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGcEventSamples(Context, TimeWindow, Events, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gc.longest"),
				FailureStage,
				FailureReason,
				TEXT("gc_scope_enumeration"),
				TEXT("failed to enumerate cpu scopes for gc"),
				TEXT("Trace-backed GC diagnostics are unavailable for this trace."));
			return true;
		}

		Events.Sort([](const FGcEventSample& A, const FGcEventSample& B)
		{
			if (A.DurationMs == B.DurationMs)
			{
				return A.StartMs < B.StartMs;
			}
			return A.DurationMs > B.DurationMs;
		});

		const int32 TakeCount = FMath::Min(Limit, Events.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			AddGcEventObject(Data, Events[Index]);
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("source"), TEXT("cpu_scope_pattern"));
		Meta.Add(TEXT("match_pattern"), TEXT("CollectGarbage*,Reachability,Sweep,Purge*"));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
