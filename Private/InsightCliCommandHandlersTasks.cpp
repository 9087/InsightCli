// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
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
		int32 CycleCount = 0;
		if (!BuildTaskTopSamples(Context, bHasFrameIndex ? TOptional<int32>(FrameIndexFilter) : TOptional<int32>(), Tasks, FailureStage, FailureReason, bFrameFound, CycleCount))
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
		Meta.Add(TEXT("algorithm"), TEXT("dag_longest_path"));
		Meta.Add(TEXT("cycle_count"), FString::FromInt(CycleCount));
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
		int32 CycleCount = 0;
		if (!BuildTaskTopSamples(Context, FrameIndex, Tasks, FailureStage, FailureReason, bFrameFound, CycleCount))
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

		Tasks.Sort([](const FTaskSample& A, const FTaskSample& B)
		{
			if (A.CriticalPathMs == B.CriticalPathMs)
			{
				return A.CriticalPathTaskChain.Num() > B.CriticalPathTaskChain.Num();
			}
			return A.CriticalPathMs > B.CriticalPathMs;
		});

		const int32 TakeCount = FMath::Min(TopK, Tasks.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FTaskSample& Candidate = Tasks[Index];
			const TSharedRef<FJsonObject> PathObject = MakeShared<FJsonObject>();
			PathObject->SetNumberField(TEXT("path_rank"), Index + 1);
			PathObject->SetNumberField(TEXT("frame_index"), FrameIndex);
			PathObject->SetNumberField(TEXT("total_duration_ms"), Candidate.CriticalPathMs);
			PathObject->SetNumberField(TEXT("node_count"), Candidate.CriticalPathTaskChain.Num());
			PathObject->SetBoolField(TEXT("partial"), Candidate.DependencyStatus != TEXT("resolved"));

			TArray<TSharedPtr<FJsonValue>> PathNodes;
			PathNodes.Reserve(Candidate.CriticalPathTaskChain.Num());
			for (const int32 TaskId : Candidate.CriticalPathTaskChain)
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
		Meta.Add(TEXT("algorithm"), TEXT("dag_longest_path"));
		Meta.Add(TEXT("cycle_count"), FString::FromInt(CycleCount));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
