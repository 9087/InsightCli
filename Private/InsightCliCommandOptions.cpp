// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Misc/DefaultValueHelper.h"

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
}