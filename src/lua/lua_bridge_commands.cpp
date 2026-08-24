// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "lua/lua_bridge_commands.h"

#include "gui/osd/osd.h"
#include "webserver/webserver.h"

#include "libs/json/json.h"

#include "misc/cross.h"
#include "misc/logging.h"

#include <cstdio>
#include <optional>

extern "C" {
#include "lua.h"
}

using json = nlohmann::json;

namespace Lua {

// -- ScriptManager --

ScriptManager& ScriptManager::Instance()
{
	static ScriptManager instance;
	return instance;
}

void ScriptManager::ReportStateTransition(const uint64_t frame_number,
                                          const ScriptState prev_state,
                                          const ScriptState new_state)
{
	const bool running = (new_state == ScriptState::Running ||
	                      new_state == ScriptState::Yielded);
	OSD::OsdManager::Instance().SetIcon(OSD::IconId::ScriptRunning, running);

	if (prev_state != new_state && (new_state == ScriptState::Completed ||
	                                new_state == ScriptState::Error)) {
		if (debug_log.IsOpen()) {
			debug_log.Trace(frame_number,
			                "script %s: %s",
			                ScriptStateName(new_state),
			                new_state == ScriptState::Error
			                        ? coroutine.ErrorMessage().c_str()
			                        : "ok");
			debug_log.Close();
		}
		LOG_MSG("LUA: Script '%s' %s",
		        params.name.c_str(),
		        ScriptStateName(new_state));
	}
}

void ScriptManager::DispatchFrame(const uint64_t frame_number)
{
	const auto prev_state = coroutine.State();
	coroutine.DispatchFrame(frame_number);
	ReportStateTransition(frame_number, prev_state, coroutine.State());
}

void ScriptManager::ReapStalledWaits()
{
	const auto prev_state = coroutine.State();
	coroutine.ReapStalledWaits();
	ReportStateTransition(coroutine.CurrentFrame(), prev_state, coroutine.State());
}

// -- ScriptRateLimiter --

bool ScriptRateLimiter::ShouldReject(int64_t& retry_after_ms)
{
	std::lock_guard<std::mutex> lock(mtx);
	const auto now = Clock::now();
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
	        now - last_load);

	if (last_load != Clock::time_point{} && elapsed.count() < MinIntervalMs) {
		retry_after_ms = MinIntervalMs - elapsed.count();
		return true;
	}

	last_load = now;
	return false;
}

// -- LuaLoadCommand --

LuaLoadCommand::LuaLoadCommand(std::string source, ScriptParams params)
        : source(std::move(source)),
          params(std::move(params))
{}

void LuaLoadCommand::Execute()
{
	auto& mgr = ScriptManager::Instance();

	const auto state = mgr.Coroutine().State();
	if (state == ScriptState::Running || state == ScriptState::Yielded) {
		error = "a script is already running";
		return;
	}

	mgr.Engine().Reset();
	mgr.Params() = params;

	auto result = mgr.Engine().LoadScript(source, params.name);
	if (!result.ok) {
		error = result.error;
		return;
	}

	if (params.debug) {
		const auto log_dir = (get_config_dir() / "logs").string();
		if (mgr.Log().Open(log_dir, params.name)) {
			LOG_MSG("LUA: Debug log at %s",
			        mgr.Log().FilePath().c_str());
		} else {
			// Not fatal to the load - the script still runs without
			// a debug log - but worth a loud log line since
			// script/log will otherwise silently 404 with no other
			// explanation.
			LOG_WARNING("LUA: Failed to open debug log in '%s'",
			            log_dir.c_str());
		}
	} else {
		mgr.Log().Close();
	}

	LOG_MSG("LUA: Script '%s' loaded", params.name.c_str());
}

void LuaLoadCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	const auto ct_check = ScriptValidator::ValidateContentType(
	        req.get_header_value("Content-Type"));
	if (!ct_check.ok) {
		res.status = ct_check.http_status;
		json err;
		err["error"] = ct_check.error;
		Webserver::send_json(res, err);
		return;
	}

	const auto body_check = ScriptValidator::ValidateBody(req.body);
	if (!body_check.ok) {
		res.status = body_check.http_status;
		json err;
		err["error"] = body_check.error;
		Webserver::send_json(res, err);
		return;
	}

	ScriptParams params;
	const auto param_check =
	        ScriptValidator::ValidateParams(req.get_param_value("name"),
	                                        req.get_param_value("seed"),
	                                        req.get_param_value("debug"),
	                                        params);
	if (!param_check.ok) {
		res.status = param_check.http_status;
		json err;
		err["error"] = param_check.error;
		Webserver::send_json(res, err);
		return;
	}

	// Checked last, after every free (parse-only) rejection: a request
	// that was always going to 415/413/400 should not also burn the
	// rate limiter's slot, or a caller fixing a Content-Type typo pays
	// a second 2-second wait for a request that could never have
	// succeeded anyway.
	static ScriptRateLimiter rate_limiter;
	int64_t retry_after_ms = 0;
	if (rate_limiter.ShouldReject(retry_after_ms)) {
		res.status = 429;
		res.set_header("Retry-After",
		               std::to_string((retry_after_ms + 999) / 1000));
		json err;
		err["error"] = "too many requests";
		Webserver::send_json(res, err);
		return;
	}

	LuaLoadCommand cmd(req.body, params);
	cmd.WaitForCompletion(5000);

	if (!cmd.error.empty()) {
		res.status = 400;
		json err;
		err["error"] = cmd.error;
		Webserver::send_json(res, err);
		return;
	}

	json result;
	result["status"] = "loaded";
	result["name"]   = params.name;
	Webserver::send_json(res, result);
}

// -- LuaStartCommand --

void LuaStartCommand::Execute()
{
	auto& mgr = ScriptManager::Instance();

	if (!mgr.Engine().HasLoadedScript()) {
		error = "no script loaded";
		return;
	}

	const auto state = mgr.Coroutine().State();
	if (state == ScriptState::Running || state == ScriptState::Yielded) {
		error = "script is already running";
		return;
	}

	mgr.Engine().ResetTimers();

	if (mgr.Params().seed.has_value()) {
		mgr.Engine().SeedRandom(mgr.Params().seed.value());
	}

	if (!mgr.Coroutine().Start(&mgr.Log())) {
		error = "failed to create coroutine";
		return;
	}

	if (mgr.Log().IsOpen()) {
		mgr.Log().Trace(0, "script started: %s", mgr.Params().name.c_str());
	}

	LOG_MSG("LUA: Script '%s' started", mgr.Params().name.c_str());
}

void LuaStartCommand::Post(const httplib::Request&, httplib::Response& res)
{
	LuaStartCommand cmd;
	cmd.WaitForCompletion(5000);

	if (!cmd.error.empty()) {
		res.status = 400;
		json err;
		err["error"] = cmd.error;
		Webserver::send_json(res, err);
		return;
	}

	json result;
	result["status"] = "started";
	Webserver::send_json(res, result);
}

// -- LuaStopCommand --

void LuaStopCommand::Execute()
{
	auto& mgr = ScriptManager::Instance();
	mgr.Coroutine().Stop();
	mgr.Log().Close();

	LOG_MSG("LUA: Script stopped");
}

void LuaStopCommand::Post(const httplib::Request&, httplib::Response& res)
{
	LuaStopCommand cmd;
	cmd.WaitForCompletion(5000);

	if (!cmd.error.empty()) {
		res.status = 400;
		json err;
		err["error"] = cmd.error;
		Webserver::send_json(res, err);
		return;
	}

	json result;
	result["status"] = "stopped";
	Webserver::send_json(res, result);
}

// -- LuaStatusCommand --

namespace {

// A truncation marker, matching the pre-3.8 depth cutoff's own placeholder
// ("...") so existing callers/tests that already expect that string for a
// cut-off subtree keep working unchanged.
LuaOutputNode TruncatedNode(LuaOutputBudget& budget)
{
	budget.truncated = true;
	LuaOutputNode node;
	node.kind         = LuaOutputNode::Kind::String;
	node.string_value = "...";
	return node;
}

} // namespace

LuaOutputNode LuaTableToNode(lua_State* L, int idx, int depth, LuaOutputBudget& budget)
{
	if (depth > MaxOutputDepth) {
		return TruncatedNode(budget);
	}

	// insert().second is false when the pointer was already present -
	// a cycle (this table is its own ancestor) or the same table
	// reachable via more than one path. Either way, stop here instead
	// of re-walking it: that's what keeps a handful of tables sharing
	// subtables from expanding into an exponential number of nodes.
	if (!budget.visited.insert(lua_topointer(L, idx)).second) {
		return TruncatedNode(budget);
	}

	LuaOutputNode node;
	node.kind = LuaOutputNode::Kind::Object;

	lua_pushnil(L);
	while (lua_next(L, idx) != 0) {
		// Every pair examined counts, whether or not it ends up
		// accepted below - otherwise a table built entirely of
		// unsupported-type keys or values (which never touches
		// nodes_used/bytes_used at all) would iterate in full
		// regardless of size.
		if (budget.pairs_examined >= MaxOutputNodes) {
			budget.truncated = true;
			lua_pop(L, 2); // both the key and the value lua_next
			               // just pushed
			break;
		}
		++budget.pairs_examined;

		std::string key;
		if (lua_isinteger(L, -2)) {
			key = std::to_string(lua_tointeger(L, -2));
		} else if (lua_type(L, -2) == LUA_TSTRING) {
			// lua_tolstring (not lua_tostring, which relies on the
			// caller running strlen on the result) so a key
			// containing an embedded NUL byte is captured whole
			// rather than silently truncated at the first one.
			size_t key_len      = 0;
			const char* key_ptr = lua_tolstring(L, -2, &key_len);
			key.assign(key_ptr, key_len);
		} else {
			lua_pop(L, 1);
			continue;
		}

		// Check order matters: lua_isstring returns true for numbers
		// due to coercion, so check specific types first. String
		// values are deliberately NOT copied into value.string_value
		// yet - only lua_tolstring'd for a pointer and length, an O(1)
		// peek into Lua's own buffer - so an oversized string can be
		// rejected by the budget check below without first paying for
		// the copy: without this, a 15 MB string referenced from many
		// small sibling tables would get fully memcpy'd into a
		// std::string on every occurrence before its budget rejection
		// discarded it, defeating the whole point of the byte budget.
		LuaOutputNode value;
		bool matched               = true;
		bool is_pending_lua_string = false;
		const char* pending_string = nullptr;
		size_t pending_string_len  = 0;
		if (lua_isboolean(L, -1)) {
			value.kind = LuaOutputNode::Kind::Bool;
			value.bool_value = static_cast<bool>(lua_toboolean(L, -1));
		} else if (lua_isinteger(L, -1)) {
			value.kind      = LuaOutputNode::Kind::Int;
			value.int_value = lua_tointeger(L, -1);
		} else if (lua_isnumber(L, -1)) {
			value.kind         = LuaOutputNode::Kind::Double;
			value.double_value = lua_tonumber(L, -1);
		} else if (lua_type(L, -1) == LUA_TSTRING) {
			value.kind            = LuaOutputNode::Kind::String;
			is_pending_lua_string = true;
			pending_string = lua_tolstring(L, -1, &pending_string_len);
		} else if (lua_istable(L, -1)) {
			// Recursing here can itself return a Kind::String node
			// (a truncation placeholder from a deeper
			// depth/visited-set cutoff) - is_pending_lua_string
			// stays false in that case, so it's never mistaken for
			// an actual Lua string value still awaiting its
			// deferred copy below.
			value = LuaTableToNode(L, lua_absindex(L, -1), depth + 1, budget);
		} else {
			matched = false;
		}

		lua_pop(L, 1);

		if (!matched) {
			continue;
		}

		const size_t added_bytes =
		        key.size() +
		        (value.kind == LuaOutputNode::Kind::String
		                 ? (is_pending_lua_string ? pending_string_len
		                                          : value.string_value.size())
		                 : 0);

		if (budget.nodes_used >= MaxOutputNodes ||
		    budget.bytes_used + added_bytes > MaxOutputBytes) {
			budget.truncated = true;
			lua_pop(L, 1); // the key lua_next left on the stack for
			               // its own resumption, which we're not doing
			break;
		}

		if (is_pending_lua_string) {
			value.string_value.assign(pending_string, pending_string_len);
		}

		++budget.nodes_used;
		budget.bytes_used += added_bytes;
		node.object_value.emplace_back(std::move(key), std::move(value));
	}

	return node;
}

nlohmann::json LuaOutputNodeToJson(const LuaOutputNode& node)
{
	switch (node.kind) {
	case LuaOutputNode::Kind::Bool: return node.bool_value;
	case LuaOutputNode::Kind::Int: return node.int_value;
	case LuaOutputNode::Kind::Double: return node.double_value;
	case LuaOutputNode::Kind::String: return node.string_value;
	case LuaOutputNode::Kind::Object: {
		json j;
		for (const auto& [key, child] : node.object_value) {
			j[key] = LuaOutputNodeToJson(child);
		}
		return j;
	}
	case LuaOutputNode::Kind::Null:
	default: return nullptr;
	}
}

void LuaStatusCommand::Execute()
{
	auto& mgr    = ScriptManager::Instance();
	result.state = mgr.Coroutine().State();
	result.error = mgr.Coroutine().ErrorMessage();
	result.frame = mgr.Coroutine().CurrentFrame();
	result.name  = mgr.Params().name;
	// mgr.Engine().HasLoadedScript() rules out the window right after a
	// *failed* reload: LuaLoadCommand::Execute commits mgr.Params() (and
	// resets the engine) before LoadScript is known to succeed, so on a
	// compile error mgr.Params().debug can still be true while nothing
	// is actually loaded - without this check, log_path would keep
	// naming a completely unrelated earlier script's log.
	if (mgr.Params().debug && mgr.Engine().HasLoadedScript()) {
		result.log_path = mgr.Log().FilePath();
	}

	// Serialize the dosbox.output table. The budget-bounded walk into a
	// plain owned structure happens here, on the emulation thread, while
	// the Lua state is actually valid; the nlohmann::json conversion and
	// dump happen later in Get(), on the web thread.
	auto* L = mgr.Engine().GetState();
	if (L) {
		lua_getglobal(L, "dosbox");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "output");
			if (lua_istable(L, -1)) {
				LuaOutputBudget budget;
				result.output = LuaTableToNode(
				        L, lua_absindex(L, -1), 0, budget);
				result.output_truncated = budget.truncated;
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
}

void LuaStatusCommand::Get(const httplib::Request&, httplib::Response& res)
{
	LuaStatusCommand cmd;
	cmd.WaitForCompletion(15000);

	json j;
	j["state"] = ScriptStateName(cmd.result.state);
	j["frame"] = cmd.result.frame;
	j["name"]  = cmd.result.name;

	if (!cmd.result.error.empty()) {
		j["error"] = cmd.result.error;
	}

	if (!cmd.result.output.IsEmpty()) {
		j["output"] = LuaOutputNodeToJson(cmd.result.output);
	}
	j["output_truncated"] = cmd.result.output_truncated;

	if (!cmd.result.log_path.empty()) {
		j["log_path"] = cmd.result.log_path;
	}

	Webserver::send_json(res, j);
}

// -- LuaLogCommand --

// Deliberately never routed through the Bridge: the emulation thread
// holds its own FILE* open on this same path and writes to it
// (DebugLog::Trace), so this independent read handle can observe a
// torn last line if it lands mid-write. Accepted rather than
// serializing log reads behind emulation-thread scheduling for what is
// fundamentally a disk read, not emulator state.
//
// `path` must come from trusted, in-process state (ScriptManager's own
// DebugLog::FilePath()) - never from caller-supplied/HTTP input. This
// function does no canonicalization, allowed-root check, or rejection
// of `..`/symlinks; LuaLogCommand::Get is safe only because it never
// wires request data into `path` in the first place.
std::optional<std::string> ReadLogTail(const std::string& path, bool& truncated)
{
	truncated = false;

	auto* f = std::fopen(path.c_str(), "rb");
	if (!f) {
		return std::nullopt;
	}

	// Plain fseek/ftell truncate to a 32-bit `long` under MSVC even in
	// 64-bit builds (LLP64) - cross_fseeko/cross_ftello (misc/cross.h)
	// are this codebase's existing fix for exactly that, already used
	// for the same reason in support.cpp/makeimg.cpp/bios_disk.cpp.
	if (cross_fseeko(f, 0, SEEK_END) != 0) {
		std::fclose(f);
		return std::nullopt;
	}
	const auto size = cross_ftello(f);
	if (size < 0) {
		std::fclose(f);
		return std::nullopt;
	}

	const auto file_size = static_cast<size_t>(size);
	const auto start = file_size > MaxLogTailBytes ? file_size - MaxLogTailBytes
	                                               : 0;
	truncated = start > 0;

	if (cross_fseeko(f, static_cast<cross_off_t>(start), SEEK_SET) != 0) {
		std::fclose(f);
		return std::nullopt;
	}

	std::string content(file_size - start, '\0');
	const auto bytes_read = std::fread(content.data(), 1, content.size(), f);
	content.resize(bytes_read);

	std::fclose(f);
	return content;
}

void LuaLogCommand::Execute()
{
	auto& mgr = ScriptManager::Instance();
	// Gated on the currently loaded script's own debug flag AND on a
	// script actually being loaded right now, not merely on whether a
	// log file exists on disk: once a non-debug script is loaded, or a
	// reload fails to compile (LuaLoadCommand::Execute commits
	// mgr.Params() and resets the engine before LoadScript is known to
	// succeed), the previous run's log no longer describes what's
	// currently loaded, even though DebugLog::Close() (deliberately)
	// leaves FilePath() pointing at it.
	debug_script_loaded = mgr.Params().debug && mgr.Engine().HasLoadedScript();
	if (debug_script_loaded) {
		log_path = mgr.Log().FilePath();
	}
}

void LuaLogCommand::Get(const httplib::Request&, httplib::Response& res)
{
	LuaLogCommand cmd;
	cmd.WaitForCompletion(5000);

	if (!cmd.debug_script_loaded) {
		res.status = 400;
		json err;
		err["error"] = "no debug script is loaded";
		Webserver::send_json(res, err);
		return;
	}

	if (cmd.log_path.empty()) {
		// A debug script genuinely is loaded, but its log never opened
		// (DebugLog::Open failed - unwritable/full/read-only logs dir);
		// LuaLoadCommand::Execute doesn't fail the load over this, so
		// distinguish it from "no debug script is loaded" rather than
		// reusing that (factually wrong here) message.
		res.status = 404;
		json err;
		err["error"] = "debug log was requested but failed to open on the engine side";
		Webserver::send_json(res, err);
		return;
	}

	bool truncated     = false;
	const auto content = ReadLogTail(cmd.log_path, truncated);
	if (!content) {
		res.status = 404;
		json err;
		err["error"] = "debug log file is not readable";
		Webserver::send_json(res, err);
		return;
	}

	json j;
	j["path"]      = cmd.log_path;
	j["truncated"] = truncated;
	j["content"]   = *content;
	Webserver::send_json(res, j);
}

} // namespace Lua

void LuaDispatchFrame(const uint64_t frame_number)
{
	Lua::ScriptManager::Instance().DispatchFrame(frame_number);
}

void LuaReapStalledWaits()
{
	Lua::ScriptManager::Instance().ReapStalledWaits();
}
