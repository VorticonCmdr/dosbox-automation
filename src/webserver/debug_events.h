// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_DEBUG_EVENTS_H
#define DOSBOX_WEBSERVER_DEBUG_EVENTS_H

#include "private/cpu.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace Webserver {

// A breakpoint's shape as reported inside a stop record. Deliberately
// self-contained rather than reusing debugger.h's DebugBreakpointInfo, so
// this header stays includable regardless of C_DEBUGGER; debugger.cpp
// converts its own DebugBreakpointInfo into this when publishing a stop.
struct DebugStopBreakpoint {
	std::string type = {}; // "execute" | "interrupt" | "memory"
	uint16_t segment  = 0;
	uint32_t offset   = 0;
	uint8_t int_num   = 0;
	uint16_t ah       = 0;
	uint16_t al       = 0;
	// The breakpoint's position in DEBUG_ListBreakpoints() at the moment
	// it fired - not a stable identifier (see debugger.h's own caveat on
	// DebugBreakpointInfo::index; a real stable id is 2.4's job).
	uint16_t index = 0;
	bool once      = false;
};

struct DebugStopInfo {
	uint64_t stop_id = 0;
	// "never_stopped" is this struct's own default (before Publish() has
	// ever run, e.g. a freshly started server the debugger hasn't
	// touched yet) - never published by PublishDebugStop, which always
	// sets one of "paused" | "breakpoint" | "step".
	std::string reason  = "never_stopped";
	Registers registers = {};
	uint32_t linear_eip = 0;
	bool protected_mode = false;
	std::string core    = {}; // CoreKind (cpu.h), rendered as a string
	// Whether the debugger is (still) paused as of this record - captured
	// at publish time on the emulation thread, same as everything else
	// here. Distinct from a live DEBUG_IsDebugging() read taken later on
	// the web thread, which could disagree if a debug/continue or a
	// fresh breakpoint hit lands in between (see DebugWaitHandlers::Get).
	bool debugging = false;
	std::optional<DebugStopBreakpoint> breakpoint = std::nullopt;
	std::array<uint8_t, 16> code_bytes            = {};
};

// Builds a full DebugStopInfo from live emulator state (registers, CPU
// mode and core, 16 bytes at CS:EIP) and publishes it. Called from the
// emulation thread (DEBUG_Enable, DEBUG_SingleStep) - callers only supply
// the reason, whether the debugger is still paused (the caller already
// knows this - it's the same state that decided the reason), and, for a
// breakpoint-triggered stop, which breakpoint fired. Returns the assigned
// stop_id.
uint64_t PublishDebugStop(const std::string& reason, bool debugging,
                          std::optional<DebugStopBreakpoint> breakpoint);

// Mirrors WaitRegistry's shape (wait.h): a web-thread condvar wait fed
// from the emulation thread, never a Bridge Command - debug/wait's long
// poll would otherwise freeze the emulation thread for its whole
// duration. A distinct registry from WaitRegistry because it publishes a
// structured record on every stop rather than evaluating a predicate.
class DebugEvents {
public:
	static DebugEvents& Instance();

	// Called from the emulation thread. Assigns a monotonic stop_id,
	// stores the record as the latest, wakes any waiter whose
	// since_stop_id is now satisfied. Returns the assigned stop_id.
	uint64_t Publish(DebugStopInfo info);

	struct WaitResult {
		bool satisfied     = false;
		DebugStopInfo info = {};
	};

	// Called from a web thread (DebugWaitHandlers::Get). Blocks up to
	// timeout_ms until a stop newer than since_stop_id is published.
	// info is always the latest known record, even on timeout, so a
	// caller always has something to show; satisfied is false only on a
	// genuine timeout or shutdown.
	WaitResult WaitFor(uint64_t since_stop_id, uint32_t timeout_ms);

	// The latest published record, or a default-constructed one
	// (stop_id 0) if nothing has been published since startup. Used by
	// the status/pause/continue/step responses.
	DebugStopInfo Current();

	// Wakes every blocked WaitFor call so WEBSERVER_Destroy's
	// server.stop() doesn't block on an in-flight long poll.
	void DrainAll();

private:
	mutable std::mutex mtx;
	std::condition_variable cv;
	size_t waiter_count  = 0;
	uint64_t next_stop_id = 0;
	DebugStopInfo latest  = {};
	bool draining         = false;

	DebugEvents()                              = default;
	DebugEvents(const DebugEvents&)            = delete;
	DebugEvents& operator=(const DebugEvents&) = delete;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DEBUG_EVENTS_H
