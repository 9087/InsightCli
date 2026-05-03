// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliTypes.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

namespace UE::InsightCli
{
namespace
{
FString SerializeJson(const TSharedRef<FJsonObject>& JsonObject)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonObject, Writer);
	return Output;
}

TSharedRef<FJsonObject> MakeObjectFromMap(const TMap<FString, FString>& InValues)
{
	const TSharedRef<FJsonObject> JsonObject = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : InValues)
	{
		JsonObject->SetStringField(Pair.Key, Pair.Value);
	}
	return JsonObject;
}
}

FInsightCliResponse FInsightCliResponse::Ok(const FString& InStdOut)
{
	FInsightCliResponse Response;
	Response.ExitCode = 0;
	Response.StdOut = InStdOut;
	return Response;
}

FInsightCliResponse FInsightCliResponse::Error(int32 InExitCode, const FString& InCode, const FString& InMessage, const TMap<FString, FString>& InDetails)
{
	FInsightCliResponse Response;
	Response.ExitCode = InExitCode;
	Response.StdErr = MakeErrorEnvelope(InCode, InMessage, InDetails);
	return Response;
}

FString MakeSuccessEnvelope(const TMap<FString, FString>& InData, const TMap<FString, FString>& InMeta)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("data"), MakeObjectFromMap(InData));
	if (!InMeta.IsEmpty())
	{
		Root->SetObjectField(TEXT("meta"), MakeObjectFromMap(InMeta));
	}
	return SerializeJson(Root);
}

FString MakeErrorEnvelope(const FString& InCode, const FString& InMessage, const TMap<FString, FString>& InDetails)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("code"), InCode);
	Root->SetStringField(TEXT("message"), InMessage);
	if (!InDetails.IsEmpty())
	{
		Root->SetObjectField(TEXT("details"), MakeObjectFromMap(InDetails));
	}
	return SerializeJson(Root);
}
}
