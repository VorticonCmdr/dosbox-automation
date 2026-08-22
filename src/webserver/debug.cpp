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

#include <limits>

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
		bp["id"]        = stop.breakpoint->id;
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
	j["id"]     = bp.id;
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

// nlohmann only tags a parsed number as the unsigned representation when
// the JSON text had no leading '-'; a value built from a C++ integer
// literal (as in tests, and some client serialization paths) is always
// the signed representation regardless of value - is_number_integer()
// covers both, so validate the value itself rather than which
// representation happens to be present (see wait.cpp's own
// RequireNonNegative for the same fix, same reasoning). It also rejects
// non-integral numbers outright: get<int64_t>() on a float silently
// truncates (2.9 -> 2) rather than throwing, which we don't want for
// values that end up as breakpoint addresses or step counts.
int64_t RequireInt(const json& j, const char* key)
{
	if (!j.contains(key) || !j[key].is_number_integer()) {
		throw std::invalid_argument(std::string(key) + " must be an integer");
	}
	return j[key].get<int64_t>();
}

uint16_t ByteOrWildcard(const json& j, const char* key)
{
	if (!j.contains(key)) {
		return DEBUG_BPINT_ANY;
	}
	const int64_t v = RequireInt(j, key);
	if (v < 0 || v > 0xFF) {
		throw std::invalid_argument(std::string(key) + " must be 0x00..0xFF");
	}
	return static_cast<uint16_t>(v);
}

uint16_t RequireU16(const json& j, const char* key)
{
	const int64_t v = RequireInt(j, key);
	if (v < 0 || v > 0xFFFF) {
		throw std::invalid_argument(std::string(key) +
		                            " must be 0x0000..0xFFFF");
	}
	return static_cast<uint16_t>(v);
}

uint32_t RequireU32(const json& j, const char* key)
{
	const int64_t v = RequireInt(j, key);
	if (v < 0 || v > std::numeric_limits<uint32_t>::max()) {
		throw std::invalid_argument(std::string(key) +
		                            " must be 0x00000000..0xFFFFFFFF");
	}
	return static_cast<uint32_t>(v);
}

uint64_t RequireU64(const json& j, const char* key)
{
	const int64_t raw = RequireInt(j, key);
	if (raw < 0) {
		throw std::invalid_argument(std::string(key) + " must not be negative");
	}
	return static_cast<uint64_t>(raw);
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
	stepped   = DEBUG_SingleStep(count);
	debugging = DEBUG_IsDebugging();
	stop      = DebugEvents::Instance().Current();
}

void DebugStepCommand::Post(const Request& req, Response& res)
{
	int32_t count = 1;
	if (!req.body.empty()) {
		auto j = json::parse(req.body);
		if (j.contains("count")) {
			const int64_t v = RequireInt(j, "count");
			if (v < 1 || v > DEBUG_MaxStepCount) {
				throw std::invalid_argument(
				        "count must be 1.." +
				        std::to_string(DEBUG_MaxStepCount));
			}
			count = static_cast<int32_t>(v);
		}
	}

	DebugStepCommand cmd(count);
	// A single step uses the Bridge's own default; a multi-instruction
	// burst gets the same raised deadline SearchMemoryCommand uses for
	// its own bounded worst case (memory.cpp) - count is capped at
	// DEBUG_MaxStepCount precisely so this stays a bounded wait, not an
	// open-ended one, while the Bridge mutex is held for the whole call.
	if (count > 1) {
		cmd.WaitForCompletion(2000);
	} else {
		cmd.WaitForCompletion();
	}

	json j;
	j["status"]    = cmd.stepped ? "ok" : "not_paused";
	j["debugging"] = cmd.debugging;
	j["stop"]      = DebugStopToJson(cmd.stop);
	send_json(res, j);
}

void DebugStepOverCommand::Execute()
{
	resumed_from_stop_id = DebugEvents::Instance().Current().stop_id;
	stepped_over         = DEBUG_StepOver();
	if (!stepped_over) {
		// Not a call/int/loop/rep at the current instruction (or not
		// paused at all) - fall back to a plain step, matching the F10
		// key handler's own fallthrough to F11.
		stepped = DEBUG_SingleStep();
	}
	debugging = DEBUG_IsDebugging();
	stop      = DebugEvents::Instance().Current();
}

void DebugStepOverCommand::Post(const Request&, Response& res)
{
	DebugStepOverCommand cmd;
	cmd.WaitForCompletion();

	json j;
	j["status"] = (cmd.stepped_over || cmd.stepped) ? "ok" : "not_paused";
	j["stepped_over"]         = cmd.stepped_over;
	j["debugging"]            = cmd.debugging;
	j["resumed_from_stop_id"] = cmd.resumed_from_stop_id;
	// Only include stop when something was actually published during
	// this call - the stepped fallback always publishes one, and the
	// plant-and-resume path usually doesn't (the real stop happens later,
	// poll debug/wait with resumed_from_stop_id), except in the rare case
	// where the forced instruction itself hit a different already-armed
	// breakpoint synchronously. Without this check, the field would
	// otherwise silently carry a stale, unrelated prior stop on the
	// common plant-and-resume path.
	if (cmd.stop.stop_id != cmd.resumed_from_stop_id) {
		j["stop"] = DebugStopToJson(cmd.stop);
	}
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

void DebugRunToCommand::Execute()
{
	resumed_from_stop_id = DebugEvents::Instance().Current().stop_id;
	started              = DEBUG_RunToAddress(segment, offset);
}

void DebugRunToCommand::Post(const Request& req, Response& res)
{
	auto j                     = json::parse(req.body);
	const uint16_t req_segment = RequireU16(j, "segment");
	const uint32_t req_offset  = RequireU32(j, "offset");

	DebugRunToCommand cmd(req_segment, req_offset);
	cmd.WaitForCompletion();

	json j2;
	j2["status"]               = cmd.started ? "ok" : "not_paused";
	j2["resumed_from_stop_id"] = cmd.resumed_from_stop_id;
	send_json(res, j2);
}

void DebugAddBreakpointCommand::Execute()
{
	switch (type) {
	case DebugBreakpointType::Execute:
		DEBUG_AddExecuteBreakpoint(segment, offset, once);
		break;
	case DebugBreakpointType::Interrupt:
		DEBUG_AddIntBreakpoint(int_num, ah, al, once);
		break;
	case DebugBreakpointType::Memory:
		DEBUG_AddMemBreakpoint(segment, offset, once);
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
	const bool once          = j.value("once", false);

	if (type_str == "execute") {
		type    = DebugBreakpointType::Execute;
		segment = RequireU16(j, "segment");
		offset  = RequireU32(j, "offset");
	} else if (type_str == "interrupt") {
		type = DebugBreakpointType::Interrupt;
		const int64_t intNr = RequireInt(j, "int");
		if (intNr < 0 || intNr > 0xFF) {
			throw std::invalid_argument("int must be 0x00..0xFF");
		}
		int_num = static_cast<uint8_t>(intNr);
		ah      = ByteOrWildcard(j, "ah");
		al      = ByteOrWildcard(j, "al");
	} else if (type_str == "memory") {
		type    = DebugBreakpointType::Memory;
		segment = RequireU16(j, "segment");
		offset  = RequireU32(j, "offset");
	} else {
		throw std::invalid_argument(
		        "type must be one of: execute, interrupt, memory");
	}

	DebugAddBreakpointCommand cmd(type, segment, offset, int_num, ah, al, once);
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
	switch (by) {
	case By::All:
		DEBUG_DeleteAllBreakpoints();
		deleted = true;
		break;
	case By::Index:
		deleted = DEBUG_DeleteBreakpointByIndex(static_cast<uint16_t>(value));
		break;
	case By::Id: deleted = DEBUG_DeleteBreakpointById(value); break;
	}
}

void DebugDeleteBreakpointCommand::Delete(const Request& req, Response& res)
{
	if (req.body.empty()) {
		DebugDeleteBreakpointCommand cmd(By::All, 0);
		cmd.WaitForCompletion();

		json result;
		result["status"] = "cleared";
		send_json(res, result);
		return;
	}

	auto j = json::parse(req.body);
	const bool has_id    = j.contains("id");
	const bool has_index = j.contains("index");
	if (has_id && has_index) {
		throw std::invalid_argument("specify only one of 'id' or 'index'");
	}
	if (!has_id && !has_index) {
		throw std::invalid_argument(
		        "body must contain 'id' or 'index', or be empty to delete all");
	}

	const By by = has_id ? By::Id : By::Index;
	const uint64_t value = has_id ? RequireU64(j, "id") : RequireU16(j, "index");

	DebugDeleteBreakpointCommand cmd(by, value);
	cmd.WaitForCompletion();

	if (!cmd.deleted) {
		res.status = httplib::StatusCode::NotFound_404;
		json err;
		err["error"] = "No breakpoint at " +
		               std::string(has_id ? "id " : "index ") +
		               std::to_string(value);
		send_json(res, err);
		return;
	}

	json result;
	result["status"]                = "removed";
	result[has_id ? "id" : "index"] = value;
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

void DebugStepOverCommand::Execute() {}
void DebugStepOverCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_step_over", res);
}

void DebugWaitHandlers::Get(const Request&, Response& res)
{
	NotBuilt("debug_wait", res);
}

void DebugRunToCommand::Execute() {}
void DebugRunToCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_run_to", res);
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
