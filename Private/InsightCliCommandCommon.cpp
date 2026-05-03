// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Containers/UnrealString.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/ContextSwitches.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Log.h"
#include "TraceServices/Model/Memory.h"
#include "TraceServices/Model/Modules.h"
#include "TraceServices/Model/AllocationsProvider.h"
#include "TraceServices/Model/TasksProfiler.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

#if PLATFORM_WINDOWS
#include <excpt.h>
#endif

namespace UE::InsightCli::Internal
{
namespace
{
struct FOptionCache
{
	TArray<FString> CachedArgs;
	TMap<FString, FString> ValueByName;
	TSet<FString> PresentOptions;
};

FString GetTraceExtension(const FString& InPath)
{
	FString Ext = FPaths::GetExtension(InPath, true);
	Ext = Ext.ToLower();
	return Ext;
}

FString GetOptionName(const FString& Token)
{
	if (Token.StartsWith(TEXT("--")))
	{
		return Token.Mid(2);
	}

	if (Token.StartsWith(TEXT("-")))
	{
		return Token.Mid(1);
	}

	return FString();
}

FString GetCanonicalOptionName(const TCHAR* LongName)
{
	FString Name = LongName;
	if (Name.StartsWith(TEXT("--")))
	{
		Name.RightChopInline(2, EAllowShrinking::No);
	}
	else if (Name.StartsWith(TEXT("-")))
	{
		Name.RightChopInline(1, EAllowShrinking::No);
	}

	return Name;
}

bool IsSameArgs(const TArray<FString>& A, const TArray<FString>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index] != B[Index])
		{
			return false;
		}
	}

	return true;
}

bool IsNegativeNumberToken(const FString& Token)
{
	if (Token.Len() <= 1 || Token[0] != TCHAR('-'))
	{
		return false;
	}

	double Parsed = 0.0;
	return FDefaultValueHelper::ParseDouble(Token, Parsed);
}

bool ShouldTreatNextTokenAsOptionValue(const FString& NextToken)
{
	if (!NextToken.StartsWith(TEXT("-")))
	{
		return true;
	}

	// Keep support for negative numeric values while avoiding flag swallowing.
	return IsNegativeNumberToken(NextToken);
}

bool IsDecompStrictGuardBypassed()
{
	static TOptional<bool> bCachedBypass;
	if (bCachedBypass.IsSet())
	{
		return bCachedBypass.GetValue();
	}

	FString Value = FPlatformMisc::GetEnvironmentVariable(TEXT("INSIGHTCLI_ALLOW_DECOMP_STRICT"));
	Value.TrimStartAndEndInline();

	const bool bBypass =
		Value.Equals(TEXT("1"), ESearchCase::CaseSensitive) ||
		Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
		Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase) ||
		Value.Equals(TEXT("on"), ESearchCase::IgnoreCase);

	bCachedBypass = bBypass;
	return bBypass;
}

TSharedPtr<const TraceServices::IAnalysisSession> StartAnalysisSafely(
	const TSharedPtr<TraceServices::IAnalysisService>& AnalysisService,
	const FString& TracePath,
	uint32& OutSehCode)
{
	OutSehCode = 0;

#if PLATFORM_WINDOWS
	__try
	{
		return AnalysisService->StartAnalysis(*TracePath);
	}
	__except ((OutSehCode = static_cast<uint32>(_exception_code())), 1)
	{
		return nullptr;
	}
#else
	return AnalysisService->StartAnalysis(*TracePath);
#endif
}

bool OpenAnalysisSession(
	const FString& TracePath,
	TSharedPtr<TraceServices::IAnalysisService>& OutService,
	TSharedPtr<const TraceServices::IAnalysisSession>& OutSession,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutService.Reset();
	OutSession.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	ITraceServicesModule* TraceServicesModule = FModuleManager::LoadModulePtr<ITraceServicesModule>(TEXT("TraceServices"));
	if (TraceServicesModule == nullptr)
	{
		OutFailureStage = TEXT("module_load");
		OutFailureReason = TEXT("TraceServices module not available");
		return false;
	}

	OutService = TraceServicesModule->GetAnalysisService();
	if (!OutService.IsValid())
	{
		OutService = TraceServicesModule->CreateAnalysisService();
	}

	if (!OutService.IsValid())
	{
		OutFailureStage = TEXT("analysis_service");
		OutFailureReason = TEXT("analysis service unavailable");
		OutSession.Reset();
		return false;
	}

	uint32 StartAnalysisSehCode = 0;
	OutSession = StartAnalysisSafely(OutService, TracePath, StartAnalysisSehCode);
	if (!OutSession.IsValid())
	{
		OutFailureStage = TEXT("start_analysis");
		OutFailureReason = (StartAnalysisSehCode != 0)
			? FString::Printf(TEXT("StartAnalysis raised SEH 0x%08X"), StartAnalysisSehCode)
			: TEXT("StartAnalysis returned null session");
		OutService.Reset();
		return false;
	}

	OutSession->Wait();
	return true;
}

const FOptionCache& GetOptionCache(const TArray<FString>& Args)
{
	static thread_local FOptionCache Cache;
	if (IsSameArgs(Cache.CachedArgs, Args))
	{
		return Cache;
	}

	Cache.CachedArgs = Args;
	Cache.ValueByName.Reset();
	Cache.PresentOptions.Reset();

	for (int32 Index = 0; Index < Args.Num(); ++Index)
	{
		const FString& Arg = Args[Index];
		if (!Arg.StartsWith(TEXT("-")))
		{
			continue;
		}

		const int32 EqualPos = Arg.Find(TEXT("="));
		if (EqualPos >= 0)
		{
			const FString NamePart = Arg.Left(EqualPos);
			const FString OptionName = GetOptionName(NamePart);
			if (!OptionName.IsEmpty())
			{
				Cache.PresentOptions.Add(OptionName);
				if (!Cache.ValueByName.Contains(OptionName))
				{
					Cache.ValueByName.Add(OptionName, Arg.Mid(EqualPos + 1));
				}
			}
			continue;
		}

		const FString OptionName = GetOptionName(Arg);
		if (OptionName.IsEmpty())
		{
			continue;
		}

		Cache.PresentOptions.Add(OptionName);
		if (Index + 1 < Args.Num())
		{
			const FString& NextToken = Args[Index + 1];
			if (ShouldTreatNextTokenAsOptionValue(NextToken))
			{
				if (!Cache.ValueByName.Contains(OptionName))
				{
					Cache.ValueByName.Add(OptionName, NextToken);
				}
				++Index;
			}
		}
	}

	return Cache;
}

FString SerializeJson(const TSharedRef<FJsonObject>& Root)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

bool IsSwitchToken(const FString& Arg, const TCHAR* LongName)
{
	return Arg == LongName || Arg == FString::Printf(TEXT("-%s"), LongName + 2);
}

bool TryParseInt(const FString& Text, int32& OutValue)
{
	if (Text.IsEmpty())
	{
		return false;
	}

	const TCHAR* Buffer = *Text;
	TCHAR* End = nullptr;
	const int64 Parsed = FCString::Strtoi64(Buffer, &End, 10);
	if (End == nullptr || *End != 0)
	{
		return false;
	}

	if (Parsed < MIN_int32 || Parsed > MAX_int32)
	{
		return false;
	}

	OutValue = static_cast<int32>(Parsed);
	return true;
}

void SetFrameSampleFailure(const FTraceContext& Context, const TCHAR* Stage, const TCHAR* Reason)
{
	Context.bFrameSamplesTraceBacked = false;
	Context.bFrameSamplesFailed = true;
	Context.TraceGameFrameCount = 0;
	Context.TraceRenderingFrameCount = 0;
	Context.TraceDurationMs = 0.0;
	Context.CachedFrameSamples.Reset();
	Context.FrameSamplesFailureStage = Stage;
	Context.FrameSamplesFailureReason = Reason;
	Context.bHasFrameSamples = true;
}
}

bool TryGetIntOption(const TArray<FString>& Args, const TCHAR* LongName, int32& OutValue)
{
	const FOptionCache& Cache = GetOptionCache(Args);
	const FString OptionName = GetCanonicalOptionName(LongName);
	const FString* Value = Cache.ValueByName.Find(OptionName);
	if (Value == nullptr)
	{
		return false;
	}

	return TryParseInt(*Value, OutValue);
}

bool TryGetDoubleOption(const TArray<FString>& Args, const TCHAR* LongName, double& OutValue)
{
	const FOptionCache& Cache = GetOptionCache(Args);
	const FString OptionName = GetCanonicalOptionName(LongName);
	const FString* Value = Cache.ValueByName.Find(OptionName);
	if (Value == nullptr)
	{
		return false;
	}

	return FDefaultValueHelper::ParseDouble(*Value, OutValue);
}

bool TryGetStringOption(const TArray<FString>& Args, const TCHAR* LongName, FString& OutValue)
{
	const FOptionCache& Cache = GetOptionCache(Args);
	const FString OptionName = GetCanonicalOptionName(LongName);
	const FString* Value = Cache.ValueByName.Find(OptionName);
	if (Value == nullptr)
	{
		return false;
	}

	OutValue = *Value;
	return !OutValue.IsEmpty();
}

bool HasOption(const TArray<FString>& Args, const TCHAR* LongName)
{
	const FOptionCache& Cache = GetOptionCache(Args);
	const FString OptionName = GetCanonicalOptionName(LongName);
	return Cache.PresentOptions.Contains(OptionName);
}

bool ValidateNoUnknownOptionsWithGlobals(const TArray<FString>& Args, const TArray<FString>& CommandOptionNames, FInsightCliResponse& OutError)
{
	const FOptionCache& Cache = GetOptionCache(Args);
	TSet<FString> AllowedOptions;

	for (const FString& CommandOptionName : CommandOptionNames)
	{
		AllowedOptions.Add(GetCanonicalOptionName(*CommandOptionName));
	}

	TArray<FString> UnknownOptions;
	for (const FString& PresentOption : Cache.PresentOptions)
	{
		if (!AllowedOptions.Contains(PresentOption))
		{
			UnknownOptions.Add(PresentOption);
		}
	}

	if (UnknownOptions.IsEmpty())
	{
		return true;
	}

	UnknownOptions.Sort();
	TMap<FString, FString> Details;
	Details.Add(TEXT("unknown_options"), FString::Join(UnknownOptions, TEXT(",")));
	OutError = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("Unknown option(s) for command."), Details);
	return false;
}

bool TryGetLimitAndOptionalFrameIndexFilter(const TArray<FString>& Args, int32& OutLimit, int32& OutFrameIndexFilter, bool& bOutHasFrameIndex, FInsightCliResponse& OutError)
{
	OutLimit = 100;
	OutFrameIndexFilter = -1;
	bOutHasFrameIndex = TryGetIntOption(Args, TEXT("--frame-index"), OutFrameIndexFilter);

	const bool bHasLimit = TryGetIntOption(Args, TEXT("--limit"), OutLimit);
	if (bHasLimit && OutLimit <= 0)
	{
		OutError = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("limit must be > 0."));
		return false;
	}

	if (bOutHasFrameIndex && OutFrameIndexFilter < 0)
	{
		OutError = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("frame-index must be >= 0."));
		return false;
	}

	return true;
}

FString ToNumberString(double Value)
{
	return FString::Printf(TEXT("%.3f"), Value);
}

TMap<FString, FString> MakeNotFoundMeta(const FInsightCliRequest& Request, const FString& Reason, const FString& QueryKey, const FString& QueryValue)
{
	TMap<FString, FString> Meta;
	Meta.Add(TEXT("found"), TEXT("false"));
	Meta.Add(TEXT("reason"), Reason);
	Meta.Add(TEXT("request_group"), Request.Group);
	Meta.Add(TEXT("request_action"), Request.Action);

	if (!QueryKey.IsEmpty())
	{
		Meta.Add(TEXT("query_key"), QueryKey);
	}
	if (!QueryValue.IsEmpty())
	{
		Meta.Add(TEXT("query_value"), QueryValue);
	}

	return Meta;
}

FInsightCliResponse MakeTraceUnavailableError(
	const FTraceContext& Context,
	const TCHAR* Consumer,
	const FString& FailureStage,
	const FString& FailureReason,
	const TCHAR* DefaultStage,
	const TCHAR* DefaultReason,
	const TCHAR* Message,
	const TMap<FString, FString>& ExtraDetails)
{
	TMap<FString, FString> Details;
	Details.Add(TEXT("trace_path"), Context.FullPath);
	Details.Add(TEXT("consumer"), Consumer);
	Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? DefaultStage : FailureStage);
	Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? DefaultReason : FailureReason);
	Details.Add(TEXT("data_source"), TEXT("unavailable"));

	for (const TPair<FString, FString>& Detail : ExtraDetails)
	{
		Details.Add(Detail.Key, Detail.Value);
	}

	return FInsightCliResponse::Error(10, TEXT("E3001"), Message, Details);
}

FInsightCliResponse ValidateTraceAndBuildContext(const FInsightCliRequest& Request, FTraceContext& OutContext)
{
	if (Request.TracePath.IsEmpty())
	{
		return FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("trace_path is required."));
	}

	OutContext.FullPath = FPaths::ConvertRelativePathToFull(Request.TracePath);
	if (!IFileManager::Get().FileExists(*OutContext.FullPath))
	{
		TMap<FString, FString> Details;
		Details.Add(TEXT("trace_path"), OutContext.FullPath);
		return FInsightCliResponse::Error(2, TEXT("E1001"), TEXT("Input trace file does not exist."), Details);
	}

	const FString Extension = GetTraceExtension(OutContext.FullPath);
	if (Extension != TEXT(".utrace") && Extension != TEXT(".trace"))
	{
		TMap<FString, FString> Details;
		Details.Add(TEXT("trace_path"), OutContext.FullPath);
		Details.Add(TEXT("extension"), Extension);
		return FInsightCliResponse::Error(3, TEXT("E1002"), TEXT("Unsupported trace file extension. Expected .utrace or .trace."), Details);
	}

	OutContext.FileSize = IFileManager::Get().FileSize(*OutContext.FullPath);
	OutContext.TimeStamp = IFileManager::Get().GetTimeStamp(*OutContext.FullPath);
	return FInsightCliResponse();
}

bool AcquireAnalysisSession(
	const FTraceContext& Context,
	TSharedPtr<const TraceServices::IAnalysisSession>& OutSession,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutSession.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	if (Context.CachedAnalysisSession.IsValid())
	{
		OutSession = Context.CachedAnalysisSession;
		return true;
	}

	if (Context.bAnalysisAttempted)
	{
		OutFailureStage = Context.AnalysisFailureStage;
		OutFailureReason = Context.AnalysisFailureReason;
		return false;
	}

	Context.bAnalysisAttempted = true;
	Context.AnalysisFailureStage.Reset();
	Context.AnalysisFailureReason.Reset();

	if (!OpenAnalysisSession(
		Context.FullPath,
		Context.CachedAnalysisService,
		Context.CachedAnalysisSession,
		Context.AnalysisFailureStage,
		Context.AnalysisFailureReason))
	{
		OutFailureStage = Context.AnalysisFailureStage;
		OutFailureReason = Context.AnalysisFailureReason;
		return false;
	}

	OutSession = Context.CachedAnalysisSession;
	return true;
}

TArray<FFrameSample> BuildFrameSamples(const FTraceContext& Context)
{
	if (Context.bHasFrameSamples)
	{
		return Context.CachedFrameSamples;
	}

	TArray<FFrameSample> Frames;
	Context.bFrameSamplesTraceBacked = false;
	Context.bFrameSamplesFailed = false;
	Context.TraceGameFrameCount = 0;
	Context.TraceRenderingFrameCount = 0;
	Context.TraceDurationMs = 0.0;
	Context.FrameSamplesFailureStage.Reset();
	Context.FrameSamplesFailureReason.Reset();

	const FString TraceFileName = FPaths::GetCleanFilename(Context.FullPath);
	if (TraceFileName.Contains(TEXT(".decomp."), ESearchCase::IgnoreCase) && !IsDecompStrictGuardBypassed())
	{
		SetFrameSampleFailure(Context, TEXT("compatibility_guard"), TEXT("decomp_trace_not_supported_in_strict_mode"));
		return Context.CachedFrameSamples;
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	FString AnalysisFailureStage;
	FString AnalysisFailureReason;
	if (!AcquireAnalysisSession(Context, Session, AnalysisFailureStage, AnalysisFailureReason))
	{
		SetFrameSampleFailure(
			Context,
			AnalysisFailureStage.IsEmpty() ? TEXT("analysis_session") : *AnalysisFailureStage,
			AnalysisFailureReason.IsEmpty() ? TEXT("failed to acquire analysis session") : *AnalysisFailureReason);
		return Context.CachedFrameSamples;
	}

	const auto SafeFrameIndex = [](const uint64 Index)
	{
		return (Index > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(Index);
	};

	TArray<TraceServices::FFrame> GameFrames;
	TArray<TraceServices::FFrame> RenderingFrames;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IFrameProvider* FrameProvider = Session->ReadProvider<TraceServices::IFrameProvider>(TraceServices::GetFrameProviderName());
		if (FrameProvider == nullptr)
		{
			SetFrameSampleFailure(Context, TEXT("frame_provider"), TEXT("frame provider missing in analysis session"));
			return Context.CachedFrameSamples;
		}

		const uint64 GameCount64 = FrameProvider->GetFrameCount(TraceFrameType_Game);
		const uint64 RenderingCount64 = FrameProvider->GetFrameCount(TraceFrameType_Rendering);

		Context.TraceGameFrameCount = (GameCount64 > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(GameCount64);
		Context.TraceRenderingFrameCount = (RenderingCount64 > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(RenderingCount64);

		if (GameCount64 > 0)
		{
			GameFrames.Reserve(Context.TraceGameFrameCount);
			FrameProvider->EnumerateFrames(TraceFrameType_Game, 0, GameCount64, [&GameFrames](const TraceServices::FFrame& Frame)
			{
				GameFrames.Add(Frame);
			});
		}

		if (RenderingCount64 > 0)
		{
			RenderingFrames.Reserve(Context.TraceRenderingFrameCount);
			FrameProvider->EnumerateFrames(TraceFrameType_Rendering, 0, RenderingCount64, [&RenderingFrames](const TraceServices::FFrame& Frame)
			{
				RenderingFrames.Add(Frame);
			});
		}

		Context.TraceDurationMs = Session->GetDurationSeconds() * 1000.0;
	}

	Frames.Reserve(GameFrames.Num() > 0 ? GameFrames.Num() : RenderingFrames.Num());

	for (const TraceServices::FFrame& Frame : GameFrames)
	{
		const double StartMs = Frame.StartTime * 1000.0;
		const double EndMs = FMath::Max(StartMs, Frame.EndTime * 1000.0);

		FFrameSample Sample;
		Sample.FrameIndex = SafeFrameIndex(Frame.Index);
		Sample.FrameStartMs = StartMs;
		Sample.FrameEndMs = EndMs;
		Sample.FrameTimeMs = EndMs - StartMs;
		Sample.GameThreadMs = Sample.FrameTimeMs;
		Sample.GameFrameIndex = SafeFrameIndex(Frame.Index);
		Sample.bTraceBacked = true;
		Frames.Add(Sample);
	}

	if (!Frames.IsEmpty() && !RenderingFrames.IsEmpty())
	{
		int32 RenderingCursor = 0;
		for (FFrameSample& Sample : Frames)
		{
			const double GameStartSec = Sample.FrameStartMs / 1000.0;
			const double GameEndSec = Sample.FrameEndMs / 1000.0;

			while (RenderingCursor < RenderingFrames.Num() && RenderingFrames[RenderingCursor].EndTime <= GameStartSec)
			{
				++RenderingCursor;
			}

			double RenderOverlapSec = 0.0;
			double MaxOverlapSec = 0.0;
			int32 BestRenderingFrameIndex = -1;

			for (int32 Index = RenderingCursor; Index < RenderingFrames.Num(); ++Index)
			{
				const TraceServices::FFrame& RenderingFrame = RenderingFrames[Index];
				if (RenderingFrame.StartTime >= GameEndSec)
				{
					break;
				}

				const double OverlapStart = FMath::Max(GameStartSec, RenderingFrame.StartTime);
				const double OverlapEnd = FMath::Min(GameEndSec, RenderingFrame.EndTime);
				if (OverlapEnd <= OverlapStart)
				{
					continue;
				}

				const double OverlapSec = OverlapEnd - OverlapStart;
				RenderOverlapSec += OverlapSec;
				if (OverlapSec > MaxOverlapSec)
				{
					MaxOverlapSec = OverlapSec;
					BestRenderingFrameIndex = SafeFrameIndex(RenderingFrame.Index);
				}
			}

			Sample.RenderThreadMs = RenderOverlapSec * 1000.0;
			Sample.RenderingFrameIndex = BestRenderingFrameIndex;
		}
	}
	else if (Frames.IsEmpty())
	{
		for (const TraceServices::FFrame& Frame : RenderingFrames)
		{
			const double StartMs = Frame.StartTime * 1000.0;
			const double EndMs = FMath::Max(StartMs, Frame.EndTime * 1000.0);

			FFrameSample Sample;
			Sample.FrameIndex = SafeFrameIndex(Frame.Index);
			Sample.FrameStartMs = StartMs;
			Sample.FrameEndMs = EndMs;
			Sample.FrameTimeMs = EndMs - StartMs;
			Sample.RenderThreadMs = Sample.FrameTimeMs;
			Sample.RenderingFrameIndex = SafeFrameIndex(Frame.Index);
			Sample.bTraceBacked = true;
			Frames.Add(Sample);
		}
	}

	if (Context.TraceDurationMs <= 0.0 && !Frames.IsEmpty())
	{
		Context.TraceDurationMs = Frames.Last().FrameEndMs;
	}

	if (Frames.IsEmpty())
	{
		SetFrameSampleFailure(Context, TEXT("enumeration"), TEXT("no frame records found in trace"));
		return Context.CachedFrameSamples;
	}

	Context.bFrameSamplesTraceBacked = !Frames.IsEmpty() && Frames[0].bTraceBacked;
	Context.bFrameSamplesFailed = false;
	Context.CachedFrameSamples = MoveTemp(Frames);
	Context.bHasFrameSamples = true;
	return Context.CachedFrameSamples;
}

bool EnsureTraceBackedFrameSamples(const FTraceContext& Context, FInsightCliResponse& OutError, const FString& ConsumerTag)
{
	BuildFrameSamples(Context);
	if (Context.bFrameSamplesTraceBacked)
	{
		return true;
	}

	OutError = MakeTraceUnavailableError(
		Context,
		*ConsumerTag,
		Context.FrameSamplesFailureStage,
		Context.FrameSamplesFailureReason,
		TEXT("unknown"),
		TEXT("unknown"),
		TEXT("Trace-backed frame timeline is unavailable for this trace."));
	return false;
}

void ApplyTimeWindowFilter(TArray<FFrameSample>& Frames, const TArray<FString>& Args, bool& bUsedWindow, FInsightCliResponse& OutError)
{
	bUsedWindow = false;
	double Start = 0.0;
	double End = 0.0;
	const bool bHasStart = TryGetDoubleOption(Args, TEXT("--time-start"), Start);
	const bool bHasEnd = TryGetDoubleOption(Args, TEXT("--time-end"), End);

	if (!bHasStart && !bHasEnd)
	{
		return;
	}

	bUsedWindow = true;
	if (bHasStart && bHasEnd && Start > End)
	{
		OutError = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
		return;
	}

	const double EffectiveStart = bHasStart ? Start : 0.0;
	const double EffectiveEnd = bHasEnd ? End : TNumericLimits<double>::Max();
	Frames = Frames.FilterByPredicate([EffectiveStart, EffectiveEnd](const FFrameSample& Sample)
	{
		return Sample.FrameStartMs >= EffectiveStart && Sample.FrameStartMs < EffectiveEnd;
	});
}

TSharedRef<FJsonObject> MakeInfoSummaryData(const FTraceContext& Context)
{
	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	const double DurationMs = (Context.TraceDurationMs > 0.0) ? Context.TraceDurationMs : (!Frames.IsEmpty() ? Frames.Last().FrameEndMs : 0.0);

	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("trace_name"), FPaths::GetCleanFilename(Context.FullPath));
	Data->SetStringField(TEXT("trace_path"), Context.FullPath);
	Data->SetStringField(TEXT("trace_size_bytes"), FString::Printf(TEXT("%lld"), Context.FileSize));
	Data->SetStringField(TEXT("start_timestamp"), Context.TimeStamp.ToIso8601());
	Data->SetStringField(TEXT("end_timestamp"), Context.TimeStamp.ToIso8601());
	Data->SetStringField(TEXT("duration_ms"), FString::Printf(TEXT("%.3f"), DurationMs));
	Data->SetStringField(TEXT("thread_count"), TEXT("unavailable"));
	Data->SetStringField(TEXT("event_count"), TEXT("unavailable"));
	Data->SetStringField(TEXT("data_source"), Context.bFrameSamplesTraceBacked ? TEXT("trace") : TEXT("unavailable"));
	Data->SetNumberField(TEXT("game_frame_count"), Context.TraceGameFrameCount);
	Data->SetNumberField(TEXT("rendering_frame_count"), Context.TraceRenderingFrameCount);
	if (Context.bFrameSamplesFailed)
	{
		Data->SetStringField(TEXT("trace_parse_failure_stage"), Context.FrameSamplesFailureStage);
		Data->SetStringField(TEXT("trace_parse_failure_reason"), Context.FrameSamplesFailureReason);
	}
	Data->SetStringField(TEXT("platform"), FPlatformProperties::IniPlatformName());
	Data->SetStringField(TEXT("build_version"), TEXT("unavailable"));
	return Data;
}

TSharedRef<FJsonObject> MakeFramesSummaryData(const TArray<FFrameSample>& Frames)
{
	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	if (Frames.IsEmpty())
	{
		return Data;
	}

	TArray<double> SortedTimes;
	SortedTimes.Reserve(Frames.Num());
	double Sum = 0.0;
	double MinValue = TNumericLimits<double>::Max();
	double MaxValue = 0.0;

	for (const FFrameSample& Sample : Frames)
	{
		SortedTimes.Add(Sample.FrameTimeMs);
		Sum += Sample.FrameTimeMs;
		MinValue = FMath::Min(MinValue, Sample.FrameTimeMs);
		MaxValue = FMath::Max(MaxValue, Sample.FrameTimeMs);
	}

	SortedTimes.Sort();
	const auto PercentileValue = [&SortedTimes](const double Ratio)
	{
		const int32 Index = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(Ratio * (SortedTimes.Num() - 1))), 0, SortedTimes.Num() - 1);
		return SortedTimes[Index];
	};

	Data->SetNumberField(TEXT("frame_count"), Frames.Num());
	Data->SetNumberField(TEXT("avg_frame_ms"), Sum / Frames.Num());
	Data->SetNumberField(TEXT("max_frame_ms"), MaxValue);
	Data->SetNumberField(TEXT("min_frame_ms"), MinValue);
	Data->SetNumberField(TEXT("p50"), PercentileValue(0.50));
	Data->SetNumberField(TEXT("p90"), PercentileValue(0.90));
	Data->SetNumberField(TEXT("p95"), PercentileValue(0.95));
	Data->SetNumberField(TEXT("p99"), PercentileValue(0.99));
	return Data;
}

TSharedRef<FJsonObject> MakeFrameObject(const FFrameSample& Sample)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), Sample.FrameIndex);
	Item->SetNumberField(TEXT("frame_start_ms"), Sample.FrameStartMs);
	Item->SetNumberField(TEXT("frame_end_ms"), Sample.FrameEndMs);
	Item->SetNumberField(TEXT("frame_time_ms"), Sample.FrameTimeMs);
	Item->SetNumberField(TEXT("game_thread_ms"), Sample.GameThreadMs);
	Item->SetNumberField(TEXT("render_thread_ms"), Sample.RenderThreadMs);
	Item->SetNumberField(TEXT("rhi_thread_ms"), Sample.RhiThreadMs);
	Item->SetNumberField(TEXT("gpu_ms"), Sample.GpuMs);
	Item->SetNumberField(TEXT("game_frame_index"), Sample.GameFrameIndex);
	Item->SetNumberField(TEXT("rendering_frame_index"), Sample.RenderingFrameIndex);
	Item->SetStringField(TEXT("data_source"), Sample.bTraceBacked ? TEXT("trace") : TEXT("unavailable"));
	return Item;
}

bool ResolveCpuThreadFilterToTraceId(const FTraceContext& Context, const FString& ThreadFilter, uint32& OutThreadId, FString& OutNormalizedThread, FString& OutFailureStage, FString& OutFailureReason)
{
	const FString TraceFileName = FPaths::GetCleanFilename(Context.FullPath);
	if (TraceFileName.Contains(TEXT(".decomp."), ESearchCase::IgnoreCase) && !IsDecompStrictGuardBypassed())
	{
		OutFailureStage = TEXT("compatibility_guard");
		OutFailureReason = TEXT("decomp_trace_not_supported_in_strict_mode");
		return false;
	}

	int32 ParsedThreadId = -1;
	if (TryParseInt(ThreadFilter, ParsedThreadId) && ParsedThreadId >= 0)
	{
		OutThreadId = static_cast<uint32>(ParsedThreadId);
		OutNormalizedThread = FString::FromInt(ParsedThreadId);
		return true;
	}

	FString DesiredThreadName;
	if (ThreadFilter.Equals(TEXT("GameThread"), ESearchCase::IgnoreCase) || ThreadFilter == TEXT("42"))
	{
		DesiredThreadName = TEXT("GameThread");
		OutNormalizedThread = TEXT("GameThread");
	}
	else if (ThreadFilter.Equals(TEXT("RenderThread"), ESearchCase::IgnoreCase) || ThreadFilter == TEXT("43"))
	{
		DesiredThreadName = TEXT("RenderThread");
		OutNormalizedThread = TEXT("RenderThread");
	}
	else if (ThreadFilter.Equals(TEXT("RHIThread"), ESearchCase::IgnoreCase) || ThreadFilter == TEXT("44"))
	{
		DesiredThreadName = TEXT("RHIThread");
		OutNormalizedThread = TEXT("RHIThread");
	}
	else
	{
		OutFailureStage = TEXT("thread_filter");
		OutFailureReason = TEXT("unsupported_thread_filter");
		return false;
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	bool bFoundThread = false;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IThreadProvider* ThreadProvider = Session->ReadProvider<TraceServices::IThreadProvider>(TraceServices::GetThreadProviderName());
		if (ThreadProvider == nullptr)
		{
			OutFailureStage = TEXT("thread_provider");
			OutFailureReason = TEXT("thread provider not available");
			return false;
		}

		ThreadProvider->EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
		{
			if (bFoundThread || ThreadInfo.Name == nullptr)
			{
				return;
			}

			const FString Name = ThreadInfo.Name;
			if (Name.Contains(DesiredThreadName, ESearchCase::IgnoreCase))
			{
				bFoundThread = true;
				OutThreadId = ThreadInfo.Id;
			}
		});
	}

	if (!bFoundThread)
	{
		OutFailureStage = TEXT("thread_lookup");
		OutFailureReason = TEXT("requested cpu thread not found in trace");
		return false;
	}

	return true;
}

bool BuildCpuTopSamples(const FTraceContext& Context, const TOptional<uint32>& CpuThreadId, TArray<FCpuScopeSample>& OutSamples, FString& OutFailureStage, FString& OutFailureReason)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	const FString TraceFileName = FPaths::GetCleanFilename(Context.FullPath);
	if (TraceFileName.Contains(TEXT(".decomp."), ESearchCase::IgnoreCase) && !IsDecompStrictGuardBypassed())
	{
		OutFailureStage = TEXT("compatibility_guard");
		OutFailureReason = TEXT("decomp_trace_not_supported_in_strict_mode");
		return false;
	}

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
		Params.IntervalStart = 0.0;
		Params.IntervalEnd = Session->GetDurationSeconds();
		Params.IncludeGpu = false;
		Params.FrameType = ETraceFrameType::TraceFrameType_Count;
		if (CpuThreadId.IsSet())
		{
			const uint32 FilterThreadId = CpuThreadId.GetValue();
			Params.CpuThreadFilter = [FilterThreadId](uint32 ThreadId)
			{
				return ThreadId == FilterThreadId;
			};
		}
		else
		{
			Params.CpuThreadFilter = [](uint32)
			{
				return true;
			};
		}

		StatsTable.Reset(TimingProfilerProvider->CreateAggregation(Params));
	}

	if (!StatsTable.IsValid())
	{
		OutFailureStage = TEXT("aggregation");
		OutFailureReason = TEXT("failed to build cpu aggregation table");
		return false;
	}

	TUniquePtr<TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>> Reader(StatsTable->CreateReader());
	if (!Reader.IsValid())
	{
		OutFailureStage = TEXT("aggregation_reader");
		OutFailureReason = TEXT("failed to create cpu aggregation table reader");
		return false;
	}

	while (Reader->IsValid())
	{
		const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
		if (Row != nullptr && Row->Timer != nullptr && !Row->Timer->IsGpuTimer)
		{
			FCpuScopeSample Sample;
			Sample.ScopeName = Row->Timer->Name != nullptr ? Row->Timer->Name : TEXT("<unknown>");
			Sample.ThreadId = CpuThreadId.IsSet() ? static_cast<int32>(CpuThreadId.GetValue()) : 0;
			Sample.CallCount = (Row->InstanceCount > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(Row->InstanceCount);
			Sample.TotalMs = Row->TotalInclusiveTime * 1000.0;
			Sample.AvgMs = Row->AverageInclusiveTime * 1000.0;
			Sample.MaxMs = Row->MaxInclusiveTime * 1000.0;
			Sample.MinMs = Row->MinInclusiveTime * 1000.0;
			Sample.SelfMs = Row->TotalExclusiveTime * 1000.0;
			OutSamples.Add(Sample);
		}

		Reader->NextRow();
	}

	if (OutSamples.IsEmpty())
	{
		OutFailureStage = TEXT("aggregation");
		OutFailureReason = TEXT("no cpu timing stats found in trace");
		return false;
	}

	OutSamples.Sort([](const FCpuScopeSample& A, const FCpuScopeSample& B)
	{
		if (A.TotalMs == B.TotalMs)
		{
			return A.ScopeName < B.ScopeName;
		}
		return A.TotalMs > B.TotalMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeCpuTopObject(const FCpuScopeSample& Sample)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("scope_name"), Sample.ScopeName);
	Item->SetNumberField(TEXT("thread_id"), Sample.ThreadId);
	Item->SetNumberField(TEXT("call_count"), Sample.CallCount);
	Item->SetNumberField(TEXT("total_ms"), Sample.TotalMs);
	Item->SetNumberField(TEXT("avg_ms"), Sample.AvgMs);
	Item->SetNumberField(TEXT("max_ms"), Sample.MaxMs);
	Item->SetNumberField(TEXT("min_ms"), Sample.MinMs);
	Item->SetNumberField(TEXT("self_ms"), Sample.SelfMs);
	return Item;
}

bool BuildCpuStackObject(
	const FTraceContext& Context,
	int32 FrameIndex,
	const TOptional<uint32>& CpuThreadId,
	int32 StackLimit,
	TSharedPtr<FJsonObject>& OutObject,
	bool& bOutFound,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutObject.Reset();
	bOutFound = false;
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	const FFrameSample* FoundFrame = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
	{
		return Item.FrameIndex == FrameIndex;
	});

	if (FoundFrame == nullptr)
	{
		return true;
	}

	bOutFound = true;

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	struct FTimerSnapshot
	{
		FString Name;
		FString File;
		int32 Line = 0;
		bool bValid = false;
	};

	struct FRangeEvent
	{
		double StartSec = 0.0;
		double EndSec = 0.0;
		uint32 Depth = 0;
		uint32 TimerId = uint32(-1);
	};

	const auto InferModuleName = [](const FString& FilePath, const FString& SymbolName) -> FString
	{
		if (FilePath.Contains(TEXT("/Engine/"), ESearchCase::IgnoreCase) || FilePath.Contains(TEXT("\\Engine\\"), ESearchCase::IgnoreCase))
		{
			return TEXT("Engine");
		}
		if (FilePath.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase) || FilePath.Contains(TEXT("\\Game\\"), ESearchCase::IgnoreCase))
		{
			return TEXT("Game");
		}
		if (SymbolName.Contains(TEXT("::"), ESearchCase::CaseSensitive))
		{
			return TEXT("Trace");
		}
		return TEXT("Unknown");
	};

	uint32 TargetThreadId = 0;
	FString TargetThreadName;
	uint32 TimelineIndex = uint32(-1);
	TArray<FTimerSnapshot> Timers;
	TArray<FRangeEvent> RangeEvents;

	const double IntervalStartSec = FMath::Max(0.0, FoundFrame->FrameStartMs / 1000.0);
	const double IntervalEndSec = FMath::Max(IntervalStartSec + KINDA_SMALL_NUMBER, FoundFrame->FrameEndMs / 1000.0);

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());

		const TraceServices::ITimingProfilerProvider* TimingProfilerProvider = TraceServices::ReadTimingProfilerProvider(*Session.Get());
		if (TimingProfilerProvider == nullptr)
		{
			OutFailureStage = TEXT("timing_provider");
			OutFailureReason = TEXT("TimingProfiler provider not available");
			return false;
		}

		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session.Get());

		TimingProfilerProvider->ReadTimers([&Timers](const TraceServices::ITimingProfilerTimerReader& TimerReader)
		{
			const uint32 TimerCount = TimerReader.GetTimerCount();
			Timers.SetNum(static_cast<int32>(TimerCount));
			for (uint32 TimerId = 0; TimerId < TimerCount; ++TimerId)
			{
				const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(TimerId);
				if (Timer == nullptr)
				{
					continue;
				}

				FTimerSnapshot& Snapshot = Timers[static_cast<int32>(TimerId)];
				Snapshot.Name = Timer->Name != nullptr ? Timer->Name : TEXT("<unknown>");
				Snapshot.File = Timer->File != nullptr ? Timer->File : TEXT("");
				Snapshot.Line = static_cast<int32>(Timer->Line);
				Snapshot.bValid = true;
			}
		});

		if (CpuThreadId.IsSet())
		{
			TargetThreadId = CpuThreadId.GetValue();
			TargetThreadName = ThreadProvider.GetThreadName(TargetThreadId);
			if (!TimingProfilerProvider->GetCpuThreadTimelineIndex(TargetThreadId, TimelineIndex))
			{
				OutFailureStage = TEXT("timeline_lookup");
				OutFailureReason = TEXT("cpu thread timeline not found");
				return false;
			}
		}
		else
		{
			bool bFoundPreferredThread = false;
			ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
			{
				if (bFoundPreferredThread || ThreadInfo.Name == nullptr)
				{
					return;
				}

				const FString Name = ThreadInfo.Name;
				if (!Name.Contains(TEXT("GameThread"), ESearchCase::IgnoreCase))
				{
					return;
				}

				uint32 CandidateTimelineIndex = uint32(-1);
				if (!TimingProfilerProvider->GetCpuThreadTimelineIndex(ThreadInfo.Id, CandidateTimelineIndex))
				{
					return;
				}

				TargetThreadId = ThreadInfo.Id;
				TargetThreadName = Name;
				TimelineIndex = CandidateTimelineIndex;
				bFoundPreferredThread = true;
			});

			if (!bFoundPreferredThread)
			{
				ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
				{
					if (TimelineIndex != uint32(-1))
					{
						return;
					}

					uint32 CandidateTimelineIndex = uint32(-1);
					if (!TimingProfilerProvider->GetCpuThreadTimelineIndex(ThreadInfo.Id, CandidateTimelineIndex))
					{
						return;
					}

					TargetThreadId = ThreadInfo.Id;
					TargetThreadName = ThreadInfo.Name != nullptr ? ThreadInfo.Name : FString();
					TimelineIndex = CandidateTimelineIndex;
				});
			}

			if (TimelineIndex == uint32(-1))
			{
				OutFailureStage = TEXT("thread_lookup");
				OutFailureReason = TEXT("no cpu thread timeline available in trace");
				return false;
			}
		}

		if (!TimingProfilerProvider->ReadTimeline(TimelineIndex, [&RangeEvents, IntervalStartSec, IntervalEndSec](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(IntervalStartSec, IntervalEndSec, [&RangeEvents](double StartTime, double EndTime, uint32 Depth, const TraceServices::FTimingProfilerEvent& Event)
			{
				if (EndTime <= StartTime)
				{
					return TraceServices::EEventEnumerate::Continue;
				}

				FRangeEvent Row;
				Row.StartSec = StartTime;
				Row.EndSec = EndTime;
				Row.Depth = Depth;
				Row.TimerId = Event.TimerIndex;
				RangeEvents.Add(Row);
				return TraceServices::EEventEnumerate::Continue;
			});
		}))
		{
			OutFailureStage = TEXT("timeline_read");
			OutFailureReason = TEXT("failed to read cpu thread timeline");
			return false;
		}
	}

	const auto BuildActiveChain = [&RangeEvents](const double SampleTimeSec)
	{
		TArray<FRangeEvent> Active;
		for (const FRangeEvent& Row : RangeEvents)
		{
			if (Row.StartSec <= SampleTimeSec && SampleTimeSec < Row.EndSec)
			{
				Active.Add(Row);
			}
		}

		if (Active.IsEmpty())
		{
			return Active;
		}

		Active.Sort([](const FRangeEvent& A, const FRangeEvent& B)
		{
			if (A.Depth == B.Depth)
			{
				return A.StartSec < B.StartSec;
			}
			return A.Depth < B.Depth;
		});

		TMap<uint32, FRangeEvent> BestByDepth;
		for (const FRangeEvent& Row : Active)
		{
			const FRangeEvent* Existing = BestByDepth.Find(Row.Depth);
			if (Existing == nullptr || Row.StartSec > Existing->StartSec)
			{
				BestByDepth.Add(Row.Depth, Row);
			}
		}

		TArray<uint32> Depths;
		BestByDepth.GetKeys(Depths);
		Depths.Sort();

		TArray<FRangeEvent> Chain;
		Chain.Reserve(Depths.Num());
		for (const uint32 Depth : Depths)
		{
			if (const FRangeEvent* Found = BestByDepth.Find(Depth))
			{
				Chain.Add(*Found);
			}
		}

		return Chain;
	};

	const double MidSec = (IntervalStartSec + IntervalEndSec) * 0.5;
	TArray<FRangeEvent> Chain = BuildActiveChain(MidSec);
	if (Chain.IsEmpty() && !RangeEvents.IsEmpty())
	{
		const FRangeEvent* Longest = &RangeEvents[0];
		for (const FRangeEvent& Row : RangeEvents)
		{
			if ((Row.EndSec - Row.StartSec) > (Longest->EndSec - Longest->StartSec))
			{
				Longest = &Row;
			}
		}

		const double LongestMidSec = (Longest->StartSec + Longest->EndSec) * 0.5;
		Chain = BuildActiveChain(LongestMidSec);
	}

	if (StackLimit > 0 && Chain.Num() > StackLimit)
	{
		Chain.RemoveAt(0, Chain.Num() - StackLimit, EAllowShrinking::No);
	}

	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), FoundFrame->FrameIndex);
	Item->SetNumberField(TEXT("thread_id"), static_cast<double>(TargetThreadId));
	Item->SetStringField(TEXT("thread_name"), TargetThreadName.IsEmpty() ? FString::Printf(TEXT("Thread-%u"), TargetThreadId) : TargetThreadName);

	if (!Chain.IsEmpty())
	{
		const FRangeEvent& Leaf = Chain.Last();
		const FString LeafName = (Leaf.TimerId < static_cast<uint32>(Timers.Num()) && Timers[Leaf.TimerId].bValid) ? Timers[Leaf.TimerId].Name : TEXT("<unknown>");
		const double TotalMs = (Leaf.EndSec - Leaf.StartSec) * 1000.0;

		double DirectChildrenMs = 0.0;
		for (const FRangeEvent& Row : RangeEvents)
		{
			if (Row.Depth != Leaf.Depth + 1)
			{
				continue;
			}
			const double OverlapStart = FMath::Max(Row.StartSec, Leaf.StartSec);
			const double OverlapEnd = FMath::Min(Row.EndSec, Leaf.EndSec);
			if (OverlapEnd > OverlapStart)
			{
				DirectChildrenMs += (OverlapEnd - OverlapStart) * 1000.0;
			}
		}

		Item->SetStringField(TEXT("scope_name"), LeafName);
		Item->SetNumberField(TEXT("total_ms"), TotalMs);
		Item->SetNumberField(TEXT("self_ms"), FMath::Max(0.0, TotalMs - DirectChildrenMs));
	}
	else
	{
		Item->SetStringField(TEXT("scope_name"), TEXT(""));
		Item->SetNumberField(TEXT("total_ms"), 0.0);
		Item->SetNumberField(TEXT("self_ms"), 0.0);
	}

	TArray<TSharedPtr<FJsonValue>> Stack;
	Stack.Reserve(Chain.Num());
	for (const FRangeEvent& Row : Chain)
	{
		const TSharedRef<FJsonObject> StackItem = MakeShared<FJsonObject>();
		FString Name = TEXT("<unknown>");
		FString File;
		int32 Line = 0;
		if (Row.TimerId < static_cast<uint32>(Timers.Num()) && Timers[Row.TimerId].bValid)
		{
			const FTimerSnapshot& Snapshot = Timers[Row.TimerId];
			Name = Snapshot.Name;
			File = Snapshot.File;
			Line = Snapshot.Line;
		}

		StackItem->SetStringField(TEXT("function"), Name);
		StackItem->SetStringField(TEXT("module"), InferModuleName(File, Name));
		if (!File.IsEmpty())
		{
			StackItem->SetStringField(TEXT("file"), File);
		}
		if (Line > 0)
		{
			StackItem->SetNumberField(TEXT("line"), Line);
		}
		Stack.Add(MakeShared<FJsonValueObject>(StackItem));
	}

	Item->SetArrayField(TEXT("stack"), Stack);
	OutObject = Item;
	return true;
}

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
	Item->SetNumberField(TEXT("avg_ms"), Sample.AvgMs);
	Item->SetNumberField(TEXT("max_ms"), Sample.MaxMs);
	return Item;
}

TSharedRef<FJsonObject> MakeGpuPassDetailObject(const FFrameSample& FrameSample, const FGpuScopeSample& Sample)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), FrameSample.FrameIndex);
	Item->SetStringField(TEXT("pass_name"), Sample.ScopeName);
	Item->SetNumberField(TEXT("draw_calls"), Sample.CallCount);
	Item->SetNumberField(TEXT("duration_ms"), Sample.TotalMs);
	return Item;
}

bool BuildThreadWaitSamplesTrace(
	const FTraceContext& Context,
	TOptional<int32> FrameIndexFilter,
	TArray<FThreadWaitSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	bool& bOutFrameFound)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();
	bOutFrameFound = true;

	TArray<FFrameSample> Frames;
	TOptional<double> IntervalStartSec;
	TOptional<double> IntervalEndSec;
	if (FrameIndexFilter.IsSet())
	{
		Frames = BuildFrameSamples(Context);
		const FFrameSample* FoundFrame = Frames.FindByPredicate([&FrameIndexFilter](const FFrameSample& Frame)
		{
			return Frame.FrameIndex == FrameIndexFilter.GetValue();
		});

		if (FoundFrame == nullptr)
		{
			bOutFrameFound = false;
			return true;
		}

		IntervalStartSec = FMath::Max(0.0, FoundFrame->FrameStartMs / 1000.0);
		IntervalEndSec = FMath::Max(IntervalStartSec.GetValue() + KINDA_SMALL_NUMBER, FoundFrame->FrameEndMs / 1000.0);
	}
	else
	{
		Frames = BuildFrameSamples(Context);
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	const auto GetFrameIndexForTimestampMs = [&Frames](const double TimestampMs) -> int32
	{
		for (const FFrameSample& Frame : Frames)
		{
			if (Frame.FrameStartMs <= TimestampMs && TimestampMs <= Frame.FrameEndMs)
			{
				return Frame.FrameIndex;
			}
		}
		return -1;
	};

	const double QueryStartSec = IntervalStartSec.IsSet() ? IntervalStartSec.GetValue() : 0.0;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const double QueryEndSec = IntervalEndSec.IsSet() ? IntervalEndSec.GetValue() : Session->GetDurationSeconds();
		const TraceServices::IContextSwitchesProvider* ContextSwitchesProvider = TraceServices::ReadContextSwitchesProvider(*Session.Get());
		if (ContextSwitchesProvider == nullptr || !ContextSwitchesProvider->HasData())
		{
			return true;
		}

		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session.Get());
		ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
		{
			double LastRunEndSec = QueryStartSec;
			bool bSeenAnyRun = false;

			const auto AddWaitGap = [&](double GapStartSec, double GapEndSec)
			{
				if (GapEndSec <= GapStartSec)
				{
					return;
				}

				FThreadWaitSample Wait;
				Wait.FrameIndex = FrameIndexFilter.IsSet() ? FrameIndexFilter.GetValue() : GetFrameIndexForTimestampMs(GapStartSec * 1000.0);
				Wait.ThreadId = static_cast<int32>(ThreadInfo.Id);
				Wait.ThreadName = ThreadInfo.Name != nullptr ? ThreadInfo.Name : FString();
				Wait.WaitType = TEXT("NotRunning");
				Wait.WaitObject = TEXT("Scheduler");
				Wait.BeginMs = GapStartSec * 1000.0;
				Wait.EndMs = GapEndSec * 1000.0;
				Wait.WaitMs = Wait.EndMs - Wait.BeginMs;

				Wait.OwnerThreadId = -1;
				Wait.OwnerThreadName.Reset();
				Wait.BlockerThreadId = -1;
				Wait.BlockerThreadName.Reset();
				Wait.BlockedToBlockerThreadChain.Reset();
				Wait.BlockedToBlockerThreadChain.Add(Wait.ThreadId);
				Wait.ChainDepth = 0;
				Wait.ChainStatus = TEXT("unresolved");
				Wait.UnresolvedReason = TEXT("blocker_unknown");

				OutSamples.Add(MoveTemp(Wait));
			};

			ContextSwitchesProvider->EnumerateContextSwitches(ThreadInfo.Id, QueryStartSec, QueryEndSec,
				[&](const TraceServices::FContextSwitch& ContextSwitch)
				{
					bSeenAnyRun = true;
					const double GapStartSec = LastRunEndSec;
					const double GapEndSec = FMath::Max(QueryStartSec, ContextSwitch.Start);
					AddWaitGap(GapStartSec, GapEndSec);

					LastRunEndSec = FMath::Max(LastRunEndSec, ContextSwitch.End);
					return TraceServices::EContextSwitchEnumerationResult::Continue;
				});

			if (bSeenAnyRun)
			{
				AddWaitGap(LastRunEndSec, QueryEndSec);
			}
		});
	}

	OutSamples.Sort([](const FThreadWaitSample& A, const FThreadWaitSample& B)
	{
		if (A.WaitMs == B.WaitMs)
		{
			if (A.FrameIndex == B.FrameIndex)
			{
				return A.ThreadId < B.ThreadId;
			}
			return A.FrameIndex < B.FrameIndex;
		}
		return A.WaitMs > B.WaitMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeThreadWaitObject(const FThreadWaitSample& Wait)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("frame_index"), Wait.FrameIndex);
	Item->SetNumberField(TEXT("thread_id"), Wait.ThreadId);
	Item->SetStringField(TEXT("thread_name"), Wait.ThreadName);
	Item->SetStringField(TEXT("wait_type"), Wait.WaitType);
	Item->SetStringField(TEXT("wait_object"), Wait.WaitObject);
	Item->SetNumberField(TEXT("wait_ms"), Wait.WaitMs);
	Item->SetNumberField(TEXT("owner_thread_id"), Wait.OwnerThreadId);
	Item->SetStringField(TEXT("owner_thread_name"), Wait.OwnerThreadName);
	Item->SetNumberField(TEXT("blocker_thread_id"), Wait.BlockerThreadId);
	Item->SetStringField(TEXT("blocker_thread_name"), Wait.BlockerThreadName);
	Item->SetNumberField(TEXT("chain_depth"), Wait.ChainDepth);
	Item->SetStringField(TEXT("chain_status"), Wait.ChainStatus);
	Item->SetStringField(TEXT("unresolved_reason"), Wait.UnresolvedReason);

	TArray<TSharedPtr<FJsonValue>> ChainThreadIds;
	ChainThreadIds.Reserve(Wait.BlockedToBlockerThreadChain.Num());
	for (const int32 ThreadId : Wait.BlockedToBlockerThreadChain)
	{
		ChainThreadIds.Add(MakeShared<FJsonValueNumber>(ThreadId));
	}
	Item->SetArrayField(TEXT("blocked_to_blocker_thread_chain"), ChainThreadIds);

	Item->SetNumberField(TEXT("begin_ms"), Wait.BeginMs);
	Item->SetNumberField(TEXT("end_ms"), Wait.EndMs);
	return Item;
}

bool BuildTaskTopSamples(
	const FTraceContext& Context,
	TOptional<int32> FrameIndexFilter,
	TArray<FTaskSample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	bool& bOutFrameFound)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();
	bOutFrameFound = true;

	TOptional<double> IntervalStartSec;
	TOptional<double> IntervalEndSec;
	TArray<FFrameSample> Frames;
	if (FrameIndexFilter.IsSet())
	{
		Frames = BuildFrameSamples(Context);
		const FFrameSample* FoundFrame = Frames.FindByPredicate([&FrameIndexFilter](const FFrameSample& Frame)
		{
			return Frame.FrameIndex == FrameIndexFilter.GetValue();
		});

		if (FoundFrame == nullptr)
		{
			bOutFrameFound = false;
			return true;
		}

		IntervalStartSec = FMath::Max(0.0, FoundFrame->FrameStartMs / 1000.0);
		IntervalEndSec = FMath::Max(IntervalStartSec.GetValue() + KINDA_SMALL_NUMBER, FoundFrame->FrameEndMs / 1000.0);
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	const auto GetFrameIndexForTimestampMs = [&Frames](const double TimestampMs) -> int32
	{
		for (const FFrameSample& Frame : Frames)
		{
			if (Frame.FrameStartMs <= TimestampMs && TimestampMs <= Frame.FrameEndMs)
			{
				return Frame.FrameIndex;
			}
		}
		return -1;
	};

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::ITasksProvider* TasksProvider = TraceServices::ReadTasksProvider(*Session.Get());
		if (TasksProvider == nullptr)
		{
			OutFailureStage = TEXT("tasks_provider");
			OutFailureReason = TEXT("Tasks provider not available");
			return false;
		}

		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session.Get());
		const double QueryStartSec = IntervalStartSec.IsSet() ? IntervalStartSec.GetValue() : 0.0;
		const double QueryEndSec = IntervalEndSec.IsSet() ? IntervalEndSec.GetValue() : Session->GetDurationSeconds();

		TasksProvider->EnumerateTasks(QueryStartSec, QueryEndSec, TraceServices::ETaskEnumerationOption::Alive,
			[&OutSamples, &ThreadProvider, &Frames, &GetFrameIndexForTimestampMs](const TraceServices::FTaskInfo& TaskInfo)
			{
				const double EnqueueSec =
					(TaskInfo.ScheduledTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.ScheduledTimestamp :
					(TaskInfo.LaunchedTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.LaunchedTimestamp :
					TaskInfo.CreatedTimestamp;

				if (TaskInfo.StartedTimestamp == TraceServices::FTaskInfo::InvalidTimestamp)
				{
					return TraceServices::ETaskEnumerationResult::Continue;
				}

				const double EndSec =
					(TaskInfo.FinishedTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.FinishedTimestamp :
					(TaskInfo.CompletedTimestamp != TraceServices::FTaskInfo::InvalidTimestamp) ? TaskInfo.CompletedTimestamp :
					TaskInfo.StartedTimestamp;

				FTaskSample Task;
				Task.TaskId = (TaskInfo.Id > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(TaskInfo.Id);
				Task.TaskName = TaskInfo.DebugName != nullptr ? TaskInfo.DebugName : TEXT("<unknown>");
				Task.EnqueueMs = FMath::Max(0.0, EnqueueSec * 1000.0);
				Task.StartMs = FMath::Max(0.0, TaskInfo.StartedTimestamp * 1000.0);
				Task.EndMs = FMath::Max(Task.StartMs, EndSec * 1000.0);
				Task.QueueWaitMs = FMath::Max(0.0, Task.StartMs - Task.EnqueueMs);
				Task.RunMs = FMath::Max(0.0, Task.EndMs - Task.StartMs);
				Task.WorkerThreadId = static_cast<int32>(TaskInfo.StartedThreadId);

				const FString StartedThreadName = ThreadProvider.GetThreadName(TaskInfo.StartedThreadId);
				Task.QueueName = !StartedThreadName.IsEmpty()
					? StartedThreadName
					: FString::Printf(TEXT("ThreadToExecuteOn:%d"), TaskInfo.ThreadToExecuteOn);

				for (const TraceServices::FTaskInfo::FRelationInfo& Prerequisite : TaskInfo.Prerequisites)
				{
					const int32 PrerequisiteId = (Prerequisite.RelativeId > static_cast<uint64>(MAX_int32)) ? MAX_int32 : static_cast<int32>(Prerequisite.RelativeId);
					Task.DependencyTaskIds.Add(PrerequisiteId);
				}

				Task.DependencyStatus = TEXT("resolved");
				Task.DependencyIssue.Reset();
				Task.CriticalPathTaskChain = Task.DependencyTaskIds;
				Task.CriticalPathTaskChain.Add(Task.TaskId);
				Task.CriticalPathDepth = Task.DependencyTaskIds.Num();
				Task.CriticalPathMs = Task.QueueWaitMs + Task.RunMs;

				Task.FrameIndex = Frames.IsEmpty() ? -1 : GetFrameIndexForTimestampMs(Task.StartMs);
				OutSamples.Add(MoveTemp(Task));
				return TraceServices::ETaskEnumerationResult::Continue;
			});
	}

	OutSamples.Sort([](const FTaskSample& A, const FTaskSample& B)
	{
		if (A.QueueWaitMs == B.QueueWaitMs)
		{
			if (A.RunMs == B.RunMs)
			{
				return A.TaskId < B.TaskId;
			}
			return A.RunMs > B.RunMs;
		}
		return A.QueueWaitMs > B.QueueWaitMs;
	});

	return true;
}
TSharedRef<FJsonObject> MakeTaskObject(const FTaskSample& Task)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("task_id"), Task.TaskId);
	Item->SetNumberField(TEXT("frame_index"), Task.FrameIndex);
	Item->SetStringField(TEXT("task_name"), Task.TaskName);
	Item->SetStringField(TEXT("queue_name"), Task.QueueName);

	TArray<TSharedPtr<FJsonValue>> DependencyTaskIds;
	DependencyTaskIds.Reserve(Task.DependencyTaskIds.Num());
	for (const int32 DependencyTaskId : Task.DependencyTaskIds)
	{
		DependencyTaskIds.Add(MakeShared<FJsonValueNumber>(DependencyTaskId));
	}
	Item->SetArrayField(TEXT("dependency_task_ids"), DependencyTaskIds);
	Item->SetStringField(TEXT("dependency_status"), Task.DependencyStatus);
	Item->SetStringField(TEXT("dependency_issue"), Task.DependencyIssue);

	Item->SetNumberField(TEXT("enqueue_ms"), Task.EnqueueMs);
	Item->SetNumberField(TEXT("start_ms"), Task.StartMs);
	Item->SetNumberField(TEXT("end_ms"), Task.EndMs);
	Item->SetNumberField(TEXT("queue_wait_ms"), Task.QueueWaitMs);
	Item->SetNumberField(TEXT("run_ms"), Task.RunMs);
	Item->SetNumberField(TEXT("critical_path_ms"), Task.CriticalPathMs);
	Item->SetNumberField(TEXT("critical_path_depth"), Task.CriticalPathDepth);

	TArray<TSharedPtr<FJsonValue>> CriticalPathTaskChain;
	CriticalPathTaskChain.Reserve(Task.CriticalPathTaskChain.Num());
	for (const int32 TaskId : Task.CriticalPathTaskChain)
	{
		CriticalPathTaskChain.Add(MakeShared<FJsonValueNumber>(TaskId));
	}
	Item->SetArrayField(TEXT("critical_path_task_chain"), CriticalPathTaskChain);

	Item->SetNumberField(TEXT("worker_thread_id"), Task.WorkerThreadId);
	return Item;
}

bool BuildSymbolsResolveObject(
	const FTraceContext& Context,
	const FString& ScopeName,
	TSharedPtr<FJsonObject>& OutObject,
	bool& bOutFound,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutObject.Reset();
	bOutFound = false;
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	struct FSymbolCandidate
	{
		FString Symbol;
		FString File;
		FString Module;
		int32 Line = 0;
		double Confidence = 0.0;
	};

	auto InferModuleName = [](const FString& FilePath, const FString& SymbolName) -> FString
	{
		if (FilePath.Contains(TEXT("/Engine/"), ESearchCase::IgnoreCase) || FilePath.Contains(TEXT("\\Engine\\"), ESearchCase::IgnoreCase))
		{
			return TEXT("Engine");
		}
		if (FilePath.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase) || FilePath.Contains(TEXT("\\Game\\"), ESearchCase::IgnoreCase))
		{
			return TEXT("Game");
		}
		if (SymbolName.Contains(TEXT("::"), ESearchCase::CaseSensitive))
		{
			return TEXT("Trace");
		}
		return TEXT("Unknown");
	};

	TArray<FSymbolCandidate> Candidates;
	FString AvailableModuleNames;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::ITimingProfilerProvider* TimingProfilerProvider = TraceServices::ReadTimingProfilerProvider(*Session.Get());
		if (TimingProfilerProvider == nullptr)
		{
			OutFailureStage = TEXT("timing_provider");
			OutFailureReason = TEXT("TimingProfiler provider not available");
			return false;
		}

		const TraceServices::IModuleProvider* ModuleProvider = TraceServices::ReadModuleProvider(*Session.Get());
		if (ModuleProvider != nullptr)
		{
			TArray<FString> ModuleNames;
			ModuleProvider->EnumerateModules(0, [&ModuleNames](const TraceServices::FModule& Module)
			{
				if (Module.Name != nullptr)
				{
					const FString ModuleName = Module.Name;
					if (!ModuleName.IsEmpty())
					{
						ModuleNames.AddUnique(ModuleName);
					}
				}
			});

			ModuleNames.Sort();
			if (!ModuleNames.IsEmpty())
			{
				AvailableModuleNames = FString::Join(ModuleNames, TEXT(","));
			}
		}

		FString TrimmedScope = ScopeName;
		TrimmedScope.TrimStartAndEndInline();
		TimingProfilerProvider->ReadTimers([&Candidates, &TrimmedScope, &InferModuleName](const TraceServices::ITimingProfilerTimerReader& TimerReader)
		{
			if (TrimmedScope.IsEmpty())
			{
				return;
			}
			for (uint32 TimerIndex = 0; TimerIndex < TimerReader.GetTimerCount(); ++TimerIndex)
			{
				const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(TimerIndex);
				if (Timer == nullptr || Timer->Name == nullptr)
				{
					continue;
				}

				const FString TimerName = Timer->Name;
				if (TimerName.IsEmpty())
				{
					continue;
				}

				double Score = -1.0;
				if (TimerName.Equals(TrimmedScope, ESearchCase::IgnoreCase))
				{
					Score = 0.98;
				}
				else if (TimerName.Contains(TrimmedScope, ESearchCase::IgnoreCase) || TrimmedScope.Contains(TimerName, ESearchCase::IgnoreCase))
				{
					Score = 0.72;
				}

				if (Score < 0.0)
				{
					continue;
				}

				const FString File = Timer->File != nullptr ? FString(Timer->File) : FString();
				const int32 Line = static_cast<int32>(Timer->Line);
				if (!File.IsEmpty())
				{
					Score += 0.05;
				}
				if (Line > 0)
				{
					Score += 0.02;
				}

				FSymbolCandidate Candidate;
				Candidate.Symbol = TimerName;
				Candidate.File = File;
				Candidate.Line = Line;
				Candidate.Module = InferModuleName(File, TimerName);
				Candidate.Confidence = FMath::Clamp(Score, 0.0, 0.99);
				Candidates.Add(MoveTemp(Candidate));
			}
		});
	}

	if (Candidates.IsEmpty())
	{
		return true;
	}

	Candidates.Sort([](const FSymbolCandidate& A, const FSymbolCandidate& B)
	{
		if (!FMath::IsNearlyEqual(A.Confidence, B.Confidence))
		{
			return A.Confidence > B.Confidence;
		}
		if (A.Symbol != B.Symbol)
		{
			return A.Symbol < B.Symbol;
		}
		return A.Line < B.Line;
	});

	const FSymbolCandidate& Best = Candidates[0];
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("scope_name"), ScopeName);
	Item->SetStringField(TEXT("module"), Best.Module);
	Item->SetStringField(TEXT("symbol"), Best.Symbol);
	Item->SetStringField(TEXT("function"), Best.Symbol);
	Item->SetStringField(TEXT("file"), Best.File);
	Item->SetNumberField(TEXT("line"), Best.Line);
	Item->SetNumberField(TEXT("confidence"), Best.Confidence);

	if (!AvailableModuleNames.IsEmpty())
	{
		Item->SetStringField(TEXT("available_modules"), AvailableModuleNames);
	}

	TArray<TSharedPtr<FJsonValue>> Alternatives;
	for (int32 Index = 1; Index < Candidates.Num(); ++Index)
	{
		const FSymbolCandidate& Candidate = Candidates[Index];
		const TSharedRef<FJsonObject> Alternative = MakeShared<FJsonObject>();
		Alternative->SetStringField(TEXT("module"), Candidate.Module);
		Alternative->SetStringField(TEXT("symbol"), Candidate.Symbol);
		Alternative->SetStringField(TEXT("function"), Candidate.Symbol);
		Alternative->SetStringField(TEXT("file"), Candidate.File);
		Alternative->SetNumberField(TEXT("line"), Candidate.Line);
		Alternative->SetNumberField(TEXT("confidence"), Candidate.Confidence);
		Alternatives.Add(MakeShared<FJsonValueObject>(Alternative));
	}
	Item->SetArrayField(TEXT("alternatives"), Alternatives);

	bOutFound = true;
	OutObject = Item;
	return true;
}

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

bool BuildMemorySamplesTrace(
	const FTraceContext& Context,
	TArray<FMemorySample>& OutSamples,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs)
{
	OutSamples.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}
	const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
	const auto GetFrameIndexForTimestampMs = [&Frames](const double TimestampMs) -> int32
	{
		for (const FFrameSample& Frame : Frames)
		{
			if (Frame.FrameStartMs <= TimestampMs && TimestampMs <= Frame.FrameEndMs)
			{
				return Frame.FrameIndex;
			}
		}
		return -1;
	};

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IAllocationsProvider* AllocationsProvider = TraceServices::ReadAllocationsProvider(*Session.Get());
		if (AllocationsProvider == nullptr)
		{
			return true;
		}

		AllocationsProvider->BeginRead();
		if (!AllocationsProvider->IsInitialized())
		{
			AllocationsProvider->EndRead();
			return true;
		}

		const double DurationSec = Session->GetDurationSeconds();
		const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
		const double EndSecRaw = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;
		const double EndSec = FMath::Max(StartSec + KINDA_SMALL_NUMBER, EndSecRaw);

		int32 StartIndex = 0;
		int32 EndIndex = -1;
		AllocationsProvider->GetTimelineIndexRange(StartSec, EndSec, StartIndex, EndIndex);
		if (EndIndex < StartIndex)
		{
			AllocationsProvider->EndRead();
			return true;
		}

		AllocationsProvider->EnumerateMaxTotalAllocatedMemoryTimeline(StartIndex, EndIndex,
			[&OutSamples, &GetFrameIndexForTimestampMs](double Time, double, uint64 Value)
			{
				FMemorySample Sample;
				Sample.TimestampMs = Time * 1000.0;
				Sample.Bytes = static_cast<int64>(Value > static_cast<uint64>(MAX_int64) ? MAX_int64 : Value);
				Sample.FrameIndex = GetFrameIndexForTimestampMs(Sample.TimestampMs);
				OutSamples.Add(MoveTemp(Sample));
			});
		AllocationsProvider->EndRead();
	}

	OutSamples.Sort([](const FMemorySample& A, const FMemorySample& B)
	{
		return A.TimestampMs < B.TimestampMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeMemorySummaryObject(const TArray<FMemorySample>& Samples)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	if (Samples.IsEmpty())
	{
		Item->SetNumberField(TEXT("min_bytes"), 0.0);
		Item->SetNumberField(TEXT("max_bytes"), 0.0);
		Item->SetNumberField(TEXT("avg_bytes"), 0.0);
		Item->SetNumberField(TEXT("end_bytes"), 0.0);
		return Item;
	}

	int64 MinBytes = TNumericLimits<int64>::Max();
	int64 MaxBytes = 0;
	long double SumBytes = 0.0;
	for (const FMemorySample& Sample : Samples)
	{
		MinBytes = FMath::Min(MinBytes, Sample.Bytes);
		MaxBytes = FMath::Max(MaxBytes, Sample.Bytes);
		SumBytes += static_cast<long double>(Sample.Bytes);
	}

	Item->SetNumberField(TEXT("min_bytes"), static_cast<double>(MinBytes));
	Item->SetNumberField(TEXT("max_bytes"), static_cast<double>(MaxBytes));
	Item->SetNumberField(TEXT("avg_bytes"), static_cast<double>(SumBytes / static_cast<long double>(Samples.Num())));
	Item->SetNumberField(TEXT("end_bytes"), static_cast<double>(Samples.Last().Bytes));
	return Item;
}

TSharedRef<FJsonObject> MakeMemoryPeakObject(const TArray<FMemorySample>& Samples)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	if (Samples.IsEmpty())
	{
		Item->SetNumberField(TEXT("peak_bytes"), 0.0);
		Item->SetNumberField(TEXT("peak_timestamp_ms"), 0.0);
		Item->SetNumberField(TEXT("peak_frame_index"), -1);
		Item->SetArrayField(TEXT("peak_context"), {});
		return Item;
	}

	int32 PeakIndex = 0;
	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		if (Samples[Index].Bytes > Samples[PeakIndex].Bytes)
		{
			PeakIndex = Index;
		}
	}

	const FMemorySample& PeakSample = Samples[PeakIndex];
	Item->SetNumberField(TEXT("peak_bytes"), static_cast<double>(PeakSample.Bytes));
	Item->SetNumberField(TEXT("peak_timestamp_ms"), PeakSample.TimestampMs);
	Item->SetNumberField(TEXT("peak_frame_index"), PeakSample.FrameIndex);

	TArray<TSharedPtr<FJsonValue>> PeakContext;
	for (int32 ContextIndex = FMath::Max(0, PeakIndex - 1); ContextIndex <= FMath::Min(PeakIndex + 1, Samples.Num() - 1); ++ContextIndex)
	{
		const FMemorySample& ContextSample = Samples[ContextIndex];
		const TSharedRef<FJsonObject> ContextItem = MakeShared<FJsonObject>();
		ContextItem->SetNumberField(TEXT("timestamp_ms"), ContextSample.TimestampMs);
		ContextItem->SetNumberField(TEXT("bytes"), static_cast<double>(ContextSample.Bytes));
		ContextItem->SetNumberField(TEXT("frame_index"), ContextSample.FrameIndex);
		PeakContext.Add(MakeShared<FJsonValueObject>(ContextItem));
	}
	Item->SetArrayField(TEXT("peak_context"), PeakContext);

	return Item;
}

bool BuildMemoryTagsTrace(
	const FTraceContext& Context,
	TArray<FMemoryTagSample>& OutTags,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs)
{
	OutTags.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}
	TMap<FString, int64> TagBytes;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		const TraceServices::IMemoryProvider* MemoryProvider = TraceServices::ReadMemoryProvider(*Session.Get());
		if (MemoryProvider == nullptr || MemoryProvider->GetTrackerCount() == 0)
		{
			return true;
		}

		TArray<TraceServices::FMemoryTrackerInfo> Trackers;
		MemoryProvider->EnumerateTrackers([&Trackers](const TraceServices::FMemoryTrackerInfo& Tracker)
		{
			Trackers.Add(Tracker);
		});
		if (Trackers.IsEmpty())
		{
			return true;
		}

		const TraceServices::FMemoryTrackerId TrackerId = Trackers[0].Id;
		const double DurationSec = Session->GetDurationSeconds();
		const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
		const double EndSecRaw = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;
		const double EndSec = FMath::Max(StartSec + KINDA_SMALL_NUMBER, EndSecRaw);

		MemoryProvider->EnumerateTags([&](const TraceServices::FMemoryTagInfo& TagInfo)
		{
			if (TagInfo.Name.IsEmpty())
			{
				return;
			}

			if ((TagInfo.Trackers & (1ull << static_cast<uint64>(TrackerId))) == 0)
			{
				return;
			}

			int64 LastValue = 0;
			bool bHasSample = false;
			MemoryProvider->EnumerateTagSamples(TrackerId, TagInfo.Id, StartSec, EndSec, true,
				[&LastValue, &bHasSample](double, double, const TraceServices::FMemoryTagSample& Sample)
				{
					LastValue = Sample.Value;
					bHasSample = true;
				});

			if (bHasSample && LastValue > 0)
			{
				TagBytes.Add(TagInfo.Name, LastValue);
			}
		});
	}

	int64 TotalBytes = 0;
	for (const TPair<FString, int64>& Pair : TagBytes)
	{
		TotalBytes += Pair.Value;
	}

	if (TotalBytes <= 0)
	{
		return true;
	}

	for (const TPair<FString, int64>& Pair : TagBytes)
	{
		FMemoryTagSample Tag;
		Tag.TagName = Pair.Key;
		Tag.Bytes = Pair.Value;
		Tag.PercentRatio = static_cast<double>(Pair.Value) / static_cast<double>(TotalBytes);
		OutTags.Add(MoveTemp(Tag));
	}

	OutTags.Sort([](const FMemoryTagSample& A, const FMemoryTagSample& B)
	{
		if (A.Bytes == B.Bytes)
		{
			return A.TagName < B.TagName;
		}
		return A.Bytes > B.Bytes;
	});

	return true;
}

TSharedRef<FJsonObject> MakeMemoryTagObject(const FMemoryTagSample& Tag)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("tag_name"), Tag.TagName);
	Item->SetNumberField(TEXT("bytes"), static_cast<double>(Tag.Bytes));
	Item->SetNumberField(TEXT("percent_ratio"), Tag.PercentRatio);
	return Item;
}

bool BuildMarks(
	const FTraceContext& Context,
	TArray<FMarkSample>& OutMarks,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs)
{
	OutMarks.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	double DurationSec = 0.0;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		DurationSec = Session->GetDurationSeconds();
	}
	const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
	const double EndSec = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());

		const TraceServices::IBookmarkProvider& BookmarkProvider = TraceServices::ReadBookmarkProvider(*Session.Get());
		BookmarkProvider.EnumerateBookmarks(StartSec, EndSec, [&OutMarks](const TraceServices::FBookmark& Bookmark)
		{
			FMarkSample Mark;
			Mark.TimestampMs = Bookmark.Time * 1000.0;
			Mark.Category = TEXT("bookmark");
			Mark.Channel = TEXT("Bookmark");
			Mark.Message = Bookmark.Text != nullptr ? Bookmark.Text : TEXT("");
			Mark.ThreadId = -1;
			OutMarks.Add(MoveTemp(Mark));
		});

		const TraceServices::ILogProvider& LogProvider = TraceServices::ReadLogProvider(*Session.Get());
		LogProvider.EnumerateMessages(StartSec, EndSec, [&OutMarks](const TraceServices::FLogMessageInfo& Message)
		{
			FMarkSample Mark;
			Mark.TimestampMs = Message.Time * 1000.0;
			Mark.Category = TEXT("log");
			Mark.Channel = (Message.Category != nullptr && Message.Category->Name != nullptr) ? Message.Category->Name : TEXT("Log");
			Mark.Message = Message.Message != nullptr ? Message.Message : TEXT("");
			Mark.ThreadId = -1;
			OutMarks.Add(MoveTemp(Mark));
		});
	}

	OutMarks.Sort([](const FMarkSample& A, const FMarkSample& B)
	{
		if (A.TimestampMs == B.TimestampMs)
		{
			if (A.Category == B.Category)
			{
				return A.Message < B.Message;
			}
			return A.Category < B.Category;
		}
		return A.TimestampMs < B.TimestampMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeMarkObject(const FMarkSample& Mark)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("timestamp_ms"), Mark.TimestampMs);
	Item->SetStringField(TEXT("category"), Mark.Category);
	Item->SetStringField(TEXT("channel"), Mark.Channel);
	Item->SetStringField(TEXT("message"), Mark.Message);
	Item->SetNumberField(TEXT("thread_id"), Mark.ThreadId);
	return Item;
}

FString MakeEnvelopeWithObject(const TSharedRef<FJsonObject>& Data, const TMap<FString, FString>& Meta)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("data"), Data);
	if (!Meta.IsEmpty())
	{
		const TSharedRef<FJsonObject> MetaObject = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Meta)
		{
			MetaObject->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("meta"), MetaObject);
	}
	return SerializeJson(Root);
}

FString MakeEnvelopeWithArray(const TArray<TSharedPtr<FJsonValue>>& Data, const TMap<FString, FString>& Meta)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("data"), Data);
	if (!Meta.IsEmpty())
	{
		const TSharedRef<FJsonObject> MetaObject = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Meta)
		{
			MetaObject->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("meta"), MetaObject);
	}
	return SerializeJson(Root);
}
}
