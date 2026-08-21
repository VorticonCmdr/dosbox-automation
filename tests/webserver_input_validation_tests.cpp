// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/input.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

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

} // namespace
