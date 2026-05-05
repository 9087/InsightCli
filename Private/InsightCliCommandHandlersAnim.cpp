// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FAnimAggregateRow
{
	FString Key;
	int32 CallCount = 0;
	double TotalMs = 0.0;
	double MaxMs = 0.0;
};

bool IsAnimScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("anim"))
		|| Lower.Contains(TEXT("animation"))
		|| Lower.Contains(TEXT("animgraph"));
}

bool IsSkinningScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("skin"))
		|| Lower.Contains(TEXT("skinning"))
		|| Lower.Contains(TEXT("skeletalmesh"))
		|| Lower.Contains(TEXT("skelmesh"));
}

FString GuessActorFromScope(const FString& ScopeName)
{
	FString Candidate = ScopeName;

	int32 DelimiterIndex = INDEX_NONE;
	if (Candidate.FindChar(TEXT(':'), DelimiterIndex) && DelimiterIndex > 0)
	{
		Candidate = Candidate.Left(DelimiterIndex);
	}
	else if (Candidate.FindChar(TEXT('.'), DelimiterIndex) && DelimiterIndex > 0)
	{
		Candidate = Candidate.Left(DelimiterIndex);
	}
	else if (Candidate.FindChar(TEXT('/'), DelimiterIndex) && DelimiterIndex > 0)
	{
		Candidate = Candidate.Left(DelimiterIndex);
	}

	Candidate.TrimStartAndEndInline();
	return Candidate.IsEmpty() ? ScopeName : Candidate;
}

void AddAggregateSample(TMap<FString, FAnimAggregateRow>& InOutRows, const FString& Key, const FCpuScopeSample& Scope)
{
	FAnimAggregateRow& Row = InOutRows.FindOrAdd(Key);
	if (Row.Key.IsEmpty())
	{
		Row.Key = Key;
	}
	Row.CallCount += FMath::Max(0, Scope.CallCount);
	Row.TotalMs += Scope.TotalMs;
	Row.MaxMs = FMath::Max(Row.MaxMs, Scope.MaxMs);
}

void SortAggregateRows(TArray<FAnimAggregateRow>& Rows)
{
	Rows.Sort([](const FAnimAggregateRow& A, const FAnimAggregateRow& B)
	{
		if (A.TotalMs == B.TotalMs)
		{
			return A.Key < B.Key;
		}
		return A.TotalMs > B.TotalMs;
	});
}

void AddFallbackMeta(TMap<FString, FString>& Meta, const TCHAR* Warning)
{
	Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern"));
	AddMetaWarning(Meta, Warning);
}
}

bool HandleAnimCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("anim") && Request.Action == TEXT("top-actors"))
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

		TArray<FCpuScopeSample> CpuRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, {}, CpuRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("anim.top-actors"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build anim top-actors approximation"),
				TEXT("Trace-backed animation data is unavailable for this trace."));
			return true;
		}

		TMap<FString, FAnimAggregateRow> Aggregates;
		for (const FCpuScopeSample& Scope : CpuRows)
		{
			if (!IsAnimScope(Scope.ScopeName))
			{
				continue;
			}

			const FString ActorName = GuessActorFromScope(Scope.ScopeName);
			AddAggregateSample(Aggregates, ActorName, Scope);
		}

		TArray<FAnimAggregateRow> Rows;
		Aggregates.GenerateValueArray(Rows);
		SortAggregateRows(Rows);

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FAnimAggregateRow& Row = Rows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("actor"), Row.Key);
			Item->SetNumberField(TEXT("anim_ms"), Row.TotalMs);
			Item->SetNumberField(TEXT("call_count"), Row.CallCount);
			Item->SetNumberField(TEXT("max_ms"), Row.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		AddFallbackMeta(Meta, TEXT("Animation channel unavailable; values approximated from CPU scope patterns."));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("anim") && Request.Action == TEXT("graph"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("actor") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString ActorName;
		if (!RequireStringOption(Request.Args, TEXT("--actor"), TEXT("anim graph"), ActorName, OutResponse))
		{
			return true;
		}

		TArray<FCpuScopeSample> CpuRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, {}, CpuRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("anim.graph"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build anim graph approximation"),
				TEXT("Trace-backed animation graph data is unavailable for this trace."));
			return true;
		}

		TArray<FCpuScopeSample> Filtered;
		for (const FCpuScopeSample& Scope : CpuRows)
		{
			if (!IsAnimScope(Scope.ScopeName))
			{
				continue;
			}
			if (!Scope.ScopeName.Contains(ActorName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			Filtered.Add(Scope);
		}

		Filtered.Sort([](const FCpuScopeSample& A, const FCpuScopeSample& B)
		{
			if (A.TotalMs == B.TotalMs)
			{
				return A.ScopeName < B.ScopeName;
			}
			return A.TotalMs > B.TotalMs;
		});

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Filtered.Num());
		for (const FCpuScopeSample& Scope : Filtered)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("node"), Scope.ScopeName);
			Item->SetNumberField(TEXT("call_count"), Scope.CallCount);
			Item->SetNumberField(TEXT("total_ms"), Scope.TotalMs);
			Item->SetNumberField(TEXT("avg_ms"), Scope.AvgMs);
			Item->SetNumberField(TEXT("max_ms"), Scope.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("actor"), ActorName);
		if (Filtered.IsEmpty())
		{
			Meta.Add(TEXT("found"), TEXT("false"));
			Meta.Add(TEXT("reason"), TEXT("not_found"));
			Meta.Add(TEXT("query_key"), TEXT("actor"));
			Meta.Add(TEXT("query_value"), ActorName);
		}
		AddFallbackMeta(Meta, TEXT("Animation graph values are approximated from CPU scope patterns."));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("anim") && Request.Action == TEXT("skinning"))
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

		TArray<FCpuScopeSample> CpuRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, {}, CpuRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("anim.skinning"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build anim skinning approximation"),
				TEXT("Trace-backed animation skinning data is unavailable for this trace."));
			return true;
		}

		TArray<FCpuScopeSample> SkinningRows;
		for (const FCpuScopeSample& Scope : CpuRows)
		{
			if (IsSkinningScope(Scope.ScopeName) || (IsAnimScope(Scope.ScopeName) && Scope.ScopeName.Contains(TEXT("skin"), ESearchCase::IgnoreCase)))
			{
				SkinningRows.Add(Scope);
			}
		}

		SkinningRows.Sort([](const FCpuScopeSample& A, const FCpuScopeSample& B)
		{
			if (A.TotalMs == B.TotalMs)
			{
				return A.ScopeName < B.ScopeName;
			}
			return A.TotalMs > B.TotalMs;
		});

		const int32 TakeCount = FMath::Min(Limit, SkinningRows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FCpuScopeSample& Row = SkinningRows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("scope_name"), Row.ScopeName);
			Item->SetNumberField(TEXT("call_count"), Row.CallCount);
			Item->SetNumberField(TEXT("total_ms"), Row.TotalMs);
			Item->SetNumberField(TEXT("avg_ms"), Row.AvgMs);
			Item->SetNumberField(TEXT("max_ms"), Row.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		AddFallbackMeta(Meta, TEXT("Animation skinning values are approximated from CPU scope patterns."));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
