// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/memory_snapshot.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

using Webserver::DiffOp;
using Webserver::ParseDiffOp;
using Webserver::SnapshotMode;
using Webserver::SnapshotRegistry;

namespace {

// --- ParseDiffOp ---

TEST(ParseDiffOpTest, ParsesEveryDocumentedName)
{
	EXPECT_EQ(ParseDiffOp("changed"), DiffOp::Changed);
	EXPECT_EQ(ParseDiffOp("unchanged"), DiffOp::Unchanged);
	EXPECT_EQ(ParseDiffOp("increased"), DiffOp::Increased);
	EXPECT_EQ(ParseDiffOp("decreased"), DiffOp::Decreased);
}

TEST(ParseDiffOpTest, EqualsIsASynonymForUnchanged)
{
	EXPECT_EQ(ParseDiffOp("equals"), DiffOp::Unchanged);
}

TEST(ParseDiffOpTest, RejectsUnknownNames)
{
	EXPECT_THROW(ParseDiffOp("bogus"), std::invalid_argument);
	EXPECT_THROW(ParseDiffOp(""), std::invalid_argument);
	EXPECT_THROW(ParseDiffOp("Changed"), std::invalid_argument);
}

// --- SnapshotRegistry: Create / GetReadPlan ---

class SnapshotRegistryTest : public ::testing::Test {
protected:
	SnapshotRegistry reg;

	// A freshly Create()d handle always starts at generation 1 - used
	// throughout below for a handle's first DiffDense call, where a
	// GetReadPlan round-trip to fetch the generation would just be
	// restating this fact.
	static constexpr uint64_t FreshGeneration = 1;
};

TEST_F(SnapshotRegistryTest, CreateReturnsIncreasingHandlesAndDenseReadPlan)
{
	const auto h1 = reg.Create(0x1000, 0x1004, {1, 2, 3, 4});
	const auto h2 = reg.Create(0x2000, 0x2002, {5, 6});
	EXPECT_NE(h1, h2);

	const auto plan = reg.GetReadPlan(h1);
	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->mode, SnapshotMode::Dense);
	EXPECT_EQ(plan->start, 0x1000u);
	EXPECT_EQ(plan->end, 0x1004u);
	EXPECT_EQ(plan->generation, FreshGeneration);
}

TEST_F(SnapshotRegistryTest, GetReadPlanOnUnknownHandleReturnsNullopt)
{
	EXPECT_FALSE(reg.GetReadPlan(999999).has_value());
}

// --- DiffDense ---

TEST_F(SnapshotRegistryTest, DiffDenseFindsChangedBytesAtWidthOne)
{
	const auto h = reg.Create(0x1000, 0x1005, {0x10, 0x20, 0x30, 0x40, 0x50});
	// Byte 1 (0x20 -> 0x21) and byte 3 (0x40 -> 0x99) changed.
	const std::vector<uint8_t> fresh = {0x10, 0x21, 0x30, 0x99, 0x50};

	const auto result =
	        reg.DiffDense(h, FreshGeneration, DiffOp::Changed, 1, 256, fresh);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->total, 2u);
	EXPECT_FALSE(result->truncated);
	EXPECT_EQ(result->candidates, 2u);
	ASSERT_EQ(result->matches.size(), 2u);
	EXPECT_EQ(result->matches[0].addr, 0x1001u);
	EXPECT_EQ(result->matches[0].value, 0x21u);
	EXPECT_EQ(result->matches[1].addr, 0x1003u);
	EXPECT_EQ(result->matches[1].value, 0x99u);
}

TEST_F(SnapshotRegistryTest, DiffDenseUnchangedFindsUntouchedBytes)
{
	const auto h = reg.Create(0x1000, 0x1003, {0x01, 0x02, 0x03});
	const std::vector<uint8_t> fresh = {0x01, 0x99, 0x03};

	const auto result = reg.DiffDense(
	        h, FreshGeneration, DiffOp::Unchanged, 1, 256, fresh);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->total, 2u);
	ASSERT_EQ(result->matches.size(), 2u);
	EXPECT_EQ(result->matches[0].addr, 0x1000u);
	EXPECT_EQ(result->matches[1].addr, 0x1002u);
}

TEST_F(SnapshotRegistryTest, DiffDenseIncreasedAndDecreased)
{
	const auto h = reg.Create(0x1000, 0x1003, {10, 10, 10});
	const std::vector<uint8_t> fresh = {20, 5, 10};

	const auto increased = reg.DiffDense(
	        h, FreshGeneration, DiffOp::Increased, 1, 256, fresh);
	ASSERT_TRUE(increased.has_value());
	ASSERT_EQ(increased->matches.size(), 1u);
	EXPECT_EQ(increased->matches[0].addr, 0x1000u);

	// The first diff already narrowed to Sparse mode with only the
	// survivor above, so a second Create is needed to test "decreased"
	// against the same original values.
	const auto h2        = reg.Create(0x2000, 0x2003, {10, 10, 10});
	const auto decreased = reg.DiffDense(
	        h2, FreshGeneration, DiffOp::Decreased, 1, 256, fresh);
	ASSERT_TRUE(decreased.has_value());
	ASSERT_EQ(decreased->matches.size(), 1u);
	EXPECT_EQ(decreased->matches[0].addr, 0x2001u);
}

TEST_F(SnapshotRegistryTest, DiffDenseRespectsWidthAsLittleEndian)
{
	// Overlapping width-2 windows at offsets 0, 1, 2 (like
	// ScanBufferForValue): only byte 0 differs between original and
	// fresh, so only the offset-0 window's little-endian value changes -
	// offsets 1 and 2 read the same two (untouched) bytes either way.
	const auto h = reg.Create(0x1000, 0x1004, {0x34, 0x12, 0x02, 0x00});
	const std::vector<uint8_t> fresh = {0x35, 0x12, 0x02, 0x00};

	const auto result =
	        reg.DiffDense(h, FreshGeneration, DiffOp::Changed, 2, 256, fresh);
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->matches.size(), 1u);
	EXPECT_EQ(result->matches[0].addr, 0x1000u);
	EXPECT_EQ(result->matches[0].value, 0x1235u);
}

TEST_F(SnapshotRegistryTest, DiffDenseLimitCapsMatchesButNotTotalOrCandidates)
{
	const std::vector<uint8_t> original(10, 0x00);
	const auto h = reg.Create(0x1000, 0x100A, original);
	const std::vector<uint8_t> fresh(10, 0x01); // every byte changed

	const auto result = reg.DiffDense(h, FreshGeneration, DiffOp::Changed, 1, 3, fresh);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->matches.size(), 3u);
	EXPECT_EQ(result->total, 10u);
	EXPECT_EQ(result->candidates, 10u);
	EXPECT_TRUE(result->truncated);
}

TEST_F(SnapshotRegistryTest, DiffDenseTransitionsHandleToSparseModeAndBumpsGeneration)
{
	const auto h = reg.Create(0x1000, 0x1003, {1, 2, 3});
	const std::vector<uint8_t> fresh = {9, 2, 9};
	reg.DiffDense(h, FreshGeneration, DiffOp::Changed, 1, 256, fresh);

	const auto plan = reg.GetReadPlan(h);
	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->mode, SnapshotMode::Sparse);
	EXPECT_EQ(plan->width, 1);
	ASSERT_EQ(plan->addresses.size(), 2u);
	EXPECT_NE(plan->generation, FreshGeneration);
}

TEST_F(SnapshotRegistryTest, DiffDenseRejectsAStaleGeneration)
{
	const auto h = reg.Create(0x1000, 0x1003, {1, 2, 3});
	const std::vector<uint8_t> fresh = {9, 2, 9};

	// A generation that doesn't match the entry's current one (as if
	// another request's GetReadPlan/Diff round already ran) must be
	// refused, not silently compared against whatever the entry holds
	// now.
	const auto result = reg.DiffDense(
	        h, FreshGeneration + 1, DiffOp::Changed, 1, 256, fresh);
	EXPECT_FALSE(result.has_value());
}

// --- DiffSparse (refine) ---

TEST_F(SnapshotRegistryTest, RefineNarrowsCandidatesAcrossRounds)
{
	const std::vector<uint8_t> original(5, 0x00);
	const auto h = reg.Create(0x1000, 0x1005, original);
	const std::vector<uint8_t> round1_fresh = {1, 0, 1, 0, 1}; // 3 changed:
	                                                           // 0,2,4

	const auto round1 = reg.DiffDense(
	        h, FreshGeneration, DiffOp::Changed, 1, 256, round1_fresh);
	ASSERT_TRUE(round1.has_value());
	ASSERT_EQ(round1->candidates, 3u);

	const auto plan = reg.GetReadPlan(h);
	ASSERT_TRUE(plan.has_value());
	ASSERT_EQ(plan->mode, SnapshotMode::Sparse);
	ASSERT_EQ(plan->addresses.size(), 3u);

	// Round 2: only address 0x1000 increases further (2 > 1); the
	// other two survivors stay at their round-1 value.
	std::vector<std::optional<uint32_t>> fresh_values;
	for (const auto addr : plan->addresses) {
		fresh_values.push_back(addr == 0x1000 ? 2u : 1u);
	}

	const auto round2 = reg.DiffSparse(h,
	                                   plan->generation,
	                                   DiffOp::Increased,
	                                   256,
	                                   plan->addresses,
	                                   fresh_values);
	ASSERT_TRUE(round2.has_value());
	ASSERT_EQ(round2->matches.size(), 1u);
	EXPECT_EQ(round2->matches[0].addr, 0x1000u);
	EXPECT_EQ(round2->matches[0].value, 2u);
	EXPECT_EQ(round2->candidates, 1u);
}

TEST_F(SnapshotRegistryTest, DiffSparseDropsAddressWithNulloptFreshValue)
{
	const std::vector<uint8_t> original(3, 0x00);
	const auto h = reg.Create(0x1000, 0x1003, original);
	const std::vector<uint8_t> round1_fresh = {1, 1, 1};
	reg.DiffDense(h, FreshGeneration, DiffOp::Changed, 1, 256, round1_fresh);

	const auto plan = reg.GetReadPlan(h);
	ASSERT_TRUE(plan.has_value());
	ASSERT_EQ(plan->addresses.size(), 3u);

	// Drop the first address (simulating it going out of range), keep
	// the rest unchanged (so op=Unchanged still matches them).
	std::vector<std::optional<uint32_t>> fresh_values;
	for (size_t i = 0; i < plan->addresses.size(); ++i) {
		fresh_values.push_back(i == 0 ? std::nullopt
		                              : std::optional<uint32_t>(1));
	}

	const auto result = reg.DiffSparse(h,
	                                   plan->generation,
	                                   DiffOp::Unchanged,
	                                   256,
	                                   plan->addresses,
	                                   fresh_values);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->candidates, 2u);
}

TEST_F(SnapshotRegistryTest, HandleIsRemovedOnceNoCandidatesSurvive)
{
	const std::vector<uint8_t> original(2, 0x00);
	const auto h                     = reg.Create(0x1000, 0x1002, original);
	const std::vector<uint8_t> fresh = {1, 1};
	reg.DiffDense(h, FreshGeneration, DiffOp::Changed, 1, 256, fresh);

	const auto plan = reg.GetReadPlan(h);
	ASSERT_TRUE(plan.has_value());

	// Nothing satisfies "decreased" - the candidate set drops to zero.
	std::vector<std::optional<uint32_t>> fresh_values(plan->addresses.size(), 1u);
	const auto result = reg.DiffSparse(h,
	                                   plan->generation,
	                                   DiffOp::Decreased,
	                                   256,
	                                   plan->addresses,
	                                   fresh_values);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->candidates, 0u);

	EXPECT_FALSE(reg.GetReadPlan(h).has_value());
}

TEST_F(SnapshotRegistryTest, DiffSparseRejectsAStaleGeneration)
{
	const std::vector<uint8_t> original(3, 0x00);
	const auto h = reg.Create(0x1000, 0x1003, original);
	reg.DiffDense(h, FreshGeneration, DiffOp::Changed, 1, 256, {1, 1, 1});

	const auto plan = reg.GetReadPlan(h);
	ASSERT_TRUE(plan.has_value());

	std::vector<std::optional<uint32_t>> fresh_values(plan->addresses.size(), 1u);
	// A generation one behind what the entry actually holds now -
	// simulates a second request racing on the same handle after a
	// first one already diffed it.
	const auto result = reg.DiffSparse(h,
	                                   plan->generation - 1,
	                                   DiffOp::Unchanged,
	                                   256,
	                                   plan->addresses,
	                                   fresh_values);
	EXPECT_FALSE(result.has_value());
}

TEST_F(SnapshotRegistryTest, DiffSparseOnUnknownHandleReturnsNullopt)
{
	std::vector<uint32_t> addresses             = {0x1000};
	std::vector<std::optional<uint32_t>> values = {1u};
	EXPECT_FALSE(reg.DiffSparse(999999, 0, DiffOp::Changed, 256, addresses, values)
	                     .has_value());
}

TEST_F(SnapshotRegistryTest, DiffDenseOnUnknownHandleReturnsNullopt)
{
	EXPECT_FALSE(reg.DiffDense(999999, 0, DiffOp::Changed, 1, 256, {}).has_value());
}

// --- Eviction ---

TEST_F(SnapshotRegistryTest, LeastRecentlyUsedEntryEvictedWhenByteBudgetExceeded)
{
	const std::vector<uint8_t> big(20 * 1024 * 1024, 0xAA); // 20 MiB
	const auto older = reg.Create(0x0, 20 * 1024 * 1024, big);

	// A second 20 MiB snapshot pushes the total past the 32 MiB budget -
	// the only existing entry must be evicted to make room.
	const auto newer = reg.Create(0x0, 20 * 1024 * 1024, big);

	EXPECT_FALSE(reg.GetReadPlan(older).has_value());
	EXPECT_TRUE(reg.GetReadPlan(newer).has_value());
}

TEST_F(SnapshotRegistryTest, TouchingAnEntryProtectsItFromEviction)
{
	const std::vector<uint8_t> small = {1, 2, 3, 4};
	const auto keep_alive            = reg.Create(0x0, 4, small);

	const std::vector<uint8_t> big(20 * 1024 * 1024, 0xAA);
	const auto first_big = reg.Create(0x1000, 0x1000 + 20 * 1024 * 1024, big);
	// Touch keep_alive again so it's more recently used than first_big.
	reg.DiffDense(keep_alive, FreshGeneration, DiffOp::Changed, 1, 256, {9, 2, 3, 4});
	const auto second_big = reg.Create(0x2000, 0x2000 + 20 * 1024 * 1024, big);

	// first_big (never touched after creation, and the least recently
	// used once keep_alive was re-touched) should be the one evicted,
	// not keep_alive.
	EXPECT_TRUE(reg.GetReadPlan(keep_alive).has_value());
	EXPECT_FALSE(reg.GetReadPlan(first_big).has_value());
	EXPECT_TRUE(reg.GetReadPlan(second_big).has_value());
}

TEST_F(SnapshotRegistryTest, EntryCountBackstopEvictsEvenUnderByteBudget)
{
	// Each snapshot here is 1 byte - nowhere near the 32 MiB byte
	// budget, but MaxEntries+1 of them must still trigger eviction.
	// Deliberately does not GetReadPlan(first_handle) before the
	// triggering Create() below - GetReadPlan counts as a touch, which
	// would make first_handle artificially the most-recently-used
	// entry and defeat the point of this test.
	uint64_t first_handle = 0;
	for (size_t i = 0; i < SnapshotRegistry::MaxEntries; ++i) {
		const auto h = reg.Create(static_cast<uint32_t>(i),
		                          static_cast<uint32_t>(i) + 1,
		                          {0x00});
		if (i == 0) {
			first_handle = h;
		}
	}

	const auto one_more = reg.Create(1'000'000, 1'000'001, {0x00});
	(void)one_more;

	EXPECT_FALSE(reg.GetReadPlan(first_handle).has_value());
}

TEST_F(SnapshotRegistryTest, ClearRemovesEverything)
{
	const auto h = reg.Create(0x1000, 0x1004, {1, 2, 3, 4});
	reg.Clear();
	EXPECT_FALSE(reg.GetReadPlan(h).has_value());
}

} // namespace
