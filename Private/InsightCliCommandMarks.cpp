// Copyright Epic Games, Inc. All Rights Reserved.

#include "InsightCliCommandContext.h"

#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/Log.h"

namespace UE::InsightCli::Internal
{
bool BuildMarks(
	const FTraceContext& Context,
	TArray<FMarkSample>& OutMarks,
	FString& OutFailureStage,
	FString& OutFailureReason,
	TOptional<double> WindowStartMs,
	TOptional<double> WindowEndMs)
{
	OutMarks.Reset();
	OutFailureStage.Reset();
	OutFailureReason.Reset();

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	if (!AcquireAnalysisSession(Context, Session, OutFailureStage, OutFailureReason))
	{
		return false;
	}

	double DurationSec = 0.0;
	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());
		DurationSec = Session->GetDurationSeconds();
	}
	const double StartSec = WindowStartMs.IsSet() ? FMath::Max(0.0, WindowStartMs.GetValue() / 1000.0) : 0.0;
	const double EndSec = WindowEndMs.IsSet() ? FMath::Max(StartSec, WindowEndMs.GetValue() / 1000.0) : DurationSec;

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());

		const TraceServices::IBookmarkProvider& BookmarkProvider = TraceServices::ReadBookmarkProvider(*Session.Get());
		BookmarkProvider.EnumerateBookmarks(StartSec, EndSec, [&OutMarks](const TraceServices::FBookmark& Bookmark)
		{
			FMarkSample Mark;
			Mark.TimestampMs = Bookmark.Time * 1000.0;
			Mark.Category = TEXT("bookmark");
			Mark.Channel = TEXT("Bookmark");
			Mark.Message = Bookmark.Text != nullptr ? Bookmark.Text : TEXT("");
			Mark.ThreadId = -1;
			OutMarks.Add(MoveTemp(Mark));
		});

		const TraceServices::ILogProvider& LogProvider = TraceServices::ReadLogProvider(*Session.Get());
		LogProvider.EnumerateMessages(StartSec, EndSec, [&OutMarks](const TraceServices::FLogMessageInfo& Message)
		{
			FMarkSample Mark;
			Mark.TimestampMs = Message.Time * 1000.0;
			Mark.Category = TEXT("log");
			Mark.Channel = (Message.Category != nullptr && Message.Category->Name != nullptr) ? Message.Category->Name : TEXT("Log");
			Mark.Message = Message.Message != nullptr ? Message.Message : TEXT("");
			Mark.ThreadId = -1;
			OutMarks.Add(MoveTemp(Mark));
		});
	}

	OutMarks.Sort([](const FMarkSample& A, const FMarkSample& B)
	{
		if (A.TimestampMs == B.TimestampMs)
		{
			if (A.Category == B.Category)
			{
				return A.Message < B.Message;
			}
			return A.Category < B.Category;
		}
		return A.TimestampMs < B.TimestampMs;
	});

	return true;
}

TSharedRef<FJsonObject> MakeMarkObject(const FMarkSample& Mark)
{
	const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("timestamp_ms"), Mark.TimestampMs);
	Item->SetStringField(TEXT("category"), Mark.Category);
	Item->SetStringField(TEXT("channel"), Mark.Channel);
	Item->SetStringField(TEXT("message"), Mark.Message);
	Item->SetNumberField(TEXT("thread_id"), Mark.ThreadId);
	return Item;
}
}
