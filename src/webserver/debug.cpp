// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/debug.h"
#include "wait.h"
#include "webserver.h"

#include "dosbox.h"

#if C_DEBUGGER
#include "debugger/debugger.h"
#endif

#include "base64/base64.h"
#include "json/json.h"

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

#if C_DEBUGGER

namespace {

json DebugStopToJson(const DebugStopInfo& stop)
{
	json j;
	j["stop_id"]        = stop.stop_id;
	j["reason"]         = stop.reason;
	j["registers"]      = stop.registers;
	j["linear_eip"]     = stop.linear_eip;
	j["protected_mode"] = stop.protected_mode;
	j["core"]           = stop.core;
	j["code_bytes"]     = base64::to_base64(
                std::string(reinterpret_cast<const char*>(stop.code_bytes.data()),
                            stop.code_bytes.size()));

	if (stop.breakpoint) {
		json bp;
		bp["type"]      = stop.breakpoint->type;
		bp["segment"]   = stop.breakpoint->segment;
		bp["offset"]    = stop.breakpoint->offset;
		bp["int"]       = stop.breakpoint->int_num;
		bp["ah"]        = stop.breakpoint->ah;
		bp["al"]        = stop.breakpoint->al;
		bp["index"]     = stop.breakpoint->index;
		bp["once"]      = stop.breakpoint->once;
		j["breakpoint"] = bp;
	} else {
		j["breakpoint"] = nullptr;
	}
	return j;
}

json BreakpointToJson(const DebugBreakpointInfo& bp)
{
	json j;
	j["index"]  = bp.index;
	j["once"]   = bp.once;
	j["active"] = bp.active;
	switch (bp.type) {
	case DebugBreakpointType::Interrupt:
		j["type"] = "interrupt";
		j["int"]  = bp.int_num;
		j["ah"]   = bp.ah;
		j["al"]   = bp.al;
		break;
	case DebugBreakpointType::Memory:
		j["type"]    = "memory";
		j["segment"] = bp.segment;
		j["offset"]  = bp.offset;
		break;
	case DebugBreakpointType::Execute:
	default:
		j["type"]    = "execute";
		j["segment"] = bp.segment;
		j["offset"]  = bp.offset;
		break;
	}
	return j;
}

uint16_t ByteOrWildcard(const json& j, const char* key)
{
	if (!j.contains(key)) {
		return DEBUG_BPINT_ANY;
	}
	const int v = j.at(key).get<int>();
	if (v < 0 || v > 0xFF) {
		throw std::invalid_argument(std::string(key) + " must be 0x00..0xFF");
	}
	return static_cast<uint16_t>(v);
}

uint16_t RequireU16(const json& j, const char* key)
{
	const int v = j.at(key).get<int>();
	if (v < 0 || v > 0xFFFF) {
		throw std::invalid_argument(std::string(key) +
		                            " must be 0x0000..0xFFFF");
	}
	return static_cast<uint16_t>(v);
}

} // namespace

void DebugStatusCommand::Execute()
{
	debugging = DEBUG_IsDebugging();
	stop      = DebugEvents::Instance().Current();
}

void DebugStatusCommand::Get(const Request&, Response& res)
{
	DebugStatusCommand cmd;
	cmd.WaitForCompletion();

	json j;
	j["debugging"] = cmd.debugging;
	j["stop"]      = DebugStopToJson(cmd.stop);
	send_json(res, j);
}

void DebugPauseCommand::Execute()
{
	DEBUG_Enable(true);
	debugging = DEBUG_IsDebugging();
	stop      = DebugEvents::Instance().Current();
}

void DebugPauseCommand::Post(const Request&, Response& res)
{
	DebugPauseCommand cmd;
	cmd.WaitForCompletion();

	if (!cmd.debugging) {
		res.status = httplib::StatusCode::InternalServerError_500;
	}

	json j;
	j["status"]    = cmd.debugging ? "ok" : "failed";
	j["debugging"] = cmd.debugging;
	j["stop"]      = DebugStopToJson(cmd.stop);
	send_json(res, j);
}

void DebugContinueCommand::Execute()
{
	// Capture what we're resuming FROM before DEBUG_Resume() runs - it
	// executes one instruction and arms breakpoints synchronously, but
	// the actual next stop happens arbitrarily later, published by
	// whatever runs next (DEBUG_Enable on a breakpoint hit, or nothing
	// at all if it just keeps running).
	resumed_from_stop_id = DebugEvents::Instance().Current().stop_id;
	resumed              = DEBUG_Resume();
	// Read back rather than assuming !resumed: DEBUG_Resume() returning
	// false means it was never paused to begin with, which is also
	// !debugging - assuming the negation happened to be right for that
	// case, but only because "wasn't paused" and "no longer paused"
	// collapse to the same debugging=false. Reading the real state keeps
	// this correct regardless of why resumed came back false.
	debugging = DEBUG_IsDebugging();
}

void DebugContinueCommand::Post(const Request&, Response& res)
{
	DebugContinueCommand cmd;
	cmd.WaitForCompletion();

	json j;
	j["status"]               = cmd.resumed ? "ok" : "not_paused";
	j["debugging"]            = cmd.debugging;
	j["resumed_from_stop_id"] = cmd.resumed_from_stop_id;
	send_json(res, j);
}

void DebugStepCommand::Execute()
{
	stepped   = DEBUG_SingleStep();
	debugging = DEBUG_IsDebugging();
	stop      = DebugEvents::Instance().Current();
}

void DebugStepCommand::Post(const Request&, Response& res)
{
	DebugStepCommand cmd;
	cmd.WaitForCompletion();

	json j;
	j["status"]    = cmd.stepped ? "ok" : "not_paused";
	j["debugging"] = cmd.debugging;
	j["stop"]      = DebugStopToJson(cmd.stop);
	send_json(res, j);
}

void DebugWaitHandlers::Get(const Request& req, Response& res)
{
	uint64_t since_stop_id = 0;
	if (req.has_param("since_stop_id")) {
		since_stop_id = num_param<uint64_t>(req, Source::Param, "since_stop_id");
	}

	uint32_t timeout_ms = DefaultWaitTimeoutMs;
	if (req.has_param("timeout_ms")) {
		timeout_ms = num_param<uint32_t>(req,
		                                 Source::Param,
		                                 "timeout_ms",
		                                 MinWaitTimeoutMs,
		                                 MaxWaitTimeoutMs);
	}

	const auto result = DebugEvents::Instance().WaitFor(since_stop_id, timeout_ms);

	json j         = DebugStopToJson(result.info);
	j["satisfied"] = result.satisfied;
	// On a genuine wakeup, report the debugging state captured together
	// with the rest of this stop record at publish time (result.info),
	// not a fresh, separate DEBUG_IsDebugging() read - the emulation
	// thread is free to run further debug transitions (a concurrent
	// debug/continue, a fresh breakpoint) between WaitFor() returning and
	// any later read on this (web) thread, which could make an
	// independent read disagree with the reason/stop_id it's paired
	// with. A timeout has no specific stop to be coherent with, so a live
	// read is exactly what's wanted there.
	j["debugging"] = result.satisfied ? result.info.debugging
	                                  : DEBUG_IsDebugging();
	send_json(res, j);
}

void DebugAddBreakpointCommand::Execute()
{
	switch (type) {
	case DebugBreakpointType::Execute:
		DEBUG_AddExecuteBreakpoint(segment, offset);
		break;
	case DebugBreakpointType::Interrupt:
		DEBUG_AddIntBreakpoint(int_num, ah, al);
		break;
	case DebugBreakpointType::Memory:
		DEBUG_AddMemBreakpoint(segment, offset);
		break;
	}

	// New breakpoints are always pushed to the front of the list, so the
	// one we just added is whatever is now at the front. Read it back
	// from the engine rather than assuming index/active/once here, since
	// those are the engine's to decide.
	auto all = DEBUG_ListBreakpoints();
	if (!all.empty()) {
		result = all.front();
	}
}

void DebugAddBreakpointCommand::Post(const Request& req, Response& res)
{
	auto j = json::parse(req.body);
	const auto type_str = j.at("type").get<std::string>();

	DebugBreakpointType type = DebugBreakpointType::Execute;
	uint16_t segment = 0;
	uint32_t offset  = 0;
	uint8_t int_num  = 0;
	uint16_t ah      = 0;
	uint16_t al      = 0;

	if (type_str == "execute") {
		type    = DebugBreakpointType::Execute;
		segment = RequireU16(j, "segment");
		offset  = j.at("offset").get<uint32_t>();
	} else if (type_str == "interrupt") {
		type = DebugBreakpointType::Interrupt;
		const int intNr = j.at("int").get<int>();
		if (intNr < 0 || intNr > 0xFF) {
			throw std::invalid_argument("int must be 0x00..0xFF");
		}
		int_num = static_cast<uint8_t>(intNr);
		ah      = ByteOrWildcard(j, "ah");
		al      = ByteOrWildcard(j, "al");
	} else if (type_str == "memory") {
		type    = DebugBreakpointType::Memory;
		segment = RequireU16(j, "segment");
		offset  = j.at("offset").get<uint32_t>();
	} else {
		throw std::invalid_argument(
		        "type must be one of: execute, interrupt, memory");
	}

	DebugAddBreakpointCommand cmd(type, segment, offset, int_num, ah, al);
	cmd.WaitForCompletion();

	json j2 = BreakpointToJson(cmd.result);
	j2["status"] = "ok";
	send_json(res, j2);
}

void DebugListBreakpointsCommand::Execute()
{
	breakpoints = DEBUG_ListBreakpoints();
}

void DebugListBreakpointsCommand::Get(const Request&, Response& res)
{
	DebugListBreakpointsCommand cmd;
	cmd.WaitForCompletion();

	json list = json::array();
	for (const auto& bp : cmd.breakpoints) {
		list.push_back(BreakpointToJson(bp));
	}

	json j;
	j["breakpoints"] = list;
	j["count"]       = cmd.breakpoints.size();
	send_json(res, j);
}

void DebugDeleteBreakpointCommand::Execute()
{
	if (delete_all) {
		DEBUG_DeleteAllBreakpoints();
		deleted = true;
	} else {
		deleted = DEBUG_DeleteBreakpointByIndex(index);
	}
}

void DebugDeleteBreakpointCommand::Delete(const Request& req, Response& res)
{
	if (req.body.empty()) {
		DebugDeleteBreakpointCommand cmd(true, 0);
		cmd.WaitForCompletion();

		json result;
		result["status"] = "cleared";
		send_json(res, result);
		return;
	}

	auto j = json::parse(req.body);
	const uint16_t index = RequireU16(j, "index");

	DebugDeleteBreakpointCommand cmd(false, index);
	cmd.WaitForCompletion();

	if (!cmd.deleted) {
		res.status = httplib::StatusCode::NotFound_404;
		json err;
		err["error"] = "No breakpoint at index " + std::to_string(index);
		send_json(res, err);
		return;
	}

	json result;
	result["status"] = "removed";
	result["index"]  = index;
	send_json(res, result);
}

#else // !C_DEBUGGER

namespace {
void NotBuilt(const std::string_view tool_name, Response& res)
{
	res.status = httplib::StatusCode::NotImplemented_501;
	json j;
	j["error"] = std::string(tool_name) +
	             ": debugger capability not built in this binary";
	send_json(res, j);
}
} // namespace

void DebugStatusCommand::Execute() {}
void DebugStatusCommand::Get(const Request&, Response& res)
{
	NotBuilt("debug_status", res);
}

void DebugPauseCommand::Execute() {}
void DebugPauseCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_pause", res);
}

void DebugContinueCommand::Execute() {}
void DebugContinueCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_continue", res);
}

void DebugStepCommand::Execute() {}
void DebugStepCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_step", res);
}

void DebugWaitHandlers::Get(const Request&, Response& res)
{
	NotBuilt("debug_wait", res);
}

void DebugAddBreakpointCommand::Execute() {}
void DebugAddBreakpointCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_breakpoints", res);
}

void DebugListBreakpointsCommand::Execute() {}
void DebugListBreakpointsCommand::Get(const Request&, Response& res)
{
	NotBuilt("debug_breakpoints", res);
}

void DebugDeleteBreakpointCommand::Execute() {}
void DebugDeleteBreakpointCommand::Delete(const Request&, Response& res)
{
	NotBuilt("debug_breakpoints", res);
}

#endif // C_DEBUGGER

} // namespace Webserver
