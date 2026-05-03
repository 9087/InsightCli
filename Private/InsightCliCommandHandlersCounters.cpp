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
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("name"), TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
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

		TArray<FCounterCatalogEntry> Catalog;
		FString CatalogFailureStage;
		FString CatalogFailureReason;
		if (!BuildCounterCatalog(Context, Catalog, CatalogFailureStage, CatalogFailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("counters.series"),
				CatalogFailureStage,
				CatalogFailureReason,
				TEXT("counter_catalog"),
				TEXT("failed to build counter catalog"),
				TEXT("Trace-backed counters are unavailable for this trace."));
			return true;
		}

		const FCounterCatalogEntry* CounterEntry = Catalog.FindByPredicate([&CounterName](const FCounterCatalogEntry& Entry)
		{
			return Entry.Name.Equals(CounterName, ESearchCase::IgnoreCase);
		});

		if (CounterEntry == nullptr)
		{
			TArray<FString> CounterNames;
			CounterNames.Reserve(Catalog.Num());
			for (const FCounterCatalogEntry& Entry : Catalog)
			{
				CounterNames.Add(Entry.Name);
			}
			CounterNames.Sort();

			TMap<FString, FString> Details;
			Details.Add(TEXT("name"), CounterName);
			Details.Add(TEXT("available_counters"), FString::Join(CounterNames, TEXT(",")));
			OutResponse = FInsightCliResponse::Error(5, TEXT("E2001"), TEXT("Counter name not found."), Details);
			return true;
		}

		FTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryGetTimeWindowMs(Request.Args, TimeWindow, TimeWindowError))
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
			CounterEntry->Name,
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
				{{TEXT("counter_name"), CounterEntry->Name}});
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
		Meta.Add(TEXT("counter_name"), CounterEntry->Name);
		Meta.Add(TEXT("counter_type"), CounterType);
		Meta.Add(TEXT("counter_unit"), CounterUnit);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (TimeWindow.StartMs.IsSet())
		{
			Meta.Add(TEXT("filter_time_start"), ToNumberString(TimeWindow.StartMs.GetValue()));
		}
		if (TimeWindow.EndMs.IsSet())
		{
			Meta.Add(TEXT("filter_time_end"), ToNumberString(TimeWindow.EndMs.GetValue()));
		}
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

		TArray<FCounterCatalogEntry> Catalog;
		FString CatalogFailureStage;
		FString CatalogFailureReason;
		if (!BuildCounterCatalog(Context, Catalog, CatalogFailureStage, CatalogFailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("counters.stats"),
				CatalogFailureStage,
				CatalogFailureReason,
				TEXT("counter_catalog"),
				TEXT("failed to build counter catalog"),
				TEXT("Trace-backed counters are unavailable for this trace."));
			return true;
		}

		const FCounterCatalogEntry* CounterEntry = Catalog.FindByPredicate([&CounterName](const FCounterCatalogEntry& Entry)
		{
			return Entry.Name.Equals(CounterName, ESearchCase::IgnoreCase);
		});

		if (CounterEntry == nullptr)
		{
			TArray<FString> CounterNames;
			CounterNames.Reserve(Catalog.Num());
			for (const FCounterCatalogEntry& Entry : Catalog)
			{
				CounterNames.Add(Entry.Name);
			}
			CounterNames.Sort();

			TMap<FString, FString> Details;
			Details.Add(TEXT("name"), CounterName);
			Details.Add(TEXT("available_counters"), FString::Join(CounterNames, TEXT(",")));
			OutResponse = FInsightCliResponse::Error(5, TEXT("E2001"), TEXT("Counter name not found."), Details);
			return true;
		}

		TArray<FCounterPoint> Series;
		FString CounterType;
		FString CounterUnit;
		FString FailureStage;
		FString FailureReason;
		if (!BuildCounterSeries(
			Context,
			CounterEntry->Name,
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
				{{TEXT("counter_name"), CounterEntry->Name}});
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("counter_type"), CounterType);
		Meta.Add(TEXT("counter_unit"), CounterUnit);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeCounterStatsObject(Series, CounterEntry->Name), Meta));
		return true;
	}

	return false;
}
}
