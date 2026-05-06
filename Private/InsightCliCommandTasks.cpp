// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TasksProfiler.h"
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

enum class ETaskVisitState : uint8
{
	NotVisited,
	Visiting,
	Done,
};

struct FTaskCriticalPathResult
{
	double CompletionMs = 0.0;
	TArray<int32> Chain;
	bool bCycle = false;
	bool bPartial = false;
};

uint64 MakeCycleEdgeKey(const int32 FromTaskId, const int32 ToTaskId)
{
	return (static_cast<uint64>(static_cast<uint32>(FromTaskId)) << 32) | static_cast<uint32>(ToTaskId);
}

bool ComputeTaskCriticalPath(
	const int32 TaskId,
	const TMap<int32, int32>& TaskIndexById,
	const TArray<FTaskSample>& Tasks,
	TMap<int32, ETaskVisitState>& VisitState,
	TMap<int32, FTaskCriticalPathResult>& Memo,
	TSet<uint64>& CycleEdges)
{
	if (const ETaskVisitState* ExistingState = VisitState.Find(TaskId))
	{
		if (*ExistingState == ETaskVisitState::Done)
		{
			return true;
		}
		if (*ExistingState == ETaskVisitState::Visiting)
		{
			return false;
		}
	}

	const int32* TaskIndexPtr = TaskIndexById.Find(TaskId);
	if (TaskIndexPtr == nullptr)
	{
		return true;
	}

	const FTaskSample& Task = Tasks[*TaskIndexPtr];
	VisitState.Add(TaskId, ETaskVisitState::Visiting);

	FTaskCriticalPathResult Result;
	double BestParentCompletionMs = -1.0;
	TArray<int32> BestParentChain;

	for (const int32 DependencyId : Task.DependencyTaskIds)
	{
		const int32* DependencyIndexPtr = TaskIndexById.Find(DependencyId);
		if (DependencyIndexPtr == nullptr)
		{
			Result.bPartial = true;
			continue;
		}

		const ETaskVisitState* DependencyState = VisitState.Find(DependencyId);
		if (DependencyState != nullptr && *DependencyState == ETaskVisitState::Visiting)
		{
			Result.bCycle = true;
			CycleEdges.Add(MakeCycleEdgeKey(DependencyId, TaskId));
			continue;
		}

		if (!ComputeTaskCriticalPath(DependencyId, TaskIndexById, Tasks, VisitState, Memo, CycleEdges))
		{
			Result.bCycle = true;
			CycleEdges.Add(MakeCycleEdgeKey(DependencyId, TaskId));
			continue;
		}

		const FTaskCriticalPathResult* ParentResult = Memo.Find(DependencyId);
		if (ParentResult == nullptr)
		{
			Result.bPartial = true;
			continue;
		}

		const FTaskSample& ParentTask = Tasks[*DependencyIndexPtr];
		const double EdgeWaitMs = FMath::Max(0.0, Task.StartMs - ParentTask.EndMs);
		const double CandidateParentCompletionMs = ParentResult->CompletionMs + EdgeWaitMs;

		if (ParentResult->bCycle)
		{
			Result.bCycle = true;
		}
		if (ParentResult->bPartial)
		{
			Result.bPartial = true;
		}

		if (CandidateParentCompletionMs > BestParentCompletionMs
			|| (CandidateParentCompletionMs == BestParentCompletionMs && ParentResult->Chain.Num() > BestParentChain.Num()))
		{
			BestParentCompletionMs = CandidateParentCompletionMs;
			BestParentChain = ParentResult->Chain;
		}
	}

	Result.CompletionMs = Task.RunMs;
	if (BestParentCompletionMs >= 0.0)
	{
		Result.CompletionMs += BestParentCompletionMs;
	}
	else if (Task.DependencyTaskIds.Num() > 0)
	{
		Result.bPartial = true;
	}

	Result.Chain = MoveTemp(BestParentChain);
	Result.Chain.Add(TaskId);

	VisitState.Add(TaskId, ETaskVisitState::Done);
	Memo.Add(TaskId, MoveTemp(Result));
	return true;
}
}

bool BuildTaskTopSamples(
	const FTraceContext& Context,
	TOptional<int32> FrameIndexFilter,
	EFrameDomain FrameDomain,
	TArray<FTaskSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	bool& bOutFrameFound,
	int32& OutCycleCount)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();
	bOutFrameFound = true;
	OutCycleCount = 0;

	TOptional<double> IntervalStartSec;
	TOptional<double> IntervalEndSec;
	TArray<FFrameSample> Frames;
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

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::ITasksProvider* TasksProvider = TraceServices::ReadTasksProvider(*Session.Get());
		if (TasksProvider == nullptr)
		{
			OutFailureStage = TEXT("tasks_provider");
			OutFailureReason = TEXT("Tasks provider not available");
			return false;
		}

		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session.Get());
		const double QueryStartSec = IntervalStartSec.IsSet() ? IntervalStartSec.GetValue() : 0.0;
		const double QueryEndSec = IntervalEndSec.IsSet() ? IntervalEndSec.GetValue() : Session->GetDurationSeconds();

		TasksProvider->EnumerateTasks(QueryStartSec, QueryEndSec, TraceServices::ETaskEnumerationOption::Alive,
			[&OutSamples, &ThreadProvider, &Frames, FrameDomain](const TraceServices::FTaskInfo& TaskInfo)
			{
				const double EnqueueSec =
					(TaskInfo.ScheduledTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.ScheduledTimestamp :
					(TaskInfo.LaunchedTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.LaunchedTimestamp :
					TaskInfo.CreatedTimestamp;

				if (TaskInfo.StartedTimestamp == TraceServices::FTaskInfo::InvalidTimestamp)
				{
					return TraceServices::ETaskEnumerationResult::Continue;
				}

				const double EndSec =
					(TaskInfo.FinishedTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.FinishedTimestamp :
					(TaskInfo.CompletedTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.CompletedTimestamp :
					TaskInfo.StartedTimestamp;

				FTaskSample Task;
				Task.TaskId = (TaskInfo.Id > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(TaskInfo.Id);
				Task.TaskName = TaskInfo.DebugName != nullptr ? TaskInfo.DebugName : TEXT("<unknown>");
				Task.EnqueueMs = FMath::Max(0.0, EnqueueSec * 1000.0);
				Task.StartMs = FMath::Max(0.0, TaskInfo.StartedTimestamp * 1000.0);
				Task.EndMs = FMath::Max(Task.StartMs, EndSec * 1000.0);
				Task.QueueWaitMs = FMath::Max(0.0, Task.StartMs - Task.EnqueueMs);
				Task.RunMs = FMath::Max(0.0, Task.EndMs - Task.StartMs);
				Task.WorkerThreadId = static_cast<int32>(TaskInfo.StartedThreadId);

				const FString StartedThreadName = ThreadProvider.GetThreadName(TaskInfo.StartedThreadId);
				Task.QueueName = !StartedThreadName.IsEmpty()
					? StartedThreadName
					: FString::Printf(TEXT("ThreadToExecuteOn:%d"), TaskInfo.ThreadToExecuteOn);

				for (const TraceServices::FTaskInfo::FRelationInfo& Prerequisite : TaskInfo.Prerequisites)
				{
					const int32 PrerequisiteId = (Prerequisite.RelativeId > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(Prerequisite.RelativeId);
					Task.DependencyTaskIds.Add(PrerequisiteId);
				}

				Task.DependencyStatus = TEXT("resolved");
				Task.DependencyIssue.Reset();
				Task.CriticalPathTaskChain = Task.DependencyTaskIds;
				Task.CriticalPathTaskChain.Add(Task.TaskId);
				Task.CriticalPathDepth = Task.DependencyTaskIds.Num();
				Task.CriticalPathMs = Task.QueueWaitMs + Task.RunMs;

				if (!Frames.IsEmpty())
				{
					ResolveFrameIndicesForTimestamp(
						Frames,
						Task.StartMs,
						Task.GameFrameIndex,
						Task.RenderingFrameIndex,
						Task.FrameIndex,
						FrameDomain);
				}
				else
				{
					Task.FrameIndex = -1;
					Task.GameFrameIndex = -1;
					Task.RenderingFrameIndex = -1;
				}
				OutSamples.Add(MoveTemp(Task));
				return TraceServices::ETaskEnumerationResult::Continue;
			});
	}

	TMap<int32, int32> TaskIndexById;
	TaskIndexById.Reserve(OutSamples.Num());
	for (int32 Index = 0; Index < OutSamples.Num(); ++Index)
	{
		TaskIndexById.Add(OutSamples[Index].TaskId, Index);
	}

	TMap<int32, ETaskVisitState> VisitState;
	TMap<int32, FTaskCriticalPathResult> Memo;
	TSet<uint64> CycleEdges;
	VisitState.Reserve(OutSamples.Num());
	Memo.Reserve(OutSamples.Num());

	for (const FTaskSample& Task : OutSamples)
	{
		ComputeTaskCriticalPath(Task.TaskId, TaskIndexById, OutSamples, VisitState, Memo, CycleEdges);
	}

	OutCycleCount = CycleEdges.Num();

	for (FTaskSample& Task : OutSamples)
	{
		const FTaskCriticalPathResult* Result = Memo.Find(Task.TaskId);
		if (Result == nullptr)
		{
			Task.CriticalPathTaskChain = { Task.TaskId };
			Task.CriticalPathDepth = 0;
			Task.CriticalPathMs = Task.RunMs;
			Task.DependencyStatus = TEXT("partial");
			Task.DependencyIssue = TEXT("critical path computation unavailable");
			continue;
		}

		Task.CriticalPathTaskChain = Result->Chain;
		Task.CriticalPathDepth = FMath::Max(0, Result->Chain.Num() - 1);
		Task.CriticalPathMs = Result->CompletionMs;

		if (Result->bCycle)
		{
			Task.DependencyStatus = TEXT("cycle");
			Task.DependencyIssue = TEXT("cycle detected in task prerequisites");
		}
		else if (Result->bPartial)
		{
			Task.DependencyStatus = TEXT("partial");
			Task.DependencyIssue = TEXT("missing prerequisite task samples");
		}
		else
		{
			Task.DependencyStatus = TEXT("resolved");
			Task.DependencyIssue.Reset();
		}
	}

	OutSamples.Sort([](const FTaskSample& A, const FTaskSample& B)
	{
		if (A.QueueWaitMs == B.QueueWaitMs)
		{
			if (A.RunMs == B.RunMs)
			{
				return A.TaskId < B.TaskId;
			}
			return A.RunMs > B.RunMs;
		}
		return A.QueueWaitMs > B.QueueWaitMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeTaskObject(const FTaskSample& Task)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("task_id"), Task.TaskId);
	Item->SetNumberField(TEXT("frame_index"), Task.FrameIndex);
	Item->SetNumberField(TEXT("game_frame_index"), Task.GameFrameIndex);
	Item->SetNumberField(TEXT("rendering_frame_index"), Task.RenderingFrameIndex);
	Item->SetStringField(TEXT("task_name"), Task.TaskName);
	Item->SetStringField(TEXT("queue_name"), Task.QueueName);

	TArray<TSharedPtr<FJsonValue>> DependencyTaskIds;
	DependencyTaskIds.Reserve(Task.DependencyTaskIds.Num());
	for (const int32 DependencyTaskId : Task.DependencyTaskIds)
	{
		DependencyTaskIds.Add(MakeShared<FJsonValueNumber>(DependencyTaskId));
	}
	Item->SetArrayField(TEXT("dependency_task_ids"), DependencyTaskIds);
	Item->SetStringField(TEXT("dependency_status"), Task.DependencyStatus);
	Item->SetStringField(TEXT("dependency_issue"), Task.DependencyIssue);

	Item->SetNumberField(TEXT("enqueue_ms"), Task.EnqueueMs);
	Item->SetNumberField(TEXT("start_ms"), Task.StartMs);
	Item->SetNumberField(TEXT("end_ms"), Task.EndMs);
	Item->SetNumberField(TEXT("queue_wait_ms"), Task.QueueWaitMs);
	Item->SetNumberField(TEXT("run_ms"), Task.RunMs);
	Item->SetNumberField(TEXT("critical_path_ms"), Task.CriticalPathMs);
	Item->SetNumberField(TEXT("critical_path_depth"), Task.CriticalPathDepth);

	TArray<TSharedPtr<FJsonValue>> CriticalPathTaskChain;
	CriticalPathTaskChain.Reserve(Task.CriticalPathTaskChain.Num());
	for (const int32 TaskId : Task.CriticalPathTaskChain)
	{
		CriticalPathTaskChain.Add(MakeShared<FJsonValueNumber>(TaskId));
	}
	Item->SetArrayField(TEXT("critical_path_task_chain"), CriticalPathTaskChain);

	Item->SetNumberField(TEXT("worker_thread_id"), Task.WorkerThreadId);
	return Item;
}
}
