// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "Containers/UnrealString.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"

#if PLATFORM_WINDOWS
#include <excpt.h>
#endif

namespace UE::InsightCli::Internal
{
namespace
{
FString GetTraceExtension(const FString& InPath)
{
	FString Ext = FPaths::GetExtension(InPath, true);
	Ext = Ext.ToLower();
	return Ext;
}

TSharedPtr<const TraceServices::IAnalysisSession> StartAnalysisSafely(
	const TSharedPtr<TraceServices::IAnalysisService>& AnalysisService,
	const FString& TracePath,
	uint32& OutSehCode)
{
	OutSehCode = 0;

#if PLATFORM_WINDOWS
	__try
	{
		return AnalysisService->StartAnalysis(*TracePath);
	}
	__except ((OutSehCode = static_cast<uint32>(_exception_code())), 1)
	{
		return nullptr;
	}
#else
	return AnalysisService->StartAnalysis(*TracePath);
#endif
}

bool OpenAnalysisSession(
	const FString& TracePath,
	TSharedPtr<TraceServices::IAnalysisService>& OutService,
	TSharedPtr<const TraceServices::IAnalysisSession>& OutSession,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutService.Reset();
	OutSession.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	ITraceServicesModule* TraceServicesModule = FModuleManager::LoadModulePtr<ITraceServicesModule>(TEXT("TraceServices"));
	if (TraceServicesModule == nullptr)
	{
		OutFailureStage = TEXT("module_load");
		OutFailureReason = TEXT("TraceServices module not available");
		return false;
	}

	OutService = TraceServicesModule->GetAnalysisService();
	if (!OutService.IsValid())
	{
		OutService = TraceServicesModule->CreateAnalysisService();
	}

	if (!OutService.IsValid())
	{
		OutFailureStage = TEXT("analysis_service");
		OutFailureReason = TEXT("analysis service unavailable");
		OutSession.Reset();
		return false;
	}

	uint32 StartAnalysisSehCode = 0;
	OutSession = StartAnalysisSafely(OutService, TracePath, StartAnalysisSehCode);
	if (!OutSession.IsValid())
	{
		OutFailureStage = TEXT("start_analysis");
		OutFailureReason = (StartAnalysisSehCode != 0)
			? FString::Printf(TEXT("StartAnalysis raised SEH 0x%08X"), StartAnalysisSehCode)
			: TEXT("StartAnalysis returned null session");
		OutService.Reset();
		return false;
	}

	OutSession->Wait();
	return true;
}
FString SerializeJson(const TSharedRef<FJsonObject>& Root)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

}

FString ToNumberString(double Value)
{
	return FString::Printf(TEXT("%.3f"), Value);
}

TMap<FString, FString> MakeNotFoundMeta(const FInsightCliRequest& Request, const FString& Reason, const FString& QueryKey, const FString& QueryValue)
{
	TMap<FString, FString> Meta;
	Meta.Add(TEXT("found"), TEXT("false"));
	Meta.Add(TEXT("reason"), Reason);
	Meta.Add(TEXT("request_group"), Request.Group);
	Meta.Add(TEXT("request_action"), Request.Action);

	if (!QueryKey.IsEmpty())
	{
		Meta.Add(TEXT("query_key"), QueryKey);
	}
	if (!QueryValue.IsEmpty())
	{
		Meta.Add(TEXT("query_value"), QueryValue);
	}

	return Meta;
}

FInsightCliResponse MakeOptionError(const FString& Message, const TMap<FString, FString>& Details)
{
	return FInsightCliResponse::Error(4, TEXT("E1003"), Message, Details);
}

FInsightCliResponse MakeNotFoundError(const FString& Message, const TMap<FString, FString>& Details)
{
	return FInsightCliResponse::Error(5, TEXT("E2001"), Message, Details);
}

FInsightCliResponse MakeTraceUnavailableError(
	const FTraceContext& Context,
	const TCHAR* Consumer,
	const FString& FailureStage,
	const FString& FailureReason,
	const TCHAR* DefaultStage,
	const TCHAR* DefaultReason,
	const TCHAR* Message,
	const TMap<FString, FString>& ExtraDetails)
{
	TMap<FString, FString> Details;
	Details.Add(TEXT("trace_path"), Context.FullPath);
	Details.Add(TEXT("consumer"), Consumer);
	Details.Add(TEXT("failure_stage"), FailureStage.IsEmpty() ? DefaultStage : FailureStage);
	Details.Add(TEXT("failure_reason"), FailureReason.IsEmpty() ? DefaultReason : FailureReason);
	Details.Add(TEXT("data_source"), TEXT("unavailable"));

	for (const TPair<FString, FString>& Detail : ExtraDetails)
	{
		Details.Add(Detail.Key, Detail.Value);
	}

	return FInsightCliResponse::Error(10, TEXT("E3001"), Message, Details);
}

FInsightCliResponse ValidateTraceAndBuildContext(const FInsightCliRequest& Request, FTraceContext& OutContext)
{
	if (Request.TracePath.IsEmpty())
	{
		return MakeOptionError(TEXT("trace_path is required."));
	}

	OutContext.FullPath = FPaths::ConvertRelativePathToFull(Request.TracePath);
	if (!IFileManager::Get().FileExists(*OutContext.FullPath))
	{
		TMap<FString, FString> Details;
		Details.Add(TEXT("trace_path"), OutContext.FullPath);
		return FInsightCliResponse::Error(2, TEXT("E1001"), TEXT("Input trace file does not exist."), Details);
	}

	const FString Extension = GetTraceExtension(OutContext.FullPath);
	if (Extension != TEXT(".utrace") && Extension != TEXT(".trace"))
	{
		TMap<FString, FString> Details;
		Details.Add(TEXT("trace_path"), OutContext.FullPath);
		Details.Add(TEXT("extension"), Extension);
		return FInsightCliResponse::Error(3, TEXT("E1002"), TEXT("Unsupported trace file extension. Expected .utrace or .trace."), Details);
	}

	OutContext.FileSize = IFileManager::Get().FileSize(*OutContext.FullPath);
	OutContext.TimeStamp = IFileManager::Get().GetTimeStamp(*OutContext.FullPath);
	return FInsightCliResponse();
}

bool AcquireAnalysisSession(
	const FTraceContext& Context,
	TSharedPtr<const TraceServices::IAnalysisSession>& OutSession,
	FString& OutFailureStage,
	FString& OutFailureReason)
{
	OutSession.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	if (Context.CachedAnalysisSession.IsValid())
	{
		OutSession = Context.CachedAnalysisSession;
		return true;
	}

	if (Context.bAnalysisAttempted)
	{
		OutFailureStage = Context.AnalysisFailureStage;
		OutFailureReason = Context.AnalysisFailureReason;
		return false;
	}

	Context.bAnalysisAttempted = true;
	Context.AnalysisFailureStage.Reset();
	Context.AnalysisFailureReason.Reset();

	if (!OpenAnalysisSession(
		Context.FullPath,
		Context.CachedAnalysisService,
		Context.CachedAnalysisSession,
		Context.AnalysisFailureStage,
		Context.AnalysisFailureReason))
	{
		OutFailureStage = Context.AnalysisFailureStage;
		OutFailureReason = Context.AnalysisFailureReason;
		return false;
	}

	OutSession = Context.CachedAnalysisSession;
	return true;
}

FString MakeEnvelopeWithObject(const TSharedRef<FJsonObject>& Data, const TMap<FString, FString>& Meta)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("data"), Data);
	if (!Meta.IsEmpty())
	{
		const TSharedRef<FJsonObject> MetaObject = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Meta)
		{
			MetaObject->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("meta"), MetaObject);
	}
	return SerializeJson(Root);
}

FString MakeEnvelopeWithObjectAndMeta(const TSharedRef<FJsonObject>& Data, const TSharedPtr<FJsonObject>& MetaObject)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("data"), Data);
	if (MetaObject.IsValid() && MetaObject->Values.Num() > 0)
	{
		Root->SetObjectField(TEXT("meta"), MetaObject.ToSharedRef());
	}
	return SerializeJson(Root);
}

FString MakeEnvelopeWithArray(const TArray<TSharedPtr<FJsonValue>>& Data, const TMap<FString, FString>& Meta)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("data"), Data);
	if (!Meta.IsEmpty())
	{
		const TSharedRef<FJsonObject> MetaObject = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Meta)
		{
			MetaObject->SetStringField(Pair.Key, Pair.Value);
		}
		Root->SetObjectField(TEXT("meta"), MetaObject);
	}
	return SerializeJson(Root);
}
}
