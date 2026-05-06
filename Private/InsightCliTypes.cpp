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

const TCHAR* LexToString(EInsightCliTraceUnavailableSubcode Subcode)
{
	switch (Subcode)
	{
	case EInsightCliTraceUnavailableSubcode::ChannelDisabled:
		return TEXT("E3001");
	case EInsightCliTraceUnavailableSubcode::ProviderUnavailable:
		return TEXT("E3002");
	case EInsightCliTraceUnavailableSubcode::FrameRangeOutOfBounds:
		return TEXT("E3003");
	case EInsightCliTraceUnavailableSubcode::EntityNotFound:
		return TEXT("E3004");
	case EInsightCliTraceUnavailableSubcode::AnalysisTimeout:
		return TEXT("E3005");
	case EInsightCliTraceUnavailableSubcode::TraceCorrupted:
		return TEXT("E3006");
	case EInsightCliTraceUnavailableSubcode::ThreadTaskMissing:
		return TEXT("E3007");
	case EInsightCliTraceUnavailableSubcode::IncompatibleTraceVersion:
		return TEXT("E3008");
	case EInsightCliTraceUnavailableSubcode::Unknown:
	default:
		return TEXT("E3000");
	}
}

const TCHAR* LexToString(EInsightCliFailureStage Stage)
{
	switch (Stage)
	{
	case EInsightCliFailureStage::ModuleLoad:
		return TEXT("module_load");
	case EInsightCliFailureStage::AnalysisService:
		return TEXT("analysis_service");
	case EInsightCliFailureStage::StartAnalysis:
		return TEXT("start_analysis");
	case EInsightCliFailureStage::AnalysisSession:
		return TEXT("analysis_session");
	case EInsightCliFailureStage::FrameProvider:
		return TEXT("frame_provider");
	case EInsightCliFailureStage::ThreadProvider:
		return TEXT("thread_provider");
	case EInsightCliFailureStage::TimingProvider:
		return TEXT("timing_provider");
	case EInsightCliFailureStage::TasksProvider:
		return TEXT("tasks_provider");
	case EInsightCliFailureStage::ContextSwitchesProvider:
		return TEXT("context_switches_provider");
	case EInsightCliFailureStage::Aggregation:
		return TEXT("aggregation");
	case EInsightCliFailureStage::ThreadLookup:
		return TEXT("thread_lookup");
	case EInsightCliFailureStage::CommandDispatch:
		return TEXT("command_dispatch");
	case EInsightCliFailureStage::Unknown:
	default:
		return TEXT("unknown");
	}
}

EInsightCliFailureStage ParseFailureStage(const FString& StageText)
{
	const FString Normalized = StageText.ToLower();
	if (Normalized == TEXT("module_load"))
	{
		return EInsightCliFailureStage::ModuleLoad;
	}
	if (Normalized == TEXT("analysis_service"))
	{
		return EInsightCliFailureStage::AnalysisService;
	}
	if (Normalized == TEXT("start_analysis"))
	{
		return EInsightCliFailureStage::StartAnalysis;
	}
	if (Normalized == TEXT("analysis_session"))
	{
		return EInsightCliFailureStage::AnalysisSession;
	}
	if (Normalized == TEXT("frame_provider"))
	{
		return EInsightCliFailureStage::FrameProvider;
	}
	if (Normalized == TEXT("thread_provider"))
	{
		return EInsightCliFailureStage::ThreadProvider;
	}
	if (Normalized == TEXT("timing_provider"))
	{
		return EInsightCliFailureStage::TimingProvider;
	}
	if (Normalized == TEXT("tasks_provider"))
	{
		return EInsightCliFailureStage::TasksProvider;
	}
	if (Normalized == TEXT("context_switches_provider"))
	{
		return EInsightCliFailureStage::ContextSwitchesProvider;
	}
	if (Normalized == TEXT("aggregation"))
	{
		return EInsightCliFailureStage::Aggregation;
	}
	if (Normalized == TEXT("thread_lookup"))
	{
		return EInsightCliFailureStage::ThreadLookup;
	}
	if (Normalized == TEXT("command_dispatch"))
	{
		return EInsightCliFailureStage::CommandDispatch;
	}

	return EInsightCliFailureStage::Unknown;
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
