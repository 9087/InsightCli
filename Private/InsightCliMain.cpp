// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandRegistry.h"
#include "InsightCliCommandContext.h"
#include "InsightCliTypes.h"
#include "RequiredProgramMainCPPInclude.h"

#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <cstdio>
#include <iostream>

IMPLEMENT_APPLICATION(InsightCli, "InsightCli");

namespace
{
constexpr TCHAR InvalidFormatMessage[] = TEXT("Invalid command format. Expected: insight-cli <trace_path> <group> <action> [options]");
constexpr TCHAR InvalidBatchFormatMessage[] = TEXT("Invalid batch format. Expected: insight-cli <trace_path> --batch <commands.json|- >");

bool IsHelpToken(const TCHAR* Token)
{
	return FCString::Stricmp(Token, TEXT("help")) == 0
		|| FCString::Stricmp(Token, TEXT("--help")) == 0
		|| FCString::Stricmp(Token, TEXT("-h")) == 0;
}

bool IsSchemaToken(const TCHAR* Token)
{
	return FCString::Stricmp(Token, TEXT("schema")) == 0;
}

bool IsBoolOptionName(const FString& OptionName)
{
	return OptionName == TEXT("case-sensitive") || OptionName == TEXT("exact");
}

FString DataQualityToString(const UE::InsightCli::EInsightCliCommandDataQuality Quality)
{
	switch (Quality)
	{
	case UE::InsightCli::EInsightCliCommandDataQuality::TraceBacked:
		return TEXT("trace");
	case UE::InsightCli::EInsightCliCommandDataQuality::ApproxOrTraceBacked:
		return TEXT("approx_or_trace");
	case UE::InsightCli::EInsightCliCommandDataQuality::Approx:
		return TEXT("approx");
	default:
		return TEXT("trace");
	}
}

FString DataQualityHelpTag(const UE::InsightCli::EInsightCliCommandDataQuality Quality)
{
	switch (Quality)
	{
	case UE::InsightCli::EInsightCliCommandDataQuality::TraceBacked:
		return TEXT("trace");
	case UE::InsightCli::EInsightCliCommandDataQuality::Approx:
		return TEXT("approx");
	case UE::InsightCli::EInsightCliCommandDataQuality::ApproxOrTraceBacked:
		return TEXT("trace/approx");
	default:
		return TEXT("trace");
	}
}

TSharedRef<FJsonObject> MakeOptionSchemaObject(const FString& OptionName, bool bRequired)
{
	const TSharedRef<FJsonObject> OptionObject = MakeShared<FJsonObject>();
	OptionObject->SetStringField(TEXT("name"), OptionName);
	OptionObject->SetStringField(TEXT("type"), IsBoolOptionName(OptionName) ? TEXT("boolean") : TEXT("string"));
	OptionObject->SetBoolField(TEXT("required"), bRequired);
	if (!bRequired)
	{
		OptionObject->SetField(TEXT("default"), MakeShared<FJsonValueNull>());
	}
	return OptionObject;
}

TSharedRef<FJsonObject> MakeCommandSchemaEntry(const UE::InsightCli::FInsightCliCommandCatalogEntry& Entry)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("command"), FString::Printf(TEXT("%s %s"), *Entry.Group, *Entry.Action));
	Item->SetStringField(TEXT("group"), Entry.Group);
	Item->SetStringField(TEXT("action"), Entry.Action);

	TArray<TSharedPtr<FJsonValue>> RequiredOptions;
	RequiredOptions.Reserve(Entry.RequiredOptions.Num());
	for (const FString& Option : Entry.RequiredOptions)
	{
		RequiredOptions.Add(MakeShared<FJsonValueString>(Option));
	}

	TArray<TSharedPtr<FJsonValue>> OptionalOptions;
	OptionalOptions.Reserve(Entry.OptionalOptions.Num());
	for (const FString& Option : Entry.OptionalOptions)
	{
		OptionalOptions.Add(MakeShared<FJsonValueString>(Option));
	}

	const TSharedRef<FJsonObject> InputSchema = MakeShared<FJsonObject>();
	InputSchema->SetStringField(TEXT("type"), TEXT("object"));
	InputSchema->SetStringField(TEXT("trace_path"), TEXT("string"));
	InputSchema->SetArrayField(TEXT("required_options"), RequiredOptions);
	InputSchema->SetArrayField(TEXT("optional_options"), OptionalOptions);

	TArray<TSharedPtr<FJsonValue>> RequiredChannels;
	RequiredChannels.Reserve(Entry.RequiredChannels.Num());
	for (const FString& Channel : Entry.RequiredChannels)
	{
		RequiredChannels.Add(MakeShared<FJsonValueString>(Channel));
	}

	TArray<TSharedPtr<FJsonValue>> OptionalChannels;
	OptionalChannels.Reserve(Entry.OptionalChannels.Num());
	for (const FString& Channel : Entry.OptionalChannels)
	{
		OptionalChannels.Add(MakeShared<FJsonValueString>(Channel));
	}

	InputSchema->SetArrayField(TEXT("required_channels"), RequiredChannels);
	InputSchema->SetArrayField(TEXT("optional_channels"), OptionalChannels);

	TArray<TSharedPtr<FJsonValue>> OptionSchemas;
	OptionSchemas.Reserve(Entry.RequiredOptions.Num() + Entry.OptionalOptions.Num());
	for (const FString& Option : Entry.RequiredOptions)
	{
		OptionSchemas.Add(MakeShared<FJsonValueObject>(MakeOptionSchemaObject(Option, true)));
	}
	for (const FString& Option : Entry.OptionalOptions)
	{
		OptionSchemas.Add(MakeShared<FJsonValueObject>(MakeOptionSchemaObject(Option, false)));
	}
	InputSchema->SetArrayField(TEXT("options"), OptionSchemas);

	const TSharedRef<FJsonObject> OutputSchema = MakeShared<FJsonObject>();
	OutputSchema->SetStringField(TEXT("type"), TEXT("object"));
	OutputSchema->SetStringField(TEXT("envelope"), TEXT("{ \"data\": object|array, \"meta\"?: object }"));
	OutputSchema->SetStringField(TEXT("data"), TEXT("command-specific payload"));
	OutputSchema->SetStringField(TEXT("meta"), TEXT("command-specific metadata"));
	OutputSchema->SetStringField(TEXT("data_quality"), DataQualityToString(Entry.DataQuality));
	OutputSchema->SetBoolField(TEXT("may_be_empty"), Entry.bMayBeEmpty);

	Item->SetObjectField(TEXT("input_schema"), InputSchema);
	Item->SetObjectField(TEXT("output_schema_summary"), OutputSchema);
	return Item;
}

bool TryParseSchemaCommandFilter(int32 ArgC, TCHAR* ArgV[], FString& OutGroup, FString& OutAction, UE::InsightCli::FInsightCliResponse& OutError)
{
	OutGroup.Reset();
	OutAction.Reset();

	if (ArgC == 2)
	{
		return true;
	}

	if (ArgC != 4 || FCString::Stricmp(ArgV[2], TEXT("--command")) != 0)
	{
		OutError = UE::InsightCli::Internal::MakeOptionError(TEXT("schema usage: insight-cli schema [--command \"<group> <action>\"]"));
		return false;
	}

	FString CommandText = ArgV[3];
	CommandText.TrimStartAndEndInline();
	FString Left;
	FString Right;
	if (!CommandText.Split(TEXT(" "), &Left, &Right))
	{
		OutError = UE::InsightCli::Internal::MakeOptionError(TEXT("--command must be \"<group> <action>\"."));
		return false;
	}

	OutGroup = Left.TrimStartAndEnd();
	OutAction = Right.TrimStartAndEnd();
	if (OutGroup.IsEmpty() || OutAction.IsEmpty() || OutAction.Contains(TEXT(" ")))
	{
		OutError = UE::InsightCli::Internal::MakeOptionError(TEXT("--command must be \"<group> <action>\"."));
		return false;
	}

	return true;
}

UE::InsightCli::FInsightCliResponse MakeTopLevelSchemaResponse(int32 ArgC, TCHAR* ArgV[])
{
	FString FilterGroup;
	FString FilterAction;
	UE::InsightCli::FInsightCliResponse ParseError;
	if (!TryParseSchemaCommandFilter(ArgC, ArgV, FilterGroup, FilterAction, ParseError))
	{
		return ParseError;
	}

	TArray<TSharedPtr<FJsonValue>> Schemas;
	Schemas.Reserve(64);
	int32 MatchedCount = 0;
	UE::InsightCli::EnumerateCommandCatalog([&Schemas, &MatchedCount, &FilterGroup, &FilterAction](const UE::InsightCli::FInsightCliCommandCatalogEntry& Entry)
	{
		if (!FilterGroup.IsEmpty())
		{
			if (!Entry.Group.Equals(FilterGroup, ESearchCase::IgnoreCase) || !Entry.Action.Equals(FilterAction, ESearchCase::IgnoreCase))
			{
				return;
			}
		}

		Schemas.Add(MakeShared<FJsonValueObject>(MakeCommandSchemaEntry(Entry)));
		++MatchedCount;
	});

	if (!FilterGroup.IsEmpty() && MatchedCount == 0)
	{
		TMap<FString, FString> Details;
		Details.Add(TEXT("command"), FString::Printf(TEXT("%s %s"), *FilterGroup, *FilterAction));
		return UE::InsightCli::Internal::MakeNotFoundError(TEXT("schema command not found."), Details);
	}

	const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("$schema"), TEXT("https://json-schema.org/draft/2020-12/schema"));
	Data->SetArrayField(TEXT("commands"), Schemas);

	TMap<FString, FString> Meta;
	Meta.Add(TEXT("schema_type"), TEXT("command_catalog"));
	Meta.Add(TEXT("command_count"), FString::FromInt(MatchedCount));
	if (!FilterGroup.IsEmpty())
	{
		Meta.Add(TEXT("command"), FString::Printf(TEXT("%s %s"), *FilterGroup, *FilterAction));
	}

	return UE::InsightCli::FInsightCliResponse::Ok(UE::InsightCli::Internal::MakeEnvelopeWithObject(Data, Meta));
}

UE::InsightCli::FInsightCliResponse MakeTopLevelHelpResponse()
{
	TArray<TSharedPtr<FJsonValue>> Commands;
	int32 CommandCount = 0;
	UE::InsightCli::EnumerateCommandCatalog([&Commands, &CommandCount](const UE::InsightCli::FInsightCliCommandCatalogEntry& Entry)
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("group"), Entry.Group);
		Item->SetStringField(TEXT("action"), Entry.Action);

		TArray<TSharedPtr<FJsonValue>> RequiredOptions;
		RequiredOptions.Reserve(Entry.RequiredOptions.Num());
		for (const FString& Option : Entry.RequiredOptions)
		{
			RequiredOptions.Add(MakeShared<FJsonValueString>(Option));
		}

		TArray<TSharedPtr<FJsonValue>> OptionalOptions;
		OptionalOptions.Reserve(Entry.OptionalOptions.Num());
		for (const FString& Option : Entry.OptionalOptions)
		{
			OptionalOptions.Add(MakeShared<FJsonValueString>(Option));
		}

		Item->SetArrayField(TEXT("required_options"), RequiredOptions);
		Item->SetArrayField(TEXT("optional_options"), OptionalOptions);

		TArray<TSharedPtr<FJsonValue>> RequiredChannels;
		RequiredChannels.Reserve(Entry.RequiredChannels.Num());
		for (const FString& Channel : Entry.RequiredChannels)
		{
			RequiredChannels.Add(MakeShared<FJsonValueString>(Channel));
		}

		TArray<TSharedPtr<FJsonValue>> OptionalChannels;
		OptionalChannels.Reserve(Entry.OptionalChannels.Num());
		for (const FString& Channel : Entry.OptionalChannels)
		{
			OptionalChannels.Add(MakeShared<FJsonValueString>(Channel));
		}

		Item->SetArrayField(TEXT("required_channels"), RequiredChannels);
		Item->SetArrayField(TEXT("optional_channels"), OptionalChannels);
		Item->SetStringField(TEXT("data_quality"), DataQualityToString(Entry.DataQuality));
		Item->SetStringField(TEXT("quality_tag"), DataQualityHelpTag(Entry.DataQuality));
		Item->SetBoolField(TEXT("may_be_empty"), Entry.bMayBeEmpty);
		Commands.Add(MakeShared<FJsonValueObject>(Item));
		++CommandCount;
	});

	TMap<FString, FString> Meta;
	Meta.Add(TEXT("command_count"), FString::FromInt(CommandCount));
	Meta.Add(TEXT("source"), TEXT("command_catalog"));

	return UE::InsightCli::FInsightCliResponse::Ok(UE::InsightCli::Internal::MakeEnvelopeWithArray(Commands, Meta));
}

bool TryHandleTopLevelCommand(int32 ArgC, TCHAR* ArgV[], UE::InsightCli::FInsightCliResponse& OutResponse)
{
	if (ArgC != 2)
	{
		if (ArgC >= 2 && IsSchemaToken(ArgV[1]))
		{
			OutResponse = MakeTopLevelSchemaResponse(ArgC, ArgV);
			return true;
		}

		return false;
	}

	if (IsHelpToken(ArgV[1]))
	{
		OutResponse = MakeTopLevelHelpResponse();
		return true;
	}

	if (IsSchemaToken(ArgV[1]))
	{
		OutResponse = MakeTopLevelSchemaResponse(ArgC, ArgV);
		return true;
	}

	return false;
}

void WriteUtf8(FILE* Stream, const FString& Text)
{
	FTCHARToUTF8 Converted(*Text);
	fwrite(Converted.Get(), sizeof(UTF8CHAR), Converted.Length(), Stream);
	fputc('\n', Stream);
}

void WriteResponse(const UE::InsightCli::FInsightCliResponse& Response)
{
	if (!Response.StdOut.IsEmpty())
	{
		WriteUtf8(stdout, Response.StdOut);
	}
	if (!Response.StdErr.IsEmpty())
	{
		WriteUtf8(stderr, Response.StdErr);
	}
}

bool ParseRequestFromArgv(int32 ArgC, TCHAR* ArgV[], UE::InsightCli::FInsightCliRequest& OutRequest, UE::InsightCli::FInsightCliResponse& OutErrorResponse)
{
	if (ArgC < 4)
	{
		OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(InvalidFormatMessage);
		return false;
	}

	OutRequest.TracePath = ArgV[1];
	OutRequest.Group = ArgV[2];
	OutRequest.Action = ArgV[3];

	for (int32 Index = 4; Index < ArgC; ++Index)
	{
		OutRequest.Args.Add(ArgV[Index]);
	}

	return true;
}

bool TryReadBatchJsonText(const FString& Source, FString& OutText, UE::InsightCli::FInsightCliResponse& OutErrorResponse)
{
	if (Source == TEXT("-"))
	{
		std::string InputUtf8((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
		if (InputUtf8.empty())
		{
			OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Batch stdin input is empty."));
			return false;
		}

		OutText = UTF8_TO_TCHAR(InputUtf8.c_str());
		return true;
	}

	if (!FFileHelper::LoadFileToString(OutText, *Source))
	{
		TMap<FString, FString> Details;
		Details.Add(TEXT("batch_source"), Source);
		OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Failed to load batch command file."), Details);
		return false;
	}

	return true;
}

FString JsonValueToOptionValue(const TSharedPtr<FJsonValue>& Value)
{
	switch (Value->Type)
	{
	case EJson::String:
		return Value->AsString();
	case EJson::Number:
		return FString::Printf(TEXT("%.15g"), Value->AsNumber());
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	default:
		return FString();
	}
}

bool TryBuildArgsFromOptionsObject(
	const TSharedPtr<FJsonObject>& OptionsObject,
	TArray<FString>& OutArgs,
	UE::InsightCli::FInsightCliResponse& OutErrorResponse,
	const FString& CommandIndexText)
{
	if (!OptionsObject.IsValid())
	{
		return true;
	}

	TArray<FString> OptionNames;
	OptionsObject->Values.GetKeys(OptionNames);
	OptionNames.Sort();

	for (const FString& OptionName : OptionNames)
	{
		const TSharedPtr<FJsonValue>* ValuePtr = OptionsObject->Values.Find(OptionName);
		if (ValuePtr == nullptr || !ValuePtr->IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonValue>& Value = *ValuePtr;
		if (Value->Type == EJson::Boolean)
		{
			if (Value->AsBool())
			{
				OutArgs.Add(FString::Printf(TEXT("--%s"), *OptionName));
			}
			continue;
		}

		const FString OptionValue = JsonValueToOptionValue(Value);
		if (OptionValue.IsEmpty())
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("command_index"), CommandIndexText);
			Details.Add(TEXT("option"), OptionName);
			OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Batch option value must be string/number/boolean."), Details);
			return false;
		}

		OutArgs.Add(FString::Printf(TEXT("--%s"), *OptionName));
		OutArgs.Add(OptionValue);
	}

	return true;
}

bool ParseBatchRequests(const FString& TracePath, const FString& BatchJsonText, TArray<UE::InsightCli::FInsightCliRequest>& OutRequests, UE::InsightCli::FInsightCliResponse& OutErrorResponse)
{
	OutRequests.Reset();

	TSharedPtr<FJsonValue> RootValue;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BatchJsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootValue) || !RootValue.IsValid() || RootValue->Type != EJson::Array)
	{
		OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Batch JSON root must be an array of command objects."));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>& Commands = RootValue->AsArray();
	if (Commands.IsEmpty())
	{
		OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Batch command array is empty."));
		return false;
	}

	for (int32 Index = 0; Index < Commands.Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& CommandValue = Commands[Index];
		const TSharedPtr<FJsonObject> CommandObject = CommandValue.IsValid() ? CommandValue->AsObject() : nullptr;
		if (!CommandObject.IsValid())
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("command_index"), FString::FromInt(Index));
			OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Batch command must be an object."), Details);
			return false;
		}

		FString Group;
		FString Action;
		if (!CommandObject->TryGetStringField(TEXT("group"), Group) || !CommandObject->TryGetStringField(TEXT("action"), Action) || Group.IsEmpty() || Action.IsEmpty())
		{
			TMap<FString, FString> Details;
			Details.Add(TEXT("command_index"), FString::FromInt(Index));
			OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Batch command requires non-empty group and action."), Details);
			return false;
		}

		UE::InsightCli::FInsightCliRequest Request;
		Request.TracePath = TracePath;
		Request.Group = Group;
		Request.Action = Action;

		const TSharedPtr<FJsonValue>* OptionsValuePtr = CommandObject->Values.Find(TEXT("options"));
		if (OptionsValuePtr != nullptr && OptionsValuePtr->IsValid())
		{
			const TSharedPtr<FJsonObject> OptionsObject = (*OptionsValuePtr)->AsObject();
			if (!OptionsObject.IsValid())
			{
				TMap<FString, FString> Details;
				Details.Add(TEXT("command_index"), FString::FromInt(Index));
				OutErrorResponse = UE::InsightCli::Internal::MakeOptionError(TEXT("Batch command options must be an object."), Details);
				return false;
			}

			UE::InsightCli::FInsightCliResponse OptionError;
			if (!TryBuildArgsFromOptionsObject(OptionsObject, Request.Args, OptionError, FString::FromInt(Index)))
			{
				OutErrorResponse = OptionError;
				return false;
			}
		}

		OutRequests.Add(MoveTemp(Request));
	}

	return true;
}

FString MakeCondensedJsonLine(const FString& Envelope)
{
	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Envelope);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		FString Fallback = Envelope;
		Fallback.ReplaceInline(TEXT("\r"), TEXT(""));
		Fallback.ReplaceInline(TEXT("\n"), TEXT(" "));
		Fallback.ReplaceInline(TEXT("\t"), TEXT(" "));
		Fallback.TrimStartAndEndInline();
		return Fallback;
	}

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return Out;
}

FString EncodeCursorOffset(const int32 Offset)
{
	const FString Payload = FString::Printf(TEXT("v1:%d"), Offset);
	FTCHARToUTF8 Utf8Payload(*Payload);
	return FBase64::Encode(reinterpret_cast<const uint8*>(Utf8Payload.Get()), Utf8Payload.Length());
}

bool TryDecodeCursorOffset(const FString& Cursor, int32& OutOffset)
{
	OutOffset = 0;
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Cursor, Bytes))
	{
		return false;
	}

	Bytes.Add(0);
	const FString Decoded = UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()));

	FString Version;
	FString OffsetText;
	if (!Decoded.Split(TEXT(":"), &Version, &OffsetText) || Version != TEXT("v1"))
	{
		return false;
	}

	if (OffsetText.IsEmpty())
	{
		return false;
	}

	TCHAR* End = nullptr;
	const int64 Parsed = FCString::Strtoi64(*OffsetText, &End, 10);
	if (End == nullptr || *End != 0 || Parsed < 0 || Parsed > MAX_int32)
	{
		return false;
	}

	OutOffset = static_cast<int32>(Parsed);
	return true;
}

bool ApplyGlobalOutputOptionsToStdOut(
	FString& InOutStdOut,
	const UE::InsightCli::Internal::FGlobalOutputOptions& Options,
	UE::InsightCli::FInsightCliResponse& OutError)
{
	OutError = UE::InsightCli::FInsightCliResponse();

	if (!Options.HasAny() || InOutStdOut.IsEmpty())
	{
		return true;
	}

	FString EnvelopeJson = InOutStdOut;
	const int32 FirstBrace = EnvelopeJson.Find(TEXT("{"));
	const int32 LastBrace = EnvelopeJson.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (FirstBrace != INDEX_NONE && LastBrace != INDEX_NONE && LastBrace >= FirstBrace)
	{
		EnvelopeJson = EnvelopeJson.Mid(FirstBrace, LastBrace - FirstBrace + 1);
	}

	EnvelopeJson.ReplaceInline(TEXT(": inf"), TEXT(": null"), ESearchCase::CaseSensitive);
	EnvelopeJson.ReplaceInline(TEXT(": -inf"), TEXT(": null"), ESearchCase::CaseSensitive);
	EnvelopeJson.ReplaceInline(TEXT(": nan"), TEXT(": null"), ESearchCase::CaseSensitive);
	EnvelopeJson.ReplaceInline(TEXT(":inf"), TEXT(":null"), ESearchCase::CaseSensitive);
	EnvelopeJson.ReplaceInline(TEXT(":-inf"), TEXT(":null"), ESearchCase::CaseSensitive);
	EnvelopeJson.ReplaceInline(TEXT(":nan"), TEXT(":null"), ESearchCase::CaseSensitive);

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(EnvelopeJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	TSharedPtr<FJsonObject> MetaObject;
	const TSharedPtr<FJsonValue>* MetaValuePtr = Root->Values.Find(TEXT("meta"));
	if (MetaValuePtr != nullptr && MetaValuePtr->IsValid() && (*MetaValuePtr)->Type == EJson::Object)
	{
		MetaObject = (*MetaValuePtr)->AsObject();
	}
	if (!MetaObject.IsValid())
	{
		MetaObject = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("meta"), MetaObject.ToSharedRef());
	}

	auto ProjectObjectFields = [&Options](TSharedRef<FJsonObject> Object, TSet<FString>& OutFoundFields)
	{
		if (Options.Fields.IsEmpty())
		{
			return;
		}

		TMap<FString, TSharedPtr<FJsonValue>> ProjectedValues;
		for (const FString& Field : Options.Fields)
		{
			const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
			if (Value != nullptr)
			{
				ProjectedValues.Add(Field, *Value);
				OutFoundFields.Add(Field);
			}
		}
		Object->Values = MoveTemp(ProjectedValues);
	};

	const TSharedPtr<FJsonValue>* DataValuePtr = Root->Values.Find(TEXT("data"));
	if (DataValuePtr == nullptr || !DataValuePtr->IsValid())
	{
		return true;
	}

	TSet<FString> FoundFields;
	if (Options.Fields.Num() > 0)
	{
		if ((*DataValuePtr)->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> DataObject = (*DataValuePtr)->AsObject();
			if (DataObject.IsValid())
			{
				ProjectObjectFields(DataObject.ToSharedRef(), FoundFields);
			}
		}
		else if ((*DataValuePtr)->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> DataArray = (*DataValuePtr)->AsArray();
			for (TSharedPtr<FJsonValue>& RowValue : DataArray)
			{
				if (!RowValue.IsValid() || RowValue->Type != EJson::Object)
				{
					continue;
				}

				TSharedPtr<FJsonObject> RowObject = RowValue->AsObject();
				if (RowObject.IsValid())
				{
					ProjectObjectFields(RowObject.ToSharedRef(), FoundFields);
				}
			}
			Root->SetArrayField(TEXT("data"), DataArray);
		}

		TArray<TSharedPtr<FJsonValue>> MissingFields;
		for (const FString& RequestedField : Options.Fields)
		{
			if (!FoundFields.Contains(RequestedField))
			{
				MissingFields.Add(MakeShared<FJsonValueString>(RequestedField));
			}
		}
		if (MissingFields.Num() > 0)
		{
			MetaObject->SetArrayField(TEXT("fields_missing"), MissingFields);
		}
	}

	DataValuePtr = Root->Values.Find(TEXT("data"));
	if (Options.PageSize.IsSet() && DataValuePtr != nullptr && (*DataValuePtr)->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>> DataArray = (*DataValuePtr)->AsArray();
		const int32 RowCountTotal = DataArray.Num();
		int32 Offset = 0;

		if (Options.Cursor.IsSet() && !TryDecodeCursorOffset(Options.Cursor.GetValue(), Offset))
		{
			OutError = UE::InsightCli::Internal::MakeOptionError(TEXT("cursor is invalid or uses an unsupported schema version."));
			return false;
		}

		if (Offset < 0 || Offset > RowCountTotal)
		{
			OutError = UE::InsightCli::Internal::MakeOptionError(TEXT("cursor offset is out of bounds for this result set."));
			return false;
		}

		const int32 PageSize = Options.PageSize.GetValue();
		const int32 EndOffset = FMath::Min(Offset + PageSize, RowCountTotal);

		TArray<TSharedPtr<FJsonValue>> PageRows;
		PageRows.Reserve(FMath::Max(0, EndOffset - Offset));
		for (int32 RowIndex = Offset; RowIndex < EndOffset; ++RowIndex)
		{
			PageRows.Add(DataArray[RowIndex]);
		}

		Root->SetArrayField(TEXT("data"), PageRows);
		MetaObject->SetStringField(TEXT("cursor_schema"), TEXT("v1"));
		MetaObject->SetNumberField(TEXT("row_count_total"), RowCountTotal);
		MetaObject->SetNumberField(TEXT("row_count_returned"), PageRows.Num());

		if (EndOffset < RowCountTotal)
		{
			MetaObject->SetStringField(TEXT("next_cursor"), EncodeCursorOffset(EndOffset));
		}
		else
		{
			MetaObject->SetField(TEXT("next_cursor"), MakeShared<FJsonValueNull>());
		}
	}

	DataValuePtr = Root->Values.Find(TEXT("data"));
	if (Options.MaxRows.IsSet() && DataValuePtr != nullptr && (*DataValuePtr)->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>> DataArray = (*DataValuePtr)->AsArray();
		const int32 RowCountActual = DataArray.Num();
		const int32 MaxRows = Options.MaxRows.GetValue();
		if (RowCountActual > MaxRows)
		{
			TArray<TSharedPtr<FJsonValue>> TruncatedArray;
			TruncatedArray.Reserve(MaxRows);
			for (int32 Index = 0; Index < MaxRows; ++Index)
			{
				TruncatedArray.Add(DataArray[Index]);
			}
			Root->SetArrayField(TEXT("data"), TruncatedArray);
			MetaObject->SetBoolField(TEXT("truncated"), true);
			MetaObject->SetNumberField(TEXT("row_count_actual"), RowCountActual);
		}
	}

	FString Updated;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Updated);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	InOutStdOut = MoveTemp(Updated);
	return true;
}

UE::InsightCli::FInsightCliResponse RunBatchProgram(int32 ArgC, TCHAR* ArgV[])
{
	if (ArgC < 4 || FCString::Stricmp(ArgV[2], TEXT("--batch")) != 0)
	{
		return UE::InsightCli::Internal::MakeOptionError(InvalidBatchFormatMessage);
	}

	const FString TracePath = ArgV[1];
	const FString BatchSource = ArgV[3];

	FString BatchJsonText;
	UE::InsightCli::FInsightCliResponse BatchReadError;
	if (!TryReadBatchJsonText(BatchSource, BatchJsonText, BatchReadError))
	{
		return BatchReadError;
	}

	TArray<UE::InsightCli::FInsightCliRequest> Requests;
	UE::InsightCli::FInsightCliResponse ParseBatchError;
	if (!ParseBatchRequests(TracePath, BatchJsonText, Requests, ParseBatchError))
	{
		return ParseBatchError;
	}

	UE::InsightCli::FInsightCliRequest BootstrapRequest;
	BootstrapRequest.TracePath = TracePath;
	BootstrapRequest.Group = Requests[0].Group;
	BootstrapRequest.Action = Requests[0].Action;

	UE::InsightCli::Internal::FTraceContext SharedContext;
	const UE::InsightCli::FInsightCliResponse ValidationError = UE::InsightCli::Internal::ValidateTraceAndBuildContext(BootstrapRequest, SharedContext);
	if (ValidationError.ExitCode != 0)
	{
		return ValidationError;
	}

	FString StdOut;
	int32 MaxExitCode = 0;
	for (int32 Index = 0; Index < Requests.Num(); ++Index)
	{
		UE::InsightCli::FInsightCliRequest Request = Requests[Index];
		UE::InsightCli::Internal::FGlobalOutputOptions OutputOptions;
		UE::InsightCli::FInsightCliResponse OutputOptionError;
		if (!UE::InsightCli::Internal::TryExtractGlobalOutputOptions(Request.Args, OutputOptions, OutputOptionError))
		{
			return OutputOptionError;
		}

		UE::InsightCli::FInsightCliResponse CommandResponse = UE::InsightCli::ExecuteCommandWithSharedContext(Request, SharedContext);
		if (CommandResponse.ExitCode == 0)
		{
			UE::InsightCli::FInsightCliResponse OutputTransformError;
			if (!ApplyGlobalOutputOptionsToStdOut(CommandResponse.StdOut, OutputOptions, OutputTransformError) && OutputTransformError.ExitCode != 0)
			{
				CommandResponse = OutputTransformError;
			}
		}
		MaxExitCode = FMath::Max(MaxExitCode, CommandResponse.ExitCode);

		const FString Envelope = !CommandResponse.StdOut.IsEmpty() ? CommandResponse.StdOut : CommandResponse.StdErr;
		if (Index > 0)
		{
			StdOut += TEXT("\n");
		}
		StdOut += MakeCondensedJsonLine(Envelope);
	}

	UE::InsightCli::FInsightCliResponse BatchResponse;
	BatchResponse.ExitCode = MaxExitCode;
	BatchResponse.StdOut = StdOut;
	return BatchResponse;
}

UE::InsightCli::FInsightCliResponse RunProgram(int32 ArgC, TCHAR* ArgV[])
{
	UE::InsightCli::FInsightCliResponse TopLevelResponse;
	if (TryHandleTopLevelCommand(ArgC, ArgV, TopLevelResponse))
	{
		return TopLevelResponse;
	}

	if (ArgC >= 3 && FCString::Stricmp(ArgV[2], TEXT("--batch")) == 0)
	{
		return RunBatchProgram(ArgC, ArgV);
	}

	UE::InsightCli::FInsightCliRequest Request;
	UE::InsightCli::FInsightCliResponse ParseError;
	if (!ParseRequestFromArgv(ArgC, ArgV, Request, ParseError))
	{
		return ParseError;
	}

	UE::InsightCli::Internal::FGlobalOutputOptions OutputOptions;
	UE::InsightCli::FInsightCliResponse OutputOptionError;
	if (!UE::InsightCli::Internal::TryExtractGlobalOutputOptions(Request.Args, OutputOptions, OutputOptionError))
	{
		return OutputOptionError;
	}

	UE::InsightCli::FInsightCliResponse Response = UE::InsightCli::ExecuteCommand(Request);
	if (Response.ExitCode == 0)
	{
		UE::InsightCli::FInsightCliResponse OutputTransformError;
		if (!ApplyGlobalOutputOptionsToStdOut(Response.StdOut, OutputOptions, OutputTransformError) && OutputTransformError.ExitCode != 0)
		{
			return OutputTransformError;
		}
	}
	return Response;
}
}

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	if (int32 Ret = GEngineLoop.PreInit(ArgC, ArgV))
	{
		return Ret;
	}

	const UE::InsightCli::FInsightCliResponse Response = RunProgram(ArgC, ArgV);
	WriteResponse(Response);

	FEngineLoop::AppPreExit();
	FEngineLoop::AppExit();
	return Response.ExitCode;
}
