// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FNiagaraAggregateRow
{
	FString Name;
	int32 CallCount = 0;
	double TotalMs = 0.0;
	double MaxMs = 0.0;
};

bool IsNiagaraScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("niagara"))
		|| Lower.Contains(TEXT("emitter"))
		|| Lower.Contains(TEXT("particle"))
		|| Lower.Contains(TEXT("vfx"));
}

FString GuessSystemName(const FString& ScopeName)
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
	AddMetaWarning(Meta, TEXT("Niagara channel unavailable; values are approximated from CPU scope patterns."));
}

FInsightCliResponse MakeNiagaraChannelDisabledError(const FTraceContext& Context, const TCHAR* Consumer, const TCHAR* Message)
{
	TMap<FString, FString> ExtraDetails;
	ExtraDetails.Add(TEXT("unavailable_reason"), TEXT("channel_disabled"));
	ExtraDetails.Add(TEXT("data_source"), TEXT("unavailable"));
	return MakeTraceUnavailableError(
		Context,
		Consumer,
		TEXT("provider"),
		TEXT("niagara channel disabled"),
		TEXT("provider"),
		TEXT("niagara channel disabled"),
		Message,
		ExtraDetails);
}
}

bool HandleNiagaraCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("niagara") && Request.Action == TEXT("top-systems"))
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

		OutResponse = MakeNiagaraChannelDisabledError(
			Context,
			TEXT("niagara.top-systems"),
			TEXT("Niagara trace channel is disabled or unavailable for this trace."));
		return true;

		TArray<FCpuScopeSample> CpuRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, {}, CpuRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("niagara.top-systems"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build niagara top-systems approximation"),
				TEXT("Trace-backed Niagara data is unavailable for this trace."));
			return true;
		}

		TMap<FString, FNiagaraAggregateRow> AggregateMap;
		for (const FCpuScopeSample& Scope : CpuRows)
		{
			if (!IsNiagaraScope(Scope.ScopeName))
			{
				continue;
			}

			const FString SystemName = GuessSystemName(Scope.ScopeName);
			FNiagaraAggregateRow& Row = AggregateMap.FindOrAdd(SystemName);
			if (Row.Name.IsEmpty())
			{
				Row.Name = SystemName;
			}
			Row.CallCount += FMath::Max(0, Scope.CallCount);
			Row.TotalMs += Scope.TotalMs;
			Row.MaxMs = FMath::Max(Row.MaxMs, Scope.MaxMs);
		}

		TArray<FNiagaraAggregateRow> Rows;
		AggregateMap.GenerateValueArray(Rows);
		Rows.Sort([](const FNiagaraAggregateRow& A, const FNiagaraAggregateRow& B)
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
			const FNiagaraAggregateRow& Row = Rows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("system"), Row.Name);
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

	if (Request.Group == TEXT("niagara") && Request.Action == TEXT("emitter-cost"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("system") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString SystemName;
		if (!RequireStringOption(Request.Args, TEXT("--system"), TEXT("niagara emitter-cost"), SystemName, OutResponse))
		{
			return true;
		}

		OutResponse = MakeNiagaraChannelDisabledError(
			Context,
			TEXT("niagara.emitter-cost"),
			TEXT("Niagara trace channel is disabled or unavailable for this trace."));
		return true;

		TArray<FCpuScopeSample> CpuRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCpuTopSamples(Context, {}, CpuRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("niagara.emitter-cost"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build niagara emitter-cost approximation"),
				TEXT("Trace-backed Niagara data is unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		for (const FCpuScopeSample& Scope : CpuRows)
		{
			if (!IsNiagaraScope(Scope.ScopeName) || !Scope.ScopeName.Contains(SystemName, ESearchCase::IgnoreCase))
			{
				continue;
			}

			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("emitter"), Scope.ScopeName);
			Item->SetNumberField(TEXT("call_count"), Scope.CallCount);
			Item->SetNumberField(TEXT("total_ms"), Scope.TotalMs);
			Item->SetNumberField(TEXT("avg_ms"), Scope.AvgMs);
			Item->SetNumberField(TEXT("max_ms"), Scope.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("system"), SystemName);
		if (Data.IsEmpty())
		{
			Meta.Add(TEXT("found"), TEXT("false"));
			Meta.Add(TEXT("reason"), TEXT("not_found"));
			Meta.Add(TEXT("query_key"), TEXT("system"));
			Meta.Add(TEXT("query_value"), SystemName);
		}
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
