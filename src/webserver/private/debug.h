// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_DEBUG_H
#define DOSBOX_WEBSERVER_DEBUG_H

#include "webserver/bridge.h"
#include "webserver/debug_events.h"

#include "dosbox.h"
#if C_DEBUGGER
#include "debugger/debugger.h"
#endif

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "http/http.h"

namespace Webserver {

class DebugStatusCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);

	bool debugging     = false;
	DebugStopInfo stop = {};
};

class DebugPauseCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	bool debugging     = false;
	DebugStopInfo stop = {};
};

class DebugContinueCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	bool resumed   = false;
	bool debugging = false;
	// The stop being resumed FROM, not a new one - DEBUG_Resume executes
	// one instruction and arms breakpoints synchronously, but the next
	// actual stop happens arbitrarily later. Callers poll GET
	// /api/v1/debug/wait?since_stop_id=<this> for it.
	uint64_t resumed_from_stop_id = 0;
};

class DebugStepCommand : public Command {
public:
	DebugStepCommand(int32_t count = 1) : count(count) {}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	bool stepped       = false;
	bool debugging     = false;
	DebugStopInfo stop = {};

private:
	// Only read by Execute()'s #if C_DEBUGGER body (debug.cpp) - the
	// #else stub Execute() is a no-op, so a non-debugger build never
	// reads this field.
	[[maybe_unused]] int32_t count = 1;
};

// A step-over/run-to-address doesn't return a new stop record the way
// DebugStepCommand does - DEBUG_StepOver/DEBUG_RunToAddress plant a
// one-shot breakpoint and resume, so the actual stop happens arbitrarily
// later (same reasoning as DebugContinueCommand::resumed_from_stop_id).
// The immediate response only says whether the resume was armed;
// DebugStepOverCommand additionally falls back to a plain step (with its
// own synchronous stop record) when the current instruction isn't a
// call/int/loop/rep, matching the F10 key handler's own fallthrough to F11.
class DebugStepOverCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	bool stepped_over = false;
	// Only set (via a DEBUG_SingleStep fallback) when stepped_over is
	// false and the emulator was paused to begin with.
	bool stepped                  = false;
	bool debugging                = false;
	uint64_t resumed_from_stop_id = 0;
	DebugStopInfo stop            = {};
};

// Never a Command: the wait itself blocks the calling httplib worker
// thread on DebugEvents' condvar, exactly like WaitHandlers::Post (wait.h)
// - a Bridge Command here would freeze the emulation thread for the
// whole wait. Declared unconditionally (matching the four classes above):
// the real long-poll lives in debug.cpp's #if C_DEBUGGER block, a 501
// NotBuilt stub in its #else.
struct DebugWaitHandlers {
	static void Get(const httplib::Request&, httplib::Response& res);
};

// Same "actual stop happens later" shape as DebugStepOverCommand, without
// the disassembly-based detection - the target address is explicit.
class DebugRunToCommand : public Command {
public:
	DebugRunToCommand(uint16_t segment, uint32_t offset)
	        : segment(segment),
	          offset(offset)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	bool started                  = false;
	uint64_t resumed_from_stop_id = 0;

private:
	// Same reasoning as DebugStepCommand::count above.
	[[maybe_unused]] uint16_t segment = 0;
	[[maybe_unused]] uint32_t offset  = 0;
};

// Steps out of the current frame: backtraces (2.7) to the caller's return
// address and delegates to DEBUG_RunToAddress - same "actual stop happens
// later" shape as DebugRunToCommand. Declining (started stays false while
// paused) isn't an error: it means the backtrace couldn't resolve the
// caller's frame with high confidence, so there's nowhere trustworthy to
// aim the planted breakpoint.
class DebugStepOutCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	bool paused                   = false;
	bool started                  = false;
	uint64_t resumed_from_stop_id = 0;
};

#if C_DEBUGGER

class DebugAddBreakpointCommand : public Command {
public:
	DebugAddBreakpointCommand(DebugBreakpointType type, uint16_t segment,
	                          uint32_t offset, uint8_t int_num, uint16_t ah,
	                          uint16_t al, bool once = false,
	                          int32_t ignore_count = 0,
	                          const DebugBreakpointCondition& condition = {},
	                          DebugMemoryTrigger trigger = DebugMemoryTrigger::Write)
	        : type(type),
	          segment(segment),
	          offset(offset),
	          int_num(int_num),
	          ah(ah),
	          al(al),
	          once(once),
	          ignore_count(ignore_count),
	          condition(condition),
	          trigger(trigger)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	// Filled by Execute() from the engine after insertion, so index/active
	// reflect reality rather than being assumed by the caller.
	DebugBreakpointInfo result = {};
	// Set instead of adding when this breakpoint's location (address, or
	// int/ah/al with wildcard overlap) already has another breakpoint
	// there and either one carries an ignore_count or condition -
	// CheckBreakpoint/CheckIntBreakpoint only ever act on the first match
	// in list order, so a second breakpoint at the same location would
	// silently starve whichever one's ignore_count/condition never gets
	// consulted.
	bool conflict = false;

private:
	DebugBreakpointType type;
	uint16_t segment                   = 0;
	uint32_t offset                    = 0;
	uint8_t int_num                    = 0;
	uint16_t ah                        = 0;
	uint16_t al                        = 0;
	bool once                          = false;
	int32_t ignore_count               = 0;
	DebugBreakpointCondition condition = {};
	// Only consulted when type == DebugBreakpointType::Memory.
	DebugMemoryTrigger trigger = DebugMemoryTrigger::Write;
};

class DebugAddWatchCommand : public Command {
public:
	DebugAddWatchCommand(std::string name, uint16_t segment, uint32_t offset)
	        : name(std::move(name)),
	          segment(segment),
	          offset(offset)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	// Filled by Execute() by reading the newly-inserted watch back from
	// the engine, same reasoning as DebugAddBreakpointCommand::result.
	DebugWatchInfo result = {};

private:
	std::string name;
	uint16_t segment = 0;
	uint32_t offset  = 0;
};

class DebugListWatchesCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);

	std::vector<DebugWatchInfo> watches = {};
};

class DebugDeleteWatchCommand : public Command {
public:
	// address is only consulted when all_watches is false.
	DebugDeleteWatchCommand(bool all_watches, uint32_t address)
	        : all_watches(all_watches),
	          address(address)
	{}

	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response& res);

	bool deleted = false;

private:
	bool all_watches = false;
	uint32_t address = 0;
};

class DebugListBreakpointsCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);

	std::vector<DebugBreakpointInfo> breakpoints = {};
};

class DebugDeleteBreakpointCommand : public Command {
public:
	enum class By { All, Index, Id };

	DebugDeleteBreakpointCommand(By by, uint64_t value)
	        : by(by),
	          value(value)
	{}

	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response& res);

private:
	By by          = By::All;
	uint64_t value = 0;

public:
	bool deleted = false;
};

#else // !C_DEBUGGER

class DebugAddBreakpointCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);
};

class DebugListBreakpointsCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);
};

class DebugDeleteBreakpointCommand : public Command {
public:
	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response& res);
};

class DebugAddWatchCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);
};

class DebugListWatchesCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);
};

class DebugDeleteWatchCommand : public Command {
public:
	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response& res);
};

#endif // C_DEBUGGER

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DEBUG_H
