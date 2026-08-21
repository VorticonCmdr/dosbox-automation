// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/freeze.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

using Webserver::FreezeRegistry;
using Webserver::ValidateFreezeRange;

namespace {

class FreezeTest : public ::testing::Test {
protected:
	FreezeRegistry reg;
};

TEST_F(FreezeTest, AddAndList)
{
	EXPECT_TRUE(reg.Add(0x1000, 99, 1));
	EXPECT_TRUE(reg.Add(0x2000, 5, 2));
	const auto entries = reg.List();
	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0].address, 0x1000u);
	EXPECT_EQ(entries[0].value, 99u);
	EXPECT_EQ(entries[0].width, 1);
	EXPECT_EQ(entries[1].address, 0x2000u);
	EXPECT_EQ(entries[1].width, 2);
}

TEST_F(FreezeTest, AddSameAddressUpdatesInPlace)
{
	reg.Add(0x1000, 1, 1);
	reg.Add(0x1000, 250, 1);
	ASSERT_EQ(reg.List().size(), 1u);
	EXPECT_EQ(reg.List()[0].value, 250u);
}

TEST_F(FreezeTest, RejectsBeyondCap)
{
	for (int i = 0; i < FreezeRegistry::MaxEntries; ++i) {
		EXPECT_TRUE(reg.Add(0x1000 + i * 4, 0, 1));
	}
	EXPECT_FALSE(reg.Add(0x9000, 0, 1));
}

TEST_F(FreezeTest, RemoveOne)
{
	reg.Add(0x1000, 1, 1);
	reg.Add(0x2000, 2, 1);
	EXPECT_TRUE(reg.Remove(0x1000));
	EXPECT_EQ(reg.List().size(), 1u);
	EXPECT_EQ(reg.List()[0].address, 0x2000u);
}

TEST_F(FreezeTest, RemoveNonexistentReturnsFalse)
{
	EXPECT_FALSE(reg.Remove(0x9999));
}

TEST_F(FreezeTest, ClearAll)
{
	reg.Add(0x1000, 1, 1);
	reg.Add(0x2000, 2, 1);
	reg.Clear();
	EXPECT_TRUE(reg.List().empty());
}

TEST_F(FreezeTest, RejectsBadWidth)
{
	EXPECT_FALSE(reg.Add(0x1000, 0, 3));
	EXPECT_FALSE(reg.Add(0x1000, 0, 0));
}

TEST_F(FreezeTest, AcceptsAllValidWidths)
{
	EXPECT_TRUE(reg.Add(0x1000, 0, 1));
	EXPECT_TRUE(reg.Add(0x2000, 0, 2));
	EXPECT_TRUE(reg.Add(0x3000, 0, 4));
}

TEST(FreezeRangeValidation, AcceptsInRangeAddress)
{
	EXPECT_TRUE(ValidateFreezeRange(0x1000, 4, 16 * 1024 * 1024));
}

TEST(FreezeRangeValidation, AcceptsExactUpperBoundary)
{
	EXPECT_TRUE(ValidateFreezeRange(1000 - 4, 4, 1000));
}

TEST(FreezeRangeValidation, RejectsOneBytePastTheEnd)
{
	EXPECT_FALSE(ValidateFreezeRange(1000 - 3, 4, 1000));
}

TEST(FreezeRangeValidation, RejectsAddressNearUint32MaxWithout32BitWraparound)
{
	// address=0xFFFFFFFE, width=4: address + width in 32-bit arithmetic
	// wraps to 2, which is < any real mem_total and would wrongly pass.
	// The 64-bit sum is far beyond any real mem_total and must fail.
	EXPECT_FALSE(ValidateFreezeRange(std::numeric_limits<uint32_t>::max() - 1,
	                                 4,
	                                 16 * 1024 * 1024));
}

TEST(FreezeRangeValidation, RejectsMaxUint32AddressAtAnyWidth)
{
	EXPECT_FALSE(ValidateFreezeRange(std::numeric_limits<uint32_t>::max(),
	                                 1,
	                                 16 * 1024 * 1024));
}

} // namespace
