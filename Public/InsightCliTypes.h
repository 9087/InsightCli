// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UE::InsightCli
{
enum class EInsightCliTraceUnavailableSubcode : uint8
{
	ChannelDisabled,
	ProviderUnavailable,
	FrameRangeOutOfBounds,
	EntityNotFound,
	AnalysisTimeout,
	TraceCorrupted,
	ThreadTaskMissing,
	IncompatibleTraceVersion,
	Unknown,
};

enum class EInsightCliFailureStage : uint8
{
	Unknown,
	ModuleLoad,
	AnalysisService,
	StartAnalysis,
	AnalysisSession,
	FrameProvider,
	ThreadProvider,
	TimingProvider,
	TasksProvider,
	ContextSwitchesProvider,
	Aggregation,
	ThreadLookup,
	CommandDispatch,
};

const TCHAR* LexToString(EInsightCliTraceUnavailableSubcode Subcode);
const TCHAR* LexToString(EInsightCliFailureStage Stage);
EInsightCliFailureStage ParseFailureStage(const FString& StageText);

struct FInsightCliRequest
{
	FString TracePath;
	FString Group;
	FString Action;
	TArray<FString> Args;
};

struct FInsightCliResponse
{
	int32 ExitCode = 0;
	FString StdOut;
	FString StdErr;

	static FInsightCliResponse Ok(const FString& InStdOut);
	static FInsightCliResponse Error(int32 InExitCode, const FString& InCode, const FString& InMessage, const TMap<FString, FString>& InDetails = {});
};

FString MakeErrorEnvelope(const FString& InCode, const FString& InMessage, const TMap<FString, FString>& InDetails = {});
}
