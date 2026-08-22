// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/debug_events.h"

#include "webserver/wait.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Webserver::DebugEvents;
using Webserver::DebugStopBreakpoint;
using Webserver::DebugStopInfo;
using Webserver::MaxWaiters;
using Webserver::TooManyWaiters;

namespace {

// DebugEvents::Instance() is a process-wide singleton shared across this
// whole test binary, exactly like Bridge::Instance() and
// WaitRegistry::Instance() elsewhere in this suite - never assume an
// absolute stop_id (e.g. "starts at 0"), only relative behaviour around a
// baseline read at the start of each test. PublishDebugStop() itself
// (which reads live CPU/memory state) is deliberately not exercised here:
// this binary never boots BIOS/VGA/memory, only DebugEvents' own
// publish/wait mechanics are in scope, matching webserver_wait_tests.cpp's
// own documented scoping.

TEST(DebugStopInfoTest, DefaultConstructedReasonIsNeverStoppedNotAnEmptyString)
{
	// A plain construction, not through the shared DebugEvents singleton
	// (which other tests in this process may have already published
	// into) - this checks the type's own default, unconditionally safe
	// regardless of test execution order.
	const DebugStopInfo info;
	EXPECT_EQ(info.reason, "never_stopped");
	EXPECT_FALSE(info.debugging);
}

TEST(DebugEventsTest, PublishReturnsAMonotonicallyIncreasingStopId)
{
	auto& events = DebugEvents::Instance();

	DebugStopInfo a;
	a.reason = "paused";
	DebugStopInfo b;
	b.reason = "step";

	const auto id1 = events.Publish(a);
	const auto id2 = events.Publish(b);

	EXPECT_GT(id2, id1);
}

TEST(DebugEventsTest, CurrentReflectsTheLastPublishedRecord)
{
	auto& events = DebugEvents::Instance();

	DebugStopInfo info;
	info.reason      = "breakpoint";
	info.debugging   = true;
	info.linear_eip  = 0x1234;
	info.core        = "normal";
	info.breakpoint  = DebugStopBreakpoint{};
	info.breakpoint->type = "execute";
	info.breakpoint->segment = 0x100;
	info.breakpoint->offset  = 0x50;

	const auto id      = events.Publish(info);
	const auto current = events.Current();

	EXPECT_EQ(current.stop_id, id);
	EXPECT_EQ(current.reason, "breakpoint");
	EXPECT_TRUE(current.debugging);
	EXPECT_EQ(current.linear_eip, 0x1234u);
	ASSERT_TRUE(current.breakpoint.has_value());
	EXPECT_EQ(current.breakpoint->type, "execute");
	EXPECT_EQ(current.breakpoint->segment, 0x100);
}

TEST(DebugEventsTest, WaitForReturnsImmediatelyWhenAlreadyPastSinceStopId)
{
	auto& events = DebugEvents::Instance();

	DebugStopInfo info;
	info.reason  = "paused";
	const auto id = events.Publish(info);

	const auto start   = std::chrono::steady_clock::now();
	const auto result  = events.WaitFor(id - 1, 5000);
	const auto elapsed = std::chrono::steady_clock::now() - start;

	EXPECT_TRUE(result.satisfied);
	EXPECT_EQ(result.info.stop_id, id);
	EXPECT_LT(elapsed, 1000ms);
}

TEST(DebugEventsTest, WaitForTimesOutWhenNothingNewIsPublished)
{
	auto& events = DebugEvents::Instance();

	const auto baseline = events.Current().stop_id;
	const auto result   = events.WaitFor(baseline, 50);

	EXPECT_FALSE(result.satisfied);
	EXPECT_EQ(result.info.stop_id, baseline);
}

TEST(DebugEventsTest, WaitForWakesWhenANewerStopIsPublishedConcurrently)
{
	auto& events = DebugEvents::Instance();

	const auto baseline = events.Current().stop_id;

	std::thread publisher([&] {
		std::this_thread::sleep_for(50ms);
		DebugStopInfo info;
		info.reason = "step";
		events.Publish(info);
	});

	const auto result = events.WaitFor(baseline, 5000);
	publisher.join();

	EXPECT_TRUE(result.satisfied);
	EXPECT_GT(result.info.stop_id, baseline);
}

TEST(DebugEventsTest, NthPlusOneConcurrentWaiterIsRefusedWithoutBlocking)
{
	auto& events = DebugEvents::Instance();

	const auto baseline = events.Current().stop_id;

	std::vector<std::thread> blockers;
	std::vector<DebugEvents::WaitResult> outcomes(MaxWaiters);
	for (size_t i = 0; i < MaxWaiters; ++i) {
		blockers.emplace_back(
		        [&, i] { outcomes[i] = events.WaitFor(baseline, 5000); });
	}

	// Give all MaxWaiters callers a chance to register before the
	// overflow attempt - registration only takes a lock, matching the
	// identical pattern in WaitRegistryTest.
	// FifthConcurrentWaiterIsRefusedWithoutBlocking (webserver_wait_tests.cpp).
	std::this_thread::sleep_for(50ms);

	EXPECT_THROW(events.WaitFor(baseline, 5000), TooManyWaiters);

	// Resolve the MaxWaiters legitimate waiters instead of leaving them
	// to time out.
	DebugStopInfo info;
	info.reason = "paused";
	events.Publish(info);

	for (auto& t : blockers) {
		t.join();
	}
	for (const auto& outcome : outcomes) {
		EXPECT_TRUE(outcome.satisfied);
	}
}

TEST(DebugEventsTest, DrainAllWakesPendingWaitersWithoutSatisfyingThem)
{
	auto& events = DebugEvents::Instance();

	const auto baseline = events.Current().stop_id;

	std::thread blocker([&] {
		const auto result = events.WaitFor(baseline, 5000);
		EXPECT_FALSE(result.satisfied);
	});

	std::this_thread::sleep_for(50ms);
	events.DrainAll();
	blocker.join();
}

} // namespace
