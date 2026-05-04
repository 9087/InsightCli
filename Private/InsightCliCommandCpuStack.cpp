// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Algo/Reverse.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UE::InsightCli::Internal
{
bool BuildCpuStackObject(
	const FTraceContext& Context,
	int32 FrameIndex,
	const TOptional<uint32>& CpuThreadId,
	int32 StackLimit,
	const FString& View,
	TSharedPtr<FJsonObject>& OutObject,
	bool& bOutFound,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutObject.Reset();
	bOutFound = false;
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	const FFrameSample* FoundFrame = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
	{
		return Item.FrameIndex == FrameIndex;
	});

	if (FoundFrame == nullptr)
	{
		return true;
	}

	bOutFound = true;

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	struct FTimerSnapshot
	{
		FString Name;
		FString File;
		int32 Line = 0;
		bool bValid = false;
	};

	struct FRangeEvent
	{
		double StartSec = 0.0;
		double EndSec = 0.0;
		uint32 Depth = 0;
		uint32 TimerId = uint32(-1);
	};

	const auto InferModuleName = [](const FString& FilePath, const FString& SymbolName) -> FString
	{
		if (FilePath.Contains(TEXT("/Engine/"), ESearchCase::IgnoreCase) || FilePath.Contains(TEXT("\\Engine\\"), ESearchCase::IgnoreCase))
		{
			return TEXT("Engine");
		}
		if (FilePath.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase) || FilePath.Contains(TEXT("\\Game\\"), ESearchCase::IgnoreCase))
		{
			return TEXT("Game");
		}
		if (SymbolName.Contains(TEXT("::"), ESearchCase::CaseSensitive))
		{
			return TEXT("Trace");
		}
		return TEXT("Unknown");
	};

	uint32 TargetThreadId = 0;
	FString TargetThreadName;
	uint32 TimelineIndex = uint32(-1);
	TArray<FTimerSnapshot> Timers;
	TArray<FRangeEvent> RangeEvents;

	const double IntervalStartSec = FMath::Max(0.0, FoundFrame->FrameStartMs / 1000.0);
	const double IntervalEndSec = FMath::Max(IntervalStartSec + KINDA_SMALL_NUMBER, FoundFrame->FrameEndMs / 1000.0);

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());

		const TraceServices::ITimingProfilerProvider* TimingProfilerProvider = TraceServices::ReadTimingProfilerProvider(*Session.Get());
		if (TimingProfilerProvider == nullptr)
		{
			OutFailureStage = TEXT("timing_provider");
			OutFailureReason = TEXT("TimingProfiler provider not available");
			return false;
		}

		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session.Get());

		TimingProfilerProvider->ReadTimers([&Timers](const TraceServices::ITimingProfilerTimerReader& TimerReader)
		{
			const uint32 TimerCount = TimerReader.GetTimerCount();
			Timers.SetNum(static_cast<int32>(TimerCount));
			for (uint32 TimerId = 0; TimerId < TimerCount; ++TimerId)
			{
				const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(TimerId);
				if (Timer == nullptr)
				{
					continue;
				}

				FTimerSnapshot& Snapshot = Timers[static_cast<int32>(TimerId)];
				Snapshot.Name = Timer->Name != nullptr ? Timer->Name : TEXT("<unknown>");
				Snapshot.File = Timer->File != nullptr ? Timer->File : TEXT("");
				Snapshot.Line = static_cast<int32>(Timer->Line);
				Snapshot.bValid = true;
			}
		});

		if (CpuThreadId.IsSet())
		{
			TargetThreadId = CpuThreadId.GetValue();
			TargetThreadName = ThreadProvider.GetThreadName(TargetThreadId);
			if (!TimingProfilerProvider->GetCpuThreadTimelineIndex(TargetThreadId, TimelineIndex))
			{
				OutFailureStage = TEXT("timeline_lookup");
				OutFailureReason = TEXT("cpu thread timeline not found");
				return false;
			}
		}
		else
		{
			bool bFoundPreferredThread = false;
			ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
			{
				if (bFoundPreferredThread || ThreadInfo.Name == nullptr)
				{
					return;
				}

				const FString Name = ThreadInfo.Name;
				if (!Name.Contains(TEXT("GameThread"), ESearchCase::IgnoreCase))
				{
					return;
				}

				uint32 CandidateTimelineIndex = uint32(-1);
				if (!TimingProfilerProvider->GetCpuThreadTimelineIndex(ThreadInfo.Id, CandidateTimelineIndex))
				{
					return;
				}

				TargetThreadId = ThreadInfo.Id;
				TargetThreadName = Name;
				TimelineIndex = CandidateTimelineIndex;
				bFoundPreferredThread = true;
			});

			if (!bFoundPreferredThread)
			{
				ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
				{
					if (TimelineIndex != uint32(-1))
					{
						return;
					}

					uint32 CandidateTimelineIndex = uint32(-1);
					if (!TimingProfilerProvider->GetCpuThreadTimelineIndex(ThreadInfo.Id, CandidateTimelineIndex))
					{
						return;
					}

					TargetThreadId = ThreadInfo.Id;
					TargetThreadName = ThreadInfo.Name != nullptr ? ThreadInfo.Name : FString();
					TimelineIndex = CandidateTimelineIndex;
				});
			}

			if (TimelineIndex == uint32(-1))
			{
				OutFailureStage = TEXT("thread_lookup");
				OutFailureReason = TEXT("no cpu thread timeline available in trace");
				return false;
			}
		}

		if (!TimingProfilerProvider->ReadTimeline(TimelineIndex, [&RangeEvents, IntervalStartSec, IntervalEndSec](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(IntervalStartSec, IntervalEndSec, [&RangeEvents](double StartTime, double EndTime, uint32 Depth, const TraceServices::FTimingProfilerEvent& Event)
			{
				if (EndTime <= StartTime)
				{
					return TraceServices::EEventEnumerate::Continue;
				}

				FRangeEvent Row;
				Row.StartSec = StartTime;
				Row.EndSec = EndTime;
				Row.Depth = Depth;
				Row.TimerId = Event.TimerIndex;
				RangeEvents.Add(Row);
				return TraceServices::EEventEnumerate::Continue;
			});
		}))
		{
			OutFailureStage = TEXT("timeline_read");
			OutFailureReason = TEXT("failed to read cpu thread timeline");
			return false;
		}
	}

	const auto BuildActiveChain = [&RangeEvents](const double SampleTimeSec)
	{
		TArray<FRangeEvent> Active;
		for (const FRangeEvent& Row : RangeEvents)
		{
			if (Row.StartSec <= SampleTimeSec && SampleTimeSec < Row.EndSec)
			{
				Active.Add(Row);
			}
		}

		if (Active.IsEmpty())
		{
			return Active;
		}

		Active.Sort([](const FRangeEvent& A, const FRangeEvent& B)
		{
			if (A.Depth == B.Depth)
			{
				return A.StartSec < B.StartSec;
			}
			return A.Depth < B.Depth;
		});

		TMap<uint32, FRangeEvent> BestByDepth;
		for (const FRangeEvent& Row : Active)
		{
			const FRangeEvent* Existing = BestByDepth.Find(Row.Depth);
			if (Existing == nullptr || Row.StartSec > Existing->StartSec)
			{
				BestByDepth.Add(Row.Depth, Row);
			}
		}

		TArray<uint32> Depths;
		BestByDepth.GetKeys(Depths);
		Depths.Sort();

		TArray<FRangeEvent> Chain;
		Chain.Reserve(Depths.Num());
		for (const uint32 Depth : Depths)
		{
			if (const FRangeEvent* Found = BestByDepth.Find(Depth))
			{
				Chain.Add(*Found);
			}
		}

		return Chain;
	};

	const double MidSec = (IntervalStartSec + IntervalEndSec) * 0.5;
	TArray<FRangeEvent> Chain = BuildActiveChain(MidSec);
	if (Chain.IsEmpty() && !RangeEvents.IsEmpty())
	{
		const FRangeEvent* Longest = &RangeEvents[0];
		for (const FRangeEvent& Row : RangeEvents)
		{
			if ((Row.EndSec - Row.StartSec) > (Longest->EndSec - Longest->StartSec))
			{
				Longest = &Row;
			}
		}

		const double LongestMidSec = (Longest->StartSec + Longest->EndSec) * 0.5;
		Chain = BuildActiveChain(LongestMidSec);
	}

	if (StackLimit > 0 && Chain.Num() > StackLimit)
	{
		Chain.RemoveAt(0, Chain.Num() - StackLimit, EAllowShrinking::No);
	}

	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), FoundFrame->FrameIndex);
	Item->SetNumberField(TEXT("thread_id"), static_cast<double>(TargetThreadId));
	Item->SetStringField(TEXT("thread_name"), TargetThreadName.IsEmpty() ? FString::Printf(TEXT("Thread-%u"), TargetThreadId) : TargetThreadName);

	if (!Chain.IsEmpty())
	{
		const FRangeEvent& Leaf = Chain.Last();
		const FString LeafName = (Leaf.TimerId < static_cast<uint32>(Timers.Num()) && Timers[Leaf.TimerId].bValid) ? Timers[Leaf.TimerId].Name : TEXT("<unknown>");
		const double TotalMs = (Leaf.EndSec - Leaf.StartSec) * 1000.0;

		double DirectChildrenMs = 0.0;
		for (const FRangeEvent& Row : RangeEvents)
		{
			if (Row.Depth != Leaf.Depth + 1)
			{
				continue;
			}
			const double OverlapStart = FMath::Max(Row.StartSec, Leaf.StartSec);
			const double OverlapEnd = FMath::Min(Row.EndSec, Leaf.EndSec);
			if (OverlapEnd > OverlapStart)
			{
				DirectChildrenMs += (OverlapEnd - OverlapStart) * 1000.0;
			}
		}

		Item->SetStringField(TEXT("scope_name"), LeafName);
		Item->SetNumberField(TEXT("total_ms"), TotalMs);
		Item->SetNumberField(TEXT("self_ms"), FMath::Max(0.0, TotalMs - DirectChildrenMs));
	}
	else
	{
		Item->SetStringField(TEXT("scope_name"), TEXT(""));
		Item->SetNumberField(TEXT("total_ms"), 0.0);
		Item->SetNumberField(TEXT("self_ms"), 0.0);
	}

	TArray<FRangeEvent> DisplayChain = Chain;
	if (View.Equals(TEXT("bottom-up"), ESearchCase::IgnoreCase))
	{
		Algo::Reverse(DisplayChain);
	}

	TArray<TSharedPtr<FJsonValue>> Stack;
	if (View.Equals(TEXT("leaf"), ESearchCase::IgnoreCase))
	{
		struct FLeafAggregate
		{
			FString Name;
			FString File;
			int32 Line = 0;
			double SelfMs = 0.0;
			int32 CallCount = 0;
		};

		TMap<FString, FLeafAggregate> AggregateByName;
		for (const FRangeEvent& Row : RangeEvents)
		{
			if (Row.EndSec <= Row.StartSec)
			{
				continue;
			}

			double DirectChildrenMs = 0.0;
			for (const FRangeEvent& Child : RangeEvents)
			{
				if (Child.Depth != Row.Depth + 1)
				{
					continue;
				}
				const double OverlapStart = FMath::Max(Child.StartSec, Row.StartSec);
				const double OverlapEnd = FMath::Min(Child.EndSec, Row.EndSec);
				if (OverlapEnd > OverlapStart)
				{
					DirectChildrenMs += (OverlapEnd - OverlapStart) * 1000.0;
				}
			}

			const double TotalMs = (Row.EndSec - Row.StartSec) * 1000.0;
			const double SelfMs = FMath::Max(0.0, TotalMs - DirectChildrenMs);
			if (SelfMs <= 0.0)
			{
				continue;
			}

			FString Name = TEXT("<unknown>");
			FString File;
			int32 Line = 0;
			if (Row.TimerId < static_cast<uint32>(Timers.Num()) && Timers[Row.TimerId].bValid)
			{
				const FTimerSnapshot& Snapshot = Timers[Row.TimerId];
				Name = Snapshot.Name;
				File = Snapshot.File;
				Line = Snapshot.Line;
			}

			FLeafAggregate* Existing = AggregateByName.Find(Name);
			if (Existing == nullptr)
			{
				FLeafAggregate NewEntry;
				NewEntry.Name = Name;
				NewEntry.File = File;
				NewEntry.Line = Line;
				NewEntry.SelfMs = SelfMs;
				NewEntry.CallCount = 1;
				AggregateByName.Add(Name, MoveTemp(NewEntry));
			}
			else
			{
				Existing->SelfMs += SelfMs;
				Existing->CallCount += 1;
				if (Existing->File.IsEmpty() && !File.IsEmpty())
				{
					Existing->File = File;
					Existing->Line = Line;
				}
			}
		}

		TArray<FLeafAggregate> LeafRows;
		AggregateByName.GenerateValueArray(LeafRows);
		LeafRows.Sort([](const FLeafAggregate& A, const FLeafAggregate& B)
		{
			if (A.SelfMs == B.SelfMs)
			{
				return A.Name < B.Name;
			}
			return A.SelfMs > B.SelfMs;
		});

		if (StackLimit > 0 && LeafRows.Num() > StackLimit)
		{
			LeafRows.SetNum(StackLimit, EAllowShrinking::No);
		}

		Stack.Reserve(LeafRows.Num());
		for (const FLeafAggregate& Row : LeafRows)
		{
			const TSharedRef<FJsonObject> StackItem = MakeShared<FJsonObject>();
			StackItem->SetStringField(TEXT("function"), Row.Name);
			StackItem->SetStringField(TEXT("module"), InferModuleName(Row.File, Row.Name));
			StackItem->SetNumberField(TEXT("self_ms"), Row.SelfMs);
			StackItem->SetNumberField(TEXT("call_count"), Row.CallCount);
			if (!Row.File.IsEmpty())
			{
				StackItem->SetStringField(TEXT("file"), Row.File);
			}
			if (Row.Line > 0)
			{
				StackItem->SetNumberField(TEXT("line"), Row.Line);
			}
			Stack.Add(MakeShared<FJsonValueObject>(StackItem));
		}
	}
	else
	{
		Stack.Reserve(DisplayChain.Num());
		for (const FRangeEvent& Row : DisplayChain)
		{
			const TSharedRef<FJsonObject> StackItem = MakeShared<FJsonObject>();
			FString Name = TEXT("<unknown>");
			FString File;
			int32 Line = 0;
			if (Row.TimerId < static_cast<uint32>(Timers.Num()) && Timers[Row.TimerId].bValid)
			{
				const FTimerSnapshot& Snapshot = Timers[Row.TimerId];
				Name = Snapshot.Name;
				File = Snapshot.File;
				Line = Snapshot.Line;
			}

			StackItem->SetStringField(TEXT("function"), Name);
			StackItem->SetStringField(TEXT("module"), InferModuleName(File, Name));
			if (!File.IsEmpty())
			{
				StackItem->SetStringField(TEXT("file"), File);
			}
			if (Line > 0)
			{
				StackItem->SetNumberField(TEXT("line"), Line);
			}
			Stack.Add(MakeShared<FJsonValueObject>(StackItem));
		}
	}

	Item->SetArrayField(TEXT("stack"), Stack);
	OutObject = Item;
	return true;
}
}