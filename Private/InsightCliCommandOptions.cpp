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

bool TryParseFrameRange(const FString& Text, FFrameRange& OutRange)
{
	const int32 ColonPos = Text.Find(TEXT(":"));
	if (ColonPos <= 0 || ColonPos >= Text.Len() - 1)
	{
		return false;
	}

	const FString StartText = Text.Left(ColonPos);
	const FString EndText = Text.Mid(ColonPos + 1);

	int32 Start = 0;
	int32 End = 0;
	if (!TryParseInt(StartText, Start) || !TryParseInt(EndText, End))
	{
		return false;
	}
	if (Start < 0 || End < 0 || End < Start)
	{
		return false;
	}

	OutRange.StartInclusive = Start;
	OutRange.EndExclusive = End;
	return true;
}

void ResolveFrameRangeToTimeWindow(const TArray<FFrameSample>& Frames, const FFrameRange& FrameRange, TOptional<double>& OutStartMs, TOptional<double>& OutEndMs)
{
	bool bFound = false;
	double StartMs = 0.0;
	double EndMs = 0.0;

	for (const FFrameSample& Sample : Frames)
	{
		if (Sample.FrameIndex < FrameRange.StartInclusive || Sample.FrameIndex >= FrameRange.EndExclusive)
		{
			continue;
		}

		if (!bFound)
		{
			bFound = true;
			StartMs = Sample.FrameStartMs;
			EndMs = Sample.FrameEndMs;
		}
		else
		{
			StartMs = FMath::Min(StartMs, Sample.FrameStartMs);
			EndMs = FMath::Max(EndMs, Sample.FrameEndMs);
		}
	}

	if (bFound)
	{
		OutStartMs = StartMs;
		OutEndMs = EndMs;
		return;
	}

	// An empty frame range maps to an empty time window.
	OutStartMs = 0.0;
	OutEndMs = 0.0;
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

bool TryExtractGlobalOutputOptions(TArray<FString>& InOutArgs, FGlobalOutputOptions& OutOptions, FInsightCliResponse& OutError)
{
	OutOptions = FGlobalOutputOptions();

	TArray<FString> FilteredArgs;
	FilteredArgs.Reserve(InOutArgs.Num());

	auto ParseFieldList = [&OutOptions](const FString& Value)
	{
		TArray<FString> RawFields;
		Value.ParseIntoArray(RawFields, TEXT(","), true);
		for (FString Field : RawFields)
		{
			Field.TrimStartAndEndInline();
			if (!Field.IsEmpty())
			{
				OutOptions.Fields.AddUnique(Field);
			}
		}
	};

	for (int32 Index = 0; Index < InOutArgs.Num(); ++Index)
	{
		const FString& Arg = InOutArgs[Index];
		if (Arg == TEXT("--fields"))
		{
			if (Index + 1 >= InOutArgs.Num())
			{
				OutError = MakeOptionError(TEXT("--fields requires a comma-separated value list."));
				return false;
			}

			const FString Value = InOutArgs[++Index];
			ParseFieldList(Value);
			continue;
		}

		if (Arg.StartsWith(TEXT("--fields=")))
		{
			const FString Value = Arg.Mid(9);
			ParseFieldList(Value);
			continue;
		}

		if (Arg == TEXT("--max-rows"))
		{
			if (Index + 1 >= InOutArgs.Num())
			{
				OutError = MakeOptionError(TEXT("--max-rows requires a positive integer value."));
				return false;
			}

			int32 ParsedMaxRows = 0;
			if (!TryParseInt(InOutArgs[++Index], ParsedMaxRows) || ParsedMaxRows <= 0)
			{
				OutError = MakeOptionError(TEXT("--max-rows must be > 0."));
				return false;
			}

			OutOptions.MaxRows = ParsedMaxRows;
			continue;
		}

		if (Arg.StartsWith(TEXT("--max-rows=")))
		{
			int32 ParsedMaxRows = 0;
			if (!TryParseInt(Arg.Mid(11), ParsedMaxRows) || ParsedMaxRows <= 0)
			{
				OutError = MakeOptionError(TEXT("--max-rows must be > 0."));
				return false;
			}

			OutOptions.MaxRows = ParsedMaxRows;
			continue;
		}

		FilteredArgs.Add(Arg);
	}

	InOutArgs = MoveTemp(FilteredArgs);
	return true;
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
	OutError = MakeOptionError(TEXT("Unknown option(s) for command."), Details);
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
		OutError = MakeOptionError(TEXT("limit must be > 0."));
		return false;
	}

	if (bOutHasFrameIndex && OutFrameIndexFilter < 0)
	{
		OutError = MakeOptionError(TEXT("frame-index must be >= 0."));
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
		OutError = MakeOptionError(TEXT("time-start must be <= time-end."));
		return false;
	}

	return true;
}

bool TryResolveTimeWindowMs(const FTraceContext& Context, const TArray<FString>& Args, bool bAllowFrameRange, FResolvedTimeWindowMs& OutWindow, FInsightCliResponse& OutError)
{
	OutWindow = {};

	const bool bHasTimeStart = HasOption(Args, TEXT("--time-start"));
	const bool bHasTimeEnd = HasOption(Args, TEXT("--time-end"));
	const bool bHasFrameRange = HasOption(Args, TEXT("--frame-range"));

	if (bHasFrameRange && !bAllowFrameRange)
	{
		OutError = MakeOptionError(TEXT("frame-range is not supported for this command."));
		return false;
	}

	if (bHasFrameRange && (bHasTimeStart || bHasTimeEnd))
	{
		OutError = MakeOptionError(TEXT("frame-range cannot be combined with time-start/time-end."));
		return false;
	}

	if (bHasFrameRange)
	{
		FString FrameRangeText;
		if (!TryGetStringOption(Args, TEXT("--frame-range"), FrameRangeText))
		{
			OutError = MakeOptionError(TEXT("frame-range requires <start:end> value, for example --frame-range 10:20."));
			return false;
		}

		FFrameRange FrameRange;
		if (!TryParseFrameRange(FrameRangeText, FrameRange))
		{
			OutError = MakeOptionError(TEXT("frame-range must match <start:end> with non-negative integers and end >= start."));
			return false;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("time_window.frame_range")))
		{
			OutError = FrameGuardError;
			return false;
		}

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		ResolveFrameRangeToTimeWindow(Frames, FrameRange, OutWindow.StartMs, OutWindow.EndMs);
		OutWindow.FrameRange = FrameRange;
		OutWindow.Source = TEXT("frame-range");
		return true;
	}

	FTimeWindowMs ExplicitWindow;
	if (!TryGetTimeWindowMs(Args, ExplicitWindow, OutError))
	{
		return false;
	}

	OutWindow.StartMs = ExplicitWindow.StartMs;
	OutWindow.EndMs = ExplicitWindow.EndMs;
	OutWindow.Source = ExplicitWindow.IsSet() ? TEXT("explicit") : TEXT("full");
	return true;
}

void AppendTimeWindowMeta(const FResolvedTimeWindowMs& TimeWindow, TMap<FString, FString>& OutMeta)
{
	OutMeta.Add(TEXT("time_window_source"), TimeWindow.Source);
	if (TimeWindow.StartMs.IsSet())
	{
		OutMeta.Add(TEXT("time_window_start_ms"), ToNumberString(TimeWindow.StartMs.GetValue()));
	}
	if (TimeWindow.EndMs.IsSet())
	{
		OutMeta.Add(TEXT("time_window_end_ms"), ToNumberString(TimeWindow.EndMs.GetValue()));
	}
	if (TimeWindow.FrameRange.IsSet())
	{
		const FFrameRange& FrameRange = TimeWindow.FrameRange.GetValue();
		OutMeta.Add(TEXT("frame_range"), FString::Printf(TEXT("%d:%d"), FrameRange.StartInclusive, FrameRange.EndExclusive));
	}
}

bool TryGetPositiveLimit(const TArray<FString>& Args, int32 DefaultLimit, int32& OutLimit, FInsightCliResponse& OutError)
{
	OutLimit = DefaultLimit;
	const bool bHasLimit = TryGetIntOption(Args, TEXT("--limit"), OutLimit);
	if (bHasLimit && OutLimit <= 0)
	{
		OutError = MakeOptionError(TEXT("limit must be > 0."));
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

	OutError = MakeOptionError(FString::Printf(TEXT("%s is required for %s."), OptionName, OwnerCommand));
	return false;
}
}