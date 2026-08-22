// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "capabilities.h"

#include "bridge.h"
#include "input.h"
#include "private/backtrace.h"
#include "private/disassemble.h"
#include "private/dos.h"
#include "private/freeze.h"
#include "private/memory.h"
#include "wait.h"
#include "webserver.h"

#include "lua/lua_engine.h"
#include "lua/script_validator.h"

#include "cpu/cpu.h"
#include "dosbox.h"

using json = nlohmann::json;

namespace Webserver {

std::string CapabilityStateName(const CapabilityState state)
{
	switch (state) {
	case CapabilityState::On: return "on";
	case CapabilityState::Off: return "off";
	case CapabilityState::Degraded: return "degraded";
	}
	return "off";
}

namespace {

std::string CoreKindName(const CoreKind core)
{
	switch (core) {
	case CoreKind::Normal: return "normal";
	case CoreKind::Simple: return "simple";
	case CoreKind::Full: return "full";
	case CoreKind::Dynamic: return "dynamic";
	}
	return "normal";
}

json ToJson(const CapabilityInfo& info)
{
	json j;
	j["state"]  = CapabilityStateName(info.state);
	j["reason"] = info.reason;
	j["limits"] = info.limits;
	return j;
}

CapabilityInfo AlwaysOn(std::string reason, json limits = json::object())
{
	CapabilityInfo info;
	info.state  = CapabilityState::On;
	info.reason = std::move(reason);
	info.limits = std::move(limits);
	return info;
}

} // namespace

CapabilityInfo ComputeDebuggerCapability(const bool built, const bool heavy,
                                         const CoreKind core)
{
	CapabilityInfo info;
	info.limits["built"]                = built;
	info.limits["heavy_debugger_built"] = heavy;
	info.limits["effective_core"]       = CoreKindName(core);

	if (!built) {
		info.state = CapabilityState::Off;
		info.reason = "debugger not built into this binary (C_DEBUGGER=0)";
		return info;
	}

	if (heavy) {
		info.state = CapabilityState::On;
		info.reason =
		        "heavy debugger built: execute, interrupt and memory "
		        "breakpoints all fire on every core";
		return info;
	}

	info.state = CapabilityState::Degraded;
	if (core == CoreKind::Dynamic) {
		info.reason =
		        "memory breakpoints unavailable (needs a heavy-debugger "
		        "build); execute/interrupt breakpoints fire indirectly on "
		        "the dynamic core via a per-instruction interpreter "
		        "fallback, not a native JIT check";
	} else {
		info.reason =
		        "memory breakpoints unavailable (needs a heavy-debugger "
		        "build); execute/interrupt breakpoints fire normally on "
		        "the " +
		        CoreKindName(core) + " core";
	}
	return info;
}

json BuildCapabilitiesBlock()
{
	json memory_limits;
	memory_limits["max_transfer_bytes"]    = MaxMemoryTransferBytes;
	memory_limits["max_search_span_bytes"] = MaxSearchSpanBytes;
	memory_limits["max_dos_allocations"]   = AllocationRegistry::MaxEntries;

	json input_limits;
	input_limits["max_events"]           = MaxInputEvents;
	input_limits["max_typed_text_chars"] = MaxTypedTextChars;
	input_limits["max_event_time_ms"]    = MaxEventTimeMs;
	input_limits["max_event_frame"]      = MaxEventFrame;
	input_limits["min_typing_cps"]       = MinTypingCps;
	input_limits["max_typing_cps"]       = MaxTypingCps;

	json cpu_control_limits;
	cpu_control_limits["cycles_min"] = CpuCyclesMin;
	cpu_control_limits["cycles_max"] = CpuCyclesMax;

	json freeze_limits;
	freeze_limits["max_entries"] = FreezeRegistry::MaxEntries;

	json script_limits;
	script_limits["max_body_bytes"] = Lua::ScriptValidator::MaxBodySize;
	script_limits["max_memory_bytes"] = Lua::LuaEngine::DefaultMemoryCapBytes;
	script_limits["instruction_limit"] = Lua::LuaEngine::DefaultInstructionLimit;
	script_limits["wall_clock_limit_ms"] = Lua::LuaEngine::DefaultWallClockLimitMs;

	json wait_limits;
	wait_limits["min_timeout_ms"]     = MinWaitTimeoutMs;
	wait_limits["max_timeout_ms"]     = MaxWaitTimeoutMs;
	wait_limits["default_timeout_ms"] = DefaultWaitTimeoutMs;
	wait_limits["max_pattern_len"]    = MaxPatternLen;
	wait_limits["max_waiters"]        = MaxWaiters;

	json j;
	j["memory"] = ToJson(AlwaysOn("always available", memory_limits));
	j["input"]  = ToJson(AlwaysOn("always available", input_limits));
	j["cpu_registers"] = ToJson(AlwaysOn("always available"));
	j["cpu_control"] = ToJson(AlwaysOn("always available", cpu_control_limits));
	j["port_io"]  = ToJson(AlwaysOn("always available"));
	j["freeze"]   = ToJson(AlwaysOn("always available", freeze_limits));
	j["debugger"] = ToJson(
	        ComputeDebuggerCapability(static_cast<bool>(C_DEBUGGER),
	                                  static_cast<bool>(C_HEAVY_DEBUGGER),
	                                  CPU_GetActiveCoreKind()));
	j["script"]  = ToJson(AlwaysOn("always available", script_limits));
	json disassemble_limits;
	disassemble_limits["max_count"] = MaxDisassembleCount;
	// Unlike every other debugger.* facility, decoding instruction bytes
	// into text doesn't need C_DEBUGGER (2.5) - the interactive debugger
	// UI and the disassembler underneath it are separate concerns.
	j["disassemble"] = ToJson(
	        AlwaysOn("always available, regardless of the debugger capability",
	                 disassemble_limits));
	json backtrace_limits;
	backtrace_limits["max_frames"]     = MaxBacktraceFrames;
	backtrace_limits["default_frames"] = DefaultBacktraceFrames;
	// Same reasoning as disassemble: walking SS:BP and decoding bytes to
	// confirm a call site doesn't need C_DEBUGGER either. debug_step_out
	// is a different story - it plants a breakpoint, so it stays behind
	// the debugger capability above like every other execution-control
	// tool.
	j["backtrace"] = ToJson(
	        AlwaysOn("always available, regardless of the debugger capability",
	                 backtrace_limits));
	j["drive"]   = ToJson(AlwaysOn("always available"));
	j["capture"] = ToJson(AlwaysOn("always available"));
	j["wait"]    = ToJson(AlwaysOn("always available", wait_limits));

	return j;
}

json FeaturesProjection(const json& capabilities)
{
	json features;
	for (const auto& [group, info] : capabilities.items()) {
		features[group] = info.value("state", "off") != "off";
	}
	return features;
}

json BuildServerLimits()
{
	json j;
	j["max_request_body_bytes"]    = MaxRequestBodyBytes;
	j["bridge_default_timeout_ms"] = DefaultBridgeTimeoutMs;
	return j;
}

} // namespace Webserver
