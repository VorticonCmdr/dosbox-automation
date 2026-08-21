// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/bridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Webserver::Bridge;
using Webserver::BridgeNotPumping;
using Webserver::BridgeQueueFull;
using Webserver::BridgeTimeout;
using Webserver::Command;
using Webserver::MaxQueueDepth;
using Webserver::StalePumpThresholdMs;

namespace {

struct ThrowingCommand : Command {
	void Execute() override
	{
		throw std::runtime_error("boom");
	}
};

struct CountingCommand : Command {
	void Execute() override
	{
		executed = true;
	}
	bool executed = false;
};

struct BlockingCommand : Command {
	void Execute() override {}
};

// Every test refreshes the pump timestamp itself rather than relying on
// leftover state from whichever test ran before it - Bridge is a
// process-wide singleton shared across this whole binary.
void RefreshPump()
{
	Bridge::Instance().ProcessRequests();
}

TEST(BridgeTest, ThrowingExecuteStillDrainsTheBatch)
{
	RefreshPump();

	ThrowingCommand a;
	CountingCommand b;

	std::thread ta([&] { a.WaitForCompletion(2000); });
	std::thread tb([&] { b.WaitForCompletion(2000); });

	while (Bridge::Instance().QueueDepth() < 2) {
		std::this_thread::sleep_for(1ms);
	}
	Bridge::Instance().ProcessRequests();

	ta.join();
	tb.join();

	// b queued after the throwing command a - its Execute() must still
	// run, and the batch must still fully drain (done set, queue
	// cleared, waiters notified) despite a's exception.
	EXPECT_TRUE(b.executed);
	EXPECT_FALSE(a.error.empty());
	EXPECT_NE(a.error.find("boom"), std::string::npos);
}

TEST(BridgeTest, TimedOutCommandIsErasedAndNeverExecutedAfterwards)
{
	RefreshPump();

	CountingCommand cmd;
	EXPECT_THROW(cmd.WaitForCompletion(10), BridgeTimeout);

	// A pump well after the deadline must not reach back and execute a
	// command whose caller already gave up - ExecuteCommand erases a
	// timed-out command from the queue before returning.
	Bridge::Instance().ProcessRequests();
	EXPECT_FALSE(cmd.executed);
}

TEST(BridgeTest, QueueFullIsRefusedWithoutDroppingQueuedCommands)
{
	RefreshPump();

	std::vector<std::unique_ptr<BlockingCommand>> blockers;
	std::vector<std::thread> threads;
	for (size_t i = 0; i < MaxQueueDepth; ++i) {
		blockers.push_back(std::make_unique<BlockingCommand>());
		auto* cmd = blockers.back().get();
		threads.emplace_back([cmd] { cmd->WaitForCompletion(5000); });
	}

	while (Bridge::Instance().QueueDepth() < MaxQueueDepth) {
		std::this_thread::sleep_for(1ms);
	}

	CountingCommand overflow;
	EXPECT_THROW(overflow.WaitForCompletion(1000), BridgeQueueFull);

	// The MaxQueueDepth commands already accepted must still make it
	// through a later pump - refusing the new arrival must not corrupt
	// or drop what was already queued.
	Bridge::Instance().ProcessRequests();
	for (auto& t : threads) {
		t.join();
	}
	for (auto& b : blockers) {
		EXPECT_TRUE(b->error.empty());
	}
}

TEST(BridgeTest, StalePumpIsRefusedFastWithoutQueuing)
{
	// Let the pump go quiet long enough to cross the staleness
	// threshold, then confirm ExecuteCommand refuses immediately -
	// never enqueues, never waits out the command's own timeout -
	// rather than discovering the same fact 5 seconds later.
	std::this_thread::sleep_for(
	        std::chrono::milliseconds(StalePumpThresholdMs + 100));

	CountingCommand cmd;
	const auto start = std::chrono::steady_clock::now();
	EXPECT_THROW(cmd.WaitForCompletion(5000), BridgeNotPumping);
	const auto elapsed = std::chrono::steady_clock::now() - start;

	EXPECT_LT(elapsed, 1000ms);
	EXPECT_EQ(Bridge::Instance().QueueDepth(), 0u);

	RefreshPump(); // leave the singleton fresh for tests that run after
	               // this one
}

} // namespace
