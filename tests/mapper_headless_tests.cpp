// SPDX-FileCopyrightText:  2025 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "gui/mapper.h"

namespace {

// MAPPER_DisplayUI's event loop only ends on a click or a keypress, so opening
// it on a driver with neither wedges the emulation thread for good. This
// predicate is what keeps that from happening; a false negative hangs the
// emulator, so the matching has to be exact about which names are safe.

TEST(MapperHeadlessDriver, DummyIsHeadless)
{
	EXPECT_TRUE(MAPPER_IsHeadlessVideoDriver("dummy"));
}

TEST(MapperHeadlessDriver, OffscreenIsHeadless)
{
	EXPECT_TRUE(MAPPER_IsHeadlessVideoDriver("offscreen"));
}

TEST(MapperHeadlessDriver, MatchingIsCaseInsensitive)
{
	EXPECT_TRUE(MAPPER_IsHeadlessVideoDriver("DUMMY"));
	EXPECT_TRUE(MAPPER_IsHeadlessVideoDriver("Dummy"));
	EXPECT_TRUE(MAPPER_IsHeadlessVideoDriver("OffScreen"));
}

TEST(MapperHeadlessDriver, UnnamedDriverIsTreatedAsHeadless)
{
	// SDL_GetCurrentVideoDriver() returning null reaches here as an empty
	// view. Refusing is the safe default: opening a mapper we cannot close
	// costs the whole emulator, declining one costs a warning.
	EXPECT_TRUE(MAPPER_IsHeadlessVideoDriver(""));
}

TEST(MapperHeadlessDriver, InteractiveDriversAreNotHeadless)
{
	for (const auto* driver : {"cocoa", "x11", "wayland", "windows", "kmsdrm"}) {
		EXPECT_FALSE(MAPPER_IsHeadlessVideoDriver(driver)) << driver;
	}
}

TEST(MapperHeadlessDriver, DoesNotMatchOnSubstrings)
{
	// A driver whose name merely contains one of the headless names still
	// has a real display; refusing there would break the mapper for it.
	EXPECT_FALSE(MAPPER_IsHeadlessVideoDriver("dummy2"));
	EXPECT_FALSE(MAPPER_IsHeadlessVideoDriver("notdummy"));
	EXPECT_FALSE(MAPPER_IsHeadlessVideoDriver("offscreen_gl"));
}

TEST(MapperHeadlessDriver, HandlesEmbeddedNulAndPadding)
{
	EXPECT_FALSE(MAPPER_IsHeadlessVideoDriver(std::string_view("dummy\0x", 7)));
	EXPECT_FALSE(MAPPER_IsHeadlessVideoDriver(" dummy"));
	EXPECT_FALSE(MAPPER_IsHeadlessVideoDriver("dummy "));
}

} // namespace
