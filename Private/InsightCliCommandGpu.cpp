// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UE::InsightCli::Internal
{
bool BuildGpuTopSamples(
	const FTraceContext& Context,
	TArray<FGpuScopeSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> IntervalStartSec,
	TOptional<double> IntervalEndSec)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	TUniquePtr<TraceServices::ITable<TraceServices::FTimingProfilerAggregatedStats>> StatsTable;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::ITimingProfilerProvider* TimingProfilerProvider = TraceServices::ReadTimingProfilerProvider(*Session.Get());
		if (TimingProfilerProvider == nullptr)
		{
			OutFailureStage = TEXT("timing_provider");
			OutFailureReason = TEXT("TimingProfiler provider not available");
			return false;
		}

		const double SessionDurationSec = Session->GetDurationSeconds();
		const double RequestedStartSec = IntervalStartSec.IsSet() ? FMath::Max(0.0, IntervalStartSec.GetValue()) : 0.0;
		double RequestedEndSec = IntervalEndSec.IsSet() ? FMath::Max(RequestedStartSec, IntervalEndSec.GetValue()) : SessionDurationSec;
		if (RequestedEndSec <= RequestedStartSec)
		{
			RequestedEndSec = RequestedStartSec + KINDA_SMALL_NUMBER;
		}

		TraceServices::FCreateAggreationParams Params;
		Params.IntervalStart = RequestedStartSec;
		Params.IntervalEnd = RequestedEndSec;
		Params.IncludeGpu = true;
		Params.FrameType = ETraceFrameType::TraceFrameType_Count;
		Params.CpuThreadFilter = [](uint32)
		{
			return false;
		};

		StatsTable.Reset(TimingProfilerProvider->CreateAggregation(Params));
	}

	if (!StatsTable.IsValid())
	{
		// Some traces legitimately have no GPU timing stream; return an empty result set.
		return true;
	}

	TUniquePtr<TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>> Reader(StatsTable->CreateReader());
	if (!Reader.IsValid())
	{
		OutFailureStage = TEXT("aggregation_reader");
		OutFailureReason = TEXT("failed to create gpu aggregation table reader");
		return false;
	}

	while (Reader->IsValid())
	{
		const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
		if (Row != nullptr && Row->Timer != nullptr && Row->Timer->IsGpuTimer)
		{
			FGpuScopeSample Sample;
			Sample.ScopeName = Row->Timer->Name != nullptr ? Row->Timer->Name : TEXT("<unknown>");
			Sample.CallCount = (Row->InstanceCount > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(Row->InstanceCount);
			Sample.TotalMs = Row->TotalInclusiveTime * 1000.0;
			Sample.SelfMs = Row->TotalExclusiveTime * 1000.0;
			Sample.AvgMs = Row->AverageInclusiveTime * 1000.0;
			Sample.MaxMs = Row->MaxInclusiveTime * 1000.0;
			OutSamples.Add(Sample);
		}

		Reader->NextRow();
	}

	if (OutSamples.IsEmpty())
	{
		// No GPU timer rows in this interval is a valid trace state; callers should return an empty data array.
		return true;
	}

	OutSamples.Sort([](const FGpuScopeSample& A, const FGpuScopeSample& B)
	{
		if (A.TotalMs == B.TotalMs)
		{
			return A.ScopeName < B.ScopeName;
		}
		return A.TotalMs > B.TotalMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeGpuTopObject(const FGpuScopeSample& Sample)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("gpu_scope_name"), Sample.ScopeName);
	Item->SetNumberField(TEXT("call_count"), Sample.CallCount);
	Item->SetNumberField(TEXT("total_ms"), Sample.TotalMs);
	Item->SetNumberField(TEXT("self_ms"), Sample.SelfMs);
	Item->SetNumberField(TEXT("avg_ms"), Sample.AvgMs);
	Item->SetNumberField(TEXT("max_ms"), Sample.MaxMs);
	return Item;
}

TSharedRef<FJsonObject> MakeGpuPassesObject(const FGpuScopeSample& Sample)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("pass"), Sample.ScopeName);
	Item->SetNumberField(TEXT("gpu_ms"), Sample.TotalMs);
	Item->SetNumberField(TEXT("self_ms"), Sample.SelfMs);
	Item->SetNumberField(TEXT("draw_call_count"), Sample.CallCount);
	return Item;
}

TSharedRef<FJsonObject> MakeGpuPassDetailObject(const FFrameSample& FrameSample, const FGpuScopeSample& Sample)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), FrameSample.FrameIndex);
	Item->SetStringField(TEXT("pass_name"), Sample.ScopeName);
	Item->SetNumberField(TEXT("draw_calls"), Sample.CallCount);
	Item->SetNumberField(TEXT("duration_ms"), Sample.TotalMs);
	Item->SetNumberField(TEXT("self_ms"), Sample.SelfMs);
	return Item;
}
}
