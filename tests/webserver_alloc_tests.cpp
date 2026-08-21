// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/dos.h"

#include <gtest/gtest.h>

using Webserver::AllocationRegistry;

namespace {

class AllocRegistryTest : public ::testing::Test {
protected:
	AllocationRegistry reg;
};

TEST_F(AllocRegistryTest, RemoveFailsForAnAddressNeverAdded)
{
	// The exact bug this registry closes: FreeMemoryCommand must not
	// reach DOS_FreeMemory/MEM_ReleasePages for an address the API
	// never minted, whether that address is simply wrong, or chosen to
	// index MEM_ReleasePages' handle vector out of bounds.
	EXPECT_FALSE(reg.Remove(0x1234));
	EXPECT_FALSE(reg.Remove(0xFFFFFFFF));
}

TEST_F(AllocRegistryTest, AddThenRemoveSucceedsExactlyOnce)
{
	reg.Add(0x1000);
	EXPECT_TRUE(reg.Remove(0x1000));
	// Second free of the same address: must not silently succeed
	// (a double free reaching MEM_ReleasePages would corrupt the
	// allocator's free list).
	EXPECT_FALSE(reg.Remove(0x1000));
}

TEST_F(AllocRegistryTest, TracksMultipleIndependentAddresses)
{
	reg.Add(0x1000);
	reg.Add(0x2000);
	reg.Add(0x3000);
	EXPECT_TRUE(reg.Remove(0x2000));
	EXPECT_TRUE(reg.Remove(0x1000));
	EXPECT_TRUE(reg.Remove(0x3000));
}

TEST_F(AllocRegistryTest, AddingTheSameAddressTwiceStaysRemovableOnce)
{
	// AllocMemoryCommand::Execute() only calls Add() after a successful
	// allocation, so a repeat Add() for the same address would mean two
	// distinct allocations happened to return the same address (the
	// allocator's problem, not the registry's) - either way, Remove()
	// must still only ever free once.
	reg.Add(0x1000);
	reg.Add(0x1000);
	EXPECT_TRUE(reg.Remove(0x1000));
	EXPECT_FALSE(reg.Remove(0x1000));
}

TEST_F(AllocRegistryTest, NotFullBelowCapacity)
{
	EXPECT_FALSE(reg.IsFull());
	reg.Add(0x1000);
	EXPECT_FALSE(reg.IsFull());
}

TEST_F(AllocRegistryTest, FullAtCapacityRefusesFurtherAllocation)
{
	for (size_t i = 0; i < AllocationRegistry::MaxEntries; ++i) {
		ASSERT_FALSE(reg.IsFull());
		reg.Add(static_cast<uint32_t>(0x10000 + i * 4));
	}
	EXPECT_TRUE(reg.IsFull());
}

TEST_F(AllocRegistryTest, RemovingBelowCapacityAgain)
{
	for (size_t i = 0; i < AllocationRegistry::MaxEntries; ++i) {
		reg.Add(static_cast<uint32_t>(0x10000 + i * 4));
	}
	ASSERT_TRUE(reg.IsFull());
	EXPECT_TRUE(reg.Remove(0x10000));
	EXPECT_FALSE(reg.IsFull());
}

} // namespace
