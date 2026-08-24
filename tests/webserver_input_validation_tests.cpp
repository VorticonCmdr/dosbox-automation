// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/input.h"

#include "hardware/input/keyboard.h"

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

// -- MouseCoordinateValidation --

using Webserver::IsValidMouseCoordinate;

TEST(MouseCoordinateValidation, AcceptsZero)
{
	EXPECT_TRUE(IsValidMouseCoordinate(0));
}

TEST(MouseCoordinateValidation, AcceptsOrdinaryValue)
{
	EXPECT_TRUE(IsValidMouseCoordinate(320));
}

TEST(MouseCoordinateValidation, RejectsNegative)
{
	EXPECT_FALSE(IsValidMouseCoordinate(-1));
}

TEST(MouseCoordinateValidation, AcceptsUpperBoundary)
{
	EXPECT_TRUE(IsValidMouseCoordinate(65535));
}

TEST(MouseCoordinateValidation, RejectsJustPastUpperBoundary)
{
	EXPECT_FALSE(IsValidMouseCoordinate(65536));
}

TEST(MouseCoordinateValidation,
     RejectsValueThatWouldWrapToInRangeOnNaiveUint16Conversion)
{
	// get<uint16_t>() on this value silently wraps to 4464 (70000 %
	// 65536) rather than rejecting it - confirmed directly against
	// nlohmann, not assumed.
	EXPECT_FALSE(IsValidMouseCoordinate(70000));
}

// -- MouseCoordinateValidationDouble --
//
// The double-based check parse_mouse_coordinate actually calls on every
// POST /api/v1/input/mouse and mouse_move x_abs/y_abs request - see
// input.h's IsValidMouseCoordinateDouble doc comment.

using Webserver::IsValidMouseCoordinateDouble;

TEST(MouseCoordinateValidationDouble, AcceptsZero)
{
	EXPECT_TRUE(IsValidMouseCoordinateDouble(0.0));
}

TEST(MouseCoordinateValidationDouble, AcceptsIntegralFloat)
{
	// 100.0 (a JSON float that happens to be integral) must be treated
	// the same as the bare integer literal 100 - e.g. a recorded
	// event's dumped host_x_abs/host_y_abs, or an agent's own
	// width/2-style arithmetic, is at least as likely to produce this
	// as a bare integer.
	EXPECT_TRUE(IsValidMouseCoordinateDouble(320.0));
}

TEST(MouseCoordinateValidationDouble, RejectsFractional)
{
	EXPECT_FALSE(IsValidMouseCoordinateDouble(100.5));
}

TEST(MouseCoordinateValidationDouble, RejectsNegative)
{
	EXPECT_FALSE(IsValidMouseCoordinateDouble(-1.0));
}

TEST(MouseCoordinateValidationDouble, RejectsJustPastUpperBoundary)
{
	EXPECT_FALSE(IsValidMouseCoordinateDouble(65536.0));
}

TEST(MouseCoordinateValidationDouble, AcceptsUpperBoundary)
{
	EXPECT_TRUE(IsValidMouseCoordinateDouble(65535.0));
}

TEST(MouseCoordinateValidationDouble, RejectsNaN)
{
	EXPECT_FALSE(IsValidMouseCoordinateDouble(
	        std::numeric_limits<double>::quiet_NaN()));
}

TEST(MouseCoordinateValidationDouble, RejectsPositiveInfinity)
{
	EXPECT_FALSE(IsValidMouseCoordinateDouble(
	        std::numeric_limits<double>::infinity()));
}

TEST(MouseCoordinateValidationDouble, RejectsNegativeInfinity)
{
	EXPECT_FALSE(IsValidMouseCoordinateDouble(
	        -std::numeric_limits<double>::infinity()));
}

TEST(MouseCoordinateValidationDouble,
     RejectsExtremeValueThatWouldOverflowInt64OnNaiveConversion)
{
	// Casting this straight to int64_t (skipping the pre-bound check)
	// would be undefined behaviour.
	EXPECT_FALSE(IsValidMouseCoordinateDouble(1e300));
}

// -- EventToJson --

using Webserver::EventToJson;
using Webserver::InputEvent;

TEST(EventToJsonTest, MouseMoveSerializesHostAbsFieldsNotRequestAbsFields)
{
	// A recorded MouseMove's absolute position must come out as
	// host_x_abs/host_y_abs, in a genuinely different coordinate space
	// (host window pixels) from the request-body 'x_abs'/'y_abs' field
	// (guest DOS screen pixels) - reposting this dump verbatim as a
	// /input/sequence body must fail loudly on the unrecognized field
	// name rather than silently misinterpreting the coordinate space.
	InputEvent ev;
	ev.type    = InputEvent::Type::MouseMove;
	ev.x_rel   = 5.0f;
	ev.y_rel   = -2.0f;
	ev.x_abs   = 320.0f;
	ev.y_abs   = 180.0f;
	ev.has_abs = false; // never set by the recording pipeline

	const auto j = EventToJson(ev);

	EXPECT_EQ(j.at("type"), "mouse_move");
	EXPECT_EQ(j.at("x_rel"), 5.0f);
	EXPECT_EQ(j.at("y_rel"), -2.0f);
	EXPECT_EQ(j.at("host_x_abs"), 320.0f);
	EXPECT_EQ(j.at("host_y_abs"), 180.0f);
	EXPECT_FALSE(j.contains("x_abs"));
	EXPECT_FALSE(j.contains("y_abs"));
}

TEST(EventToJsonTest, KeyEventSerializesTypeKeyAndPressed)
{
	InputEvent ev;
	ev.type    = InputEvent::Type::Key;
	ev.key     = static_cast<int>(KBD_enter);
	ev.pressed = true;

	const auto j = EventToJson(ev);

	EXPECT_EQ(j.at("type"), "key");
	EXPECT_EQ(j.at("key"), "KBD_enter");
	EXPECT_EQ(j.at("pressed"), true);
}

// -- RecordingStore --

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
