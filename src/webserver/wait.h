// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_WAIT_H
#define DOSBOX_WEBSERVER_WAIT_H

#include "libs/http/http.h"
#include "libs/json/json.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace Webserver {

// Thrown by WaitRegistry::WaitFor when MaxWaiters callers are already
// blocked in POST /api/v1/wait. Distinct from BridgeQueueFull: this
// registry never touches the Bridge - evaluation runs directly off the
// frame hook, DEBUG_Loop and the SDL pause loops, not a Command.
class TooManyWaiters : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

enum class WaitCondition {
	Text,
	ScreenChange,
	Frames,
	ReplayDone,
	Memory,
	Stopped,
	ScriptDone,
	Program,
};

enum class HashSource { Text, Frame };
enum class MemoryCompareOp { Eq, Ne, Lt, Gt, Le, Ge };

// Clamped well under DosboxClient's 30s HTTP timeout so the server-side
// deadline always fires first, not the transport.
constexpr uint32_t MinWaitTimeoutMs     = 1;
constexpr uint32_t MaxWaitTimeoutMs     = 15000;
constexpr uint32_t DefaultWaitTimeoutMs = 5000;

constexpr size_t MaxPatternLen = 256;
constexpr size_t MaxWaiters    = 4;

struct WaitSpec {
	WaitCondition condition = WaitCondition::Text;
	uint32_t timeout_ms     = DefaultWaitTimeoutMs;

	// Text, Program (pattern form)
	std::string pattern = {};
	bool ignore_case    = false;

	// ScreenChange
	uint64_t baseline_hash = 0;
	HashSource hash_source = HashSource::Text;

	// Frames - resolved to an absolute target at registration so Tick()
	// only ever does a >= compare, never sees the caller's relative count.
	uint64_t target_frame = 0;

	// Memory
	uint32_t mem_addr      = 0;
	int mem_width          = 1;
	uint64_t mem_value     = 0;
	MemoryCompareOp mem_op = MemoryCompareOp::Eq;

	// Program (edge-triggered form, when pattern is empty)
	bool has_baseline_program    = false;
	std::string baseline_program = {};
};

struct WaitOutcome {
	bool satisfied             = false;
	std::string reason         = "timeout";
	nlohmann::json observation = nlohmann::json::object();
};

// Parsing/validation, exposed for testing. Throws std::invalid_argument
// on anything malformed - the same convention num_param() uses, caught
// by webserver.cpp's error_handler and turned into a 400. Pure: the
// live frame count and current program name are supplied by the caller
// (read on the web thread before registering) rather than read here.
WaitCondition ParseWaitCondition(const std::string& value);
uint64_t ParseHexHash(const std::string& value);
WaitSpec ParseWaitRequest(const nlohmann::json& body, uint64_t current_frame,
                          const std::string& current_program);

class WaitRegistry {
public:
	static WaitRegistry& Instance();

	// Called from the web thread (WaitHandlers::Post). Registers spec,
	// blocks the calling thread up to spec.timeout_ms, returns the
	// outcome. Throws TooManyWaiters if MaxWaiters are already
	// registered.
	WaitOutcome WaitFor(const WaitSpec& spec);

	// Called from the emulation thread: the frame hook
	// (frames_flowing=true), and DEBUG_Loop / the SDL pause loops
	// (frames_flowing=false, since no frames are presented in either).
	// Evaluates every registered waiter against current emulator
	// state, reading any expensive shared resource (the screen text)
	// at most once per call regardless of how many waiters need it.
	void Tick(bool frames_flowing);

	// Wakes every waiter with an unsatisfied outcome so in-flight
	// httplib worker threads don't block WEBSERVER_Destroy's
	// server.stop() from joining.
	void DrainAll();

private:
	struct Waiter {
		WaitSpec spec;
		bool done = false;
		WaitOutcome outcome;
	};

	std::mutex mtx;
	std::condition_variable cv;
	std::vector<Waiter*> waiters;

	WaitRegistry()                               = default;
	WaitRegistry(const WaitRegistry&)            = delete;
	WaitRegistry& operator=(const WaitRegistry&) = delete;
};

// Thin wrapper so callers outside webserver/ (sdl_gui.cpp, debugger.cpp)
// don't need to know about the WaitRegistry singleton directly.
void EvaluateWaits(bool frames_flowing);

struct WaitHandlers {
	static void Post(const httplib::Request& req, httplib::Response& res);
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_WAIT_H
