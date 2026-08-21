// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "bridge.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace Webserver {

namespace {
uint64_t SteadyNowMs()
{
	return static_cast<uint64_t>(
	        std::chrono::duration_cast<std::chrono::milliseconds>(
	                std::chrono::steady_clock::now().time_since_epoch())
	                .count());
}
} // namespace

Bridge& Bridge::Instance()
{
	static Bridge instance;
	return instance;
}

Bridge::Bridge()
{
	// Treat "never pumped yet" as fresh, not stale: WEBSERVER_Init() can
	// start accepting connections slightly before normal_loop()'s first
	// tick, and a command queued in that window should get its normal
	// timeout, not an immediate not_pumping refusal.
	last_pump_ms = SteadyNowMs();
}

void Command::WaitForCompletion(const uint32_t timeout_ms)
{
	Bridge::Instance().ExecuteCommand(*this, timeout_ms);
}

void Bridge::ExecuteCommand(Command& cmd, const uint32_t timeout_ms)
{
	if (PumpAgeMs() > StalePumpThresholdMs) {
		throw BridgeNotPumping(
		        "The emulation thread has not processed requests "
		        "recently - it may be paused, minimized, or unresponsive");
	}

	std::unique_lock<std::mutex> lock(mtx);

	if (queue.size() >= MaxQueueDepth) {
		throw BridgeQueueFull(
		        "Too many commands are already queued for the "
		        "emulation thread");
	}

	cmd.done = false;
	queue.push_back(&cmd);

	bool success = cv.wait_for(lock,
	                           std::chrono::milliseconds(timeout_ms),
	                           [&] { return cmd.done; });

	if (!success) {
		auto it = std::find(queue.begin(), queue.end(), &cmd);
		if (it != queue.end()) {
			queue.erase(it);
		}
		throw BridgeTimeout(
		        "Command execution timed out - the emulator may be "
		        "paused, minimized, or unresponsive");
	}
}

void Bridge::ProcessRequests()
{
	std::lock_guard<std::mutex> lock(mtx);

	last_pump_ms = SteadyNowMs();

	for (auto* cmd : queue) {
		try {
			cmd->Execute();
		} catch (const std::exception& e) {
			cmd->error = e.what();
		} catch (...) {
			cmd->error = "Unknown error executing command";
		}
		cmd->done = true;
	}

	queue.clear();
	cv.notify_all();
}

uint64_t Bridge::PumpAgeMs() const
{
	return SteadyNowMs() - last_pump_ms.load();
}

size_t Bridge::QueueDepth() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return queue.size();
}

} // namespace Webserver
