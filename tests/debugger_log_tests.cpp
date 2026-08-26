// SPDX-FileCopyrightText:  2025 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dosbox.h"

#include <gtest/gtest.h>

#if C_DEBUGGER

#include <string>
#include <string_view>

#include "debugger/debugger.h"

namespace {

// DBGUI_TrimTrailingNewlines feeds every log sink in a debugger build: loguru
// appends its own line ending, the Output window and log file store lines with
// an explicit '\n'. Getting the trim wrong either doubles blank lines in the
// debugger UI or splits one message across two stderr records.

TEST(DebuggerLogTrim, LeavesLineWithoutTrailingNewlineAlone)
{
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("WEBSERVER: listening"),
	          "WEBSERVER: listening");
}

TEST(DebuggerLogTrim, StripsSingleTrailingNewline)
{
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("MIXER: ready\n"), "MIXER: ready");
}

TEST(DebuggerLogTrim, StripsCarriageReturnNewlinePair)
{
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("DOS: booted\r\n"), "DOS: booted");
}

TEST(DebuggerLogTrim, StripsRepeatedTrailingNewlines)
{
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("CPU: halted\n\n\r\n\n"), "CPU: halted");
}

TEST(DebuggerLogTrim, KeepsNewlinesInsideTheMessage)
{
	// A multi-line message stays one log record; only the tail is trimmed.
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("first\nsecond\n"), "first\nsecond");
}

TEST(DebuggerLogTrim, EmptyInputStaysEmpty)
{
	EXPECT_TRUE(DBGUI_TrimTrailingNewlines("").empty());
}

TEST(DebuggerLogTrim, NewlineOnlyInputCollapsesToEmpty)
{
	// find_last_not_of returns npos here; the guard must not underflow into
	// a huge substring length.
	EXPECT_TRUE(DBGUI_TrimTrailingNewlines("\n").empty());
	EXPECT_TRUE(DBGUI_TrimTrailingNewlines("\r\n\r\n").empty());
}

TEST(DebuggerLogTrim, DoesNotStripOtherWhitespaceOrControlBytes)
{
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("padded  \t"), "padded  \t");
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("bell\a"), "bell\a");
}

// Log messages carry guest-controlled text: DOS file names, mount paths and
// API request data all reach LOG_MSG. The trim must treat them as data, and
// callers must pass the result as a printf argument rather than as a format
// string. These cases pin the data half down.

TEST(DebuggerLogTrim, PreservesPrintfConversionsInTheMessage)
{
	EXPECT_EQ(DBGUI_TrimTrailingNewlines("MOUNT: opened %s%n%d\n"),
	          "MOUNT: opened %s%n%d");
}

TEST(DebuggerLogTrim, HandlesEmbeddedNulByteWithoutTruncating)
{
	// string_view keeps its own length, so a NUL in the middle must not end
	// the message the way a C string would.
	const std::string_view with_nul("a\0b\n", 4);
	const auto trimmed = DBGUI_TrimTrailingNewlines(with_nul);

	EXPECT_EQ(trimmed.size(), 3u);
	EXPECT_EQ(trimmed, std::string_view("a\0b", 3));
}

TEST(DebuggerLogTrim, ReturnsAViewIntoTheOriginalBuffer)
{
	const std::string message = "RENDER: presenting\n";
	const auto trimmed        = DBGUI_TrimTrailingNewlines(message);

	EXPECT_EQ(trimmed.data(), message.data());
	EXPECT_EQ(trimmed.size(), message.size() - 1);
}

} // namespace

#endif // C_DEBUGGER
