// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TasksProfiler.h"
#include "TraceServices/Model/Threads.h"

namespace UE::InsightCli::Internal
{
bool BuildTaskTopSamples(
	const FTraceContext& Context,
	TOptional<int32> FrameIndexFilter,
	TArray<FTaskSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	bool& bOutFrameFound)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();
	bOutFrameFound = true;

	TOptional<double> IntervalStartSec;
	TOptional<double> IntervalEndSec;
	TArray<FFrameSample> Frames;
	if (FrameIndexFilter.IsSet())
	{
		Frames = BuildFrameSamples(Context);
		const FFrameSample* FoundFrame = Frames.FindByPredicate([&FrameIndexFilter](const FFrameSample& Frame)
		{
			return Frame.FrameIndex == FrameIndexFilter.GetValue();
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
			[&OutSamples, &ThreadProvider, &Frames, &GetFrameIndexForTimestampMs](const TraceServices::FTaskInfo& TaskInfo)
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

				Task.FrameIndex = Frames.IsEmpty() ? -1 : GetFrameIndexForTimestampMs(Task.StartMs);
				OutSamples.Add(MoveTemp(Task));
				return TraceServices::ETaskEnumerationResult::Continue;
			});
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
