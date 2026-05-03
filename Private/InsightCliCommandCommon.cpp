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

bool TryGetTimeWindowMs(const TArray<FString>& Args, FTimeWindowMs& OutWindow, FInsightCliResponse& OutError)
{
	OutWindow.StartMs.Reset();
	OutWindow.EndMs.Reset();

	double StartMs = 0.0;
	double EndMs = 0.0;
	if (TryGetDoubleOption(Args, TEXT("--time-start"), StartMs))
	{
		OutWindow.StartMs = StartMs;
	}
	if (TryGetDoubleOption(Args, TEXT("--time-end"), EndMs))
	{
		OutWindow.EndMs = EndMs;
	}

	if (OutWindow.StartMs.IsSet() && OutWindow.EndMs.IsSet() && OutWindow.StartMs.GetValue() > OutWindow.EndMs.GetValue())
	{
		OutError = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("time-start must be <= time-end."));
		return false;
	}

	return true;
}

bool TryGetPositiveLimit(const TArray<FString>& Args, int32 DefaultLimit, int32& OutLimit, FInsightCliResponse& OutError)
{
	OutLimit = DefaultLimit;
	const bool bHasLimit = TryGetIntOption(Args, TEXT("--limit"), OutLimit);
	if (bHasLimit && OutLimit <= 0)
	{
		OutError = FInsightCliResponse::Error(4, TEXT("E1003"), TEXT("limit must be > 0."));
		return false;
	}

	return true;
}

bool RequireStringOption(const TArray<FString>& Args, const TCHAR* OptionName, const TCHAR* OwnerCommand, FString& OutValue, FInsightCliResponse& OutError)
{
	if (TryGetStringOption(Args, OptionName, OutValue))
	{
		return true;
	}

	OutError = FInsightCliResponse::Error(
		4,
		TEXT("E1003"),
		FString::Printf(TEXT("%s is required for %s."), OptionName, OwnerCommand));
	return false;
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
