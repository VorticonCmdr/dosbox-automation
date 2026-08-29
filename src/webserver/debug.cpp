// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/debug.h"
#include "wait.h"
#include "webserver.h"

#include "dosbox.h"

#if C_DEBUGGER
#include "cpu/cpu.h"
#include "debugger/debugger.h"
#include "hardware/memory.h"
#endif

#include "base64/base64.h"
#include "json/json.h"

#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

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
		bp["hit_count"] = stop.breakpoint->hit_count;
		j["breakpoint"] = bp;
	} else {
		j["breakpoint"] = nullptr;
	}
	return j;
}

// Deliberately a self-contained table, not a reuse/extension of
// Webserver::RegisterKind (cpu.cpp) - see the comment on ConditionRegister
// in debugger.h for why.
constexpr std::array<std::pair<std::string_view, ConditionRegister>, 30> ConditionRegisterNames = {
        {{"eax", ConditionRegister::Eax}, {"ebx", ConditionRegister::Ebx},
         {"ecx", ConditionRegister::Ecx}, {"edx", ConditionRegister::Edx},
         {"esi", ConditionRegister::Esi}, {"edi", ConditionRegister::Edi},
         {"esp", ConditionRegister::Esp}, {"ebp", ConditionRegister::Ebp},
         {"ax", ConditionRegister::Ax},   {"bx", ConditionRegister::Bx},
         {"cx", ConditionRegister::Cx},   {"dx", ConditionRegister::Dx},
         {"si", ConditionRegister::Si},   {"di", ConditionRegister::Di},
         {"sp", ConditionRegister::Sp},   {"bp", ConditionRegister::Bp},
         {"al", ConditionRegister::Al},   {"bl", ConditionRegister::Bl},
         {"cl", ConditionRegister::Cl},   {"dl", ConditionRegister::Dl},
         {"ah", ConditionRegister::Ah},   {"bh", ConditionRegister::Bh},
         {"ch", ConditionRegister::Ch},   {"dh", ConditionRegister::Dh},
         {"cs", ConditionRegister::Cs},   {"ds", ConditionRegister::Ds},
         {"es", ConditionRegister::Es},   {"ss", ConditionRegister::Ss},
         {"fs", ConditionRegister::Fs},   {"gs", ConditionRegister::Gs}}
};

std::optional<ConditionRegister> ParseConditionRegister(const std::string& name)
{
	for (const auto& [n, r] : ConditionRegisterNames) {
		if (n == name) {
			return r;
		}
	}
	return std::nullopt;
}

std::string_view ConditionRegisterName(ConditionRegister reg)
{
	for (const auto& [n, r] : ConditionRegisterNames) {
		if (r == reg) {
			return n;
		}
	}
	return "unknown";
}

constexpr std::array<std::pair<std::string_view, ConditionOp>, 6> ConditionOpNames = {
        {{"eq", ConditionOp::Eq},
         {"ne", ConditionOp::Ne},
         {"lt", ConditionOp::Lt},
         {"le", ConditionOp::Le},
         {"gt", ConditionOp::Gt},
         {"ge", ConditionOp::Ge}}
};

std::optional<ConditionOp> ParseConditionOp(const std::string& name)
{
	for (const auto& [n, o] : ConditionOpNames) {
		if (n == name) {
			return o;
		}
	}
	return std::nullopt;
}

std::string_view ConditionOpName(ConditionOp op)
{
	for (const auto& [n, o] : ConditionOpNames) {
		if (o == op) {
			return n;
		}
	}
	return "unknown";
}

json BreakpointToJson(const DebugBreakpointInfo& bp)
{
	json j;
	j["id"]           = bp.id;
	j["index"]        = bp.index;
	j["once"]         = bp.once;
	j["active"]       = bp.active;
	j["hit_count"]    = bp.hit_count;
	j["ignore_count"] = bp.ignore_count;
	if (bp.condition.kind == DebugBreakpointCondition::Kind::None) {
		j["condition"] = nullptr;
	} else {
		json cond;
		cond["op"]    = std::string(ConditionOpName(bp.condition.op));
		cond["value"] = bp.condition.value;
		if (bp.condition.kind == DebugBreakpointCondition::Kind::Register) {
			cond["register"] = std::string(
			        ConditionRegisterName(bp.condition.reg));
		} else {
			cond["segment"] = bp.condition.segment;
			cond["offset"]  = bp.condition.offset;
			cond["width"]   = bp.condition.width;
		}
		j["condition"] = cond;
	}
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
		j["trigger"] = bp.memory_trigger == DebugMemoryTrigger::Read
		                     ? "read"
		                     : "write";
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

// 64-bit arithmetic deliberately: seg/off reach here from breakpoint-add
// and run_to requests (untrusted HTTP input, and debug_map_to_live in the
// bridge is another indirect source), and a uint32_t (seg<<4)+off can
// overflow and wrap into a small, in-range-looking address instead of
// correctly failing this check (e.g. seg=0xFFFF, off=0xFFFFFFFF wraps to a
// valid low address in 32-bit math) - same reasoning as
// DisassembleCommand::Execute (disassemble.cpp).
//
// This validates the flat real-mode formula only. debugger.cpp's
// GetAddress() - what CBreakpoint::SetAddress/DEBUG_RunToAddress
// actually resolve seg:off through - takes this same flat path only
// when the CPU isn't in protected mode; a live GDT selector resolves
// through PhysMakeProt (desc.GetBase()+offset) instead, a different
// formula this check knows nothing about. Call sites gate on
// !cpu.pmode for exactly that reason - see their own comments.
bool SegOffsetWithinMemory(uint16_t seg, uint32_t off)
{
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;
	const uint64_t addr      = (static_cast<uint64_t>(seg) << 4) +
	                           static_cast<uint64_t>(off);
	return addr < mem_total;
}

int32_t RequireIgnoreCount(const json& j)
{
	if (!j.contains("ignore_count")) {
		return 0;
	}
	const int64_t v = RequireInt(j, "ignore_count");
	if (v < 0 || v > std::numeric_limits<int32_t>::max()) {
		throw std::invalid_argument(
		        "ignore_count must be a non-negative 32-bit integer");
	}
	return static_cast<int32_t>(v);
}

// Fixed shape, never an expression parser: either {register, op, value} or
// {segment, offset, width, op, value}. Absent or null "condition" means no
// condition at all (Kind::None) - the breakpoint always stops, as before
// this existed.
DebugBreakpointCondition ParseCondition(const json& j)
{
	DebugBreakpointCondition cond;
	if (!j.contains("condition") || j.at("condition").is_null()) {
		return cond;
	}
	const auto& c = j.at("condition");
	if (!c.is_object()) {
		throw std::invalid_argument("condition must be an object");
	}

	const bool has_register = c.contains("register");
	const bool has_memory = c.contains("segment") || c.contains("offset") ||
	                        c.contains("width");
	if (has_register && has_memory) {
		throw std::invalid_argument(
		        "condition must be either register-based ('register') "
		        "or memory-based ('segment'/'offset'/'width'), not both");
	}
	if (!has_register && !has_memory) {
		throw std::invalid_argument(
		        "condition must specify 'register' or "
		        "'segment'/'offset'/'width'");
	}

	if (!c.contains("op") || !c.at("op").is_string()) {
		throw std::invalid_argument("condition.op must be a string");
	}
	const auto op = ParseConditionOp(c.at("op").get<std::string>());
	if (!op) {
		throw std::invalid_argument(
		        "condition.op must be one of: eq, ne, lt, le, gt, ge");
	}
	cond.op    = *op;
	cond.value = RequireU32(c, "value");

	if (has_register) {
		if (!c.at("register").is_string()) {
			throw std::invalid_argument(
			        "condition.register must be a string");
		}
		const auto reg = ParseConditionRegister(
		        c.at("register").get<std::string>());
		if (!reg) {
			throw std::invalid_argument(
			        "condition.register must be a valid register name");
		}
		cond.kind = DebugBreakpointCondition::Kind::Register;
		cond.reg  = *reg;
	} else {
		cond.segment        = RequireU16(c, "segment");
		cond.offset         = RequireU32(c, "offset");
		const int64_t width = RequireInt(c, "width");
		if (width != 1 && width != 2 && width != 4) {
			throw std::invalid_argument(
			        "condition.width must be 1, 2, or 4");
		}
		cond.kind  = DebugBreakpointCondition::Kind::Memory;
		cond.width = static_cast<uint8_t>(width);
	}
	return cond;
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
	// See SegOffsetWithinMemory's own comment: only valid against the
	// flat real-mode formula DEBUG_RunToAddress -> GetAddress actually
	// uses when the CPU isn't in protected mode. Skipping the check in
	// protected mode avoids wrongly rejecting a legitimate GDT-selector
	// target - the pre-existing (unchecked) behavior for that case,
	// left as-is rather than half-fixed.
	if (!cpu.pmode && !SegOffsetWithinMemory(segment, offset)) {
		error = "segment:offset is outside emulated memory";
		return;
	}
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
	if (!cmd.error.empty()) {
		throw std::invalid_argument(cmd.error);
	}

	json j2;
	j2["status"]               = cmd.started ? "ok" : "not_paused";
	j2["resumed_from_stop_id"] = cmd.resumed_from_stop_id;
	send_json(res, j2);
}

void DebugStepOutCommand::Execute()
{
	resumed_from_stop_id = DebugEvents::Instance().Current().stop_id;
	paused               = DEBUG_IsDebugging();
	if (paused) {
		started = DEBUG_StepOut();
	}
}

void DebugStepOutCommand::Post(const Request&, Response& res)
{
	DebugStepOutCommand cmd;
	// DEBUG_StepOut backtraces just 2 frames (debugger_backtrace.cpp) -
	// far cheaper than the general backtrace route's worst case, so the
	// Bridge's own default deadline is enough.
	cmd.WaitForCompletion();

	json j;
	if (!cmd.paused) {
		j["status"] = "not_paused";
	} else if (!cmd.started) {
		j["status"] = "no_confident_caller_frame";
	} else {
		j["status"] = "ok";
	}
	j["resumed_from_stop_id"] = cmd.resumed_from_stop_id;
	send_json(res, j);
}

void DebugAddBreakpointCommand::Execute()
{
	// See SegOffsetWithinMemory's own comment: only valid against the
	// flat real-mode formula CBreakpoint::SetAddress -> GetAddress
	// actually uses when the CPU isn't in protected mode. Skipping in
	// protected mode avoids wrongly rejecting a legitimate GDT-selector
	// target - the pre-existing (unchecked) behavior for that case,
	// left as-is rather than half-fixed.
	if (!cpu.pmode) {
		if ((type == DebugBreakpointType::Execute ||
		     type == DebugBreakpointType::Memory) &&
		    !SegOffsetWithinMemory(segment, offset)) {
			error = "segment:offset is outside emulated memory";
			return;
		}
		// The condition's own segment:offset (memory-kind condition,
		// any breakpoint type) resolves through the identical
		// GetAddress() call at evaluation time - CBreakpoint::
		// EvaluateCondition, debugger.cpp - and is just as reachable
		// from untrusted HTTP input as the breakpoint's own location.
		if (condition.kind == DebugBreakpointCondition::Kind::Memory &&
		    !SegOffsetWithinMemory(condition.segment, condition.offset)) {
			error = "condition segment:offset is outside emulated memory";
			return;
		}
	}

	// CheckBreakpoint/CheckIntBreakpoint act on the first match in list
	// order and never consider any other breakpoint at the same location
	// in that same pass - condition/ignore_count only get consulted for
	// whichever one that is. Refuse to create a second breakpoint at a
	// location that already has one, whenever either side carries logic
	// that could end up silently never firing because the other always
	// matches first. Two breakpoints with neither is unaffected: every
	// match behaves identically (stop), so which one matched never
	// mattered before this feature and still doesn't.
	const bool new_has_logic = ignore_count > 0 ||
	                           condition.kind !=
	                                   DebugBreakpointCondition::Kind::None;
	for (const auto& existing : DEBUG_ListBreakpoints()) {
		if (existing.type != type) {
			continue;
		}
		const bool same_location =
		        (type == DebugBreakpointType::Interrupt)
		                ? (existing.int_num == int_num &&
		                   (existing.ah == DEBUG_BPINT_ANY ||
		                    ah == DEBUG_BPINT_ANY || existing.ah == ah) &&
		                   (existing.al == DEBUG_BPINT_ANY ||
		                    al == DEBUG_BPINT_ANY || existing.al == al))
		                : (existing.segment == segment &&
		                   existing.offset == offset);
		if (!same_location) {
			continue;
		}
		const bool existing_has_logic = existing.ignore_count > 0 ||
		                                existing.condition.kind !=
		                                        DebugBreakpointCondition::Kind::None;
		if (new_has_logic || existing_has_logic) {
			conflict = true;
			return;
		}
	}

	switch (type) {
	case DebugBreakpointType::Execute:
		DEBUG_AddExecuteBreakpoint(segment, offset, once, ignore_count, condition);
		break;
	case DebugBreakpointType::Interrupt:
		DEBUG_AddIntBreakpoint(int_num, ah, al, once, ignore_count, condition);
		break;
	case DebugBreakpointType::Memory:
		DEBUG_AddMemBreakpoint(
		        segment, offset, once, ignore_count, condition, trigger);
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
	const bool once                          = j.value("once", false);
	const int32_t ignore_count               = RequireIgnoreCount(j);
	const DebugBreakpointCondition condition = ParseCondition(j);

	if (once && (ignore_count > 0 ||
	             condition.kind != DebugBreakpointCondition::Kind::None)) {
		throw std::invalid_argument(
		        "once-only breakpoints always stop on their first "
		        "match and cannot combine with ignore_count or condition");
	}

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
#if !C_HEAVY_DEBUGGER
		// A memory breakpoint's match logic (CheckBreakpoint's watched-
		// value-changed check) is entirely compiled out on a non-heavy
		// build (debugger.cpp, #if C_HEAVY_DEBUGGER) - accepting the
		// request here would return 200 for a breakpoint that can never
		// fire. Refuse it instead of silently no-oping; the reason is
		// also visible ahead of time via capabilities.debugger.
		res.status = httplib::StatusCode::NotImplemented_501;
		json err;
		err["error"] =
		        "memory (watchpoint) breakpoints need a heavy-debugger "
		        "build (C_HEAVY_DEBUGGER=1); this binary doesn't have "
		        "one, so a memory breakpoint would accept the request "
		        "and then never fire - see capabilities.debugger in "
		        "GET /api/v1/dosbox/info";
		send_json(res, err);
		return;
#else
		type    = DebugBreakpointType::Memory;
		segment = RequireU16(j, "segment");
		offset  = RequireU32(j, "offset");
#endif
	} else {
		throw std::invalid_argument(
		        "type must be one of: execute, interrupt, memory");
	}

	DebugMemoryTrigger trigger = DebugMemoryTrigger::Write;
	if (type == DebugBreakpointType::Memory && j.contains("trigger")) {
		const auto trigger_str = j.at("trigger").get<std::string>();
		if (trigger_str == "write") {
			trigger = DebugMemoryTrigger::Write;
		} else if (trigger_str == "read") {
			trigger = DebugMemoryTrigger::Read;
		} else {
			throw std::invalid_argument(
			        "trigger must be one of: write, read");
		}
	}

	DebugAddBreakpointCommand cmd(
	        type, segment, offset, int_num, ah, al, once, ignore_count, condition, trigger);
	cmd.WaitForCompletion();
	if (!cmd.error.empty()) {
		throw std::invalid_argument(cmd.error);
	}

	if (cmd.conflict) {
		res.status = httplib::StatusCode::Conflict_409;
		json err;
		err["error"] =
		        "a breakpoint already exists at this location and "
		        "either it or this one has an ignore_count or "
		        "condition - list existing breakpoints and delete "
		        "the conflicting one first";
		send_json(res, err);
		return;
	}

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

namespace {
json WatchToJson(const DebugWatchInfo& w)
{
	json j;
	j["name"]     = w.name;
	j["address"]  = w.address;
	j["segment"]  = w.has_segment_offset ? json(w.segment) : json(nullptr);
	j["offset"]   = w.has_segment_offset ? json(w.offset) : json(nullptr);
	j["hasValue"] = w.has_value;
	j["value"]    = w.value;
	return j;
}
} // namespace

void DebugAddWatchCommand::Execute()
{
	if (!cpu.pmode && !SegOffsetWithinMemory(segment, offset)) {
		error = "segment:offset is outside emulated memory";
		return;
	}
	DEBUG_AddWatch(name, segment, offset);
	// Just pushed to the back of the engine's list, so it's whatever is
	// now last - read it back rather than assuming its address/value
	// here, matching DebugAddBreakpointCommand::Execute's own reasoning.
	auto all = DEBUG_ListWatches();
	if (!all.empty()) {
		result = all.back();
	}
}

void DebugAddWatchCommand::Post(const Request& req, Response& res)
{
	auto j = json::parse(req.body);
	if (!j.contains("name") || !j.at("name").is_string()) {
		throw std::invalid_argument("name must be a string");
	}
	const auto name = j.at("name").get<std::string>();
	if (name.empty() || name.size() > 15) {
		throw std::invalid_argument("name must be 1..15 characters");
	}
	const uint16_t segment = RequireU16(j, "segment");
	const uint32_t offset  = RequireU32(j, "offset");

	DebugAddWatchCommand cmd(name, segment, offset);
	cmd.WaitForCompletion();
	if (!cmd.error.empty()) {
		throw std::invalid_argument(cmd.error);
	}

	json j2      = WatchToJson(cmd.result);
	j2["status"] = "ok";
	send_json(res, j2);
}

void DebugListWatchesCommand::Execute()
{
	watches = DEBUG_ListWatches();
}

void DebugListWatchesCommand::Get(const Request&, Response& res)
{
	DebugListWatchesCommand cmd;
	cmd.WaitForCompletion();

	json list = json::array();
	for (const auto& w : cmd.watches) {
		list.push_back(WatchToJson(w));
	}

	json j;
	j["watches"] = list;
	j["count"]   = cmd.watches.size();
	send_json(res, j);
}

void DebugDeleteWatchCommand::Execute()
{
	if (all_watches) {
		DEBUG_RemoveAllWatches();
		deleted = true;
	} else {
		deleted = DEBUG_RemoveWatch(address);
	}
}

void DebugDeleteWatchCommand::Delete(const Request& req, Response& res)
{
	if (req.body.empty()) {
		DebugDeleteWatchCommand cmd(true, 0);
		cmd.WaitForCompletion();

		json result;
		result["status"] = "cleared";
		send_json(res, result);
		return;
	}

	auto j                 = json::parse(req.body);
	const uint32_t address = RequireU32(j, "address");

	DebugDeleteWatchCommand cmd(false, address);
	cmd.WaitForCompletion();

	if (!cmd.deleted) {
		res.status = httplib::StatusCode::NotFound_404;
		json err;
		err["error"] = "No watch at address " + std::to_string(address);
		send_json(res, err);
		return;
	}

	json result;
	result["status"]  = "removed";
	result["address"] = address;
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

void DebugStepOutCommand::Execute() {}
void DebugStepOutCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_step_out", res);
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

void DebugAddWatchCommand::Execute() {}
void DebugAddWatchCommand::Post(const Request&, Response& res)
{
	NotBuilt("debug_watches", res);
}

void DebugListWatchesCommand::Execute() {}
void DebugListWatchesCommand::Get(const Request&, Response& res)
{
	NotBuilt("debug_watches", res);
}

void DebugDeleteWatchCommand::Execute() {}
void DebugDeleteWatchCommand::Delete(const Request&, Response& res)
{
	NotBuilt("debug_watches", res);
}

#endif // C_DEBUGGER

} // namespace Webserver
