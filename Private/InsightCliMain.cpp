// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandRegistry.h"
#include "InsightCliCommandContext.h"
#include "InsightCliTypes.h"
#include "RequiredProgramMainCPPInclude.h"

#include <cstdio>

IMPLEMENT_APPLICATION(InsightCli, "InsightCli");

namespace
{
constexpr TCHAR InvalidFormatMessage[] = TEXT("Invalid command format. Expected: insight-cli <trace_path> <group> <action> [options]");

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

UE::InsightCli::FInsightCliResponse RunProgram(int32 ArgC, TCHAR* ArgV[])
{
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
