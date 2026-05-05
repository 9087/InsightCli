// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
bool IsShaderCompileScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("shader"))
		|| Lower.Contains(TEXT("compile"))
		|| Lower.Contains(TEXT("pso"));
}

bool IsPsoMissScope(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();
	return Lower.Contains(TEXT("pso"))
		&& (Lower.Contains(TEXT("miss")) || Lower.Contains(TEXT("compile")) || Lower.Contains(TEXT("cache")));
}

void AddFallbackMeta(TMap<FString, FString>& Meta)
{
	Meta.Add(TEXT("data_source"), TEXT("cpu_scope_pattern"));
	AddMetaWarning(Meta, TEXT("Shader/PSO channel unavailable; values are approximated from CPU scope patterns."));
}

bool BuildShaderRows(const FTraceContext& Context, TArray<FCpuScopeSample>& OutRows, FString& OutFailureStage, FString& OutFailureReason)
{
	OutRows.Reset();

	TArray<FCpuScopeSample> CpuRows;
	if (!BuildCpuTopSamples(Context, {}, CpuRows, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	for (const FCpuScopeSample& Scope : CpuRows)
	{
		if (IsShaderCompileScope(Scope.ScopeName))
		{
			OutRows.Add(Scope);
		}
	}

	OutRows.Sort([](const FCpuScopeSample& A, const FCpuScopeSample& B)
	{
		if (A.TotalMs == B.TotalMs)
		{
			return A.ScopeName < B.ScopeName;
		}
		return A.TotalMs > B.TotalMs;
	});
	return true;
}
}

bool HandleShadersCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("shaders") && Request.Action == TEXT("compile-events"))
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

		TArray<FCpuScopeSample> ShaderRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildShaderRows(Context, ShaderRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("shaders.compile-events"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build shader compile-events approximation"),
				TEXT("Trace-backed shader compile data is unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, ShaderRows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FCpuScopeSample& Row = ShaderRows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("event"), Row.ScopeName);
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

	if (Request.Group == TEXT("shaders") && Request.Action == TEXT("pso-cache-misses"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TArray<FCpuScopeSample> ShaderRows;
		FString FailureStage;
		FString FailureReason;
		if (!BuildShaderRows(Context, ShaderRows, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("shaders.pso-cache-misses"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build pso-cache-misses approximation"),
				TEXT("Trace-backed PSO cache miss data is unavailable for this trace."));
			return true;
		}

		int32 MissCount = 0;
		double TotalMs = 0.0;
		FString TopScope;
		double TopMs = 0.0;
		for (const FCpuScopeSample& Row : ShaderRows)
		{
			if (!IsPsoMissScope(Row.ScopeName))
			{
				continue;
			}

			MissCount += FMath::Max(1, Row.CallCount);
			TotalMs += Row.TotalMs;
			if (Row.TotalMs > TopMs)
			{
				TopMs = Row.TotalMs;
				TopScope = Row.ScopeName;
			}
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("miss_scope_count"), MissCount);
		Data->SetNumberField(TEXT("compile_cost_ms"), TotalMs);
		Data->SetStringField(TEXT("top_scope"), TopScope.IsEmpty() ? TEXT("unavailable") : TopScope);

		TMap<FString, FString> Meta;
		AddFallbackMeta(Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	return false;
}
}
