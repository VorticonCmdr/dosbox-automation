// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/input.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using Webserver::IsValidEventFrame;
using Webserver::IsValidEventTimeMs;
using Webserver::IsValidTypingCps;

namespace {

TEST(EventTimeValidation, AcceptsZero)
{
	EXPECT_TRUE(IsValidEventTimeMs(0.0));
}

TEST(EventTimeValidation, AcceptsOrdinaryDelay)
{
	EXPECT_TRUE(IsValidEventTimeMs(1500.0));
}

TEST(EventTimeValidation, RejectsNegative)
{
	EXPECT_FALSE(IsValidEventTimeMs(-1.0));
}

TEST(EventTimeValidation, AcceptsUpperBoundary)
{
	EXPECT_TRUE(IsValidEventTimeMs(24.0 * 60 * 60 * 1000));
}

TEST(EventTimeValidation, RejectsJustPastUpperBoundary)
{
	EXPECT_FALSE(IsValidEventTimeMs(24.0 * 60 * 60 * 1000 + 1.0));
}

TEST(EventTimeValidation, RejectsExtremeValueThatWouldOverflowPicMakeCycles)
{
	// This exact value (or anything near it) reaching PIC_AddEvent used
	// to assert-fail (debug) or invoke undefined behaviour (release) in
	// PIC_MakeCycles from a single malformed request.
	EXPECT_FALSE(IsValidEventTimeMs(1e308));
}

TEST(EventFrameValidation, AcceptsZero)
{
	EXPECT_TRUE(IsValidEventFrame(0));
}

TEST(EventFrameValidation, AcceptsOrdinaryFrame)
{
	EXPECT_TRUE(IsValidEventFrame(120000));
}

TEST(EventFrameValidation, RejectsNegative)
{
	EXPECT_FALSE(IsValidEventFrame(-1));
}

TEST(EventFrameValidation, RejectsValueThatWouldWrapToHugeUnsignedOnNaiveConversion)
{
	// jev["frame"].get<uint64_t>() on a negative JSON literal silently
	// reinterprets it as its unsigned bit pattern; the caller must
	// reject the sign on the signed value before that ever happens.
	EXPECT_FALSE(IsValidEventFrame(-1));
	EXPECT_FALSE(IsValidEventFrame(std::numeric_limits<int64_t>::min()));
}

TEST(EventFrameValidation, RejectsJustPastUpperBoundary)
{
	EXPECT_FALSE(IsValidEventFrame(1'000'000'001));
}

TEST(EventFrameValidation, AcceptsUpperBoundary)
{
	EXPECT_TRUE(IsValidEventFrame(1'000'000'000));
}

TEST(TypingCpsValidation, AcceptsDefaultRate)
{
	EXPECT_TRUE(IsValidTypingCps(30.0));
}

TEST(TypingCpsValidation, RejectsZero)
{
	// Not just "invalid": ExpandTextToEvents' 1000/cps step would be
	// infinite for cps=0.
	EXPECT_FALSE(IsValidTypingCps(0.0));
}

TEST(TypingCpsValidation, RejectsNegative)
{
	EXPECT_FALSE(IsValidTypingCps(-30.0));
}

TEST(TypingCpsValidation, RejectsNearZeroValueThatWouldDeriveAnExtremeEventTime)
{
	// The vulnerability this bound closes: step_ms = 1000/cps grows
	// without limit as cps approaches zero, so a tiny-but-positive cps
	// reaches the same PIC_AddEvent overflow an out-of-range 't' does.
	EXPECT_FALSE(IsValidTypingCps(1e-300));
}

TEST(TypingCpsValidation, RejectsExtremeHighValue)
{
	EXPECT_FALSE(IsValidTypingCps(1e308));
}

TEST(TypingCpsValidation, AcceptsBoundaries)
{
	EXPECT_TRUE(IsValidTypingCps(0.1));
	EXPECT_TRUE(IsValidTypingCps(1000.0));
}

// -- RecordingStore --

using Webserver::InputEvent;
namespace RecordingStore = Webserver::RecordingStore;

TEST(RecordingStoreNameValidation, AcceptsAlphanumericHyphenUnderscore)
{
	EXPECT_TRUE(RecordingStore::IsValidName("install-run_1"));
}

TEST(RecordingStoreNameValidation, RejectsEmpty)
{
	EXPECT_FALSE(RecordingStore::IsValidName(""));
}

TEST(RecordingStoreNameValidation, RejectsSpace)
{
	EXPECT_FALSE(RecordingStore::IsValidName("bad name"));
}

TEST(RecordingStoreNameValidation, RejectsPathSeparator)
{
	// Never used as a filesystem path today, but the store's whole
	// purpose is to be a safe key/identifier - same discipline as
	// Lua::ScriptValidator's script names, which this rule mirrors.
	EXPECT_FALSE(RecordingStore::IsValidName("../etc/passwd"));
}

TEST(RecordingStoreNameValidation, AcceptsUpperBoundaryLength)
{
	EXPECT_TRUE(RecordingStore::IsValidName(std::string(64, 'a')));
}

TEST(RecordingStoreNameValidation, RejectsJustPastUpperBoundaryLength)
{
	EXPECT_FALSE(RecordingStore::IsValidName(std::string(65, 'a')));
}

InputEvent MakeKeyEvent(const double t_ms, const uint64_t frame)
{
	InputEvent ev;
	ev.t_ms    = t_ms;
	ev.frame   = frame;
	ev.type    = InputEvent::Type::Key;
	ev.key     = 1;
	ev.pressed = true;
	return ev;
}

class RecordingStoreTest : public testing::Test {
protected:
	// gtest_discover_tests registers each TEST_F as its own ctest entry,
	// each a fresh process invocation, so RecordingStore's process-lifetime
	// map starts empty for every test here regardless - this TearDown is
	// defensive hygiene, not load-bearing for isolation.
	void TearDown() override
	{
		for (const auto& [name, entry] : RecordingStore::List()) {
			(void)entry;
			RecordingStore::Delete(name);
		}
	}
};

TEST_F(RecordingStoreTest, GetReturnsNulloptForUnknownName)
{
	EXPECT_FALSE(RecordingStore::Get("does-not-exist").has_value());
}

TEST_F(RecordingStoreTest, SaveThenGetRoundTrips)
{
	std::vector<InputEvent> events = {MakeKeyEvent(0, 0), MakeKeyEvent(10, 1)};
	RecordingStore::Save("round-trip",
	                     events,
	                     /*truncated=*/false,
	                     /*duration_ms=*/10);

	const auto got = RecordingStore::Get("round-trip");
	ASSERT_TRUE(got.has_value());
	ASSERT_EQ(got->size(), 2u);
	EXPECT_EQ((*got)[0].t_ms, 0);
	EXPECT_EQ((*got)[1].frame, 1u);
}

TEST_F(RecordingStoreTest, GetReturnsACopyNotTheStoredOriginal)
{
	RecordingStore::Save("copy-test", {MakeKeyEvent(0, 0)}, false, 0);

	auto first = RecordingStore::Get("copy-test");
	ASSERT_TRUE(first.has_value());
	first->push_back(MakeKeyEvent(999, 999));

	const auto second = RecordingStore::Get("copy-test");
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->size(), 1u)
	        << "mutating a previously-returned copy must not affect the "
	           "stored original - a named recording has to stay replayable "
	           "more than once";
}

TEST_F(RecordingStoreTest, SaveOverwritesAnExistingName)
{
	RecordingStore::Save("dup", {MakeKeyEvent(0, 0)}, false, 0);
	RecordingStore::Save("dup", {MakeKeyEvent(0, 0), MakeKeyEvent(1, 1)}, true, 5);

	const auto got = RecordingStore::Get("dup");
	ASSERT_TRUE(got.has_value());
	EXPECT_EQ(got->size(), 2u);
}

TEST_F(RecordingStoreTest, DeleteRemovesAndReportsTrue)
{
	RecordingStore::Save("to-delete", {}, false, 0);
	EXPECT_TRUE(RecordingStore::Delete("to-delete"));
	EXPECT_FALSE(RecordingStore::Get("to-delete").has_value());
}

TEST_F(RecordingStoreTest, DeleteOfUnknownNameReportsFalse)
{
	EXPECT_FALSE(RecordingStore::Delete("never-existed"));
}

TEST_F(RecordingStoreTest, ListReportsMetadataForEveryStoredRecording)
{
	RecordingStore::Save("a", {MakeKeyEvent(0, 0), MakeKeyEvent(1, 1)}, false, 42);
	RecordingStore::Save("b", {MakeKeyEvent(0, 0)}, true, 7);

	const auto list = RecordingStore::List();
	ASSERT_EQ(list.size(), 2u);

	const std::unordered_map<std::string, RecordingStore::Entry> by_name(
	        list.begin(), list.end());

	ASSERT_TRUE(by_name.contains("a"));
	EXPECT_EQ(by_name.at("a").event_count, 2u);
	EXPECT_EQ(by_name.at("a").duration_ms, 42);
	EXPECT_FALSE(by_name.at("a").truncated);

	ASSERT_TRUE(by_name.contains("b"));
	EXPECT_EQ(by_name.at("b").event_count, 1u);
	EXPECT_TRUE(by_name.at("b").truncated);
}

TEST_F(RecordingStoreTest, HasRoomTrueWhenUnderCapacity)
{
	RecordingStore::Save("only-one", {}, false, 0);
	EXPECT_TRUE(RecordingStore::HasRoom("another"));
}

TEST_F(RecordingStoreTest, HasRoomTrueForAnExistingNameEvenAtCapacity)
{
	for (size_t i = 0; i < Webserver::MaxStoredRecordings; ++i) {
		RecordingStore::Save("slot" + std::to_string(i), {}, false, 0);
	}
	EXPECT_TRUE(RecordingStore::HasRoom("slot0"));
}

TEST_F(RecordingStoreTest, HasRoomFalseForANewNameAtCapacity)
{
	for (size_t i = 0; i < Webserver::MaxStoredRecordings; ++i) {
		RecordingStore::Save("slot" + std::to_string(i), {}, false, 0);
	}
	EXPECT_FALSE(RecordingStore::HasRoom("brand-new-name"));
}

} // namespace
