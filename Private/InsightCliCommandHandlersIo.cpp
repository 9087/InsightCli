// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include <limits>

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/LoadTimeProfiler.h"

namespace UE::InsightCli::Internal
{
namespace
{
struct FIoReadRow
{
	FString FilePath;
	double StartMs = 0.0;
	double EndMs = 0.0;
	double DurationMs = 0.0;
	uint64 Offset = 0;
	uint64 SizeBytes = 0;
	uint64 ActualSizeBytes = 0;
	uint32 ThreadId = 0;
	bool bAsync = false;
	bool bFailed = false;
};

struct FIoFileAggregate
{
	FString FilePath;
	int32 ReadCount = 0;
	uint64 ReadBytes = 0;
	double TotalMs = 0.0;
	double MaxMs = 0.0;
};

struct FIoSummary
{
	int32 ReadCount = 0;
	uint64 TotalReadBytes = 0;
	int32 SyncReadCount = 0;
	int32 AsyncReadCount = 0;
	uint64 SyncReadBytes = 0;
	uint64 AsyncReadBytes = 0;
};

bool IsAsyncRead(const TraceServices::FFileActivity& Activity)
{
	return Activity.ReadWriteHandle != 0 && Activity.ReadWriteHandle != uint64(-1);
}

bool BuildIoReadRows(
	const FTraceContext& Context,
	TArray<FIoReadRow>& OutRows,
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
	const TraceServices::IFileActivityProvider* FileActivityProvider = TraceServices::ReadFileActivityProvider(*Session.Get());
	if (FileActivityProvider == nullptr)
	{
		OutChannelState = TEXT("provider_not_available");
		return true;
	}

	FileActivityProvider->EnumerateFileActivity([&OutRows](const TraceServices::FFileInfo& FileInfo, const TraceServices::IFileActivityProvider::Timeline& Timeline)
	{
		Timeline.EnumerateEvents(
			-std::numeric_limits<double>::infinity(),
			+std::numeric_limits<double>::infinity(),
			[&OutRows, &FileInfo](double EventStartTime, double EventEndTime, uint32 /*EventDepth*/, const TraceServices::FFileActivity* FileActivity)
			{
				if (FileActivity == nullptr || FileActivity->ActivityType != TraceServices::FileActivityType_Read)
				{
					return TraceServices::EEventEnumerate::Continue;
				}

				FIoReadRow& Row = OutRows.AddDefaulted_GetRef();
				Row.FilePath = FileInfo.Path != nullptr ? FileInfo.Path : TEXT("<unknown>");
				Row.StartMs = EventStartTime * 1000.0;
				Row.EndMs = EventEndTime * 1000.0;
				Row.DurationMs = FMath::Max(0.0, Row.EndMs - Row.StartMs);
				Row.Offset = FileActivity->Offset;
				Row.SizeBytes = FileActivity->Size;
				Row.ActualSizeBytes = FileActivity->ActualSize;
				Row.ThreadId = FileActivity->ThreadId;
				Row.bAsync = IsAsyncRead(*FileActivity);
				Row.bFailed = FileActivity->Failed;
				return TraceServices::EEventEnumerate::Continue;
			});

		return true;
	});

	OutChannelState = OutRows.IsEmpty() ? TEXT("no_io_reads") : TEXT("trace");
	return true;
}

FIoSummary MakeIoSummary(const TArray<FIoReadRow>& Rows)
{
	FIoSummary Summary;
	for (const FIoReadRow& Row : Rows)
	{
		Summary.ReadCount += 1;
		Summary.TotalReadBytes += Row.ActualSizeBytes;
		if (Row.bAsync)
		{
			Summary.AsyncReadCount += 1;
			Summary.AsyncReadBytes += Row.ActualSizeBytes;
		}
		else
		{
			Summary.SyncReadCount += 1;
			Summary.SyncReadBytes += Row.ActualSizeBytes;
		}
	}
	return Summary;
}

TMap<FString, FIoFileAggregate> BuildFileAggregates(const TArray<FIoReadRow>& Rows)
{
	TMap<FString, FIoFileAggregate> Aggregates;
	for (const FIoReadRow& Row : Rows)
	{
		FIoFileAggregate& Aggregate = Aggregates.FindOrAdd(Row.FilePath);
		if (Aggregate.FilePath.IsEmpty())
		{
			Aggregate.FilePath = Row.FilePath;
		}
		Aggregate.ReadCount += 1;
		Aggregate.ReadBytes += Row.ActualSizeBytes;
		Aggregate.TotalMs += Row.DurationMs;
		Aggregate.MaxMs = FMath::Max(Aggregate.MaxMs, Row.DurationMs);
	}
	return Aggregates;
}
}

bool HandleIoCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("io") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TArray<FIoReadRow> Rows;
		FString ChannelState;
		FString FailureStage;
		FString FailureReason;
		if (!BuildIoReadRows(Context, Rows, ChannelState, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("io.summary"),
				FailureStage,
				FailureReason,
				TEXT("io_provider"),
				TEXT("failed to build io summary"),
				TEXT("Trace-backed file activity data is unavailable for this trace."));
			return true;
		}

		const FIoSummary Summary = MakeIoSummary(Rows);
		const double SyncRatio = Summary.ReadCount > 0 ? static_cast<double>(Summary.SyncReadCount) / static_cast<double>(Summary.ReadCount) : 0.0;
		const double AsyncRatio = Summary.ReadCount > 0 ? static_cast<double>(Summary.AsyncReadCount) / static_cast<double>(Summary.ReadCount) : 0.0;

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("read_count"), Summary.ReadCount);
		Data->SetNumberField(TEXT("total_read_bytes"), static_cast<double>(Summary.TotalReadBytes));
		Data->SetNumberField(TEXT("sync_read_count"), Summary.SyncReadCount);
		Data->SetNumberField(TEXT("async_read_count"), Summary.AsyncReadCount);
		Data->SetNumberField(TEXT("sync_read_ratio"), SyncRatio);
		Data->SetNumberField(TEXT("async_read_ratio"), AsyncRatio);

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("channel_state"), ChannelState);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (ChannelState != TEXT("trace"))
		{
			AddMetaWarning(Meta, TEXT("FileActivity provider not available or read samples are empty."));
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("io") && Request.Action == TEXT("slowest-reads"))
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

		TArray<FIoReadRow> Rows;
		FString ChannelState;
		FString FailureStage;
		FString FailureReason;
		if (!BuildIoReadRows(Context, Rows, ChannelState, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("io.slowest-reads"),
				FailureStage,
				FailureReason,
				TEXT("io_provider"),
				TEXT("failed to build io slowest reads"),
				TEXT("Trace-backed file activity data is unavailable for this trace."));
			return true;
		}

		Rows.Sort([](const FIoReadRow& A, const FIoReadRow& B)
		{
			if (A.DurationMs == B.DurationMs)
			{
				return A.FilePath < B.FilePath;
			}
			return A.DurationMs > B.DurationMs;
		});

		const int32 TakeCount = FMath::Min(Limit, Rows.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FIoReadRow& Row = Rows[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("file_path"), Row.FilePath);
			Item->SetNumberField(TEXT("duration_ms"), Row.DurationMs);
			Item->SetNumberField(TEXT("size_bytes"), static_cast<double>(Row.SizeBytes));
			Item->SetNumberField(TEXT("actual_size_bytes"), static_cast<double>(Row.ActualSizeBytes));
			Item->SetNumberField(TEXT("offset"), static_cast<double>(Row.Offset));
			Item->SetNumberField(TEXT("thread_id"), static_cast<double>(Row.ThreadId));
			Item->SetStringField(TEXT("mode"), Row.bAsync ? TEXT("async") : TEXT("sync"));
			Item->SetBoolField(TEXT("failed"), Row.bFailed);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("channel_state"), ChannelState);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (ChannelState != TEXT("trace"))
		{
			AddMetaWarning(Meta, TEXT("FileActivity provider not available or read samples are empty."));
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("io") && Request.Action == TEXT("top-files"))
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

		TArray<FIoReadRow> Rows;
		FString ChannelState;
		FString FailureStage;
		FString FailureReason;
		if (!BuildIoReadRows(Context, Rows, ChannelState, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("io.top-files"),
				FailureStage,
				FailureReason,
				TEXT("io_provider"),
				TEXT("failed to build io top-files"),
				TEXT("Trace-backed file activity data is unavailable for this trace."));
			return true;
		}

		TMap<FString, FIoFileAggregate> AggregateMap = BuildFileAggregates(Rows);
		TArray<FIoFileAggregate> Aggregates;
		AggregateMap.GenerateValueArray(Aggregates);
		Aggregates.Sort([](const FIoFileAggregate& A, const FIoFileAggregate& B)
		{
			if (A.ReadBytes == B.ReadBytes)
			{
				return A.FilePath < B.FilePath;
			}
			return A.ReadBytes > B.ReadBytes;
		});

		const int32 TakeCount = FMath::Min(Limit, Aggregates.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FIoFileAggregate& Aggregate = Aggregates[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			const double AvgReadMs = Aggregate.ReadCount > 0 ? Aggregate.TotalMs / static_cast<double>(Aggregate.ReadCount) : 0.0;
			Item->SetStringField(TEXT("file_path"), Aggregate.FilePath);
			Item->SetNumberField(TEXT("read_count"), Aggregate.ReadCount);
			Item->SetNumberField(TEXT("read_bytes"), static_cast<double>(Aggregate.ReadBytes));
			Item->SetNumberField(TEXT("total_ms"), Aggregate.TotalMs);
			Item->SetNumberField(TEXT("avg_read_ms"), AvgReadMs);
			Item->SetNumberField(TEXT("max_read_ms"), Aggregate.MaxMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("channel_state"), ChannelState);
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		if (ChannelState != TEXT("trace"))
		{
			AddMetaWarning(Meta, TEXT("FileActivity provider not available or read samples are empty."));
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
