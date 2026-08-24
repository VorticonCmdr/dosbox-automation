// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_LUA_BRIDGE_COMMANDS_H
#define DOSBOX_LUA_BRIDGE_COMMANDS_H

#include "lua/lua_coroutine.h"
#include "lua/lua_debug_log.h"
#include "lua/lua_engine.h"
#include "lua/script_validator.h"
#include "webserver/bridge.h"

#include "libs/http/http.h"

#include "libs/json/json.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Lua {

class ScriptManager {
public:
	static ScriptManager& Instance();

	LuaEngine& Engine()
	{
		return engine;
	}
	LuaCoroutine& Coroutine()
	{
		return coroutine;
	}
	DebugLog& Log()
	{
		return debug_log;
	}
	ScriptParams& Params()
	{
		return params;
	}

	void DispatchFrame(uint64_t frame_number);

	// Off-frame counterpart to DispatchFrame: times out a yielded wait/type
	// on its wall deadline when frames have stalled.
	void ReapStalledWaits();

private:
	ScriptManager() = default;

	// Shared post-step bookkeeping: OSD running icon and state transition log.
	void ReportStateTransition(uint64_t frame_number, ScriptState prev_state,
	                           ScriptState new_state);

	LuaEngine engine;
	LuaCoroutine coroutine{engine};
	DebugLog debug_log;
	ScriptParams params;

	ScriptManager(const ScriptManager&)            = delete;
	ScriptManager& operator=(const ScriptManager&) = delete;
};

class ScriptRateLimiter {
public:
	bool ShouldReject(int64_t& retry_after_ms);

private:
	using Clock                            = std::chrono::steady_clock;
	static constexpr int64_t MinIntervalMs = 2000;

	Clock::time_point last_load = {};
	std::mutex mtx;
};

class LuaLoadCommand : public Webserver::Command {
public:
	LuaLoadCommand(std::string source, ScriptParams params);
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

private:
	std::string source;
	ScriptParams params;
};

class LuaStartCommand : public Webserver::Command {
public:
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);
};

class LuaStopCommand : public Webserver::Command {
public:
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);
};

// Plain, Lua-independent copy of a `dosbox.output` value. Built on the
// emulation thread (LuaStatusCommand::Execute, inside the Bridge Command,
// with direct access to the Lua state) and converted to JSON later on the
// web thread (LuaStatusCommand::Get) - it must own every string outright,
// never a raw Lua string pointer, since the Lua GC can run on the
// emulation thread in between the two phases and invalidate anything
// borrowed from the interpreter.
struct LuaOutputNode {
	enum class Kind { Null, Bool, Int, Double, String, Object };

	Kind kind                = Kind::Null;
	bool bool_value          = false;
	int64_t int_value        = 0;
	double double_value      = 0.0;
	std::string string_value = {};
	std::vector<std::pair<std::string, LuaOutputNode>> object_value = {};

	// Matches the pre-3.8 nlohmann::json behavior this replaces: a
	// still-default node (never walked, or a table with zero entries)
	// is "no output to report", not an empty JSON object.
	bool IsEmpty() const
	{
		return kind == Kind::Null ||
		       (kind == Kind::Object && object_value.empty());
	}
};

// Caps how much of `dosbox.output` a single serialization walks, shared
// across the whole call (not per-subtable): a handful of tables each
// holding references to the previous one is a small number of Lua slots
// but, without this, an exponential number of emitted JSON nodes. `visited`
// (keyed by lua_topointer identity) is what actually bounds that - it also
// makes a genuine cycle (t.self = t) terminate instead of relying solely on
// the depth cap. `visited` is never backed out once a subtree finishes: a
// table reachable via more than one path (not just an ancestor cycle) is
// deliberately walked once and truncated on later occurrences too - the
// alternative (ancestor-path-only tracking) would let a shared, densely-
// nested DAG re-expand the same subtree from every parent, exactly the
// blowup this exists to prevent. The tradeoff is that a script legitimately
// reusing the same small, harmless subtable in two unrelated places will
// see the second occurrence collapse to "...".
struct LuaOutputBudget {
	size_t nodes_used = 0;
	size_t bytes_used = 0;
	// Every key/value pair the walk looks at, accepted or not - unlike
	// nodes_used (committed pairs only), this bounds total loop
	// iterations even for a table built entirely of unsupported-type
	// keys/values, which would otherwise never trip nodes_used or
	// bytes_used at all and iterate the whole table regardless of size.
	size_t pairs_examined                   = 0;
	std::unordered_set<const void*> visited = {};
	bool truncated                          = false;
};

inline constexpr size_t MaxOutputNodes = 2000;
inline constexpr size_t MaxOutputBytes = 64 * 1024;
inline constexpr int MaxOutputDepth    = 10;

// Walks the Lua table at `idx` into an owned LuaOutputNode tree, subject to
// `budget`. Exposed for testing; LuaStatusCommand::Execute is the only
// production caller. `depth` starts at 0 for a direct call on the root
// `dosbox.output` table.
LuaOutputNode LuaTableToNode(lua_State* L, int idx, int depth,
                             LuaOutputBudget& budget);

// Converts an already-built LuaOutputNode tree to JSON. Pure data
// transformation, safe to call from any thread - unlike LuaTableToNode,
// which touches the Lua state and must only run on the emulation thread.
nlohmann::json LuaOutputNodeToJson(const LuaOutputNode& node);

struct LuaStatusResult {
	ScriptState state = ScriptState::Idle;
	std::string error = {};
	uint64_t frame    = 0;
	std::string name  = {};
	// Empty unless the currently loaded script was loaded with
	// debug=true - see LuaLogCommand for why this doubles as the gate
	// on whether a log is available at all, not just whether one
	// exists on disk from some earlier run.
	std::string log_path  = {};
	LuaOutputNode output  = {};
	bool output_truncated = false;
};

class LuaStatusCommand : public Webserver::Command {
public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

	LuaStatusResult result = {};
};

class LuaLogCommand : public Webserver::Command {
public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

	// True when the currently loaded script was loaded with debug=true.
	// log_path can still be empty even when this is true (the debug log
	// failed to open) - Get() reports that as a distinct error from "no
	// debug script is loaded" at all.
	bool debug_script_loaded = false;
	std::string log_path     = {};
};

// Bytes kept from the end of the debug log when it exceeds the cap.
inline constexpr size_t MaxLogTailBytes = 64 * 1024;

// Reads at most the last MaxLogTailBytes of `path`, or nullopt if the
// file cannot be opened. Exposed for testing; LuaLogCommand::Get is its
// only production caller.
std::optional<std::string> ReadLogTail(const std::string& path, bool& truncated);

} // namespace Lua

void LuaDispatchFrame(uint64_t frame_number);
void LuaReapStalledWaits();

#endif
