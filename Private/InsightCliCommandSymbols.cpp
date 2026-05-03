// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Misc/Paths.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Modules.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UE::InsightCli::Internal
{
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
}
