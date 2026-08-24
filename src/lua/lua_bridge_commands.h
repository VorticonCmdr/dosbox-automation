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
#include <mutex>
#include <optional>
#include <string>

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
	nlohmann::json output = {};
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
