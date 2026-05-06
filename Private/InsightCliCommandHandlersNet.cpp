// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
constexpr bool bEnableLegacyCpuScopeFallback = false;

struct FNetAggregateRow
{
	FString Key;
	int32 Count = 0;
	double TotalMs = 0.0;
	double MaxMs = 0.0;
};

bool IsNetScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("net"))
		|| Lower.Contains(TEXT("rpc"))
		|| Lower.Contains(TEXT("replication"))
		|| Lower.Contains(TEXT("packet"));
}

bool IsRpcScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("rpc")) || Lower.Contains(TEXT("remote"));
}

FString GuessActorFromNetScope(const FString& ScopeName)
{
	FString Result = ScopeName;
	int32 Index = INDEX_NONE;
	if (Result.FindChar(TEXT(':'), Index) && Index > 0)
	{
		Result = Result.Left(Index);
	}
	else if (Result.FindChar(TEXT('.'), Index) && Index > 0)
	{
		Result = Result.Left(Index);
	}
	Result.TrimStartAndEndInline();
	return Result.IsEmpty() ? ScopeName : Result;
}

void SortRows(TArray<FNetAggregateRow>& Rows)
{
	Rows.Sort([](const FNetAggregateRow& A, const FNetAggregateRow& B)
	{
		if (A.TotalMs == B.TotalMs)
		{
			return A.Key < B.Key;
		}
		return A.TotalMs > B.TotalMs;
	});
}

void AddFallbackMeta(TMap<FString, FString>& Meta)
{
	Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern_experimental"));
	AddMetaWarning(Meta, TEXT("Net channel unavailable; experimental CPU scope-pattern fallback enabled."));
}

bool BuildNetCpuRows(const FTraceContext& Context, TArray<FCpuScopeSample>& OutRows, FString& OutFailureStage, FString& OutFailureReason)
{
	OutRows.Reset();

	TArray<FCpuScopeSample> CpuRows;
	if (!BuildCpuTopSamples(Context, {}, CpuRows, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	for (const FCpuScopeSample& Scope : CpuRows)
	{
		if (IsNetScope(Scope.ScopeName))
		{
			OutRows.Add(Scope);
		}
	}

	return true;
}

FInsightCliResponse MakeNetChannelDisabledError(const FTraceContext& Context, const TCHAR* Consumer, const TCHAR* Message)
{
	TMap<FString, FString> ExtraDetails;
	ExtraDetails.Add(TEXT("unavailable_reason"), TEXT("channel_disabled"));
	ExtraDetails.Add(TEXT("data_source"), TEXT("unavailable"));
	return MakeTraceUnavailableError(
		Context,
		Consumer,
		TEXT("provider"),
		TEXT("net channel disabled"),
		TEXT("provider"),
		TEXT("net channel disabled"),
		Message,
		ExtraDetails);
}
}

bool HandleNetCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("net") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		if (!bEnableLegacyCpuScopeFallback)
		{
			OutResponse = MakeNetChannelDisabledError(
				Context,
				TEXT("net.summary"),
				TEXT("Net trace channel is disabled or unavailable for this trace."));
			return true;
		}

		TArray<FCpuScopeSample> NetRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildNetCpuRows(Context, NetRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("net.summary"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build net summary approximation"),
				TEXT("Trace-backed net data is unavailable for this trace."));
			return true;
		}

		double TotalMs = 0.0;
		int32 RpcCalls = 0;
		TSet<FString> UniqueActors;
		for (const FCpuScopeSample& Row : NetRows)
		{
			TotalMs += Row.TotalMs;
			if (IsRpcScope(Row.ScopeName))
			{
				RpcCalls += FMath::Max(0, Row.CallCount);
			}
			UniqueActors.Add(GuessActorFromNetScope(Row.ScopeName));
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("scope_sample_count"), NetRows.Num());
		Data->SetNumberField(TEXT("total_net_ms"), TotalMs);
		Data->SetNumberField(TEXT("approx_rpc_calls"), RpcCalls);
		Data->SetNumberField(TEXT("approx_actor_count"), UniqueActors.Num());

		TMap<FString, FString> Meta;
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("net") && Request.Action == TEXT("top-actors"))
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

		if (!bEnableLegacyCpuScopeFallback)
		{
			OutResponse = MakeNetChannelDisabledError(
				Context,
				TEXT("net.top-actors"),
				TEXT("Net trace channel is disabled or unavailable for this trace."));
			return true;
		}

		TArray<FCpuScopeSample> NetRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildNetCpuRows(Context, NetRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("net.top-actors"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build net top-actors approximation"),
				TEXT("Trace-backed net data is unavailable for this trace."));
			return true;
		}

		TMap<FString, FNetAggregateRow> AggregateMap;
		for (const FCpuScopeSample& Row : NetRows)
		{
			const FString ActorName = GuessActorFromNetScope(Row.ScopeName);
			FNetAggregateRow& Aggregate = AggregateMap.FindOrAdd(ActorName);
			if (Aggregate.Key.IsEmpty())
			{
				Aggregate.Key = ActorName;
			}
			Aggregate.Count += FMath::Max(0, Row.CallCount);
			Aggregate.TotalMs += Row.TotalMs;
			Aggregate.MaxMs = FMath::Max(Aggregate.MaxMs, Row.MaxMs);
		}

		TArray<FNetAggregateRow> Rows;
		AggregateMap.GenerateValueArray(Rows);
		SortRows(Rows);

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FNetAggregateRow& Row = Rows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("actor"), Row.Key);
			Item->SetNumberField(TEXT("rpc_count"), Row.Count);
			Item->SetNumberField(TEXT("net_ms"), Row.TotalMs);
			Item->SetNumberField(TEXT("max_ms"), Row.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("net") && Request.Action == TEXT("top-rpcs"))
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

		if (!bEnableLegacyCpuScopeFallback)
		{
			OutResponse = MakeNetChannelDisabledError(
				Context,
				TEXT("net.top-rpcs"),
				TEXT("Net trace channel is disabled or unavailable for this trace."));
			return true;
		}

		TArray<FCpuScopeSample> NetRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildNetCpuRows(Context, NetRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("net.top-rpcs"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build net top-rpcs approximation"),
				TEXT("Trace-backed net data is unavailable for this trace."));
			return true;
		}

		TArray<FCpuScopeSample> RpcRows;
		for (const FCpuScopeSample& Row : NetRows)
		{
			if (IsRpcScope(Row.ScopeName))
			{
				RpcRows.Add(Row);
			}
		}

		RpcRows.Sort([](const FCpuScopeSample& A, const FCpuScopeSample& B)
		{
			if (A.TotalMs == B.TotalMs)
			{
				return A.ScopeName < B.ScopeName;
			}
			return A.TotalMs > B.TotalMs;
		});

		const int32 TakeCount = FMath::Min(Limit, RpcRows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FCpuScopeSample& Row = RpcRows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("rpc"), Row.ScopeName);
			Item->SetNumberField(TEXT("call_count"), Row.CallCount);
			Item->SetNumberField(TEXT("total_ms"), Row.TotalMs);
			Item->SetNumberField(TEXT("avg_ms"), Row.AvgMs);
			Item->SetNumberField(TEXT("max_ms"), Row.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("net") && Request.Action == TEXT("bandwidth-series"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, false, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		if (!bEnableLegacyCpuScopeFallback)
		{
			OutResponse = MakeNetChannelDisabledError(
				Context,
				TEXT("net.bandwidth-series"),
				TEXT("Net trace channel is disabled or unavailable for this trace."));
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("net.bandwidth-series")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		ApplyTimeWindowFilter(Frames, TimeWindow);

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Frames.Num());
		for (const FFrameSample& Frame : Frames)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("frame_index"), Frame.FrameIndex);
			Item->SetNumberField(TEXT("timestamp_ms"), Frame.FrameEndMs);
			Item->SetNumberField(TEXT("bandwidth_kbps"), 0.0);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		AppendTimeWindowMeta(TimeWindow, Meta);
		AddFallbackMeta(Meta);
		Meta.Add(TEXT("series_source"), TEXT("frame_samples_zero_fill"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
