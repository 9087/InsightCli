// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
int32 FindBestWaitIndexForThreadOverlap(const TArray<FThreadWaitSample>& Waits, const int32 ThreadId, const double WindowStartMs, const double WindowEndMs)
{
	int32 BestIndex = INDEX_NONE;
	double BestOverlapMs = 0.0;

	for (int32 Index = 0; Index < Waits.Num(); ++Index)
	{
		const FThreadWaitSample& Candidate = Waits[Index];
		if (Candidate.ThreadId != ThreadId)
		{
			continue;
		}

		const double OverlapStartMs = FMath::Max(WindowStartMs, Candidate.BeginMs);
		const double OverlapEndMs = FMath::Min(WindowEndMs, Candidate.EndMs);
		const double OverlapMs = OverlapEndMs > OverlapStartMs ? (OverlapEndMs - OverlapStartMs) : 0.0;
		if (OverlapMs > BestOverlapMs)
		{
			BestOverlapMs = OverlapMs;
			BestIndex = Index;
		}
	}

	return BestIndex;
}

double ComputeTimeOverlapMs(const FThreadWaitSample& Wait, double StartMs, double EndMs)
{
	const double OverlapStartMs = FMath::Max(StartMs, Wait.BeginMs);
	const double OverlapEndMs = FMath::Min(EndMs, Wait.EndMs);
	if (OverlapEndMs <= OverlapStartMs)
	{
		return 0.0;
	}
	return OverlapEndMs - OverlapStartMs;
}

TSharedRef<FJsonObject> MakeWaitChainNodeObject(
	const int32 ThreadId,
	const FString& ThreadName,
	const double WaitMs,
	const int32 NextThreadId,
	const FString& NextThreadName,
	const int32 HopIndex)
{
	const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetNumberField(TEXT("hop_index"), HopIndex);
	Node->SetNumberField(TEXT("thread_id"), ThreadId);
	Node->SetStringField(TEXT("thread_name"), ThreadName);
	Node->SetNumberField(TEXT("task_id"), -1);
	Node->SetStringField(TEXT("task_name"), TEXT("unavailable"));
	Node->SetNumberField(TEXT("wait_ms"), WaitMs);
	Node->SetNumberField(TEXT("next_thread_id"), NextThreadId);
	Node->SetStringField(TEXT("next_thread_name"), NextThreadName);
	Node->SetStringField(TEXT("next_task"), TEXT("unavailable"));
	return Node;
}
}

bool HandleThreadsCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles threads/waits and threads/wait-chain; ignores unrelated command groups.
	if (Request.Group == TEXT("threads") && Request.Action == TEXT("waits"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("frame-index"), TEXT("frame-domain") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		EFrameDomain FrameDomain = EFrameDomain::Game;
		if (!TryResolveFrameDomain(Request.Args, FrameDomain, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		int32 FrameIndexFilter = -1;
		bool bHasFrameIndex = false;
		FInsightCliResponse ValidationError;
		if (!TryGetLimitAndOptionalFrameIndexFilter(Request.Args, Limit, FrameIndexFilter, bHasFrameIndex, ValidationError))
		{
			OutResponse = ValidationError;
			return true;
		}

		TArray<FThreadWaitSample> Waits;
		FString FailureStage;
		FString FailureReason;
		bool bFrameFound = true;
		if (!BuildThreadWaitSamplesTrace(Context, bHasFrameIndex ? TOptional<int32>(FrameIndexFilter) : TOptional<int32>(), FrameDomain, Waits, FailureStage, FailureReason, bFrameFound))
		{
			TMap<FString, FString> ExtraDetails;
			if (bHasFrameIndex)
			{
				ExtraDetails.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
			}
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("threads.waits"),
				FailureStage,
				FailureReason,
				TEXT("waits_extraction"),
				TEXT("failed to build thread waits"),
				TEXT("Trace-backed thread wait causality is unavailable for this trace."),
				ExtraDetails);
			return true;
		}

		if (bHasFrameIndex && !bFrameFound)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndexFilter));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Waits.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeThreadWaitObject(Waits[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		if (bHasFrameIndex)
		{
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
		}
		Meta.Add(TEXT("frame_domain"), FrameDomain == EFrameDomain::Rendering ? TEXT("rendering") : TEXT("game"));
		Meta.Add(TEXT("frame_domain_default"), TEXT("game"));
		Meta.Add(TEXT("wait_source_provider"), TEXT("ContextSwitchesProvider"));
		Meta.Add(TEXT("data_source"), TEXT("context_switch_heuristic"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("threads") && Request.Action == TEXT("wait-chain"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("thread"), TEXT("depth"), TEXT("max-chain-depth"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range"), TEXT("frame-domain") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		EFrameDomain FrameDomain = EFrameDomain::Game;
		if (!TryResolveFrameDomain(Request.Args, FrameDomain, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString ThreadFilter;
		if (!TryGetStringOption(Request.Args, TEXT("--thread"), ThreadFilter) || ThreadFilter.IsEmpty())
		{
			OutResponse = MakeOptionError(TEXT("--thread is required for threads wait-chain."));
			return true;
		}

		int32 MaxChainDepth = 5;
		const bool bHasMaxChainDepth = TryGetIntOption(Request.Args, TEXT("--max-chain-depth"), MaxChainDepth);
		if (!bHasMaxChainDepth)
		{
			TryGetIntOption(Request.Args, TEXT("--depth"), MaxChainDepth);
		}
		if (MaxChainDepth <= 0)
		{
			OutResponse = MakeOptionError(TEXT("max-chain-depth must be > 0."));
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		uint32 TargetThreadTraceId = 0;
		FString NormalizedThreadName;
		FString ResolveFailureStage;
		FString ResolveFailureReason;
		if (!ResolveCpuThreadFilterToTraceId(Context, ThreadFilter, TargetThreadTraceId, NormalizedThreadName, ResolveFailureStage, ResolveFailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("threads.wait-chain"),
				ResolveFailureStage,
				ResolveFailureReason,
				TEXT("thread_lookup"),
				TEXT("failed to resolve thread filter"),
				TEXT("Trace-backed thread lookup is unavailable for this trace."));
			return true;
		}

		const int32 TargetThreadId = static_cast<int32>(TargetThreadTraceId);

		TArray<FThreadWaitSample> Waits;
		FString FailureStage;
		FString FailureReason;
		bool bFrameFound = true;
		if (!BuildThreadWaitSamplesTrace(Context, {}, FrameDomain, Waits, FailureStage, FailureReason, bFrameFound))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("threads.wait-chain"),
				FailureStage,
				FailureReason,
				TEXT("wait_chain_extraction"),
				TEXT("failed to build thread wait chain"),
				TEXT("Trace-backed thread wait causality is unavailable for this trace."));
			return true;
		}

		double StartMs = 0.0;
		double EndMs = TNumericLimits<double>::Max();
		if (TimeWindow.StartMs.IsSet())
		{
			StartMs = TimeWindow.StartMs.GetValue();
		}
		if (TimeWindow.EndMs.IsSet())
		{
			EndMs = TimeWindow.EndMs.GetValue();
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		for (const FThreadWaitSample& Wait : Waits)
		{
			if (Wait.ThreadId != TargetThreadId)
			{
				continue;
			}

			if (ComputeTimeOverlapMs(Wait, StartMs, EndMs) <= 0.0)
			{
				continue;
			}

			TArray<TSharedPtr<FJsonValue>> Nodes;
			Nodes.Reserve(MaxChainDepth);

			FString ChainStatus = TEXT("resolved");
			FString ChainReason;
			TSet<int32> VisitedThreadIds;
			VisitedThreadIds.Reserve(MaxChainDepth + 1);

			int32 CurrentWaitIndex = static_cast<int32>(&Wait - Waits.GetData());

			int32 HopIndex = 0;
			while (HopIndex < MaxChainDepth)
			{
				const FThreadWaitSample& CurrentWait = Waits[CurrentWaitIndex];
				const int32 NextThreadId = CurrentWait.BlockerThreadId;
				const FString NextThreadName = CurrentWait.BlockerThreadName;

				Nodes.Add(MakeShared<FJsonValueObject>(
					MakeWaitChainNodeObject(
						CurrentWait.ThreadId,
						CurrentWait.ThreadName,
						CurrentWait.WaitMs,
						NextThreadId,
						NextThreadName,
						HopIndex)));

				VisitedThreadIds.Add(CurrentWait.ThreadId);

				if (NextThreadId < 0)
				{
					if (HopIndex == 0)
					{
						ChainStatus = TEXT("unresolved");
						ChainReason = TEXT("blocker_unknown");
					}
					break;
				}

				if (VisitedThreadIds.Contains(NextThreadId))
				{
					ChainStatus = TEXT("cycle");
					ChainReason = TEXT("cycle_detected");
					break;
				}

				if (HopIndex + 1 >= MaxChainDepth)
				{
					ChainStatus = TEXT("depth_limit");
					ChainReason = TEXT("max_chain_depth_reached");
					break;
				}

				const int32 NextWaitIndex = FindBestWaitIndexForThreadOverlap(Waits, NextThreadId, CurrentWait.BeginMs, CurrentWait.EndMs);
				if (NextWaitIndex == INDEX_NONE)
				{
					Nodes.Add(MakeShared<FJsonValueObject>(
						MakeWaitChainNodeObject(
							NextThreadId,
							NextThreadName,
							0.0,
							-1,
							FString(),
							HopIndex + 1)));
					ChainStatus = TEXT("unresolved");
					ChainReason = TEXT("next_wait_not_found");
					break;
				}

				CurrentWaitIndex = NextWaitIndex;
				++HopIndex;
			}

			const TSharedRef<FJsonObject> ChainItem = MakeShared<FJsonObject>();
			ChainItem->SetNumberField(TEXT("frame_index"), Wait.FrameIndex);
			ChainItem->SetNumberField(TEXT("begin_ms"), Wait.BeginMs);
			ChainItem->SetNumberField(TEXT("end_ms"), Wait.EndMs);
			ChainItem->SetNumberField(TEXT("wait_ms"), Wait.WaitMs);
			ChainItem->SetNumberField(TEXT("blocker_overlap_ratio"), Wait.BlockerOverlapRatio);
			ChainItem->SetNumberField(TEXT("chain_depth"), Nodes.Num());
			ChainItem->SetStringField(TEXT("chain_status"), ChainStatus);
			ChainItem->SetStringField(TEXT("unresolved_reason"), ChainReason);
			ChainItem->SetStringField(TEXT("confidence"), Wait.BlockerConfidence >= 0.7 ? TEXT("high") : (Wait.BlockerConfidence >= 0.3 ? TEXT("medium") : TEXT("low")));
			ChainItem->SetArrayField(TEXT("chain"), Nodes);
			Data.Add(MakeShared<FJsonValueObject>(ChainItem));
		}

		Data.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject> ObjA = A->AsObject();
			const TSharedPtr<FJsonObject> ObjB = B->AsObject();
			const double WaitA = ObjA.IsValid() ? ObjA->GetNumberField(TEXT("wait_ms")) : 0.0;
			const double WaitB = ObjB.IsValid() ? ObjB->GetNumberField(TEXT("wait_ms")) : 0.0;
			return WaitA > WaitB;
		});

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("thread"), ThreadFilter);
		Meta.Add(TEXT("thread_resolved"), NormalizedThreadName);
		Meta.Add(TEXT("depth"), FString::FromInt(MaxChainDepth));
		Meta.Add(TEXT("max_chain_depth_used"), FString::FromInt(MaxChainDepth));
		Meta.Add(TEXT("frame_domain"), FrameDomain == EFrameDomain::Rendering ? TEXT("rendering") : TEXT("game"));
		Meta.Add(TEXT("frame_domain_default"), TEXT("game"));
		Meta.Add(TEXT("wait_source_provider"), TEXT("ContextSwitchesProvider"));
		Meta.Add(TEXT("data_source"), TEXT("context_switch_heuristic"));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
