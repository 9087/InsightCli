// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FPhysicsStageAggregate
{
	FString Stage;
	int32 CallCount = 0;
	double TotalMs = 0.0;
};

struct FPhysicsBodyAggregate
{
	FString Name;
	int32 CallCount = 0;
	double TotalMs = 0.0;
	double MaxMs = 0.0;
};

bool IsPhysicsScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("physics"))
		|| Lower.Contains(TEXT("chaos"))
		|| Lower.Contains(TEXT("broadphase"))
		|| Lower.Contains(TEXT("narrowphase"))
		|| Lower.Contains(TEXT("constraint"))
		|| Lower.Contains(TEXT("solver"))
		|| Lower.Contains(TEXT("collision"));
}

FString ClassifyStage(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	if (Lower.Contains(TEXT("broadphase")))
	{
		return TEXT("broadphase");
	}
	if (Lower.Contains(TEXT("narrowphase")))
	{
		return TEXT("narrowphase");
	}
	if (Lower.Contains(TEXT("constraint")))
	{
		return TEXT("constraint");
	}
	if (Lower.Contains(TEXT("solver")) || Lower.Contains(TEXT("solve")))
	{
		return TEXT("solver");
	}
	return TEXT("other");
}

FString GuessBodyName(const FString& ScopeName)
{
	FString Name = ScopeName;
	int32 Delimiter = INDEX_NONE;
	if (Name.FindChar(TEXT(':'), Delimiter) && Delimiter > 0)
	{
		Name = Name.Left(Delimiter);
	}
	else if (Name.FindChar(TEXT('.'), Delimiter) && Delimiter > 0)
	{
		Name = Name.Left(Delimiter);
	}
	Name.TrimStartAndEndInline();
	return Name.IsEmpty() ? ScopeName : Name;
}

void AddFallbackMeta(TMap<FString, FString>& Meta)
{
	Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern"));
	Meta.Add(TEXT("warning"), TEXT("Physics channel unavailable; values are approximated from CPU scope patterns."));
}

bool BuildPhysicsRows(const FTraceContext& Context, TArray<FCpuScopeSample>& OutRows, FString& OutFailureStage, FString& OutFailureReason)
{
	OutRows.Reset();

	TArray<FCpuScopeSample> CpuRows;
	if (!BuildCpuTopSamples(Context, {}, CpuRows, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	for (const FCpuScopeSample& Scope : CpuRows)
	{
		if (IsPhysicsScope(Scope.ScopeName))
		{
			OutRows.Add(Scope);
		}
	}

	return true;
}
}

bool HandlePhysicsCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("physics") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		bool bHasFrameIndex = false;
		int32 FrameIndex = -1;
		if (TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			bHasFrameIndex = true;
			if (FrameIndex < 0)
			{
				OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
				return true;
			}

			FInsightCliResponse FrameGuardError;
			if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("physics.summary")))
			{
				OutResponse = FrameGuardError;
				return true;
			}
			const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
			const FFrameSample* FoundFrame = Frames.FindByPredicate([FrameIndex](const FFrameSample& Sample)
			{
				return Sample.FrameIndex == FrameIndex;
			});
			if (FoundFrame == nullptr)
			{
				TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
				AddFallbackMeta(Meta);
				OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
				return true;
			}
		}

		TArray<FCpuScopeSample> PhysicsRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildPhysicsRows(Context, PhysicsRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("physics.summary"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build physics summary approximation"),
				TEXT("Trace-backed physics data is unavailable for this trace."));
			return true;
		}

		double TotalMs = 0.0;
		double BroadPhaseMs = 0.0;
		double NarrowPhaseMs = 0.0;
		double ConstraintMs = 0.0;
		double SolverMs = 0.0;

		for (const FCpuScopeSample& Row : PhysicsRows)
		{
			TotalMs += Row.TotalMs;
			const FString Stage = ClassifyStage(Row.ScopeName);
			if (Stage == TEXT("broadphase"))
			{
				BroadPhaseMs += Row.TotalMs;
			}
			else if (Stage == TEXT("narrowphase"))
			{
				NarrowPhaseMs += Row.TotalMs;
			}
			else if (Stage == TEXT("constraint"))
			{
				ConstraintMs += Row.TotalMs;
			}
			else if (Stage == TEXT("solver"))
			{
				SolverMs += Row.TotalMs;
			}
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("scope_sample_count"), PhysicsRows.Num());
		Data->SetNumberField(TEXT("total_physics_ms"), TotalMs);
		Data->SetNumberField(TEXT("broadphase_ms"), BroadPhaseMs);
		Data->SetNumberField(TEXT("narrowphase_ms"), NarrowPhaseMs);
		Data->SetNumberField(TEXT("constraint_ms"), ConstraintMs);
		Data->SetNumberField(TEXT("solver_ms"), SolverMs);

		TMap<FString, FString> Meta;
		if (bHasFrameIndex)
		{
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
		}
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("physics") && Request.Action == TEXT("solver-stages"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TArray<FCpuScopeSample> PhysicsRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildPhysicsRows(Context, PhysicsRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("physics.solver-stages"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build physics solver-stages approximation"),
				TEXT("Trace-backed physics data is unavailable for this trace."));
			return true;
		}

		TMap<FString, FPhysicsStageAggregate> Aggregates;
		for (const FCpuScopeSample& Row : PhysicsRows)
		{
			const FString Stage = ClassifyStage(Row.ScopeName);
			FPhysicsStageAggregate& Aggregate = Aggregates.FindOrAdd(Stage);
			if (Aggregate.Stage.IsEmpty())
			{
				Aggregate.Stage = Stage;
			}
			Aggregate.CallCount += FMath::Max(0, Row.CallCount);
			Aggregate.TotalMs += Row.TotalMs;
		}

		TArray<FPhysicsStageAggregate> Rows;
		Aggregates.GenerateValueArray(Rows);
		Rows.Sort([](const FPhysicsStageAggregate& A, const FPhysicsStageAggregate& B)
		{
			if (A.TotalMs == B.TotalMs)
			{
				return A.Stage < B.Stage;
			}
			return A.TotalMs > B.TotalMs;
		});

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Rows.Num());
		for (const FPhysicsStageAggregate& Row : Rows)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("stage"), Row.Stage);
			Item->SetNumberField(TEXT("call_count"), Row.CallCount);
			Item->SetNumberField(TEXT("total_ms"), Row.TotalMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("physics") && Request.Action == TEXT("top-bodies"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 20;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 20, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		TArray<FCpuScopeSample> PhysicsRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildPhysicsRows(Context, PhysicsRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("physics.top-bodies"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build physics top-bodies approximation"),
				TEXT("Trace-backed physics data is unavailable for this trace."));
			return true;
		}

		TMap<FString, FPhysicsBodyAggregate> Aggregates;
		for (const FCpuScopeSample& Row : PhysicsRows)
		{
			const FString Body = GuessBodyName(Row.ScopeName);
			FPhysicsBodyAggregate& Aggregate = Aggregates.FindOrAdd(Body);
			if (Aggregate.Name.IsEmpty())
			{
				Aggregate.Name = Body;
			}
			Aggregate.CallCount += FMath::Max(0, Row.CallCount);
			Aggregate.TotalMs += Row.TotalMs;
			Aggregate.MaxMs = FMath::Max(Aggregate.MaxMs, Row.MaxMs);
		}

		TArray<FPhysicsBodyAggregate> Rows;
		Aggregates.GenerateValueArray(Rows);
		Rows.Sort([](const FPhysicsBodyAggregate& A, const FPhysicsBodyAggregate& B)
		{
			if (A.TotalMs == B.TotalMs)
			{
				return A.Name < B.Name;
			}
			return A.TotalMs > B.TotalMs;
		});

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FPhysicsBodyAggregate& Row = Rows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("body"), Row.Name);
			Item->SetNumberField(TEXT("call_count"), Row.CallCount);
			Item->SetNumberField(TEXT("total_ms"), Row.TotalMs);
			Item->SetNumberField(TEXT("max_ms"), Row.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
