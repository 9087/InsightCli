// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandRegistry.h"
#include "InsightCliCommandContext.h"
#include "InsightCliTypes.h"
#include "RequiredProgramMainCPPInclude.h"

#include "Dom/JsonObject.h"
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
		const UE::InsightCli::FInsightCliResponse CommandResponse = UE::InsightCli::ExecuteCommandWithSharedContext(Requests[Index], SharedContext);
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

	return UE::InsightCli::ExecuteCommand(Request);
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
