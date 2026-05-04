// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Algo/Sort.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/LoadTimeProfiler.h"

namespace UE::InsightCli::Internal
{
namespace
{
enum class ELoadTimeSortBy : uint8
{
	Total,
	Serialize,
	PostLoad,
};

struct FLoadTimePackageRow
{
	uint32 PackageId = 0;
	FString PackageName;
	FString ClassName;
	uint64 FileSizeBytes = 0;
	uint64 SerializedExportsCount = 0;
	double MainThreadMs = 0.0;
	double AsyncLoadingThreadMs = 0.0;
	double TotalLoadMs = 0.0;
	double CreateExportMs = 0.0;
	double SerializeMs = 0.0;
	double PostLoadMs = 0.0;
};

struct FLoadTimeSummary
{
	int32 PackageCount = 0;
	double TotalLoadMs = 0.0;
	double MainThreadMs = 0.0;
	double AsyncLoadingThreadMs = 0.0;
	double CreateExportMs = 0.0;
	double SerializeMs = 0.0;
	double PostLoadMs = 0.0;
	uint64 TotalSerializedBytes = 0;
};

struct FLoadTimeTimelineRow
{
	uint64 RequestId = 0;
	FString RequestName;
	double StartMs = 0.0;
	double DurationMs = 0.0;
	double EndMs = 0.0;
	TArray<FString> Packages;
};

struct FLoadTimeStageInfo
{
	double CreateExportMs = 0.0;
	double SerializeMs = 0.0;
	double PostLoadMs = 0.0;
	FString ClassName;
	uint64 ClassSerializedSize = 0;
};

bool BuildLoadTimePackageRows(
	const FTraceContext& Context,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs,
	TArray<FLoadTimePackageRow>& OutRows,
	FLoadTimeSummary& OutSummary,
	FString& OutChannelState,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutRows.Reset();
	OutSummary = FLoadTimeSummary();
	OutChannelState = TEXT("unavailable");
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
	const TraceServices::ILoadTimeProfilerProvider* LoadTimeProvider = TraceServices::ReadLoadTimeProfilerProvider(*Session.Get());
	if (LoadTimeProvider == nullptr)
	{
		OutChannelState = TEXT("provider_not_available");
		return true;
	}

	const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
	const double DurationSec = Session->GetDurationSeconds();
	const double EndSecRaw = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;
	const double EndSec = FMath::Max(StartSec + KINDA_SMALL_NUMBER, EndSecRaw);

	TUniquePtr<TraceServices::ITable<TraceServices::FPackagesTableRow>> PackagesTable(LoadTimeProvider->CreatePackageDetailsTable(StartSec, EndSec));
	TUniquePtr<TraceServices::ITable<TraceServices::FExportsTableRow>> ExportsTable(LoadTimeProvider->CreateExportDetailsTable(StartSec, EndSec));
	if (!PackagesTable.IsValid() || !ExportsTable.IsValid())
	{
		OutChannelState = TEXT("no_loadtime_data");
		return true;
	}

	TMap<uint32, FLoadTimeStageInfo> StageByPackage;
	{
		TUniquePtr<TraceServices::ITableReader<TraceServices::FExportsTableRow>> Reader(ExportsTable->CreateReader());
		while (Reader.IsValid() && Reader->IsValid())
		{
			const TraceServices::FExportsTableRow* Row = Reader->GetCurrentRow();
			if (Row != nullptr && Row->ExportInfo != nullptr && Row->ExportInfo->Package != nullptr)
			{
				const uint32 PackageId = Row->ExportInfo->Package->Id;
				FLoadTimeStageInfo& StageInfo = StageByPackage.FindOrAdd(PackageId);
				const double StageMs = (Row->MainThreadTime + Row->AsyncLoadingThreadTime) * 1000.0;

				switch (Row->EventType)
				{
				case TraceServices::LoadTimeProfilerObjectEventType_Create:
					StageInfo.CreateExportMs += StageMs;
					OutSummary.CreateExportMs += StageMs;
					break;
				case TraceServices::LoadTimeProfilerObjectEventType_Serialize:
					StageInfo.SerializeMs += StageMs;
					OutSummary.SerializeMs += StageMs;
					break;
				case TraceServices::LoadTimeProfilerObjectEventType_PostLoad:
					StageInfo.PostLoadMs += StageMs;
					OutSummary.PostLoadMs += StageMs;
					break;
				default:
					break;
				}

				if (Row->ExportInfo->Class != nullptr && Row->ExportInfo->Class->Name != nullptr)
				{
					const uint64 SerialSize = Row->ExportInfo->SerialSize;
					if (SerialSize > StageInfo.ClassSerializedSize)
					{
						StageInfo.ClassSerializedSize = SerialSize;
						StageInfo.ClassName = Row->ExportInfo->Class->Name;
					}
				}
			}

			Reader->NextRow();
		}
	}

	{
		TUniquePtr<TraceServices::ITableReader<TraceServices::FPackagesTableRow>> Reader(PackagesTable->CreateReader());
		while (Reader.IsValid() && Reader->IsValid())
		{
			const TraceServices::FPackagesTableRow* Row = Reader->GetCurrentRow();
			if (Row != nullptr && Row->PackageInfo != nullptr)
			{
				FLoadTimePackageRow PackageRow;
				PackageRow.PackageId = Row->PackageInfo->Id;
				PackageRow.PackageName = Row->PackageInfo->Name != nullptr ? Row->PackageInfo->Name : TEXT("<unknown>");
				PackageRow.FileSizeBytes = Row->TotalSerializedSize;
				PackageRow.SerializedExportsCount = Row->SerializedExportsCount;
				PackageRow.MainThreadMs = Row->MainThreadTime * 1000.0;
				PackageRow.AsyncLoadingThreadMs = Row->AsyncLoadingThreadTime * 1000.0;
				PackageRow.TotalLoadMs = PackageRow.MainThreadMs + PackageRow.AsyncLoadingThreadMs;

				if (const FLoadTimeStageInfo* StageInfo = StageByPackage.Find(PackageRow.PackageId))
				{
					PackageRow.CreateExportMs = StageInfo->CreateExportMs;
					PackageRow.SerializeMs = StageInfo->SerializeMs;
					PackageRow.PostLoadMs = StageInfo->PostLoadMs;
					PackageRow.ClassName = StageInfo->ClassName;
				}
				if (PackageRow.ClassName.IsEmpty())
				{
					PackageRow.ClassName = TEXT("<unknown>");
				}

				OutSummary.PackageCount += 1;
				OutSummary.TotalLoadMs += PackageRow.TotalLoadMs;
				OutSummary.MainThreadMs += PackageRow.MainThreadMs;
				OutSummary.AsyncLoadingThreadMs += PackageRow.AsyncLoadingThreadMs;
				OutSummary.TotalSerializedBytes += PackageRow.FileSizeBytes;

				OutRows.Add(MoveTemp(PackageRow));
			}

			Reader->NextRow();
		}
	}

	OutChannelState = OutRows.IsEmpty() ? TEXT("no_loadtime_data") : TEXT("trace");
	return true;
}

bool BuildLoadTimeTimelineRows(
	const FTraceContext& Context,
	const FResolvedTimeWindowMs& TimeWindow,
	TArray<FLoadTimeTimelineRow>& OutRows,
	FString& OutChannelState,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutRows.Reset();
	OutChannelState = TEXT("unavailable");
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
	const TraceServices::ILoadTimeProfilerProvider* LoadTimeProvider = TraceServices::ReadLoadTimeProfilerProvider(*Session.Get());
	if (LoadTimeProvider == nullptr)
	{
		OutChannelState = TEXT("provider_not_available");
		return true;
	}

	const double StartSec = TimeWindow.StartMs.IsSet() ? FMath::Max(0.0, TimeWindow.StartMs.GetValue() / 1000.0) : 0.0;
	const double DurationSec = Session->GetDurationSeconds();
	const double EndSecRaw = TimeWindow.EndMs.IsSet() ? FMath::Max(StartSec, TimeWindow.EndMs.GetValue() / 1000.0) : DurationSec;
	const double EndSec = FMath::Max(StartSec + KINDA_SMALL_NUMBER, EndSecRaw);

	TUniquePtr<TraceServices::ITable<TraceServices::FRequestsTableRow>> RequestsTable(LoadTimeProvider->CreateRequestsTable(StartSec, EndSec));
	if (!RequestsTable.IsValid())
	{
		OutChannelState = TEXT("no_loadtime_data");
		return true;
	}

	TUniquePtr<TraceServices::ITableReader<TraceServices::FRequestsTableRow>> Reader(RequestsTable->CreateReader());
	while (Reader.IsValid() && Reader->IsValid())
	{
		const TraceServices::FRequestsTableRow* Row = Reader->GetCurrentRow();
		if (Row != nullptr)
		{
			FLoadTimeTimelineRow Item;
			Item.RequestId = Row->Id;
			Item.RequestName = Row->Name != nullptr ? Row->Name : TEXT("<unknown>");
			Item.StartMs = Row->StartTime * 1000.0;
			Item.DurationMs = Row->Duration * 1000.0;
			Item.EndMs = Item.StartMs + Item.DurationMs;
			for (const TraceServices::FPackageInfo* PackageInfo : Row->Packages)
			{
				if (PackageInfo != nullptr && PackageInfo->Name != nullptr)
				{
					Item.Packages.Add(PackageInfo->Name);
				}
			}
			OutRows.Add(MoveTemp(Item));
		}

		Reader->NextRow();
	}

	OutRows.Sort([](const FLoadTimeTimelineRow& A, const FLoadTimeTimelineRow& B)
	{
		if (A.StartMs == B.StartMs)
		{
			return A.RequestId < B.RequestId;
		}
		return A.StartMs < B.StartMs;
	});

	OutChannelState = OutRows.IsEmpty() ? TEXT("no_loadtime_data") : TEXT("trace");
	return true;
}

ELoadTimeSortBy ParseSortBy(const FString& Value)
{
	if (Value.Equals(TEXT("serialize"), ESearchCase::IgnoreCase))
	{
		return ELoadTimeSortBy::Serialize;
	}
	if (Value.Equals(TEXT("postload"), ESearchCase::IgnoreCase))
	{
		return ELoadTimeSortBy::PostLoad;
	}
	return ELoadTimeSortBy::Total;
}

bool IsValidSortBy(const FString& Value)
{
	return Value.Equals(TEXT("total"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("serialize"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("postload"), ESearchCase::IgnoreCase);
}

void SortPackageRows(TArray<FLoadTimePackageRow>& Rows, ELoadTimeSortBy SortBy)
{
	Rows.Sort([SortBy](const FLoadTimePackageRow& A, const FLoadTimePackageRow& B)
	{
		double AValue = A.TotalLoadMs;
		double BValue = B.TotalLoadMs;
		if (SortBy == ELoadTimeSortBy::Serialize)
		{
			AValue = A.SerializeMs;
			BValue = B.SerializeMs;
		}
		else if (SortBy == ELoadTimeSortBy::PostLoad)
		{
			AValue = A.PostLoadMs;
			BValue = B.PostLoadMs;
		}

		if (!FMath::IsNearlyEqual(AValue, BValue))
		{
			return AValue > BValue;
		}
		return A.PackageName < B.PackageName;
	});
}

void AddLoadTimePackageJson(TArray<TSharedPtr<FJsonValue>>& OutData, const FLoadTimePackageRow& Row)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("package_name"), Row.PackageName);
	Item->SetStringField(TEXT("class_name"), Row.ClassName);
	Item->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(Row.FileSizeBytes));
	Item->SetNumberField(TEXT("serialized_exports_count"), static_cast<double>(Row.SerializedExportsCount));
	Item->SetNumberField(TEXT("total_load_ms"), Row.TotalLoadMs);
	Item->SetNumberField(TEXT("main_thread_ms"), Row.MainThreadMs);
	Item->SetNumberField(TEXT("async_loading_thread_ms"), Row.AsyncLoadingThreadMs);
	Item->SetNumberField(TEXT("create_export_ms"), Row.CreateExportMs);
	Item->SetNumberField(TEXT("serialize_ms"), Row.SerializeMs);
	Item->SetNumberField(TEXT("postload_ms"), Row.PostLoadMs);
	OutData.Add(MakeShared<FJsonValueObject>(Item));
}
}

bool HandleLoadTimeCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("loadtime") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TArray<FLoadTimePackageRow> Rows;
		FLoadTimeSummary Summary;
		FString ChannelState;
		FString FailureStage;
		FString FailureReason;
		if (!BuildLoadTimePackageRows(Context, {}, {}, Rows, Summary, ChannelState, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("loadtime.summary"),
				FailureStage,
				FailureReason,
				TEXT("loadtime_provider"),
				TEXT("failed to query loadtime package table"),
				TEXT("Trace-backed load time diagnostics are unavailable for this trace."));
			return true;
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("package_count"), Summary.PackageCount);
		Data->SetNumberField(TEXT("total_load_ms"), Summary.TotalLoadMs);
		Data->SetNumberField(TEXT("main_thread_ms"), Summary.MainThreadMs);
		Data->SetNumberField(TEXT("async_loading_thread_ms"), Summary.AsyncLoadingThreadMs);
		Data->SetNumberField(TEXT("create_export_ms"), Summary.CreateExportMs);
		Data->SetNumberField(TEXT("serialize_ms"), Summary.SerializeMs);
		Data->SetNumberField(TEXT("postload_ms"), Summary.PostLoadMs);
		Data->SetNumberField(TEXT("total_serialized_bytes"), static_cast<double>(Summary.TotalSerializedBytes));

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), ChannelState == TEXT("trace") ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("channel_state"), ChannelState);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("loadtime") && Request.Action == TEXT("packages"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("sort-by") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		FString SortByText = TEXT("total");
		if (TryGetStringOption(Request.Args, TEXT("--sort-by"), SortByText) && !IsValidSortBy(SortByText))
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("sort_by"), SortByText);
			OutResponse = MakeOptionError(TEXT("sort-by must be one of total|serialize|postload."), Details);
			return true;
		}
		const ELoadTimeSortBy SortBy = ParseSortBy(SortByText);

		TArray<FLoadTimePackageRow> Rows;
		FLoadTimeSummary Summary;
		FString ChannelState;
		FString FailureStage;
		FString FailureReason;
		if (!BuildLoadTimePackageRows(Context, {}, {}, Rows, Summary, ChannelState, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("loadtime.packages"),
				FailureStage,
				FailureReason,
				TEXT("loadtime_provider"),
				TEXT("failed to query loadtime package table"),
				TEXT("Trace-backed load time diagnostics are unavailable for this trace."));
			return true;
		}

		SortPackageRows(Rows, SortBy);

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			AddLoadTimePackageJson(Data, Rows[Index]);
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("sort_by"), SortByText.ToLower());
		Meta.Add(TEXT("data_source"), ChannelState == TEXT("trace") ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("channel_state"), ChannelState);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("loadtime") && Request.Action == TEXT("slowest"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		TArray<FLoadTimePackageRow> Rows;
		FLoadTimeSummary Summary;
		FString ChannelState;
		FString FailureStage;
		FString FailureReason;
		if (!BuildLoadTimePackageRows(Context, {}, {}, Rows, Summary, ChannelState, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("loadtime.slowest"),
				FailureStage,
				FailureReason,
				TEXT("loadtime_provider"),
				TEXT("failed to query loadtime package table"),
				TEXT("Trace-backed load time diagnostics are unavailable for this trace."));
			return true;
		}

		SortPackageRows(Rows, ELoadTimeSortBy::Total);

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			AddLoadTimePackageJson(Data, Rows[Index]);
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("data_source"), ChannelState == TEXT("trace") ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("channel_state"), ChannelState);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("loadtime") && Request.Action == TEXT("timeline"))
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

		TArray<FLoadTimeTimelineRow> Rows;
		FString ChannelState;
		FString FailureStage;
		FString FailureReason;
		if (!BuildLoadTimeTimelineRows(Context, TimeWindow, Rows, ChannelState, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("loadtime.timeline"),
				FailureStage,
				FailureReason,
				TEXT("loadtime_provider"),
				TEXT("failed to query loadtime request timeline"),
				TEXT("Trace-backed load time diagnostics are unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Rows.Num());
		for (const FLoadTimeTimelineRow& Row : Rows)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("request_id"), static_cast<double>(Row.RequestId));
			Item->SetStringField(TEXT("request_name"), Row.RequestName);
			Item->SetNumberField(TEXT("start_ms"), Row.StartMs);
			Item->SetNumberField(TEXT("end_ms"), Row.EndMs);
			Item->SetNumberField(TEXT("duration_ms"), Row.DurationMs);
			Item->SetNumberField(TEXT("package_count"), Row.Packages.Num());

			TArray<TSharedPtr<FJsonValue>> PackageValues;
			PackageValues.Reserve(Row.Packages.Num());
			for (const FString& PackageName : Row.Packages)
			{
				PackageValues.Add(MakeShared<FJsonValueString>(PackageName));
			}
			Item->SetArrayField(TEXT("packages"), PackageValues);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		AppendTimeWindowMeta(TimeWindow, Meta);
		Meta.Add(TEXT("data_source"), ChannelState == TEXT("trace") ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("channel_state"), ChannelState);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
