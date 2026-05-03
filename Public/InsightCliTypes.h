// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UE::InsightCli
{
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
