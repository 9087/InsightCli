// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/ContextSwitches.h"
#include "TraceServices/Model/Threads.h"

namespace UE::InsightCli::Internal
{
namespace
{
int32 ResolveDomainFrameIndex(const FFrameSample& Frame, const EFrameDomain FrameDomain)
{
	if (FrameDomain == EFrameDomain::Rendering)
	{
		return Frame.RenderingFrameIndex;
	}

	return Frame.GameFrameIndex;
}

FString ClassifyHeuristicWaitType(const FString& ThreadName, const double WaitMs)
{
	const FString NormalizedThreadName = ThreadName.ToLower();
	if (NormalizedThreadName.Contains(TEXT("rhi")) || NormalizedThreadName.Contains(TEXT("render")) || NormalizedThreadName.Contains(TEXT("gpu")))
	{
		return TEXT("fence");
	}
	if (NormalizedThreadName.Contains(TEXT("io")) || NormalizedThreadName.Contains(TEXT("file")))
	{
		return TEXT("io");
	}
	if (NormalizedThreadName.Contains(TEXT("task")) || NormalizedThreadName.Contains(TEXT("worker")))
	{
		return TEXT("task_dep");
	}
	if (WaitMs >= 1.0)
	{
		return TEXT("event");
	}

	return TEXT("sleep");
}

void ResolveFrameIndicesForTimestamp(
	const TArray<FFrameSample>& Frames,
	const double TimestampMs,
	int32& OutGameFrameIndex,
	int32& OutRenderingFrameIndex,
	int32& OutSelectedFrameIndex,
	const EFrameDomain FrameDomain)
{
	OutGameFrameIndex = -1;
	OutRenderingFrameIndex = -1;
	OutSelectedFrameIndex = -1;

	for (const FFrameSample& Frame : Frames)
	{
		if (Frame.FrameStartMs <= TimestampMs && TimestampMs <= Frame.FrameEndMs)
		{
			OutGameFrameIndex = Frame.GameFrameIndex;
			OutRenderingFrameIndex = Frame.RenderingFrameIndex;
			OutSelectedFrameIndex = ResolveDomainFrameIndex(Frame, FrameDomain);
			return;
		}
	}
}
}

bool BuildThreadWaitSamplesTrace(
	const FTraceContext& Context,
	TOptional<int32> FrameIndexFilter,
	EFrameDomain FrameDomain,
	TArray<FThreadWaitSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	bool& bOutFrameFound)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();
	bOutFrameFound = true;

	TArray<FFrameSample> Frames;
	TOptional<double> IntervalStartSec;
	TOptional<double> IntervalEndSec;
	if (FrameIndexFilter.IsSet())
	{
		Frames = BuildFrameSamples(Context);
		const FFrameSample* FoundFrame = Frames.FindByPredicate([&FrameIndexFilter, FrameDomain](const FFrameSample& Frame)
		{
			return ResolveDomainFrameIndex(Frame, FrameDomain) == FrameIndexFilter.GetValue();
		});

		if (FoundFrame == nullptr)
		{
			bOutFrameFound = false;
			return true;
		}

		IntervalStartSec = FMath::Max(0.0, FoundFrame->FrameStartMs / 1000.0);
		IntervalEndSec = FMath::Max(IntervalStartSec.GetValue() + KINDA_SMALL_NUMBER, FoundFrame->FrameEndMs / 1000.0);
	}
	else
	{
		Frames = BuildFrameSamples(Context);
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	const double QueryStartSec = IntervalStartSec.IsSet() ? IntervalStartSec.GetValue() : 0.0;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const double QueryEndSec = IntervalEndSec.IsSet() ? IntervalEndSec.GetValue() : Session->GetDurationSeconds();
		const TraceServices::IContextSwitchesProvider* ContextSwitchesProvider = TraceServices::ReadContextSwitchesProvider(*Session.Get());
		if (ContextSwitchesProvider == nullptr || !ContextSwitchesProvider->HasData())
		{
			return true;
		}

		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session.Get());

		struct FThreadRunSegment
		{
			double StartSec = 0.0;
			double EndSec = 0.0;
		};

		TMap<int32, FString> ThreadNames;
		TMap<int32, TArray<FThreadRunSegment>> ThreadRunSegments;

		ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
		{
			const int32 ThreadId = static_cast<int32>(ThreadInfo.Id);
			ThreadNames.Add(ThreadId, ThreadInfo.Name != nullptr ? ThreadInfo.Name : FString());

			TArray<FThreadRunSegment> Segments;
			ContextSwitchesProvider->EnumerateContextSwitches(ThreadInfo.Id, QueryStartSec, QueryEndSec,
				[&Segments, QueryStartSec](const TraceServices::FContextSwitch& ContextSwitch)
				{
					const double StartSec = FMath::Max(QueryStartSec, ContextSwitch.Start);
					const double EndSec = FMath::Max(StartSec, ContextSwitch.End);
					if (EndSec > StartSec)
					{
						FThreadRunSegment& Segment = Segments.AddDefaulted_GetRef();
						Segment.StartSec = StartSec;
						Segment.EndSec = EndSec;
					}
					return TraceServices::EContextSwitchEnumerationResult::Continue;
				});

			ThreadRunSegments.Add(ThreadId, MoveTemp(Segments));
		});

		ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
		{
			const int32 CurrentThreadId = static_cast<int32>(ThreadInfo.Id);
			double LastRunEndSec = QueryStartSec;
			bool bSeenAnyRun = false;

			const auto AddWaitGap = [&](double GapStartSec, double GapEndSec)
			{
				if (GapEndSec <= GapStartSec)
				{
					return;
				}

				FThreadWaitSample Wait;
				if (FrameIndexFilter.IsSet())
				{
					Wait.FrameIndex = FrameIndexFilter.GetValue();
					Wait.GameFrameIndex = (FrameDomain == EFrameDomain::Game) ? FrameIndexFilter.GetValue() : -1;
					Wait.RenderingFrameIndex = (FrameDomain == EFrameDomain::Rendering) ? FrameIndexFilter.GetValue() : -1;
				}
				else
				{
					ResolveFrameIndicesForTimestamp(
						Frames,
						GapStartSec * 1000.0,
						Wait.GameFrameIndex,
						Wait.RenderingFrameIndex,
						Wait.FrameIndex,
						FrameDomain);
				}
				Wait.ThreadId = static_cast<int32>(ThreadInfo.Id);
				Wait.ThreadName = ThreadInfo.Name != nullptr ? ThreadInfo.Name : FString();
				Wait.WaitTypeFromTrace = TEXT("Scheduler");
				Wait.WaitObject = TEXT("ContextSwitchGap");
				Wait.WaitObjectAddress = TEXT("unavailable");
				Wait.WaitSourceProvider = TEXT("ContextSwitchesProvider");
				Wait.BeginMs = GapStartSec * 1000.0;
				Wait.EndMs = GapEndSec * 1000.0;
				Wait.WaitMs = Wait.EndMs - Wait.BeginMs;
				Wait.WaitType = ClassifyHeuristicWaitType(Wait.ThreadName, Wait.WaitMs);

				double BestOverlapSec = 0.0;
				int32 BestBlockerThreadId = -1;
				for (const TPair<int32, TArray<FThreadRunSegment>>& Pair : ThreadRunSegments)
				{
					const int32 CandidateThreadId = Pair.Key;
					if (CandidateThreadId == CurrentThreadId)
					{
						continue;
					}

					double OverlapSec = 0.0;
					for (const FThreadRunSegment& Segment : Pair.Value)
					{
						const double OverlapStartSec = FMath::Max(GapStartSec, Segment.StartSec);
						const double OverlapEndSec = FMath::Min(GapEndSec, Segment.EndSec);
						if (OverlapEndSec > OverlapStartSec)
						{
							OverlapSec += (OverlapEndSec - OverlapStartSec);
						}
					}

					if (OverlapSec > BestOverlapSec)
					{
						BestOverlapSec = OverlapSec;
						BestBlockerThreadId = CandidateThreadId;
					}
				}

				Wait.BlockedToBlockerThreadChain.Reset();
				Wait.BlockedToBlockerThreadChain.Add(Wait.ThreadId);
				if (BestBlockerThreadId >= 0)
				{
					const double BestOverlapMs = BestOverlapSec * 1000.0;
					Wait.BlockerOverlapRatio = Wait.WaitMs > 0.0 ? FMath::Clamp(BestOverlapMs / Wait.WaitMs, 0.0, 1.0) : 0.0;
					Wait.BlockerConfidence = Wait.BlockerOverlapRatio;
					Wait.OwnerThreadId = BestBlockerThreadId;
					Wait.BlockerThreadId = BestBlockerThreadId;
					const FString* OwnerName = ThreadNames.Find(BestBlockerThreadId);
					Wait.OwnerThreadName = OwnerName != nullptr ? *OwnerName : FString();
					Wait.BlockerThreadName = Wait.OwnerThreadName;
					Wait.BlockedToBlockerThreadChain.Add(BestBlockerThreadId);
					Wait.ChainDepth = 1;
					Wait.ChainStatus = TEXT("resolved");
					Wait.UnresolvedReason.Reset();
				}
				else
				{
					Wait.BlockerOverlapRatio = 0.0;
					Wait.BlockerConfidence = 0.0;
					Wait.OwnerThreadId = -1;
					Wait.OwnerThreadName.Reset();
					Wait.BlockerThreadId = -1;
					Wait.BlockerThreadName.Reset();
					Wait.ChainDepth = 0;
					Wait.ChainStatus = TEXT("unresolved");
					Wait.UnresolvedReason = TEXT("blocker_unknown");
				}

				OutSamples.Add(MoveTemp(Wait));
			};

			ContextSwitchesProvider->EnumerateContextSwitches(ThreadInfo.Id, QueryStartSec, QueryEndSec,
				[&](const TraceServices::FContextSwitch& ContextSwitch)
				{
					bSeenAnyRun = true;
					const double GapStartSec = LastRunEndSec;
					const double GapEndSec = FMath::Max(QueryStartSec, ContextSwitch.Start);
					AddWaitGap(GapStartSec, GapEndSec);

					LastRunEndSec = FMath::Max(LastRunEndSec, ContextSwitch.End);
					return TraceServices::EContextSwitchEnumerationResult::Continue;
				});

			if (bSeenAnyRun)
			{
				AddWaitGap(LastRunEndSec, QueryEndSec);
			}
		});
	}

	OutSamples.Sort([](const FThreadWaitSample& A, const FThreadWaitSample& B)
	{
		if (A.WaitMs == B.WaitMs)
		{
			if (A.FrameIndex == B.FrameIndex)
			{
				return A.ThreadId < B.ThreadId;
			}
			return A.FrameIndex < B.FrameIndex;
		}
		return A.WaitMs > B.WaitMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeThreadWaitObject(const FThreadWaitSample& Wait)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), Wait.FrameIndex);
	Item->SetNumberField(TEXT("game_frame_index"), Wait.GameFrameIndex);
	Item->SetNumberField(TEXT("rendering_frame_index"), Wait.RenderingFrameIndex);
	Item->SetNumberField(TEXT("thread_id"), Wait.ThreadId);
	Item->SetStringField(TEXT("thread_name"), Wait.ThreadName);
	Item->SetStringField(TEXT("wait_type"), Wait.WaitType);
	Item->SetStringField(TEXT("wait_type_from_trace"), Wait.WaitTypeFromTrace);
	Item->SetStringField(TEXT("wait_object"), Wait.WaitObject);
	Item->SetStringField(TEXT("wait_object_address"), Wait.WaitObjectAddress);
	Item->SetStringField(TEXT("wait_source_provider"), Wait.WaitSourceProvider);
	Item->SetNumberField(TEXT("wait_ms"), Wait.WaitMs);
	Item->SetNumberField(TEXT("blocker_overlap_ratio"), Wait.BlockerOverlapRatio);
	Item->SetNumberField(TEXT("blocker_confidence"), Wait.BlockerConfidence);
	Item->SetNumberField(TEXT("owner_thread_id"), Wait.OwnerThreadId);
	Item->SetStringField(TEXT("owner_thread_name"), Wait.OwnerThreadName);
	Item->SetNumberField(TEXT("blocker_thread_id"), Wait.BlockerThreadId);
	Item->SetStringField(TEXT("blocker_thread_name"), Wait.BlockerThreadName);
	Item->SetNumberField(TEXT("chain_depth"), Wait.ChainDepth);
	Item->SetStringField(TEXT("chain_status"), Wait.ChainStatus);
	Item->SetStringField(TEXT("unresolved_reason"), Wait.UnresolvedReason);

	TArray<TSharedPtr<FJsonValue>> ChainThreadIds;
	ChainThreadIds.Reserve(Wait.BlockedToBlockerThreadChain.Num());
	for (const int32 ThreadId : Wait.BlockedToBlockerThreadChain)
	{
		ChainThreadIds.Add(MakeShared<FJsonValueNumber>(ThreadId));
	}
	Item->SetArrayField(TEXT("blocked_to_blocker_thread_chain"), ChainThreadIds);

	Item->SetNumberField(TEXT("begin_ms"), Wait.BeginMs);
	Item->SetNumberField(TEXT("end_ms"), Wait.EndMs);
	return Item;
}
}
