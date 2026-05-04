// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FTaskPathCandidate
{
	TArray<int32> TaskIds;
	double TotalDurationMs = 0.0;
	bool bPartial = false;
};

bool BuildLongestPathForTask(
	const int32 TaskId,
	const TMap<int32, const FTaskSample*>& TaskMap,
	TMap<int32, FTaskPathCandidate>& Memo,
	TSet<int32>& Visiting,
	FTaskPathCandidate& OutCandidate)
{
	if (const FTaskPathCandidate* Existing = Memo.Find(TaskId))
	{
		OutCandidate = *Existing;
		return true;
	}

	if (Visiting.Contains(TaskId))
	{
		OutCandidate.TaskIds = { TaskId };
		OutCandidate.TotalDurationMs = 0.0;
		OutCandidate.bPartial = true;
		Memo.Add(TaskId, OutCandidate);
		return true;
	}

	const FTaskSample* Task = TaskMap.FindRef(TaskId);
	if (Task == nullptr)
	{
		OutCandidate.TaskIds = { TaskId };
		OutCandidate.TotalDurationMs = 0.0;
		OutCandidate.bPartial = true;
		Memo.Add(TaskId, OutCandidate);
		return true;
	}

	Visiting.Add(TaskId);

	FTaskPathCandidate BestParent;
	BestParent.TaskIds.Reset();
	BestParent.TotalDurationMs = 0.0;
	BestParent.bPartial = false;

	for (const int32 DependencyId : Task->DependencyTaskIds)
	{
		FTaskPathCandidate ParentCandidate;
		if (!BuildLongestPathForTask(DependencyId, TaskMap, Memo, Visiting, ParentCandidate))
		{
			continue;
		}

		if (!TaskMap.Contains(DependencyId))
		{
			ParentCandidate.bPartial = true;
		}

		if (ParentCandidate.TotalDurationMs > BestParent.TotalDurationMs ||
			(ParentCandidate.TotalDurationMs == BestParent.TotalDurationMs && ParentCandidate.TaskIds.Num() > BestParent.TaskIds.Num()))
		{
			BestParent = MoveTemp(ParentCandidate);
		}
	}

	OutCandidate = BestParent;
	OutCandidate.TaskIds.Add(TaskId);
	OutCandidate.TotalDurationMs += Task->RunMs;
	OutCandidate.bPartial = OutCandidate.bPartial || Task->DependencyTaskIds.Num() > 0 && BestParent.TaskIds.IsEmpty();

	Visiting.Remove(TaskId);
	Memo.Add(TaskId, OutCandidate);
	return true;
}
}

bool HandleTasksCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles tasks/top and tasks/critical-path; returns false outside tasks group.
	if (Request.Group == TEXT("tasks") && Request.Action == TEXT("top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("frame-index") }, UnknownOptionError))
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

		TArray<FTaskSample> Tasks;
		FString FailureStage;
		FString FailureReason;
		bool bFrameFound = true;
		if (!BuildTaskTopSamples(Context, bHasFrameIndex ? TOptional<int32>(FrameIndexFilter) : TOptional<int32>(), Tasks, FailureStage, FailureReason, bFrameFound))
		{
			TMap<FString, FString> ExtraDetails;
			if (bHasFrameIndex)
			{
				ExtraDetails.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
			}
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("tasks.top"),
				FailureStage,
				FailureReason,
				TEXT("tasks_extraction"),
				TEXT("failed to build task diagnostics"),
				TEXT("Trace-backed task graph diagnostics are unavailable for this trace."),
				ExtraDetails);
			return true;
		}

		if (bHasFrameIndex && !bFrameFound)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndexFilter));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Tasks.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeTaskObject(Tasks[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		if (bHasFrameIndex)
		{
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndexFilter));
		}
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("tasks") && Request.Action == TEXT("critical-path"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("top") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 FrameIndex = -1;
		if (!TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			OutResponse = MakeOptionError(TEXT("--frame-index is required for tasks critical-path."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}

		int32 TopK = 3;
		if (TryGetIntOption(Request.Args, TEXT("--top"), TopK) && TopK <= 0)
		{
			OutResponse = MakeOptionError(TEXT("top must be > 0."));
			return true;
		}

		TArray<FTaskSample> Tasks;
		FString FailureStage;
		FString FailureReason;
		bool bFrameFound = true;
		if (!BuildTaskTopSamples(Context, FrameIndex, Tasks, FailureStage, FailureReason, bFrameFound))
		{
			TMap<FString, FString> ExtraDetails;
			ExtraDetails.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("tasks.critical-path"),
				FailureStage,
				FailureReason,
				TEXT("tasks_critical_path"),
				TEXT("failed to build task critical path"),
				TEXT("Trace-backed task graph critical path is unavailable for this trace."),
				ExtraDetails);
			return true;
		}

		if (!bFrameFound)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		TMap<int32, const FTaskSample*> TaskMap;
		TaskMap.Reserve(Tasks.Num());
		for (const FTaskSample& Task : Tasks)
		{
			TaskMap.Add(Task.TaskId, &Task);
		}

		TArray<FTaskPathCandidate> Candidates;
		Candidates.Reserve(Tasks.Num());
		TMap<int32, FTaskPathCandidate> Memo;
		TSet<int32> Visiting;

		for (const FTaskSample& Task : Tasks)
		{
			FTaskPathCandidate Candidate;
			BuildLongestPathForTask(Task.TaskId, TaskMap, Memo, Visiting, Candidate);
			Candidates.Add(MoveTemp(Candidate));
		}

		Candidates.Sort([](const FTaskPathCandidate& A, const FTaskPathCandidate& B)
		{
			if (A.TotalDurationMs == B.TotalDurationMs)
			{
				return A.TaskIds.Num() > B.TaskIds.Num();
			}
			return A.TotalDurationMs > B.TotalDurationMs;
		});

		const int32 TakeCount = FMath::Min(TopK, Candidates.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FTaskPathCandidate& Candidate = Candidates[Index];
			const TSharedRef<FJsonObject> PathObject = MakeShared<FJsonObject>();
			PathObject->SetNumberField(TEXT("path_rank"), Index + 1);
			PathObject->SetNumberField(TEXT("frame_index"), FrameIndex);
			PathObject->SetNumberField(TEXT("total_duration_ms"), Candidate.TotalDurationMs);
			PathObject->SetNumberField(TEXT("node_count"), Candidate.TaskIds.Num());
			PathObject->SetBoolField(TEXT("partial"), Candidate.bPartial);

			TArray<TSharedPtr<FJsonValue>> PathNodes;
			PathNodes.Reserve(Candidate.TaskIds.Num());
			for (const int32 TaskId : Candidate.TaskIds)
			{
				const FTaskSample* NodeTask = TaskMap.FindRef(TaskId);
				const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
				Node->SetNumberField(TEXT("task_id"), TaskId);
				Node->SetStringField(TEXT("task_name"), NodeTask != nullptr ? NodeTask->TaskName : TEXT("unknown"));
				Node->SetStringField(TEXT("thread"), NodeTask != nullptr ? NodeTask->QueueName : TEXT("unknown"));
				Node->SetNumberField(TEXT("start_ms"), NodeTask != nullptr ? NodeTask->StartMs : 0.0);
				Node->SetNumberField(TEXT("duration_ms"), NodeTask != nullptr ? NodeTask->RunMs : 0.0);

				TArray<TSharedPtr<FJsonValue>> WaitsFor;
				if (NodeTask != nullptr)
				{
					WaitsFor.Reserve(NodeTask->DependencyTaskIds.Num());
					for (const int32 DependencyId : NodeTask->DependencyTaskIds)
					{
						WaitsFor.Add(MakeShared<FJsonValueNumber>(DependencyId));
					}
				}
				Node->SetArrayField(TEXT("waits_for"), WaitsFor);

				PathNodes.Add(MakeShared<FJsonValueObject>(Node));
			}

			PathObject->SetArrayField(TEXT("path"), PathNodes);
			Data.Add(MakeShared<FJsonValueObject>(PathObject));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
		Meta.Add(TEXT("top"), FString::FromInt(TopK));
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
