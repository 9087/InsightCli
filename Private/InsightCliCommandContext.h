// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "InsightCliTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"

namespace TraceServices
{
class IAnalysisService;
class IAnalysisSession;
}

namespace UE::InsightCli::Internal
{
struct FFrameSample
{
	int32 FrameIndex = 0;
	double FrameStartMs = 0.0;
	double FrameEndMs = 0.0;
	double FrameTimeMs = 0.0;
	double GameThreadMs = 0.0;
	double RenderThreadMs = 0.0;
	double RhiThreadMs = 0.0;
	double GpuMs = 0.0;
	int32 GameFrameIndex = -1;
	int32 RenderingFrameIndex = -1;
	bool bTraceBacked = false;
};

struct FCounterCatalogEntry
{
	FString Name;
	FString Type;
	FString Unit;
	int32 SampleCount = 0;
	bool bTraceBacked = true;
};

struct FTraceContext
{
	FString FullPath;
	int64 FileSize = 0;
	FDateTime TimeStamp;
	mutable TSharedPtr<TraceServices::IAnalysisService> CachedAnalysisService;
	mutable TSharedPtr<const TraceServices::IAnalysisSession> CachedAnalysisSession;
	mutable bool bAnalysisAttempted = false;
	mutable FString AnalysisFailureStage;
	mutable FString AnalysisFailureReason;
	mutable bool bHasFrameSamples = false;
	mutable TArray<FFrameSample> CachedFrameSamples;
	mutable bool bFrameSamplesTraceBacked = false;
	mutable bool bFrameSamplesFailed = false;
	mutable int32 TraceGameFrameCount = 0;
	mutable int32 TraceRenderingFrameCount = 0;
	mutable double TraceDurationMs = 0.0;
	mutable FString FrameSamplesFailureStage;
	mutable FString FrameSamplesFailureReason;
};

struct FCpuScopeSample
{
	FString ScopeName;
	// -1 means unspecified; non-negative values are trace-backed thread IDs.
	int32 ThreadId = -1;
	int32 CallCount = 0;
	double TotalMs = 0.0;
	double AvgMs = 0.0;
	double MaxMs = 0.0;
	double MinMs = 0.0;
	double SelfMs = 0.0;
};

struct FGpuScopeSample
{
	FString ScopeName;
	int32 CallCount = 0;
	double TotalMs = 0.0;
	double AvgMs = 0.0;
	double MaxMs = 0.0;
};

struct FThreadWaitSample
{
	int32 FrameIndex = 0;
	int32 ThreadId = -1;
	FString ThreadName;
	FString WaitType;
	FString WaitObject;
	double WaitMs = 0.0;
	int32 OwnerThreadId = -1;
	FString OwnerThreadName;
	int32 BlockerThreadId = -1;
	FString BlockerThreadName;
	TArray<int32> BlockedToBlockerThreadChain;
	int32 ChainDepth = 0;
	FString ChainStatus = TEXT("unresolved");
	FString UnresolvedReason;
	double BeginMs = 0.0;
	double EndMs = 0.0;
};

struct FTaskSample
{
	int32 TaskId = -1;
	int32 FrameIndex = 0;
	FString TaskName;
	FString QueueName;
	TArray<int32> DependencyTaskIds;
	FString DependencyStatus = TEXT("resolved");
	FString DependencyIssue;
	double EnqueueMs = 0.0;
	double StartMs = 0.0;
	double EndMs = 0.0;
	double QueueWaitMs = 0.0;
	double RunMs = 0.0;
	double CriticalPathMs = 0.0;
	int32 CriticalPathDepth = 0;
	TArray<int32> CriticalPathTaskChain;
	int32 WorkerThreadId = -1;
};

struct FCounterPoint
{
	double TimestampMs = 0.0;
	double Value = 0.0;
};

struct FMemoryTagSample
{
	FString TagName;
	int64 Bytes = 0;
	double PercentRatio = 0.0;
};

struct FMemorySample
{
	double TimestampMs = 0.0;
	int64 Bytes = 0;
	int32 FrameIndex = -1;
};

struct FMarkSample
{
	double TimestampMs = 0.0;
	FString Category;
	FString Channel;
	FString Message;
	int32 ThreadId = -1;
};

struct FMarksFilter
{
	TOptional<FString> Category;
	TOptional<FString> Channel;
	TOptional<int32> ThreadId;
	TOptional<FString> Keyword;
	bool bCaseSensitive = false;
	bool bExact = false;
};

struct FTimeWindowMs
{
	TOptional<double> StartMs;
	TOptional<double> EndMs;

	bool IsSet() const
	{
		return StartMs.IsSet() || EndMs.IsSet();
	}
};

struct FFrameRange
{
	int32 StartInclusive = 0;
	int32 EndExclusive = 0;
};

struct FResolvedTimeWindowMs
{
	TOptional<double> StartMs;
	TOptional<double> EndMs;
	FString Source = TEXT("full");
	TOptional<FFrameRange> FrameRange;

	bool IsSet() const
	{
		return StartMs.IsSet() || EndMs.IsSet();
	}
};

// Option parsing helpers
bool TryGetIntOption(const TArray<FString>& Args, const TCHAR* LongName, int32& OutValue);
bool TryGetDoubleOption(const TArray<FString>& Args, const TCHAR* LongName, double& OutValue);
bool TryGetStringOption(const TArray<FString>& Args, const TCHAR* LongName, FString& OutValue);
bool HasOption(const TArray<FString>& Args, const TCHAR* LongName);
bool ValidateNoUnknownOptionsWithGlobals(const TArray<FString>& Args, const TArray<FString>& CommandOptionNames, FInsightCliResponse& OutError);
bool TryGetLimitAndOptionalFrameIndexFilter(const TArray<FString>& Args, int32& OutLimit, int32& OutFrameIndexFilter, bool& bOutHasFrameIndex, FInsightCliResponse& OutError);
bool TryGetTimeWindowMs(const TArray<FString>& Args, FTimeWindowMs& OutWindow, FInsightCliResponse& OutError);
bool TryResolveTimeWindowMs(const FTraceContext& Context, const TArray<FString>& Args, bool bAllowFrameRange, FResolvedTimeWindowMs& OutWindow, FInsightCliResponse& OutError);
void AppendTimeWindowMeta(const FResolvedTimeWindowMs& TimeWindow, TMap<FString, FString>& OutMeta);
bool TryGetPositiveLimit(const TArray<FString>& Args, int32 DefaultLimit, int32& OutLimit, FInsightCliResponse& OutError);
bool RequireStringOption(const TArray<FString>& Args, const TCHAR* OptionName, const TCHAR* OwnerCommand, FString& OutValue, FInsightCliResponse& OutError);
FString ToNumberString(double Value);
TMap<FString, FString> MakeNotFoundMeta(const FInsightCliRequest& Request, const FString& Reason, const FString& QueryKey = TEXT(""), const FString& QueryValue = TEXT(""));
FInsightCliResponse MakeOptionError(const FString& Message, const TMap<FString, FString>& Details = {});
FInsightCliResponse MakeNotFoundError(const FString& Message, const TMap<FString, FString>& Details = {});

// Trace/context bootstrap and shared frame data
FInsightCliResponse ValidateTraceAndBuildContext(const FInsightCliRequest& Request, FTraceContext& OutContext);
FInsightCliResponse MakeTraceUnavailableError(
	const FTraceContext& Context,
	const TCHAR* Consumer,
	const FString& FailureStage,
	const FString& FailureReason,
	const TCHAR* DefaultStage,
	const TCHAR* DefaultReason,
	const TCHAR* Message,
	const TMap<FString, FString>& ExtraDetails = {});
bool AcquireAnalysisSession(
	const FTraceContext& Context,
	TSharedPtr<const TraceServices::IAnalysisSession>& OutSession,
	FString& OutFailureStage,
	FString& OutFailureReason);
TArray<FFrameSample> BuildFrameSamples(const FTraceContext& Context);
bool EnsureTraceBackedFrameSamples(const FTraceContext& Context, FInsightCliResponse& OutError, const FString& ConsumerTag);
void ApplyTimeWindowFilter(TArray<FFrameSample>& Frames, const FResolvedTimeWindowMs& TimeWindow);

// JSON builders: info + frames
TSharedRef<FJsonObject> MakeInfoSummaryData(const FTraceContext& Context);
TSharedRef<FJsonObject> MakeInfoChannelsData(const FTraceContext& Context, TMap<FString, FString>& OutMeta);
TSharedRef<FJsonObject> MakeFramesSummaryData(const TArray<FFrameSample>& Frames);
TSharedRef<FJsonObject> MakeFrameObject(const FFrameSample& Sample);

// JSON/data builders: cpu
bool ResolveCpuThreadFilterToTraceId(const FTraceContext& Context, const FString& ThreadFilter, uint32& OutThreadId, FString& OutNormalizedThread, FString& OutFailureStage, FString& OutFailureReason);
bool BuildCpuTopSamples(const FTraceContext& Context, const TOptional<uint32>& CpuThreadId, TArray<FCpuScopeSample>& OutSamples, FString& OutFailureStage, FString& OutFailureReason);
bool BuildCpuStackObject(
	const FTraceContext& Context,
	int32 FrameIndex,
	const TOptional<uint32>& CpuThreadId,
	int32 StackLimit,
	const FString& View,
	TSharedPtr<FJsonObject>& OutObject,
	bool& bOutFound,
	FString& OutFailureStage,
	FString& OutFailureReason);
TSharedRef<FJsonObject> MakeCpuTopObject(const FCpuScopeSample& Sample);

// JSON/data builders: gpu
bool BuildGpuTopSamples(
	const FTraceContext& Context,
	TArray<FGpuScopeSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> IntervalStartSec = {},
	TOptional<double> IntervalEndSec = {});
TSharedRef<FJsonObject> MakeGpuTopObject(const FGpuScopeSample& Sample);
TSharedRef<FJsonObject> MakeGpuPassesObject(const FGpuScopeSample& Sample);
TSharedRef<FJsonObject> MakeGpuPassDetailObject(const FFrameSample& FrameSample, const FGpuScopeSample& Sample);

// JSON/data builders: threads + tasks
bool BuildThreadWaitSamplesTrace(
	const FTraceContext& Context,
	TOptional<int32> FrameIndexFilter,
	TArray<FThreadWaitSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	bool& bOutFrameFound);
TSharedRef<FJsonObject> MakeThreadWaitObject(const FThreadWaitSample& Wait);

bool BuildTaskTopSamples(
	const FTraceContext& Context,
	TOptional<int32> FrameIndexFilter,
	TArray<FTaskSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	bool& bOutFrameFound);
TSharedRef<FJsonObject> MakeTaskObject(const FTaskSample& Task);

// JSON/data builders: symbols + counters
bool BuildSymbolsResolveObject(
	const FTraceContext& Context,
	const FString& ScopeName,
	TSharedPtr<FJsonObject>& OutObject,
	bool& bOutFound,
	FString& OutFailureStage,
	FString& OutFailureReason);
bool BuildCounterCatalog(
	const FTraceContext& Context,
	TArray<FCounterCatalogEntry>& OutCatalog,
	FString& OutFailureStage,
	FString& OutFailureReason);
bool ResolveCounterByName(
	const FTraceContext& Context,
	const FString& RequestedName,
	const TCHAR* OwnerCommand,
	FCounterCatalogEntry& OutEntry,
	FInsightCliResponse& OutError);
bool BuildCounterSeries(
	const FTraceContext& Context,
	const FString& CounterName,
	TArray<FCounterPoint>& OutSeries,
	FString& OutCounterType,
	FString& OutCounterUnit,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs = {},
	TOptional<double> WindowEndMs = {});
TSharedRef<FJsonObject> MakeCounterStatsObject(const TArray<FCounterPoint>& Series, const FString& CounterName);

// JSON/data builders: memory
bool BuildMemorySamplesTrace(
	const FTraceContext& Context,
	TArray<FMemorySample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs = {},
	TOptional<double> WindowEndMs = {});
TSharedRef<FJsonObject> MakeMemorySummaryObject(const TArray<FMemorySample>& Samples);
TSharedRef<FJsonObject> MakeMemoryPeakObject(const TArray<FMemorySample>& Samples);
bool BuildMemoryTagsTrace(
	const FTraceContext& Context,
	TArray<FMemoryTagSample>& OutTags,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs = {},
	TOptional<double> WindowEndMs = {});
TSharedRef<FJsonObject> MakeMemoryTagObject(const FMemoryTagSample& Tag);

// JSON/data builders: marks
bool BuildMarks(
	const FTraceContext& Context,
	const FMarksFilter& Filter,
	TArray<FMarkSample>& OutMarks,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs = {},
	TOptional<double> WindowEndMs = {});
TSharedRef<FJsonObject> MakeMarkObject(const FMarkSample& Mark);

// Response envelope helpers
FString MakeEnvelopeWithObject(const TSharedRef<FJsonObject>& Data, const TMap<FString, FString>& Meta = {});
FString MakeEnvelopeWithArray(const TArray<TSharedPtr<FJsonValue>>& Data, const TMap<FString, FString>& Meta = {});

// Group handlers: info + frames
bool HandleInfoAndFramesCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);

// Group handlers: performance
bool HandleCpuCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleGpuCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleAnimCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleNiagaraCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandlePhysicsCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleSlateCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleThreadsCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleTasksCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandlePerformanceCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);

// Group handlers: data
bool HandleSymbolsCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleCountersCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleMemoryCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleMarksCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleLoadTimeCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleGcCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleIoCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleNetCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleShadersCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
bool HandleDataCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse);
}
