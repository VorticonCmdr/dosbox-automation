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
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	bool stepped       = false;
	bool debugging     = false;
	DebugStopInfo stop = {};
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

#if C_DEBUGGER

class DebugAddBreakpointCommand : public Command {
public:
	DebugAddBreakpointCommand(DebugBreakpointType type, uint16_t segment,
	                          uint32_t offset, uint8_t int_num, uint16_t ah,
	                          uint16_t al)
	        : type(type), segment(segment), offset(offset),
	          int_num(int_num), ah(ah), al(al)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response& res);

	// Filled by Execute() from the engine after insertion, so index/active
	// reflect reality rather than being assumed by the caller.
	DebugBreakpointInfo result = {};

private:
	DebugBreakpointType type;
	uint16_t segment = 0;
	uint32_t offset  = 0;
	uint8_t int_num  = 0;
	uint16_t ah      = 0;
	uint16_t al      = 0;
};

class DebugListBreakpointsCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);

	std::vector<DebugBreakpointInfo> breakpoints = {};
};

class DebugDeleteBreakpointCommand : public Command {
public:
	DebugDeleteBreakpointCommand(bool delete_all, uint16_t index)
	        : delete_all(delete_all), index(index)
	{}

	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response& res);

private:
	bool delete_all = false;
	uint16_t index  = 0;

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

#endif // C_DEBUGGER

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DEBUG_H
