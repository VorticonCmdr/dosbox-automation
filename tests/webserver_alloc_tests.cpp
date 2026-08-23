// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/dos.h"

#include <algorithm>

#include <gtest/gtest.h>

using Webserver::AllocationRegistry;
using Webserver::AreaName;
using Webserver::MemoryArea;

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
	reg.Add(0x1000, 256, MemoryArea::Conv);
	EXPECT_TRUE(reg.Remove(0x1000));
	// Second free of the same address: must not silently succeed
	// (a double free reaching MEM_ReleasePages would corrupt the
	// allocator's free list).
	EXPECT_FALSE(reg.Remove(0x1000));
}

TEST_F(AllocRegistryTest, TracksMultipleIndependentAddresses)
{
	reg.Add(0x1000, 16, MemoryArea::Conv);
	reg.Add(0x2000, 16, MemoryArea::Conv);
	reg.Add(0x3000, 16, MemoryArea::Conv);
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
	reg.Add(0x1000, 16, MemoryArea::Conv);
	reg.Add(0x1000, 16, MemoryArea::Conv);
	EXPECT_TRUE(reg.Remove(0x1000));
	EXPECT_FALSE(reg.Remove(0x1000));
}

TEST_F(AllocRegistryTest, NotFullBelowCapacity)
{
	EXPECT_FALSE(reg.IsFull());
	reg.Add(0x1000, 16, MemoryArea::Conv);
	EXPECT_FALSE(reg.IsFull());
}

TEST_F(AllocRegistryTest, FullAtCapacityRefusesFurtherAllocation)
{
	for (size_t i = 0; i < AllocationRegistry::MaxEntries; ++i) {
		ASSERT_FALSE(reg.IsFull());
		reg.Add(static_cast<uint32_t>(0x10000 + i * 4), 16, MemoryArea::Conv);
	}
	EXPECT_TRUE(reg.IsFull());
}

TEST_F(AllocRegistryTest, RemovingBelowCapacityAgain)
{
	for (size_t i = 0; i < AllocationRegistry::MaxEntries; ++i) {
		reg.Add(static_cast<uint32_t>(0x10000 + i * 4), 16, MemoryArea::Conv);
	}
	ASSERT_TRUE(reg.IsFull());
	EXPECT_TRUE(reg.Remove(0x10000));
	EXPECT_FALSE(reg.IsFull());
}

TEST_F(AllocRegistryTest, ListReportsTheSizeAndAreaEachEntryWasAddedWith)
{
	reg.Add(0x1000, 4096, MemoryArea::Conv);
	reg.Add(0x2000, 64, MemoryArea::Xms);

	const auto list = reg.List();
	ASSERT_EQ(list.size(), 2u);

	auto conv = std::find_if(list.begin(), list.end(), [](const auto& e) {
		return e.first == 0x1000;
	});
	ASSERT_NE(conv, list.end());
	EXPECT_EQ(conv->second.size, 4096u);
	EXPECT_EQ(conv->second.area, MemoryArea::Conv);

	auto xms = std::find_if(list.begin(), list.end(), [](const auto& e) {
		return e.first == 0x2000;
	});
	ASSERT_NE(xms, list.end());
	EXPECT_EQ(xms->second.size, 64u);
	EXPECT_EQ(xms->second.area, MemoryArea::Xms);
}

TEST_F(AllocRegistryTest, ListIsSortedByAddressAscendingRegardlessOfInsertOrder)
{
	reg.Add(0x3000, 16, MemoryArea::Conv);
	reg.Add(0x1000, 16, MemoryArea::Conv);
	reg.Add(0x2000, 16, MemoryArea::Conv);

	const auto list = reg.List();
	ASSERT_EQ(list.size(), 3u);
	EXPECT_TRUE(std::is_sorted(list.begin(),
	                           list.end(),
	                           [](const auto& a, const auto& b) {
		                           return a.first < b.first;
	                           }));
}

TEST_F(AllocRegistryTest, ReAddingAnAddressReplacesItsSizeAndArea)
{
	// Only reachable if an address were ever handed back out after
	// being freed and re-allocated with different parameters - List()
	// must reflect the live allocation, not a stale first Add().
	reg.Add(0x1000, 16, MemoryArea::Conv);
	reg.Add(0x1000, 32, MemoryArea::Uma);

	const auto list = reg.List();
	ASSERT_EQ(list.size(), 1u);
	EXPECT_EQ(list[0].second.size, 32u);
	EXPECT_EQ(list[0].second.area, MemoryArea::Uma);
}

TEST_F(AllocRegistryTest, RemoveReturnsTheRemovedEntrysAreaAndOwnerPsp)
{
	// FreeMemoryCommand::Execute relies on this to re-validate current
	// MCB ownership before freeing a Conv/Uma block - see
	// AllocationInfo::owner_psp's own comment for why.
	reg.Add(0x1000, 256, MemoryArea::Conv, 0x0850);
	const auto removed = reg.Remove(0x1000);
	ASSERT_TRUE(removed);
	EXPECT_EQ(removed->size, 256u);
	EXPECT_EQ(removed->area, MemoryArea::Conv);
	EXPECT_EQ(removed->owner_psp, 0x0850);
}

TEST_F(AllocRegistryTest, AddDefaultsOwnerPspToZeroWhenNotGiven)
{
	// The Xms case: MEM_AllocatePages has no PSP concept, so
	// AllocMemoryCommand::Execute never passes one for that area.
	reg.Add(0x1000, 256, MemoryArea::Xms);
	const auto removed = reg.Remove(0x1000);
	ASSERT_TRUE(removed);
	EXPECT_EQ(removed->owner_psp, 0);
}

TEST(AreaNameTest, MapsEveryMemoryAreaToItsRequestBodyName)
{
	EXPECT_EQ(AreaName(MemoryArea::Conv), "CONV");
	EXPECT_EQ(AreaName(MemoryArea::Uma), "UMA");
	EXPECT_EQ(AreaName(MemoryArea::Xms), "XMS");
}

} // namespace
