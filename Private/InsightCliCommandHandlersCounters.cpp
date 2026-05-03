// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleCountersCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles counters/series and counters/stats, including counter-name validation and window filtering.
	if (Request.Group == TEXT("counters") && Request.Action == TEXT("list"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TArray<FCounterCatalogEntry> Catalog;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCounterCatalog(Context, Catalog, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("counters.list"),
				FailureStage,
				FailureReason,
				TEXT("counter_catalog"),
				TEXT("failed to build counter catalog"),
				TEXT("Trace-backed counters are unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		for (const FCounterCatalogEntry& Entry : Catalog)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Entry.Name);
			Item->SetStringField(TEXT("type"), Entry.Type);
			Item->SetStringField(TEXT("unit"), Entry.Unit);
			Item->SetNumberField(TEXT("sample_count"), Entry.SampleCount);
			Item->SetBoolField(TEXT("trace_backed"), Entry.bTraceBacked);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("count"), FString::FromInt(Data.Num()));
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("counters") && Request.Action == TEXT("series"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("name"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString CounterName;
		FInsightCliResponse RequiredOptionError;
		if (!RequireStringOption(Request.Args, TEXT("--name"), TEXT("counters series"), CounterName, RequiredOptionError))
		{
			OutResponse = RequiredOptionError;
			return true;
		}

		FCounterCatalogEntry CounterEntry;
		FInsightCliResponse CounterError;
		if (!ResolveCounterByName(Context, CounterName, TEXT("counters.series"), CounterEntry, CounterError))
		{
			OutResponse = CounterError;
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		TArray<FCounterPoint> Series;
		FString CounterType;
		FString CounterUnit;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCounterSeries(
			Context,
			CounterEntry.Name,
			Series,
			CounterType,
			CounterUnit,
			FailureStage,
			FailureReason,
			TimeWindow.StartMs,
			TimeWindow.EndMs))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("counters.series"),
				FailureStage,
				FailureReason,
				TEXT("counter_series"),
				TEXT("failed to build counter series"),
				TEXT("Trace-backed counters are unavailable for this trace."),
				{{TEXT("counter_name"), CounterEntry.Name}});
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Series.Num());
		for (const FCounterPoint& Point : Series)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("timestamp_ms"), Point.TimestampMs);
			Item->SetNumberField(TEXT("value"), Point.Value);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("counter_name"), CounterEntry.Name);
		Meta.Add(TEXT("counter_type"), CounterType);
		Meta.Add(TEXT("counter_unit"), CounterUnit);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("counters") && Request.Action == TEXT("stats"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("name") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString CounterName;
		FInsightCliResponse RequiredOptionError;
		if (!RequireStringOption(Request.Args, TEXT("--name"), TEXT("counters stats"), CounterName, RequiredOptionError))
		{
			OutResponse = RequiredOptionError;
			return true;
		}

		FCounterCatalogEntry CounterEntry;
		FInsightCliResponse CounterError;
		if (!ResolveCounterByName(Context, CounterName, TEXT("counters.stats"), CounterEntry, CounterError))
		{
			OutResponse = CounterError;
			return true;
		}

		TArray<FCounterPoint> Series;
		FString CounterType;
		FString CounterUnit;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCounterSeries(
			Context,
			CounterEntry.Name,
			Series,
			CounterType,
			CounterUnit,
			FailureStage,
			FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("counters.stats"),
				FailureStage,
				FailureReason,
				TEXT("counter_series"),
				TEXT("failed to build counter series"),
				TEXT("Trace-backed counters are unavailable for this trace."),
				{{TEXT("counter_name"), CounterEntry.Name}});
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("counter_type"), CounterType);
		Meta.Add(TEXT("counter_unit"), CounterUnit);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeCounterStatsObject(Series, CounterEntry.Name), Meta));
		return true;
	}

	return false;
}
}
