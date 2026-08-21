// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/video.h"

#include <gtest/gtest.h>

using Webserver::EtagMatches;
using Webserver::FormatEtag;

namespace {

TEST(FormatEtagTest, ProducesSixteenLowercaseHexDigits)
{
	const auto etag = FormatEtag(0x1a);
	ASSERT_EQ(etag.size(), 16u);
	EXPECT_EQ(etag, "000000000000001a");
}

TEST(FormatEtagTest, ZeroHashFormatsAsAllZeroes)
{
	EXPECT_EQ(FormatEtag(0), "0000000000000000");
}

TEST(FormatEtagTest, MaxHashFormatsWithoutTruncation)
{
	EXPECT_EQ(FormatEtag(0xFFFFFFFFFFFFFFFFULL), "ffffffffffffffff");
}

TEST(EtagMatchesTest, EmptyHeaderNeverMatches)
{
	// The absent-header case: get_if_none_match() (video.cpp) returns
	// "" when there is no If-None-Match header at all, and that must
	// never be treated as a match against any hash.
	EXPECT_FALSE(EtagMatches("", 0));
	EXPECT_FALSE(EtagMatches("", 0x1234));
}

TEST(EtagMatchesTest, MatchesUnquotedValue)
{
	EXPECT_TRUE(EtagMatches("000000000000001a", 0x1a));
}

TEST(EtagMatchesTest, MatchesQuotedValue)
{
	// RFC 7232 requires ETags to be quoted; accept them quoted.
	EXPECT_TRUE(EtagMatches("\"000000000000001a\"", 0x1a));
}

TEST(EtagMatchesTest, RejectsDifferentHash)
{
	EXPECT_FALSE(EtagMatches("000000000000001a", 0x1b));
	EXPECT_FALSE(EtagMatches("\"000000000000001a\"", 0x1b));
}

TEST(EtagMatchesTest, RejectsAWeakOrMalformedQuoting)
{
	// A single stray quote character must not be stripped as if it
	// were a matched pair - EtagMatches should just fail the compare.
	EXPECT_FALSE(EtagMatches("\"000000000000001a", 0x1a));
	EXPECT_FALSE(EtagMatches("000000000000001a\"", 0x1a));
}

TEST(EtagMatchesTest, RejectsCaseMismatch)
{
	// FormatEtag always produces lowercase hex; an uppercase candidate
	// must not be treated as equivalent.
	EXPECT_FALSE(EtagMatches("000000000000001A", 0x1a));
}

} // namespace
