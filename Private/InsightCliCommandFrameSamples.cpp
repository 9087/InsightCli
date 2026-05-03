// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"

namespace UE::InsightCli::Internal
{
namespace
{
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
}