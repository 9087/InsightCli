// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
bool HandleSymbolsCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles symbols/resolve with data-driven matching behavior.
	if (Request.Group == TEXT("symbols") && Request.Action == TEXT("resolve"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("name") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		FString ScopeName;
		FInsightCliResponse RequiredOptionError;
		if (!RequireStringOption(Request.Args, TEXT("--name"), TEXT("symbols resolve"), ScopeName, RequiredOptionError))
		{
			OutResponse = RequiredOptionError;
			return true;
		}

		TSharedPtr<FJsonObject> DataObject;
		bool bFound = false;
		FString ResolveReason;
		TMap<FString, FString> ResolveMeta;
		FString FailureStage;
		FString FailureReason;
		if (!BuildSymbolsResolveObject(Context, ScopeName, DataObject, bFound, ResolveReason, ResolveMeta, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("symbols.resolve"),
				FailureStage,
				FailureReason,
				TEXT("symbol_lookup"),
				TEXT("failed to resolve symbols from trace"),
				TEXT("Trace-backed symbol resolution is unavailable for this trace."));
			return true;
		}

		if (!bFound)
		{
			const FString NotFoundReason = ResolveReason.IsEmpty() ? TEXT("not_found") : ResolveReason;
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, NotFoundReason, TEXT("name"), ScopeName);
			Meta.Add(TEXT("data_source"), TEXT("trace"));
			for (const TPair<FString, FString>& Pair : ResolveMeta)
			{
				Meta.Add(Pair.Key, Pair.Value);
			}
			const TSharedRef<FJsonObject> Data = DataObject.IsValid() ? DataObject.ToSharedRef() : MakeShared<FJsonObject>();
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
			return true;
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		for (const TPair<FString, FString>& Pair : ResolveMeta)
		{
			Meta.Add(Pair.Key, Pair.Value);
		}
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(DataObject.ToSharedRef(), Meta));
		return true;
	}

	return false;
}
}
