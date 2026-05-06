// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Containers/UnrealString.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UE::InsightCli::Internal
{
namespace
{
bool IsWordBoundaryChar(const TCHAR Char)
{
	return !(FChar::IsAlnum(Char) || Char == TEXT('_'));
}

bool ContainsToken(const FString& HaystackLower, const TCHAR* TokenLower)
{
	const FString Token(TokenLower);
	if (Token.IsEmpty() || HaystackLower.IsEmpty())
	{
		return false;
	}

	int32 SearchStart = 0;
	while (SearchStart < HaystackLower.Len())
	{
		const int32 FoundIndex = HaystackLower.Find(Token, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
		if (FoundIndex == INDEX_NONE)
		{
			return false;
		}

		const int32 LeftIndex = FoundIndex - 1;
		const int32 RightIndex = FoundIndex + Token.Len();
		const bool bLeftOk = (LeftIndex < 0) || IsWordBoundaryChar(HaystackLower[LeftIndex]);
		const bool bRightOk = (RightIndex >= HaystackLower.Len()) || IsWordBoundaryChar(HaystackLower[RightIndex]);
		if (bLeftOk && bRightOk)
		{
			return true;
		}

		SearchStart = FoundIndex + 1;
	}

	return false;
}
}

FString ClassifyCpuStatGroup(const FString& ScopeName)
{
	const FString Lower = ScopeName.ToLower();

	if (Lower.Contains(TEXT("uworld::tick"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("world tick"), ESearchCase::CaseSensitive)
		|| ContainsToken(Lower, TEXT("worldtick")))
	{
		return TEXT("world_tick");
	}

	if (ContainsToken(Lower, TEXT("animgraph"))
		|| ContainsToken(Lower, TEXT("animation"))
		|| ContainsToken(Lower, TEXT("animinstance"))
		|| ContainsToken(Lower, TEXT("anim")))
	{
		return TEXT("animation");
	}

	if (ContainsToken(Lower, TEXT("physics"))
		|| ContainsToken(Lower, TEXT("chaos"))
		|| ContainsToken(Lower, TEXT("broadphase"))
		|| ContainsToken(Lower, TEXT("narrowphase"))
		|| ContainsToken(Lower, TEXT("constraint"))
		|| ContainsToken(Lower, TEXT("solver"))
		|| ContainsToken(Lower, TEXT("collision")))
	{
		return TEXT("physics");
	}

	if (Lower.Contains(TEXT("collectgarbage"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("collectgarbageinternal"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("incrementalpurgegarbage"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("reachability"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("markobjectsasunreachable"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("unhashunreachableobjects"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("purgegarbage"), ESearchCase::CaseSensitive)
		|| Lower.Contains(TEXT("garbagecollection"), ESearchCase::CaseSensitive))
	{
		return TEXT("gc");
	}

	if (ContainsToken(Lower, TEXT("slate"))
		|| ContainsToken(Lower, TEXT("umg"))
		|| ContainsToken(Lower, TEXT("widget")))
	{
		return TEXT("slate");
	}

	return TEXT("other");
}

namespace
{
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
}

bool ResolveCpuThreadFilterToTraceId(const FTraceContext& Context, const FString& ThreadFilter, uint32& OutThreadId, FString& OutNormalizedThread, FString& OutFailureStage, FString& OutFailureReason)
{
	int32 ParsedThreadId = -1;
	if (TryParseInt(ThreadFilter, ParsedThreadId) && ParsedThreadId >= 0)
	{
		OutThreadId = static_cast<uint32>(ParsedThreadId);
		OutNormalizedThread = FString::FromInt(ParsedThreadId);
		return true;
	}

	FString DesiredThreadName;
	if (ThreadFilter.Equals(TEXT("GameThread"), ESearchCase::IgnoreCase))
	{
		DesiredThreadName = TEXT("GameThread");
		OutNormalizedThread = TEXT("GameThread");
	}
	else if (ThreadFilter.Equals(TEXT("RenderThread"), ESearchCase::IgnoreCase))
	{
		DesiredThreadName = TEXT("RenderThread");
		OutNormalizedThread = TEXT("RenderThread");
	}
	else if (ThreadFilter.Equals(TEXT("RHIThread"), ESearchCase::IgnoreCase))
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

bool BuildCpuTopSamples(
	const FTraceContext& Context,
	const TOptional<uint32>& CpuThreadId,
	TArray<FCpuScopeSample>& OutSamples,
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

		TraceServices::FCreateAggreationParams Params;
		Params.IntervalStart = IntervalStartSec.IsSet() ? FMath::Max(0.0, IntervalStartSec.GetValue()) : 0.0;
		const double DefaultEndSec = Session->GetDurationSeconds();
		Params.IntervalEnd = IntervalEndSec.IsSet() ? FMath::Min(DefaultEndSec, FMath::Max(Params.IntervalStart, IntervalEndSec.GetValue())) : DefaultEndSec;
		Params.IntervalEnd = FMath::Max(Params.IntervalStart + KINDA_SMALL_NUMBER, Params.IntervalEnd);
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
			Sample.ThreadId = CpuThreadId.IsSet() ? static_cast<int32>(CpuThreadId.GetValue()) : -1;
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
}
