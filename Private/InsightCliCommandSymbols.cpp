// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Misc/Paths.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Modules.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UE::InsightCli::Internal
{
namespace
{
bool IsWordBoundaryChar(const TCHAR Char)
{
	return !(FChar::IsAlnum(Char) || Char == TEXT('_'));
}

bool ContainsTokenIgnoreCase(const FString& Haystack, const FString& Needle)
{
	if (Haystack.IsEmpty() || Needle.IsEmpty())
	{
		return false;
	}

	const FString HaystackLower = Haystack.ToLower();
	const FString NeedleLower = Needle.ToLower();
	int32 SearchStart = 0;
	while (SearchStart < HaystackLower.Len())
	{
		const int32 FoundIndex = HaystackLower.Find(NeedleLower, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
		if (FoundIndex == INDEX_NONE)
		{
			return false;
		}

		const int32 LeftIndex = FoundIndex - 1;
		const int32 RightIndex = FoundIndex + NeedleLower.Len();
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

int32 MatchTypeRank(const FString& MatchType)
{
	if (MatchType == TEXT("exact"))
	{
		return 3;
	}
	if (MatchType == TEXT("prefix"))
	{
		return 2;
	}
	if (MatchType == TEXT("fuzzy"))
	{
		return 1;
	}
	return 0;
}
}

bool BuildSymbolsResolveObject(
	const FTraceContext& Context,
	const FString& ScopeName,
	TSharedPtr<FJsonObject>& OutObject,
	bool& bOutFound,
	FString& OutResolveReason,
	TMap<FString, FString>& OutResolveMeta,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutObject.Reset();
	bOutFound = false;
	OutResolveReason = TEXT("not_found");
	OutResolveMeta.Reset();
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
		FString MatchType;
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
	FString TrimmedScope = ScopeName;
	TrimmedScope.TrimStartAndEndInline();
	if (TrimmedScope.IsEmpty())
	{
		OutResolveReason = TEXT("not_found");
		return true;
	}

	const bool bAllowFuzzy = TrimmedScope.Len() >= 3;
	if (!bAllowFuzzy)
	{
		OutResolveReason = TEXT("query_too_short");
	}

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

		TimingProfilerProvider->ReadTimers([&Candidates, &TrimmedScope, &InferModuleName, bAllowFuzzy](const TraceServices::ITimingProfilerTimerReader& TimerReader)
		{
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
				FString MatchType;
				if (TimerName.Equals(TrimmedScope, ESearchCase::IgnoreCase))
				{
					Score = 0.99;
					MatchType = TEXT("exact");
				}
				else if (bAllowFuzzy && TimerName.StartsWith(TrimmedScope, ESearchCase::IgnoreCase))
				{
					Score = 0.90;
					MatchType = TEXT("prefix");
				}
				else if (bAllowFuzzy && ContainsTokenIgnoreCase(TimerName, TrimmedScope))
				{
					Score = 0.78;
					MatchType = TEXT("fuzzy");
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
				Candidate.MatchType = MatchType;
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
		const int32 RankA = MatchTypeRank(A.MatchType);
		const int32 RankB = MatchTypeRank(B.MatchType);
		if (RankA != RankB)
		{
			return RankA > RankB;
		}
		if (A.Symbol != B.Symbol)
		{
			return A.Symbol < B.Symbol;
		}
		return A.Line < B.Line;
	});

	const FSymbolCandidate& Best = Candidates[0];
	const bool bAmbiguous = Candidates.Num() > 1
		&& (FMath::Abs(Best.Confidence - Candidates[1].Confidence) <= 0.02
			|| (Best.MatchType == TEXT("fuzzy") && FMath::Abs(Best.Confidence - Candidates[1].Confidence) <= 0.05));

	OutResolveMeta.Add(TEXT("match_score"), ToNumberString(Best.Confidence));
	OutResolveMeta.Add(TEXT("candidate_count"), FString::FromInt(Candidates.Num()));

	if (bAmbiguous)
	{
		OutResolveReason = TEXT("ambiguous");
		OutResolveMeta.Add(TEXT("match_type"), TEXT("ambiguous"));

		const TSharedRef<FJsonObject> AmbiguousObject = MakeShared<FJsonObject>();
		AmbiguousObject->SetStringField(TEXT("scope_name"), ScopeName);
		AmbiguousObject->SetBoolField(TEXT("ambiguous"), true);

		TArray<TSharedPtr<FJsonValue>> CandidateRows;
		CandidateRows.Reserve(Candidates.Num());
		for (const FSymbolCandidate& Candidate : Candidates)
		{
			const TSharedRef<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
			CandidateObject->SetStringField(TEXT("module"), Candidate.Module);
			CandidateObject->SetStringField(TEXT("symbol"), Candidate.Symbol);
			CandidateObject->SetStringField(TEXT("function"), Candidate.Symbol);
			CandidateObject->SetStringField(TEXT("file"), Candidate.File);
			CandidateObject->SetNumberField(TEXT("line"), Candidate.Line);
			CandidateObject->SetNumberField(TEXT("confidence"), Candidate.Confidence);
			CandidateObject->SetStringField(TEXT("match_type"), Candidate.MatchType);
			CandidateRows.Add(MakeShared<FJsonValueObject>(CandidateObject));
		}
		AmbiguousObject->SetArrayField(TEXT("candidates"), CandidateRows);

		if (!AvailableModuleNames.IsEmpty())
		{
			AmbiguousObject->SetStringField(TEXT("available_modules"), AvailableModuleNames);
		}

		OutObject = AmbiguousObject;
		bOutFound = false;
		return true;
	}

	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("scope_name"), ScopeName);
	Item->SetStringField(TEXT("module"), Best.Module);
	Item->SetStringField(TEXT("symbol"), Best.Symbol);
	Item->SetStringField(TEXT("function"), Best.Symbol);
	Item->SetStringField(TEXT("file"), Best.File);
	Item->SetNumberField(TEXT("line"), Best.Line);
	Item->SetNumberField(TEXT("confidence"), Best.Confidence);
	Item->SetStringField(TEXT("match_type"), Best.MatchType);
	Item->SetNumberField(TEXT("score"), Best.Confidence);
	OutResolveReason = TEXT("found");
	OutResolveMeta.Add(TEXT("match_type"), Best.MatchType);

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
		Alternative->SetStringField(TEXT("match_type"), Candidate.MatchType);
		Alternatives.Add(MakeShared<FJsonValueObject>(Alternative));
	}
	Item->SetArrayField(TEXT("alternatives"), Alternatives);

	bOutFound = true;
	OutObject = Item;
	return true;
}
}
