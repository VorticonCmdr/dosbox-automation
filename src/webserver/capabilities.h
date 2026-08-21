// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_CAPABILITIES_H
#define DOSBOX_WEBSERVER_CAPABILITIES_H

#include "cpu/cpu.h"
#include "libs/json/json.h"

#include <string>

namespace Webserver {

enum class CapabilityState { On, Off, Degraded };

std::string CapabilityStateName(CapabilityState state);

struct CapabilityInfo {
	CapabilityState state = CapabilityState::On;
	std::string reason    = {};
	nlohmann::json limits = nlohmann::json::object();
};

// Pure: every input the underlying build macros or CPU_GetActiveCoreKind()
// would supply is instead a parameter, so every built/heavy/core
// combination is testable regardless of what this binary was actually
// built with. built and heavy mirror C_DEBUGGER and C_HEAVY_DEBUGGER;
// core mirrors CPU_GetActiveCoreKind().
//
// Execute/interrupt breakpoints are patched into guest memory as 0xCC and
// checked at a handful of call sites gated on C_DEBUGGER (core_normal,
// core_simple, core_full, core_prefetch all include the same check
// directly; the dynamic/JIT cores have no such site in their generated
// code, but an untranslated 0xCC or INT falls back to a single
// interpreted instruction that does check it - so they still fire on the
// dynamic core, just not from a call site native to the JIT). Memory
// (type="memory") breakpoints are a separate, heavier check compiled in
// only under C_HEAVY_DEBUGGER, on every core.
CapabilityInfo ComputeDebuggerCapability(bool built, bool heavy, CoreKind core);

// Builds the full "capabilities" block for GET /api/v1/dosbox/info: one
// entry per served API group, each with a state, a human-readable reason,
// and the operational limits sourced from the same named constants the
// validators enforce. Reads only build macros, atomics and constexpr
// values - safe to call from the info route's plain lambda, which must
// answer without crossing the Bridge.
nlohmann::json BuildCapabilitiesBlock();

// Collapses BuildCapabilitiesBlock()'s per-group state to the boolean
// shape 1.0/1.1 peers already parse: true for On and Degraded (the group
// is at least partially usable), false for Off. This is an exact
// reproduction of what the old hardcoded-true / C_DEBUGGER-only
// "features" block meant, so those peers see unchanged behaviour.
nlohmann::json FeaturesProjection(const nlohmann::json& capabilities);

// Server-wide limits that apply across every group rather than to one of
// them (the request body cap, the Bridge's default per-command
// deadline). Sibling to "capabilities" in the info response, not nested
// inside it.
nlohmann::json BuildServerLimits();

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_CAPABILITIES_H
