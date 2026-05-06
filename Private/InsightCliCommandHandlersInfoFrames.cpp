// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/Containers/Tables.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UE::InsightCli::Internal
{
namespace
{
TArray<TSharedPtr<FJsonValue>> BuildThreadBreakdownData(const FFrameSample& Frame)
{
	const double DenominatorMs = FMath::Max(KINDA_SMALL_NUMBER, Frame.FrameTimeMs);

	struct FThreadBucket
	{
		const TCHAR* Name = TEXT("");
		double Ms = 0.0;
	};

	TArray<FThreadBucket> Buckets;
	Buckets.Add({ TEXT("game_thread"), FMath::Max(0.0, Frame.GameThreadMs) });
	Buckets.Add({ TEXT("render_thread"), FMath::Max(0.0, Frame.RenderThreadMs) });
	Buckets.Add({ TEXT("rhi_thread"), FMath::Max(0.0, Frame.RhiThreadMs) });
	Buckets.Add({ TEXT("gpu"), FMath::Max(0.0, Frame.GpuMs) });

	const double KnownMs = FMath::Max(0.0, Frame.GameThreadMs) + FMath::Max(0.0, Frame.RenderThreadMs)
		+ FMath::Max(0.0, Frame.RhiThreadMs) + FMath::Max(0.0, Frame.GpuMs);
	Buckets.Add({ TEXT("other"), FMath::Max(0.0, Frame.FrameTimeMs - KnownMs) });

	Buckets.Sort([](const FThreadBucket& A, const FThreadBucket& B)
	{
		if (A.Ms == B.Ms)
		{
			return FCString::Stricmp(A.Name, B.Name) < 0;
		}
		return A.Ms > B.Ms;
	});

	TArray<TSharedPtr<FJsonValue>> Data;
	Data.Reserve(Buckets.Num());
	for (const FThreadBucket& Bucket : Buckets)
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("bucket"), Bucket.Name);
		Item->SetNumberField(TEXT("ms"), Bucket.Ms);
		Item->SetNumberField(TEXT("ratio"), Bucket.Ms / DenominatorMs);
		Data.Add(MakeShared<FJsonValueObject>(Item));
	}

	return Data;
}

bool BuildStatGroupBreakdownData(
	const FTraceContext& Context,
	const FFrameSample& Frame,
	TArray<TSharedPtr<FJsonValue>>& OutData,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutData.Reset();
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

		TraceServices::FCreateAggreationParams Params;
		Params.IntervalStart = FMath::Max(0.0, Frame.FrameStartMs / 1000.0);
		Params.IntervalEnd = FMath::Max(Params.IntervalStart + KINDA_SMALL_NUMBER, Frame.FrameEndMs / 1000.0);
		Params.IncludeGpu = false;
		Params.FrameType = ETraceFrameType::TraceFrameType_Count;
		Params.CpuThreadFilter = [](uint32)
		{
			return true;
		};

		StatsTable.Reset(TimingProfilerProvider->CreateAggregation(Params));
	}

	if (!StatsTable.IsValid())
	{
		return true;
	}

	TUniquePtr<TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>> Reader(StatsTable->CreateReader());
	if (!Reader.IsValid())
	{
		OutFailureStage = TEXT("aggregation_reader");
		OutFailureReason = TEXT("failed to create frame cpu aggregation table reader");
		return false;
	}

	TMap<FString, double> BucketMs;
	while (Reader->IsValid())
	{
		const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
		if (Row != nullptr && Row->Timer != nullptr && !Row->Timer->IsGpuTimer)
		{
			const FString ScopeName = Row->Timer->Name != nullptr ? Row->Timer->Name : TEXT("<unknown>");
			const FString Bucket = ClassifyCpuStatGroup(ScopeName);
			double& Ms = BucketMs.FindOrAdd(Bucket);
			Ms += FMath::Max(0.0, Row->TotalExclusiveTime * 1000.0);
		}

		Reader->NextRow();
	}

	struct FStatBucket
	{
		FString Name;
		double Ms = 0.0;
	};

	TArray<FStatBucket> Buckets;
	double TotalMs = 0.0;
	for (const TPair<FString, double>& Pair : BucketMs)
	{
		FStatBucket Row;
		Row.Name = Pair.Key;
		Row.Ms = Pair.Value;
		Buckets.Add(MoveTemp(Row));
		TotalMs += Pair.Value;
	}

	Buckets.Sort([](const FStatBucket& A, const FStatBucket& B)
	{
		if (A.Ms == B.Ms)
		{
			return A.Name < B.Name;
		}
		return A.Ms > B.Ms;
	});

	const double DenominatorMs = FMath::Max(KINDA_SMALL_NUMBER, TotalMs > 0.0 ? TotalMs : Frame.FrameTimeMs);
	OutData.Reserve(Buckets.Num());
	for (const FStatBucket& Bucket : Buckets)
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("bucket"), Bucket.Name);
		Item->SetNumberField(TEXT("ms"), Bucket.Ms);
		Item->SetNumberField(TEXT("ratio"), Bucket.Ms / DenominatorMs);
		OutData.Add(MakeShared<FJsonValueObject>(Item));
	}

	return true;
}
}

bool HandleInfoAndFramesCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	if (Request.Group == TEXT("info") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TArray<FString> UnavailableFields;
		const TSharedRef<FJsonObject> Data = MakeInfoSummaryData(Context, UnavailableFields);

		TSharedPtr<FJsonObject> MetaObject;
		if (UnavailableFields.Num() > 0)
		{
			MetaObject = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> UnavailableFieldValues;
			UnavailableFieldValues.Reserve(UnavailableFields.Num());
			for (const FString& FieldName : UnavailableFields)
			{
				UnavailableFieldValues.Add(MakeShared<FJsonValueString>(FieldName));
			}
			MetaObject->SetArrayField(TEXT("unavailable_fields"), UnavailableFieldValues);
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObjectAndMeta(Data, MetaObject));
		return true;
	}

	if (Request.Group == TEXT("info") && Request.Action == TEXT("channels"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, {}, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TMap<FString, FString> Meta;
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeInfoChannelsData(Context, Meta), Meta));
		return true;
	}

	if (Request.Group == TEXT("frames") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("frames.summary")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		ApplyTimeWindowFilter(Frames, TimeWindow);

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("game_frame_count"), FString::FromInt(Context.TraceGameFrameCount));
		Meta.Add(TEXT("rendering_frame_count"), FString::FromInt(Context.TraceRenderingFrameCount));
		AppendTimeWindowMeta(TimeWindow, Meta);

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeFramesSummaryData(Frames), Meta));
		return true;
	}

	if (Request.Group == TEXT("frames") && Request.Action == TEXT("slowest"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("time-start"), TEXT("time-end"), TEXT("frame-range") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FResolvedTimeWindowMs TimeWindow;
		FInsightCliResponse TimeWindowError;
		if (!TryResolveTimeWindowMs(Context, Request.Args, true, TimeWindow, TimeWindowError))
		{
			OutResponse = TimeWindowError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("frames.slowest")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		ApplyTimeWindowFilter(Frames, TimeWindow);

		Frames.Sort([](const FFrameSample& A, const FFrameSample& B)
		{
			if (A.FrameTimeMs == B.FrameTimeMs)
			{
				return A.FrameIndex < B.FrameIndex;
			}
			return A.FrameTimeMs > B.FrameTimeMs;
		});

		const int32 TakeCount = FMath::Min(Limit, Frames.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeFrameObject(Frames[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
		Meta.Add(TEXT("game_frame_count"), FString::FromInt(Context.TraceGameFrameCount));
		Meta.Add(TEXT("rendering_frame_count"), FString::FromInt(Context.TraceRenderingFrameCount));
		AppendTimeWindowMeta(TimeWindow, Meta);
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("frames") && Request.Action == TEXT("detail"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("breakdown") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 FrameIndex = -1;
		if (!TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			OutResponse = MakeOptionError(TEXT("--frame-index is required for frames detail."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}

		FString Breakdown;
		const bool bHasBreakdown = TryGetStringOption(Request.Args, TEXT("--breakdown"), Breakdown);
		if (bHasBreakdown)
		{
			Breakdown = Breakdown.ToLower();
			if (Breakdown != TEXT("thread") && Breakdown != TEXT("statgroup"))
			{
				OutResponse = MakeOptionError(TEXT("breakdown must be one of: thread, statgroup."));
				return true;
			}
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("frames.detail")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		const FFrameSample* Found = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
		{
			return Item.FrameIndex == FrameIndex;
		});

		if (Found == nullptr)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
			if (bHasBreakdown)
			{
				Meta.Add(TEXT("breakdown"), Breakdown);
			}
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(MakeShared<FJsonObject>(), Meta));
			return true;
		}

		TSharedRef<FJsonObject> DetailObject = MakeFrameObject(*Found);
		if (bHasBreakdown)
		{
			TArray<TSharedPtr<FJsonValue>> BreakdownData;
			if (Breakdown == TEXT("thread"))
			{
				BreakdownData = BuildThreadBreakdownData(*Found);
			}
			else
			{
				FString FailureStage;
				FString FailureReason;
				if (!BuildStatGroupBreakdownData(Context, *Found, BreakdownData, FailureStage, FailureReason))
				{
					OutResponse = MakeTraceUnavailableError(
						Context,
						TEXT("frames.detail"),
						FailureStage,
						FailureReason,
						TEXT("frame_breakdown"),
						TEXT("failed to build frame statgroup breakdown"),
						TEXT("Trace-backed frame detail breakdown is unavailable for this trace."),
						{{TEXT("frame_index"), FString::FromInt(FrameIndex)}});
					return true;
				}
			}

			DetailObject->SetArrayField(TEXT("breakdown"), BreakdownData);
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
		if (bHasBreakdown)
		{
			Meta.Add(TEXT("breakdown"), Breakdown);
			if (Breakdown == TEXT("statgroup"))
			{
				Meta.Add(TEXT("classifier"), TEXT("strict_token"));
			}
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(DetailObject, Meta));
		return true;
	}

	return false;
}
}
