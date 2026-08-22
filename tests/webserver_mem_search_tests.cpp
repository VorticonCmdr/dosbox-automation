// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/memory.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using Webserver::ScanBufferForValue;

namespace {

TEST(MemSearch, FindsByteMatches)
{
	std::vector<uint8_t> buf = {0x01, 0x63, 0x02, 0x63, 0x63};
	auto hits = ScanBufferForValue(buf, 0x63, 1);
	EXPECT_EQ(hits, (std::vector<uint32_t>{1, 3, 4}));
}

TEST(MemSearch, FindsWordMatchesLittleEndian)
{
	std::vector<uint8_t> buf = {0x63, 0x00, 0xFF, 0x63, 0x00};
	auto hits = ScanBufferForValue(buf, 0x0063, 2);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0, 3}));
}

TEST(MemSearch, FindsDwordMatch)
{
	std::vector<uint8_t> buf = {0x78, 0x56, 0x34, 0x12, 0x00};
	auto hits = ScanBufferForValue(buf, 0x12345678, 4);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0}));
}

TEST(MemSearch, NoMatchReturnsEmpty)
{
	std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
	auto hits = ScanBufferForValue(buf, 0xFF, 1);
	EXPECT_TRUE(hits.empty());
}

TEST(MemSearch, RejectsBadWidth)
{
	std::vector<uint8_t> buf = {0x00};
	EXPECT_THROW(ScanBufferForValue(buf, 0, 3), std::invalid_argument);
}

TEST(MemSearch, BufferSmallerThanWidthReturnsEmpty)
{
	std::vector<uint8_t> buf = {0x63};
	auto hits = ScanBufferForValue(buf, 0x0063, 2);
	EXPECT_TRUE(hits.empty());
}

TEST(MemSearch, EmptyBufferReturnsEmpty)
{
	std::vector<uint8_t> buf;
	auto hits = ScanBufferForValue(buf, 0, 1);
	EXPECT_TRUE(hits.empty());
}

TEST(MemSearch, LimitCapsReturnedMatchesButNotTotal)
{
	std::vector<uint8_t> buf = {0x63, 0x63, 0x63, 0x63, 0x63};
	size_t total             = 0;
	auto hits                = ScanBufferForValue(buf, 0x63, 1, 2, &total);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0, 1}));
	EXPECT_EQ(total, 5u);
}

TEST(MemSearch, LimitAtOrAboveMatchCountReturnsEverythingUntruncated)
{
	std::vector<uint8_t> buf = {0x63, 0x00, 0x63};
	size_t total             = 0;
	auto hits = ScanBufferForValue(buf, 0x63, 1, 100, &total);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0, 2}));
	EXPECT_EQ(total, 2u);
}

TEST(MemSearch, ZeroLimitReturnsNoMatchesButStillCountsTotal)
{
	std::vector<uint8_t> buf = {0x63, 0x63};
	size_t total             = 0;
	auto hits                = ScanBufferForValue(buf, 0x63, 1, 0, &total);
	EXPECT_TRUE(hits.empty());
	EXPECT_EQ(total, 2u);
}

TEST(MemSearch, DefaultArgumentsMatchPreExistingUnlimitedBehavior)
{
	// The exact pre-existing call shape every call site above already
	// uses - confirms the new limit/total_out parameters are additive,
	// not a behavior change for an unlimited caller.
	std::vector<uint8_t> buf = {0x63, 0x63, 0x63};
	auto hits                = ScanBufferForValue(buf, 0x63, 1);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0, 1, 2}));
}

} // namespace
