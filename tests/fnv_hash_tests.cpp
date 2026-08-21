// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "utils/fnv_hash.h"

#include <gtest/gtest.h>

namespace {

// Reference values from the FNV-1a 64-bit test suite
// (http://www.isthe.com/chongo/src/fnv/test_fnv.c), not just
// self-consistency: a wrong-but-stable implementation would still pass
// a "changes when input changes" test.

TEST(Fnv1aHashTest, EmptyStringIsTheOffsetBasis)
{
	EXPECT_EQ(Fnv1aHash(std::string_view("")), FnvOffsetBasis64);
	EXPECT_EQ(Fnv1aHash(std::string_view("")), 0xcbf29ce484222325ULL);
}

TEST(Fnv1aHashTest, MatchesKnownVectorForSingleChar)
{
	EXPECT_EQ(Fnv1aHash(std::string_view("a")), 0xaf63dc4c8601ec8cULL);
	EXPECT_EQ(Fnv1aHash(std::string_view("b")), 0xaf63df4c8601f1a5ULL);
}

TEST(Fnv1aHashTest, MatchesKnownVectorForFoobar)
{
	EXPECT_EQ(Fnv1aHash(std::string_view("foobar")), 0x85944171f73967e8ULL);
}

TEST(Fnv1aHashTest, DifferentInputsHashDifferently)
{
	EXPECT_NE(Fnv1aHash(std::string_view("frame A")),
	          Fnv1aHash(std::string_view("frame B")));
}

TEST(Fnv1aHashTest, IdenticalInputsHashIdentically)
{
	const std::string a = "the quick brown fox";
	const std::string b = "the quick brown fox";
	EXPECT_EQ(Fnv1aHash(a), Fnv1aHash(b));
}

TEST(Fnv1aHashTest, EmbeddedNullBytesParticipate)
{
	// A screen buffer or framebuffer legitimately contains 0x00 bytes;
	// the byte-pointer overload must not treat them as a terminator.
	const uint8_t a[] = {0x41, 0x00, 0x42};
	const uint8_t b[] = {0x41, 0x00, 0x43};
	EXPECT_NE(Fnv1aHash(a, sizeof(a)), Fnv1aHash(b, sizeof(b)));
}

TEST(Fnv1aHashTest, ZeroLengthBufferIsTheOffsetBasis)
{
	EXPECT_EQ(Fnv1aHash(nullptr, 0), FnvOffsetBasis64);
}

} // namespace
