// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/memory.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

using Webserver::ParseScanPattern;
using Webserver::PatternSelectiveEnoughForSpan;
using Webserver::PatternWithinScanCpuBudget;
using Webserver::ScanBufferForPattern;
using Webserver::ScanPattern;

namespace {

TEST(MemScanParse, ParsesFixedBytesAndWildcards)
{
	auto pattern = ParseScanPattern("8B 46 ?? 50 E8");
	ASSERT_EQ(pattern.size(), 5u);
	EXPECT_EQ(pattern[0], 0x8B);
	EXPECT_EQ(pattern[1], 0x46);
	EXPECT_FALSE(pattern[2].has_value());
	EXPECT_EQ(pattern[3], 0x50);
	EXPECT_EQ(pattern[4], 0xE8);
}

TEST(MemScanParse, AcceptsLowercaseHex)
{
	auto pattern = ParseScanPattern("8b 46");
	ASSERT_EQ(pattern.size(), 2u);
	EXPECT_EQ(pattern[0], 0x8B);
	EXPECT_EQ(pattern[1], 0x46);
}

TEST(MemScanParse, AllWildcardParsesOk)
{
	// ParseScanPattern only checks token shape; rejecting an
	// all-wildcard pattern is Post's policy check, not the parser's.
	auto pattern = ParseScanPattern("?? ?? ??");
	ASSERT_EQ(pattern.size(), 3u);
	for (const auto& b : pattern) {
		EXPECT_FALSE(b.has_value());
	}
}

TEST(MemScanParse, RejectsEmptyPattern)
{
	EXPECT_THROW(ParseScanPattern(""), std::invalid_argument);
	EXPECT_THROW(ParseScanPattern("   "), std::invalid_argument);
}

TEST(MemScanParse, RejectsOversizedPattern)
{
	std::string text;
	for (int i = 0; i < 257; ++i) {
		text += "41 ";
	}
	EXPECT_THROW(ParseScanPattern(text), std::invalid_argument);
}

TEST(MemScanParse, AcceptsMaxSizedPattern)
{
	std::string text;
	for (int i = 0; i < 256; ++i) {
		text += "41 ";
	}
	auto pattern = ParseScanPattern(text);
	EXPECT_EQ(pattern.size(), 256u);
}

TEST(MemScanParse, RejectsMalformedTokens)
{
	EXPECT_THROW(ParseScanPattern("GG"), std::invalid_argument);
	EXPECT_THROW(ParseScanPattern("8"), std::invalid_argument);
	EXPECT_THROW(ParseScanPattern("123"), std::invalid_argument);
	EXPECT_THROW(ParseScanPattern("0x8B"), std::invalid_argument);
	EXPECT_THROW(ParseScanPattern("?"), std::invalid_argument);
	EXPECT_THROW(ParseScanPattern("8B ??? 46"), std::invalid_argument);
}

TEST(MemScanBuffer, FindsFullyFixedMatch)
{
	std::vector<uint8_t> buf = {0x00, 0x8B, 0x46, 0x10, 0x00};
	ScanPattern pattern      = {uint8_t{0x8B}, uint8_t{0x46}};
	auto hits                = ScanBufferForPattern(buf, pattern);
	EXPECT_EQ(hits, (std::vector<uint32_t>{1}));
}

TEST(MemScanBuffer, WildcardMatchesAnyByte)
{
	std::vector<uint8_t> buf = {0x8B, 0x00, 0x50, 0x8B, 0xFF, 0x50};
	ScanPattern pattern      = {uint8_t{0x8B}, std::nullopt, uint8_t{0x50}};
	auto hits                = ScanBufferForPattern(buf, pattern);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0, 3}));
}

TEST(MemScanBuffer, NoMatchReturnsEmpty)
{
	std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
	ScanPattern pattern      = {uint8_t{0xFF}};
	auto hits                = ScanBufferForPattern(buf, pattern);
	EXPECT_TRUE(hits.empty());
}

TEST(MemScanBuffer, BufferSmallerThanPatternReturnsEmpty)
{
	std::vector<uint8_t> buf = {0x8B};
	ScanPattern pattern      = {uint8_t{0x8B}, uint8_t{0x46}};
	auto hits                = ScanBufferForPattern(buf, pattern);
	EXPECT_TRUE(hits.empty());
}

TEST(MemScanBuffer, LimitCapsReturnedMatchesButNotTotal)
{
	std::vector<uint8_t> buf = {0x90, 0x90, 0x90, 0x90, 0x90};
	ScanPattern pattern      = {uint8_t{0x90}};
	size_t total             = 0;
	auto hits = ScanBufferForPattern(buf, pattern, 2, &total);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0, 1}));
	EXPECT_EQ(total, 5u);
}

TEST(MemScanBuffer, DefaultArgumentsMatchUnlimitedBehavior)
{
	std::vector<uint8_t> buf = {0x90, 0x90, 0x90};
	ScanPattern pattern      = {uint8_t{0x90}};
	auto hits                = ScanBufferForPattern(buf, pattern);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0, 1, 2}));
}

TEST(MemScanBuffer, LeadingWildcardsStillMatchCorrectly)
{
	// Regression for the fixed-bytes precomputation in
	// ScanBufferForPattern: the inner loop only ever walks the fixed
	// (offset, value) pairs now, not the raw pattern including
	// wildcards - this confirms that optimization didn't change which
	// positions count as matches when the fixed bytes aren't at the
	// front.
	std::vector<uint8_t> buf = {0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x42};
	ScanPattern pattern = {std::nullopt, std::nullopt, std::nullopt, uint8_t{0x41}};
	auto hits = ScanBufferForPattern(buf, pattern);
	EXPECT_EQ(hits, (std::vector<uint32_t>{0}));
}

TEST(MemScanBuffer, MostlyWildcardPatternOverNearZeroBufferMatchesCorrectly)
{
	// The scenario an adversarial-review pass found: a pattern that is
	// almost all wildcards with its few fixed bytes at the tail,
	// scanned against memory that is mostly the same value as those
	// fixed bytes (the ordinary state of untouched RAM). Before the
	// fixed-bytes precomputation, this made the inner loop walk close
	// to its full length for nearly every candidate position instead
	// of breaking after a few iterations - a correctness-preserving
	// check here, the actual cost-bound fix is what matters, verified
	// live against the real binary separately.
	std::vector<uint8_t> buf(1000, 0x00);
	buf[500] = 0x41;
	buf[501] = 0x42;
	buf[502] = 0x43;

	ScanPattern pattern;
	for (int i = 0; i < 253; ++i) {
		pattern.emplace_back(std::nullopt);
	}
	pattern.push_back(uint8_t{0x41});
	pattern.push_back(uint8_t{0x42});
	pattern.push_back(uint8_t{0x43});

	auto hits = ScanBufferForPattern(buf, pattern);
	EXPECT_EQ(hits, (std::vector<uint32_t>{500 - 253}));
}

// ---------------------------------------------------------------------------
// PatternSelectiveEnoughForSpan
// ---------------------------------------------------------------------------

TEST(MemScanSelectivity, OneFixedByteSufficesForASpanUpTo256)
{
	EXPECT_TRUE(PatternSelectiveEnoughForSpan(1, 256));
	EXPECT_FALSE(PatternSelectiveEnoughForSpan(1, 257));
}

TEST(MemScanSelectivity, ThreeFixedBytesSufficeForTheMaxSpanExactly)
{
	EXPECT_TRUE(PatternSelectiveEnoughForSpan(3, 16'777'216));
	EXPECT_FALSE(PatternSelectiveEnoughForSpan(2, 16'777'216));
}

TEST(MemScanSelectivity, FourOrMoreFixedBytesAlwaysSufficeForAnyPossibleSpan)
{
	// 256^4 already exceeds the largest span a uint32_t offset pair
	// can express - confirms the 4-iteration cap in the implementation
	// never needs to go further.
	EXPECT_TRUE(PatternSelectiveEnoughForSpan(4, 0xFFFFFFFFull));
	EXPECT_TRUE(PatternSelectiveEnoughForSpan(256, 0xFFFFFFFFull));
}

TEST(MemScanSelectivity, ZeroFixedBytesFailsAnySpanLargerThanOne)
{
	EXPECT_TRUE(PatternSelectiveEnoughForSpan(0, 1));
	EXPECT_FALSE(PatternSelectiveEnoughForSpan(0, 2));
}

// ---------------------------------------------------------------------------
// PatternWithinScanCpuBudget
// ---------------------------------------------------------------------------

TEST(MemScanCpuBudget, ThePlansOwnExamplePatternPassesAtTheMaxSpan)
{
	// "8B 46 ?? 50 E8" - 4 fixed bytes.
	EXPECT_TRUE(PatternWithinScanCpuBudget(4, 16'777'216));
}

TEST(MemScanCpuBudget, ADenselySpecifiedMaxLengthPatternFailsAtTheMaxSpan)
{
	// 256 fixed bytes * 16 MiB span is the case an adversarial review
	// found blows the budget under the (now-fixed) cost model this
	// check assumes; confirms it is still rejected.
	EXPECT_FALSE(PatternWithinScanCpuBudget(256, 16'777'216));
}

TEST(MemScanCpuBudget, TheAdversarialReviewScenarioNowPassesUnderTheFixedCostModel)
{
	// 253 wildcards + 3 fixed bytes over the max span:
	// ScanBufferForPattern's inner loop cost is genuinely O(fixed_count)
	// now (see the MostlyWildcardPatternOverNearZeroBufferMatchesCorrectly
	// test above and the fixed_bytes precomputation in memory.cpp), so this
	// is cheap despite the pattern's 256-token length.
	EXPECT_TRUE(PatternWithinScanCpuBudget(3, 16'777'216));
}

TEST(MemScanCpuBudget, BoundaryIsInclusive)
{
	// span * fixed_count == MaxScanWorstCaseOps (1e9) exactly: span =
	// 500,000,000 with fixed_count = 2.
	EXPECT_TRUE(PatternWithinScanCpuBudget(2, 500'000'000ull));
	EXPECT_FALSE(PatternWithinScanCpuBudget(2, 500'000'001ull));
}

} // namespace
