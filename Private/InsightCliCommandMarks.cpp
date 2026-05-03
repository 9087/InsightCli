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
	const FMarksFilter& Filter,
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

	const auto MatchesFilter = [&Filter](const FString& Category, const FString& Channel, int32 ThreadId, const FString& Message) -> bool
	{
		if (Filter.Category.IsSet() && !Category.Equals(Filter.Category.GetValue(), ESearchCase::IgnoreCase))
		{
			return false;
		}

		if (Filter.Channel.IsSet() && !Channel.Equals(Filter.Channel.GetValue(), ESearchCase::IgnoreCase))
		{
			return false;
		}

		if (Filter.ThreadId.IsSet() && ThreadId != Filter.ThreadId.GetValue())
		{
			return false;
		}

		if (Filter.Keyword.IsSet())
		{
			const ESearchCase::Type CaseMode = Filter.bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
			if (Filter.bExact)
			{
				return Message.Equals(Filter.Keyword.GetValue(), CaseMode);
			}

			return Message.Contains(Filter.Keyword.GetValue(), CaseMode);
		}

		return true;
	};

	{
		TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session.Get());

		const TraceServices::IBookmarkProvider& BookmarkProvider = TraceServices::ReadBookmarkProvider(*Session.Get());
		BookmarkProvider.EnumerateBookmarks(StartSec, EndSec, [&OutMarks, &MatchesFilter](const TraceServices::FBookmark& Bookmark)
		{
			const FString Category = TEXT("bookmark");
			const FString Channel = TEXT("Bookmark");
			const int32 ThreadId = -1;
			const FString Message = Bookmark.Text != nullptr ? Bookmark.Text : TEXT("");
			if (!MatchesFilter(Category, Channel, ThreadId, Message))
			{
				return;
			}

			FMarkSample Mark;
			Mark.TimestampMs = Bookmark.Time * 1000.0;
			Mark.Category = Category;
			Mark.Channel = Channel;
			Mark.Message = Message;
			Mark.ThreadId = ThreadId;
			OutMarks.Add(MoveTemp(Mark));
		});

		const TraceServices::ILogProvider& LogProvider = TraceServices::ReadLogProvider(*Session.Get());
		LogProvider.EnumerateMessages(StartSec, EndSec, [&OutMarks, &MatchesFilter](const TraceServices::FLogMessageInfo& MessageInfo)
		{
			const FString Category = TEXT("log");
			const FString Channel = (MessageInfo.Category != nullptr && MessageInfo.Category->Name != nullptr) ? MessageInfo.Category->Name : TEXT("Log");
			const int32 ThreadId = -1;
			const FString Message = MessageInfo.Message != nullptr ? MessageInfo.Message : TEXT("");
			if (!MatchesFilter(Category, Channel, ThreadId, Message))
			{
				return;
			}

			FMarkSample Mark;
			Mark.TimestampMs = MessageInfo.Time * 1000.0;
			Mark.Category = Category;
			Mark.Channel = Channel;
			Mark.Message = Message;
			Mark.ThreadId = ThreadId;
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
