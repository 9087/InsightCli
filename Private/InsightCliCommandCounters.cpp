// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Counters.h"

namespace UE::InsightCli::Internal
{
bool BuildCounterCatalog(
	const FTraceContext& Context,
	TArray<FCounterCatalogEntry>& OutCatalog,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutCatalog.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	const auto InferUnit = [](const FString& Name, TraceServices::ECounterDisplayHint DisplayHint) -> FString
	{
		if (DisplayHint == TraceServices::CounterDisplayHint_Memory)
		{
			return TEXT("bytes");
		}

		if (Name.EndsWith(TEXT("Ms"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("time"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("duration"), ESearchCase::IgnoreCase))
		{
			return TEXT("ms");
		}

		return TEXT("count");
	};

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::ICounterProvider& CounterProvider = TraceServices::ReadCounterProvider(*Session.Get());

		CounterProvider.EnumerateCounters([&OutCatalog, &InferUnit](uint32, const TraceServices::ICounter& Counter)
		{
			int32 SampleCount = 0;
			if (Counter.IsFloatingPoint())
			{
				Counter.EnumerateFloatValues(0.0, TNumericLimits<double>::Max(), false, [&SampleCount](double, double)
				{
					++SampleCount;
				});
			}
			else
			{
				Counter.EnumerateValues(0.0, TNumericLimits<double>::Max(), false, [&SampleCount](double, int64)
				{
					++SampleCount;
				});
			}

			FCounterCatalogEntry Entry;
			Entry.Name = Counter.GetName() != nullptr ? Counter.GetName() : TEXT("<unnamed>");
			Entry.Type = Counter.IsFloatingPoint() ? TEXT("float") : TEXT("int64");
			Entry.Unit = InferUnit(Entry.Name, Counter.GetDisplayHint());
			Entry.SampleCount = SampleCount;
			Entry.bTraceBacked = true;
			OutCatalog.Add(MoveTemp(Entry));
		});
	}

	OutCatalog.Sort([](const FCounterCatalogEntry& A, const FCounterCatalogEntry& B)
	{
		return A.Name < B.Name;
	});

	return true;
}

bool ResolveCounterByName(
	const FTraceContext& Context,
	const FString& RequestedName,
	const TCHAR* OwnerCommand,
	FCounterCatalogEntry& OutEntry,
	FInsightCliResponse& OutError)
{
	TArray<FCounterCatalogEntry> Catalog;
	FString CatalogFailureStage;
	FString CatalogFailureReason;
	if (!BuildCounterCatalog(Context, Catalog, CatalogFailureStage, CatalogFailureReason))
	{
		OutError = MakeTraceUnavailableError(
			Context,
			OwnerCommand,
			CatalogFailureStage,
			CatalogFailureReason,
			TEXT("counter_catalog"),
			TEXT("failed to build counter catalog"),
			TEXT("Trace-backed counters are unavailable for this trace."));
		return false;
	}

	const FCounterCatalogEntry* CounterEntry = Catalog.FindByPredicate([&RequestedName](const FCounterCatalogEntry& Entry)
	{
		return Entry.Name.Equals(RequestedName, ESearchCase::IgnoreCase);
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
		Details.Add(TEXT("name"), RequestedName);
		Details.Add(TEXT("available_counters"), FString::Join(CounterNames, TEXT(",")));
		OutError = MakeNotFoundError(TEXT("Counter name not found."), Details);
		return false;
	}

	OutEntry = *CounterEntry;
	OutError = FInsightCliResponse();
	return true;
}

bool BuildCounterSeries(
	const FTraceContext& Context,
	const FString& CounterName,
	TArray<FCounterPoint>& OutSeries,
	FString& OutCounterType,
	FString& OutCounterUnit,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs)
{
	OutSeries.Reset();
	OutCounterType.Reset();
	OutCounterUnit.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	const auto InferUnit = [](const FString& Name, TraceServices::ECounterDisplayHint DisplayHint) -> FString
	{
		if (DisplayHint == TraceServices::CounterDisplayHint_Memory)
		{
			return TEXT("bytes");
		}

		if (Name.EndsWith(TEXT("Ms"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("time"), ESearchCase::IgnoreCase)
			|| Name.Contains(TEXT("duration"), ESearchCase::IgnoreCase))
		{
			return TEXT("ms");
		}

		return TEXT("count");
	};

	double DurationSec = 0.0;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		DurationSec = Session->GetDurationSeconds();
	}
	const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
	const double EndSec = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;

	bool bFoundCounter = false;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::ICounterProvider& CounterProvider = TraceServices::ReadCounterProvider(*Session.Get());

		CounterProvider.EnumerateCounters([&](uint32, const TraceServices::ICounter& Counter)
		{
			const FString Name = Counter.GetName() != nullptr ? Counter.GetName() : TEXT("");
			if (!Name.Equals(CounterName, ESearchCase::IgnoreCase))
			{
				return;
			}

			bFoundCounter = true;
			OutCounterType = Counter.IsFloatingPoint() ? TEXT("float") : TEXT("int64");
			OutCounterUnit = InferUnit(Name, Counter.GetDisplayHint());

			if (Counter.IsFloatingPoint())
			{
				Counter.EnumerateFloatValues(StartSec, EndSec, false, [&OutSeries](double TimeSec, double Value)
				{
					OutSeries.Add({ TimeSec * 1000.0, Value });
				});
			}
			else
			{
				Counter.EnumerateValues(StartSec, EndSec, false, [&OutSeries](double TimeSec, int64 Value)
				{
					OutSeries.Add({ TimeSec * 1000.0, static_cast<double>(Value) });
				});
			}
		});
	}

	if (!bFoundCounter)
	{
		return true;
	}

	OutSeries.Sort([](const FCounterPoint& A, const FCounterPoint& B)
	{
		return A.TimestampMs < B.TimestampMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeCounterStatsObject(const TArray<FCounterPoint>& Series, const FString& CounterName)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("counter_name"), CounterName);

	if (Series.IsEmpty())
	{
		Item->SetNumberField(TEXT("sample_count"), 0);
		Item->SetNumberField(TEXT("min"), 0.0);
		Item->SetNumberField(TEXT("max"), 0.0);
		Item->SetNumberField(TEXT("avg"), 0.0);
		Item->SetNumberField(TEXT("delta"), 0.0);
		return Item;
	}

	double MinValue = TNumericLimits<double>::Max();
	double MaxValue = 0.0;
	double Sum = 0.0;
	for (const FCounterPoint& Point : Series)
	{
		MinValue = FMath::Min(MinValue, Point.Value);
		MaxValue = FMath::Max(MaxValue, Point.Value);
		Sum += Point.Value;
	}

	Item->SetNumberField(TEXT("sample_count"), Series.Num());
	Item->SetNumberField(TEXT("min"), MinValue);
	Item->SetNumberField(TEXT("max"), MaxValue);
	Item->SetNumberField(TEXT("avg"), Sum / Series.Num());
	Item->SetNumberField(TEXT("delta"), Series.Last().Value - Series[0].Value);
	return Item;
}
}
