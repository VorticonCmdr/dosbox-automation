// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_BRIDGE_H
#define DOSBOX_WEBSERVER_BRIDGE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace Webserver {

// Thrown by Bridge::ExecuteCommand when the emulation thread does not
// pump the queue within the deadline. This is the *routine* failure -
// a paused or minimized emulator never calls ProcessRequests() - not a
// crash, so error_handler (webserver.cpp) maps it to a distinct,
// retryable response instead of a generic 500.
class BridgeTimeout : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Thrown by Bridge::ExecuteCommand when the pump is already known stale
// (PumpAgeMs() past StalePumpThresholdMs) before a command is even
// queued - a fast, distinguishable failure instead of queuing a command
// that would just time out the same way, tying up an httplib worker
// thread for the full deadline to learn what was already known.
class BridgeNotPumping : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Thrown by Bridge::ExecuteCommand when MaxQueueDepth commands are
// already queued. In steady state, depth never exceeds httplib's worker
// pool size (one Command in flight per blocked worker thread) - this is
// a backstop against a pathological caller flood, not a cap reachable
// under normal load.
class BridgeQueueFull : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

constexpr size_t MaxQueueDepth = 64;

// Command::WaitForCompletion's default deadline. Many call sites override
// this for a specific command that's known to run long (e.g. a 2000ms
// memory search); this is the number a caller sees when it doesn't.
constexpr uint32_t DefaultBridgeTimeoutMs = 250;

// How long the pump can go unserviced before ExecuteCommand refuses new
// commands outright. normal_loop() calls ProcessRequests() once per
// PIC_RunQueue() tick (sub-millisecond in practice), and the SDL pause
// loops poll it every PausePumpIntervalMs (sdl_gui.cpp) - a full second
// of silence only happens when nothing is pumping at all.
constexpr uint64_t StalePumpThresholdMs = 1000;

class Command {
public:
	virtual ~Command() {}
	virtual void Execute() = 0;

	void WaitForCompletion(const uint32_t timeout_ms = DefaultBridgeTimeoutMs);

	// Set by Execute() to report errors without throwing on the
	// emulation thread, or by ProcessRequests() when Execute() itself
	// threw. Handlers should check this after WaitForCompletion() and
	// throw on the webserver thread.
	std::string error = {};

private:
	friend class Bridge;

	bool done = false;
};

class Bridge {
public:
	static Bridge& Instance();

	// Called by the web server thread
	void ExecuteCommand(Command& cmd, const uint32_t timeout_ms);

	// Called by the main thread running the CPU emulation
	void ProcessRequests();

	// Milliseconds since the last ProcessRequests() call, regardless of
	// whether it had work to do. Backs both ExecuteCommand's fast-fail
	// check and GET /api/v1/status's last_tick_ms_ago.
	uint64_t PumpAgeMs() const;

	// Commands currently queued, waiting for a pump. Exposed for testing.
	size_t QueueDepth() const;

private:
	mutable std::mutex mtx             = {};
	std::condition_variable cv         = {};
	std::vector<Command*> queue        = {};
	std::atomic<uint64_t> last_pump_ms = {0};

	Bridge(const Bridge&)            = delete;
	Bridge& operator=(const Bridge&) = delete;
	Bridge();
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_BRIDGE_H
