// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "hardware/input/mouse.h"

#include <gtest/gtest.h>

// MOUSEDOS_GetPosition/GetButtons/SetPosition all guard on
// MOUSEDOS_IsDriverStarted(), which is false until MOUSEDOS_StartDriver()
// carves out a state segment in guest memory (DOS_CreateFakeTsrArea) - not
// something a bare unit-test binary ever boots. Nothing else in this test
// binary calls MOUSEDOS_StartDriver(), so the driver-not-started branch is
// the only one of these three functions' behaviour this binary can
// exercise; the driver-started success path (real state-segment reads and
// writes) is covered by live verification against a running binary
// instead, matching this module's existing all-integration-tested
// precedent for anything that depends on real emulator state.

TEST(MouseDosPosition, DriverStartsNotStarted)
{
	ASSERT_FALSE(MOUSEDOS_IsDriverStarted());
}

TEST(MouseDosPosition, GetPositionIsNulloptWhenDriverNotStarted)
{
	EXPECT_FALSE(MOUSEDOS_GetPosition().has_value());
}

TEST(MouseDosPosition, GetButtonsIsNulloptWhenDriverNotStarted)
{
	EXPECT_FALSE(MOUSEDOS_GetButtons().has_value());
}

TEST(MouseDosPosition, SetPositionFailsWhenDriverNotStarted)
{
	EXPECT_FALSE(MOUSEDOS_SetPosition(100, 50));
}
