// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

namespace UE::InsightCli::Internal
{
namespace
{
bool TryResolveRhiWindow(
	const FInsightCliRequest& Request,
	const FTraceContext& Context,
	TOptional<double>& OutStartSec,
	TOptional<double>& OutEndSec,
	double& OutRhiThreadMs,
	TMap<FString, FString>& OutMeta,
	FInsightCliResponse& OutError)
{
	OutStartSec.Reset();
	OutEndSec.Reset();
	OutRhiThreadMs = 0.0;
	OutMeta.Reset();
	OutMeta.Add(TEXT("data_source"), TEXT("approx_cpu_gpu"));

	int32 FrameIndex = -1;
	if (TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
	{
		if (FrameIndex < 0)
		{
			OutError = MakeOptionError(TEXT("frame-index must be >= 0."));
			return false;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("rhi.summary")))
		{
			OutError = FrameGuardError;
			return false;
		}

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		const FFrameSample* Found = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
		{
			return Item.FrameIndex == FrameIndex;
		});

		if (Found == nullptr)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("data_source"), TEXT("approx_cpu_gpu"));
			OutError = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return false;
		}

		OutStartSec = Found->FrameStartMs / 1000.0;
		OutEndSec = Found->FrameEndMs / 1000.0;
		OutRhiThreadMs = FMath::Max(0.0, Found->RhiThreadMs);
		OutMeta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
	}

	return true;
}
}

bool HandleGpuCommands(const FInsightCliRequest& Request, const FTraceContext& Context, FInsightCliResponse& OutResponse)
{
	// Handles gpu/top, gpu/passes and gpu/pass-detail subcommands; returns false for non-gpu requests.
	if (Request.Group == TEXT("gpu") && Request.Action == TEXT("top"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		TArray<FGpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(Context, Samples, FailureStage, FailureReason))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gpu.top"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build gpu aggregation"),
				TEXT("Trace-backed GPU timing is unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Samples.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeGpuTopObject(Samples[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("gpu") && Request.Action == TEXT("passes"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("time-start"), TEXT("time-end") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		const bool bHasTimeStart = HasOption(Request.Args, TEXT("--time-start"));
		const bool bHasTimeEnd = HasOption(Request.Args, TEXT("--time-end"));

		int32 FrameIndex = -1;
		const bool bHasFrameIndex = TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex);
		if (bHasFrameIndex && FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}
		if (bHasFrameIndex && (bHasTimeStart || bHasTimeEnd))
		{
			OutResponse = MakeOptionError(TEXT("--frame-index cannot be combined with --time-start/--time-end."));
			return true;
		}

		TOptional<double> IntervalStartSec;
		TOptional<double> IntervalEndSec;
		TMap<FString, FString> Meta;
		Meta.Add(TEXT("data_source"), TEXT("trace"));

		if (bHasFrameIndex)
		{
			FInsightCliResponse FrameGuardError;
			if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("gpu.passes")))
			{
				OutResponse = FrameGuardError;
				return true;
			}

			const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
			const FFrameSample* Found = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
			{
				return Item.FrameIndex == FrameIndex;
			});

			if (Found == nullptr)
			{
				TMap<FString, FString> NotFoundMeta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
				NotFoundMeta.Add(TEXT("data_source"), TEXT("trace"));
				OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, NotFoundMeta));
				return true;
			}

			IntervalStartSec = Found->FrameStartMs / 1000.0;
			IntervalEndSec = Found->FrameEndMs / 1000.0;
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
		}
		else
		{
			FResolvedTimeWindowMs TimeWindow;
			FInsightCliResponse TimeWindowError;
			if (!TryResolveTimeWindowMs(Context, Request.Args, false, TimeWindow, TimeWindowError))
			{
				OutResponse = TimeWindowError;
				return true;
			}

			if (TimeWindow.StartMs.IsSet())
			{
				IntervalStartSec = TimeWindow.StartMs.GetValue() / 1000.0;
			}
			if (TimeWindow.EndMs.IsSet())
			{
				IntervalEndSec = TimeWindow.EndMs.GetValue() / 1000.0;
			}
			AppendTimeWindowMeta(TimeWindow, Meta);
		}

		TArray<FGpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(Context, Samples, FailureStage, FailureReason, IntervalStartSec, IntervalEndSec))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gpu.passes"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build gpu pass aggregation"),
				TEXT("Trace-backed GPU pass data is unavailable for this trace."));
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Samples.Num());
		for (const FGpuScopeSample& Sample : Samples)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeGpuPassesObject(Sample)));
		}

		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("gpu") && Request.Action == TEXT("pass-detail"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index"), TEXT("pass"), TEXT("limit") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 FrameIndex = -1;
		if (!TryGetIntOption(Request.Args, TEXT("--frame-index"), FrameIndex))
		{
			OutResponse = MakeOptionError(TEXT("--frame-index is required for gpu pass-detail."));
			return true;
		}
		if (FrameIndex < 0)
		{
			OutResponse = MakeOptionError(TEXT("frame-index must be >= 0."));
			return true;
		}

		FString PassName;
		FInsightCliResponse RequiredOptionError;
		if (!RequireStringOption(Request.Args, TEXT("--pass"), TEXT("gpu pass-detail"), PassName, RequiredOptionError))
		{
			OutResponse = RequiredOptionError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		FInsightCliResponse FrameGuardError;
		if (!EnsureTraceBackedFrameSamples(Context, FrameGuardError, TEXT("gpu.pass-detail")))
		{
			OutResponse = FrameGuardError;
			return true;
		}

		const TArray<FFrameSample> Frames = BuildFrameSamples(Context);
		const FFrameSample* Found = Frames.FindByPredicate([FrameIndex](const FFrameSample& Item)
		{
			return Item.FrameIndex == FrameIndex;
		});

		if (Found == nullptr)
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("frame-index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("limit"), FString::FromInt(Limit));
			Meta.Add(TEXT("query_pass"), PassName);
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		TArray<FGpuScopeSample> FrameGpuSamples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(
			Context,
			FrameGpuSamples,
			FailureStage,
			FailureReason,
			Found->FrameStartMs / 1000.0,
			Found->FrameEndMs / 1000.0))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("gpu.pass-detail"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build frame gpu aggregation"),
				TEXT("Trace-backed GPU timing is unavailable for this trace."),
				{{TEXT("frame_index"), FString::FromInt(FrameIndex)}});
			return true;
		}

		TArray<FGpuScopeSample> Matched;
		for (const FGpuScopeSample& Sample : FrameGpuSamples)
		{
			if (Sample.ScopeName.Equals(PassName, ESearchCase::IgnoreCase))
			{
				Matched.Add(Sample);
			}
		}

		if (Matched.IsEmpty())
		{
			TMap<FString, FString> Meta = MakeNotFoundMeta(Request, TEXT("not_found"), TEXT("pass"), PassName);
			Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
			Meta.Add(TEXT("limit"), FString::FromInt(Limit));
			Meta.Add(TEXT("data_source"), TEXT("trace"));
			OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray({}, Meta));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Matched.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			Data.Add(MakeShared<FJsonValueObject>(MakeGpuPassDetailObject(*Found, Matched[Index])));
		}

		TMap<FString, FString> Meta;
		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("frame_index"), FString::FromInt(FrameIndex));
		Meta.Add(TEXT("data_source"), TEXT("trace"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("rhi") && Request.Action == TEXT("summary"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("frame-index") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		TOptional<double> IntervalStartSec;
		TOptional<double> IntervalEndSec;
		double RhiThreadMs = 0.0;
		TMap<FString, FString> Meta;
		FInsightCliResponse ResolveError;
		if (!TryResolveRhiWindow(Request, Context, IntervalStartSec, IntervalEndSec, RhiThreadMs, Meta, ResolveError))
		{
			OutResponse = ResolveError;
			return true;
		}

		TArray<FGpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(Context, Samples, FailureStage, FailureReason, IntervalStartSec, IntervalEndSec))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("rhi.summary"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build rhi summary approximation"),
				TEXT("Trace-backed RHI summary is unavailable for this trace."));
			return true;
		}

		int64 DrawCalls = 0;
		for (const FGpuScopeSample& Sample : Samples)
		{
			DrawCalls += FMath::Max(0, Sample.CallCount);
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("draw_call_count"), static_cast<double>(DrawCalls));
		Data->SetNumberField(TEXT("primitive_count"), 0.0);
		Data->SetNumberField(TEXT("triangle_count"), 0.0);
		Data->SetNumberField(TEXT("rhi_thread_ms"), RhiThreadMs);

		Meta.Add(TEXT("approximation"), TEXT("draw_calls_from_gpu_passes; rhi_thread_ms_from_frame_samples"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithObject(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("rhi") && Request.Action == TEXT("drawcalls"))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("frame-index") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 100;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 100, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		TOptional<double> IntervalStartSec;
		TOptional<double> IntervalEndSec;
		double IgnoredRhiThreadMs = 0.0;
		TMap<FString, FString> Meta;
		FInsightCliResponse ResolveError;
		if (!TryResolveRhiWindow(Request, Context, IntervalStartSec, IntervalEndSec, IgnoredRhiThreadMs, Meta, ResolveError))
		{
			OutResponse = ResolveError;
			return true;
		}

		TArray<FGpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(Context, Samples, FailureStage, FailureReason, IntervalStartSec, IntervalEndSec))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				TEXT("rhi.drawcalls"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build rhi drawcalls approximation"),
				TEXT("Trace-backed RHI drawcall data is unavailable for this trace."));
			return true;
		}

		const int32 TakeCount = FMath::Min(Limit, Samples.Num());
		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(TakeCount);
		for (int32 Index = 0; Index < TakeCount; ++Index)
		{
			const FGpuScopeSample& Sample = Samples[Index];
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("render_target"), Sample.ScopeName);
			Item->SetStringField(TEXT("material"), TEXT("unknown"));
			Item->SetStringField(TEXT("mesh"), TEXT("unknown"));
			Item->SetNumberField(TEXT("draw_call_count"), Sample.CallCount);
			Item->SetNumberField(TEXT("gpu_ms"), Sample.TotalMs);
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("approximation"), TEXT("draw_calls_from_gpu_passes"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	if (Request.Group == TEXT("rhi") && (Request.Action == TEXT("top-materials") || Request.Action == TEXT("top-meshes")))
	{
		FInsightCliResponse UnknownOptionError;
		if (!ValidateNoUnknownOptionsWithGlobals(Request.Args, { TEXT("limit"), TEXT("frame-index") }, UnknownOptionError))
		{
			OutResponse = UnknownOptionError;
			return true;
		}

		int32 Limit = 20;
		FInsightCliResponse LimitError;
		if (!TryGetPositiveLimit(Request.Args, 20, Limit, LimitError))
		{
			OutResponse = LimitError;
			return true;
		}

		TOptional<double> IntervalStartSec;
		TOptional<double> IntervalEndSec;
		double IgnoredRhiThreadMs = 0.0;
		TMap<FString, FString> Meta;
		FInsightCliResponse ResolveError;
		if (!TryResolveRhiWindow(Request, Context, IntervalStartSec, IntervalEndSec, IgnoredRhiThreadMs, Meta, ResolveError))
		{
			OutResponse = ResolveError;
			return true;
		}

		TArray<FGpuScopeSample> Samples;
		FString FailureStage;
		FString FailureReason;
		if (!BuildGpuTopSamples(Context, Samples, FailureStage, FailureReason, IntervalStartSec, IntervalEndSec))
		{
			OutResponse = MakeTraceUnavailableError(
				Context,
				Request.Action == TEXT("top-materials") ? TEXT("rhi.top-materials") : TEXT("rhi.top-meshes"),
				FailureStage,
				FailureReason,
				TEXT("aggregation"),
				TEXT("failed to build rhi top aggregation"),
				TEXT("Trace-backed RHI top material/mesh data is unavailable for this trace."));
			return true;
		}

		int64 TotalDrawCalls = 0;
		for (const FGpuScopeSample& Sample : Samples)
		{
			TotalDrawCalls += FMath::Max(0, Sample.CallCount);
		}

		TArray<TSharedPtr<FJsonValue>> Data;
		if (Limit > 0)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), TEXT("unknown"));
			Item->SetNumberField(TEXT("draw_call_count"), static_cast<double>(TotalDrawCalls));
			Data.Add(MakeShared<FJsonValueObject>(Item));
		}

		Meta.Add(TEXT("limit"), FString::FromInt(Limit));
		Meta.Add(TEXT("approximation"), TEXT("material/mesh channels unavailable; using unknown bucket from gpu passes"));
		OutResponse = FInsightCliResponse::Ok(MakeEnvelopeWithArray(Data, Meta));
		return true;
	}

	return false;
}
}
