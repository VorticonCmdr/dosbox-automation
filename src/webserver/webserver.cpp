// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver.h"
#include "bridge.h"
#include "capabilities.h"
#include "capture.h"
#include "control.h"
#include "drive.h"
#include "input.h"
#include "private/auth.h"
#include "private/backtrace.h"
#include "private/batch.h"
#include "private/cpu.h"
#include "private/debug.h"
#include "private/disassemble.h"
#include "private/dos.h"
#include "private/freeze.h"
#include "private/io_port.h"
#include "private/memory.h"
#include "private/memory_snapshot.h"
#include "private/shutdown.h"
#include "video.h"
#include "wait.h"

#include "lua/lua_bridge_commands.h"

#include "dos/programs/mount_policy.h"
#include "gui/osd/osd.h"
#include "gui/osd/osd_port.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(WIN32)
#include <bcrypt.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "http/http.h"
#include "json/json.h"

#include "config/config.h"
#include "dosbox.h"
#include "misc/cross.h"
#include "misc/logging.h"
#include "misc/support.h"

using json = nlohmann::json;

namespace Webserver {

void send_json(httplib::Response& res, const nlohmann::json& j)
{
	res.set_content(j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace),
	                "application/json");
}

// Shared JSON error shape for the exception handler and the two
// pre-routing rejections below. `error` stays a plain string (the
// pre-1.5 shape callers already parse); `error_code` and `retryable`
// are new, additive fields a caller can key logic off without parsing
// the message text.
static void send_error(httplib::Response& res, const httplib::StatusCode status,
                       const std::string& message, const std::string& code,
                       const bool retryable = false)
{
	res.status = status;
	json j;
	j["error"]      = message;
	j["error_code"] = code;
	j["retryable"]  = retryable;
	send_json(res, j);
}

ErrorInfo ClassifyException(std::exception_ptr ep)
{
	ErrorInfo info;

	try {
		if (ep) {
			std::rethrow_exception(ep);
		}
	} catch (const BridgeTimeout& e) {
		// The routine failure, not a crash: a paused or minimized
		// emulator times out every bridge-backed route until the
		// window regains focus, or the queue drains. Retryable - the
		// same request can succeed once the emulation thread ticks.
		info.status    = httplib::StatusCode::ServiceUnavailable_503;
		info.message   = e.what();
		info.code      = "bridge_timeout";
		info.retryable = true;
	} catch (const BridgeNotPumping& e) {
		// Distinguished from bridge_timeout: the pump is already
		// known stale before this command was even queued, so the
		// caller learns that immediately instead of burning the full
		// request deadline finding out.
		info.status    = httplib::StatusCode::ServiceUnavailable_503;
		info.message   = e.what();
		info.code      = "not_pumping";
		info.retryable = true;
	} catch (const BridgeQueueFull& e) {
		// Too many callers are already blocked waiting on the
		// emulation thread; refusing outright beats queuing behind
		// an unbounded backlog.
		info.status    = httplib::StatusCode::TooManyRequests_429;
		info.message   = e.what();
		info.code      = "queue_full";
		info.retryable = true;
	} catch (const TooManyWaiters& e) {
		// Same shape as queue_full but a distinct resource: the wait
		// registry's own 4-waiter cap, unrelated to the Bridge queue.
		info.status    = httplib::StatusCode::TooManyRequests_429;
		info.message   = e.what();
		info.code      = "too_many_waiters";
		info.retryable = true;
	} catch (const std::invalid_argument& e) {
		// Safe to echo: num_param()/handler messages here are
		// parameter names and the caller's own values, never
		// anything from deeper in the emulator.
		info.status  = httplib::StatusCode::BadRequest_400;
		info.message = e.what();
		info.code    = "invalid_argument";
	} catch (const std::out_of_range& e) {
		info.status  = httplib::StatusCode::BadRequest_400;
		info.message = e.what();
		info.code    = "out_of_range";
	} catch (const nlohmann::json::exception& e) {
		// Also safe to echo: the message describes the caller's own
		// malformed body, not server state.
		info.status  = httplib::StatusCode::BadRequest_400;
		info.message = e.what();
		info.code    = "malformed_body";
	} catch (const std::exception&) {
		// Deliberately generic (the ErrorInfo default): an exception
		// from deeper in the emulator may carry a filesystem path or
		// other detail that should not reach an HTTP caller.
	} catch (...) {
	}

	return info;
}

static void error_handler(const httplib::Request&, httplib::Response& res,
                          std::exception_ptr ep)
{
	const auto info = ClassifyException(ep);
	if (info.retryable) {
		res.set_header("Retry-After", "1");
	}
	send_error(res, info.status, info.message, info.code, info.retryable);
}

bool IsPublicDocPath(const std::string& method, const std::string& path)
{
	if (method != "GET" && method != "HEAD") {
		return false;
	}

	// Exact match only. A prefix or normalized match would let
	// "/openapi.json/../api_token" or an encoded traversal reach the config
	// mount, which holds the API token file.
	static const std::set<std::string> public_paths = {
	        "/",
	        "/index.html",
	        "/style-index.css",
	        "/api.html",
	        "/openapi.json",
	        "/swagger-ui.css",
	        "/swagger-ui-bundle.js",
	        "/debugger.html",
	};
	return public_paths.count(path) > 0;
}

bool IsPublicApiPath(const std::string& method, const std::string& path)
{
	if (method != "GET" && method != "HEAD") {
		return false;
	}
	return path == "/api/v1/hello";
}

static httplib::Server server;

enum class HttpMethod { Get, Post, Put, Delete };

struct ApiRoute {
	HttpMethod method;
	std::string path;
	httplib::Server::Handler handler;
	TokenScope required_scope;
};

// Single source of truth for the API surface: setup_api_handlers() below
// registers every route from this table, RegisteredApiRoutes()
// (webserver.h) exposes the same (method, path) pairs so a test can walk
// them against resources/webserver/openapi.json and catch drift instead
// of it silently accumulating (4.1), and RequiredScopeFor (4.7) reads
// each route's required_scope straight from the same entry - there is
// no separate scope table that could fall out of sync with either. GET
// /api/v1/dosbox/info is the one route not in this table - its handler
// captures per-instance runtime state (instance_id, pid, start time)
// generated fresh in run(), so it's registered separately below;
// RegisteredApiRoutes() and RequiredScopeFor() both special-case it back
// in.
static const std::vector<ApiRoute>& get_api_route_table()
{
	using enum TokenScope;
	static const std::vector<ApiRoute> routes = {
	        {   HttpMethod::Get,"/api/v1/cpu/state",                 CpuStateCommand::Get,Read	                                                                                                   },

	        {   HttpMethod::Get,         "/api/v1/dos/internals",             DosInternalsCommand::Get,    Read},
	        {   HttpMethod::Get,               "/api/v1/dos/ems",             EmsInternalsCommand::Get,    Read},
	        {   HttpMethod::Get,               "/api/v1/dos/xms",             XmsInternalsCommand::Get,    Read},

	        {  HttpMethod::Post,       "/api/v1/dosbox/shutdown",                ShutdownCommand::Post, Control},

	        {  HttpMethod::Post,       "/api/v1/memory/allocate",             AllocMemoryCommand::Post,   Write},
	        {  HttpMethod::Post,           "/api/v1/memory/free",              FreeMemoryCommand::Post,   Write},
	        {   HttpMethod::Get,
	         "/api/v1/memory/allocations",        MemoryAllocationsCommand::Get,
	         Read	                                                                                      },
	        {  HttpMethod::Post,         "/api/v1/memory/search",            SearchMemoryCommand::Post,    Read},
	        {  HttpMethod::Post,           "/api/v1/memory/scan",              ScanMemoryCommand::Post,    Read},
	        {  HttpMethod::Post,
	         "/api/v1/memory/snapshot",         MemorySnapshotHandlers::Post,
	         Read	                                                                                      },
	        {  HttpMethod::Post,           "/api/v1/memory/diff",             MemoryDiffHandlers::Post,    Read},
	        {  HttpMethod::Post,         "/api/v1/memory/freeze",                 FreezeHandlers::Post,   Write},
	        {   HttpMethod::Get,         "/api/v1/memory/freeze",                  FreezeHandlers::Get,    Read},
	        {HttpMethod::Delete,         "/api/v1/memory/freeze",               FreezeHandlers::Delete,   Write},
	        {   HttpMethod::Get,   "/api/v1/memory/:offset/:len",               ReadMemoryCommand::Get,    Read},
	        {   HttpMethod::Get,
	         "/api/v1/memory/:segment/:offset/:len",               ReadMemoryCommand::Get,
	         Read	                                                                                      },
	        {   HttpMethod::Put,        "/api/v1/memory/:offset",              WriteMemoryCommand::Put,   Write},
	        {   HttpMethod::Put,
	         "/api/v1/memory/:segment/:offset",              WriteMemoryCommand::Put,
	         Write	                                                                                     },

	        {   HttpMethod::Get,               "/api/v1/io/port",                 PortReadCommand::Get,    Read},
	        {   HttpMethod::Put,               "/api/v1/io/port",                PortWriteCommand::Put,   Write},
	        {   HttpMethod::Put,          "/api/v1/cpu/register",            WriteRegisterCommand::Put,   Write},

	        {  HttpMethod::Post,                 "/api/v1/batch",                   BatchCommand::Post,   Write},

	        {   HttpMethod::Get,          "/api/v1/debug/status",              DebugStatusCommand::Get,   Debug},
	        {  HttpMethod::Post,           "/api/v1/debug/pause",              DebugPauseCommand::Post,   Debug},
	        {  HttpMethod::Post,        "/api/v1/debug/continue",           DebugContinueCommand::Post,   Debug},
	        {  HttpMethod::Post,            "/api/v1/debug/step",               DebugStepCommand::Post,   Debug},
	        {  HttpMethod::Post,
	         "/api/v1/debug/step_over",           DebugStepOverCommand::Post,
	         Debug	                                                                                     },
	        {  HttpMethod::Post,          "/api/v1/debug/run_to",              DebugRunToCommand::Post,   Debug},
	        {  HttpMethod::Post,        "/api/v1/debug/step_out",            DebugStepOutCommand::Post,   Debug},
	        {   HttpMethod::Get,            "/api/v1/debug/wait",               DebugWaitHandlers::Get,   Debug},

	        {   HttpMethod::Get,
	         "/api/v1/debug/disassemble/:segment/:offset/:count",              DisassembleCommand::Get,
	         Debug	                                                                                     },
	        {   HttpMethod::Get,       "/api/v1/debug/backtrace",                BacktraceCommand::Get,   Debug},

	        {   HttpMethod::Get,
	         "/api/v1/debug/breakpoints",     DebugListBreakpointsCommand::Get,
	         Debug	                                                                                     },
	        {  HttpMethod::Post,
	         "/api/v1/debug/breakpoints",      DebugAddBreakpointCommand::Post,
	         Debug	                                                                                     },
	        {HttpMethod::Delete,
	         "/api/v1/debug/breakpoints", DebugDeleteBreakpointCommand::Delete,
	         Debug	                                                                                     },

	        {   HttpMethod::Get,         "/api/v1/debug/watches",         DebugListWatchesCommand::Get,   Debug},
	        {  HttpMethod::Post,         "/api/v1/debug/watches",           DebugAddWatchCommand::Post,   Debug},
	        {HttpMethod::Delete,
	         "/api/v1/debug/watches",      DebugDeleteWatchCommand::Delete,
	         Debug	                                                                                     },

	        {  HttpMethod::Post,        "/api/v1/input/sequence",           InputSequenceCommand::Post,   Input},
	        {  HttpMethod::Post,            "/api/v1/input/type",               InputTypeCommand::Post,   Input},
	        {   HttpMethod::Get,
	         "/api/v1/input/replay/status",            ReplayHandlers::GetStatus,
	         Input	                                                                                     },
	        {HttpMethod::Delete,
	         "/api/v1/input/replay",          ReplayCancelCommand::Delete,
	         Input	                                                                                     },
	        {   HttpMethod::Get,           "/api/v1/input/mouse",         MouseGetPositionCommand::Get,   Input},
	        {  HttpMethod::Post,           "/api/v1/input/mouse",        MouseSetPositionCommand::Post,   Input},

	        {   HttpMethod::Get,           "/api/v1/video/frame",              VideoHandlers::GetFrame,   Media},
	        {   HttpMethod::Get,
	         "/api/v1/video/frame/info",          VideoHandlers::GetFrameInfo,
	         Media	                                                                                     },
	        {   HttpMethod::Get,            "/api/v1/video/text",               ScreenTextCommand::Get,   Media},

	        {   HttpMethod::Get,
	         "/api/v1/program/state",     ControlHandlers::GetProgramState,
	         Read	                                                                                      },
	        {   HttpMethod::Get,                "/api/v1/status",           ControlHandlers::GetStatus,    Read},
	        {  HttpMethod::Post,      "/api/v1/control/shutdown",                ShutdownCommand::Post, Control},
	        {   HttpMethod::Get,                 "/api/v1/hello",            ControlHandlers::GetHello,    Read},

	        {  HttpMethod::Post,                  "/api/v1/wait",                   WaitHandlers::Post,    Read},

	        {   HttpMethod::Get,                 "/api/v1/drive",                DriveListCommand::Get,    Read},
	        {  HttpMethod::Post,            "/api/v1/drive/swap",               DriveSwapCommand::Post, Control},
	        {  HttpMethod::Post,           "/api/v1/drive/mount",              DriveMountCommand::Post, Control},

	        {  HttpMethod::Post,            "/api/v1/mount/lock",              MountHandlers::PostLock, Control},
	        {   HttpMethod::Get,            "/api/v1/mount/lock",               MountHandlers::GetLock,    Read},
	        {   HttpMethod::Get,          "/api/v1/mount/policy",             MountHandlers::GetPolicy,    Read},
	        {   HttpMethod::Get,          "/api/v1/mount/images",             MountHandlers::GetImages,    Read},

	        {  HttpMethod::Post,
	         "/api/v1/input/record/start",         RecordingHandlers::PostStart,
	         Input	                                                                                     },
	        {  HttpMethod::Post,
	         "/api/v1/input/record/pause",         RecordingHandlers::PostPause,
	         Input	                                                                                     },
	        {  HttpMethod::Post,
	         "/api/v1/input/record/stop",          RecordingHandlers::PostStop,
	         Input	                                                                                     },
	        {   HttpMethod::Get,
	         "/api/v1/input/record/status",         RecordingHandlers::GetStatus,
	         Input	                                                                                     },
	        {   HttpMethod::Get,
	         "/api/v1/input/recordings",      RecordingStoreHandlers::GetList,
	         Input	                                                                                     },
	        {HttpMethod::Delete,
	         "/api/v1/input/recordings/:name",       RecordingStoreHandlers::Delete,
	         Input	                                                                                     },

	        {  HttpMethod::Post,           "/api/v1/script/load",            Lua::LuaLoadCommand::Post,  Script},
	        {  HttpMethod::Post,          "/api/v1/script/start",           Lua::LuaStartCommand::Post,  Script},
	        {  HttpMethod::Post,           "/api/v1/script/stop",            Lua::LuaStopCommand::Post,  Script},
	        {   HttpMethod::Get,         "/api/v1/script/status",           Lua::LuaStatusCommand::Get,  Script},
	        {   HttpMethod::Get,            "/api/v1/script/log",              Lua::LuaLogCommand::Get,  Script},

	        {  HttpMethod::Post,
	         "/api/v1/capture/video/start",            CaptureStartCommand::Post,
	         Media	                                                                                     },
	        {  HttpMethod::Post,
	         "/api/v1/capture/video/stop",             CaptureStopCommand::Post,
	         Media	                                                                                     },
	        {   HttpMethod::Get,
	         "/api/v1/capture/video/status",            CaptureStatusCommand::Get,
	         Media	                                                                                     },
	        {   HttpMethod::Get,
	         "/api/v1/capture/video/compression",    CaptureCompressionGetCommand::Get,
	         Media	                                                                                     },
	        {   HttpMethod::Put,
	         "/api/v1/capture/video/compression",    CaptureCompressionSetCommand::Put,
	         Media	                                                                                     },
	};
	return routes;
}

static void setup_api_handlers()
{
	for (const auto& route : get_api_route_table()) {
		switch (route.method) {
		case HttpMethod::Get:
			server.Get(route.path, route.handler);
			break;
		case HttpMethod::Post:
			server.Post(route.path, route.handler);
			break;
		case HttpMethod::Put:
			server.Put(route.path, route.handler);
			break;
		case HttpMethod::Delete:
			server.Delete(route.path, route.handler);
			break;
		}
	}
}

std::vector<std::pair<std::string, std::string>> RegisteredApiRoutes()
{
	std::vector<std::pair<std::string, std::string>> routes;
	for (const auto& route : get_api_route_table()) {
		const char* method = "GET";
		switch (route.method) {
		case HttpMethod::Get: method = "GET"; break;
		case HttpMethod::Post: method = "POST"; break;
		case HttpMethod::Put: method = "PUT"; break;
		case HttpMethod::Delete: method = "DELETE"; break;
		}
		routes.emplace_back(method, route.path);
	}
	// See get_api_route_table()'s comment: the one route registered outside
	// that table, because its handler closes over per-instance runtime
	// state rather than being a plain static function.
	routes.emplace_back("GET", "/api/v1/dosbox/info");
	return routes;
}

std::string TokenScopeName(const TokenScope scope)
{
	switch (scope) {
	case TokenScope::Read: return "read";
	case TokenScope::Write: return "write";
	case TokenScope::Input: return "input";
	case TokenScope::Script: return "script";
	case TokenScope::Media: return "media";
	case TokenScope::Debug: return "debug";
	case TokenScope::Control: return "control";
	}
	return "";
}

static const std::vector<std::pair<std::string, TokenScope>>& scope_names_table()
{
	static const std::vector<std::pair<std::string, TokenScope>> names = {
	        {   TokenScopeName(TokenScope::Read),    TokenScope::Read},
	        {  TokenScopeName(TokenScope::Write),   TokenScope::Write},
	        {  TokenScopeName(TokenScope::Input),   TokenScope::Input},
	        { TokenScopeName(TokenScope::Script),  TokenScope::Script},
	        {  TokenScopeName(TokenScope::Media),   TokenScope::Media},
	        {  TokenScopeName(TokenScope::Debug),   TokenScope::Debug},
	        {TokenScopeName(TokenScope::Control), TokenScope::Control},
	};
	return names;
}

std::optional<std::set<TokenScope>> ParseTokenScopes(const std::string& value)
{
	if (value.empty()) {
		return std::nullopt;
	}
	std::set<TokenScope> granted = {};
	std::stringstream stream(value);
	std::string entry;
	while (std::getline(stream, entry, ',')) {
		// Trim surrounding whitespace so "read, input" parses the same
		// as "read,input".
		const auto first = entry.find_first_not_of(" \t");
		const auto last  = entry.find_last_not_of(" \t");
		if (first == std::string::npos) {
			continue;
		}
		entry = entry.substr(first, last - first + 1);

		const auto it = std::find_if(scope_names_table().begin(),
		                             scope_names_table().end(),
		                             [&](const auto& pair) {
			                             return pair.first == entry;
		                             });
		if (it == scope_names_table().end()) {
			LOG_WARNING(
			        "WEBSERVER: Unknown webserver_token_scopes entry "
			        "'%s', ignoring",
			        entry.c_str());
			continue;
		}
		granted.insert(it->second);
	}
	return granted;
}

namespace {
std::mutex granted_scopes_mutex;
std::optional<std::set<TokenScope>> granted_scopes_storage;
} // namespace

void SetGrantedTokenScopes(std::optional<std::set<TokenScope>> scopes)
{
	const std::lock_guard<std::mutex> lock(granted_scopes_mutex);
	granted_scopes_storage = std::move(scopes);
}

bool ScopeAllowed(const TokenScope scope)
{
	const std::lock_guard<std::mutex> lock(granted_scopes_mutex);
	return !granted_scopes_storage.has_value() ||
	       granted_scopes_storage->count(scope) > 0;
}

// httplib's own path pattern matches segment-by-segment, with a ':name'
// segment accepting any single non-empty path segment - this reimplements
// just that subset so scope lookup can classify a route before httplib's
// real router runs (the pre-routing handler fires first, on the request's
// concrete path, not the matched pattern).
static bool path_matches_pattern(const std::string& path, const std::string& pattern)
{
	const auto split = [](const std::string& s) {
		std::vector<std::string> parts;
		std::stringstream stream(s);
		std::string part;
		while (std::getline(stream, part, '/')) {
			parts.push_back(part);
		}
		return parts;
	};
	const auto path_parts    = split(path);
	const auto pattern_parts = split(pattern);
	if (path_parts.size() != pattern_parts.size()) {
		return false;
	}
	for (size_t i = 0; i < pattern_parts.size(); ++i) {
		const auto& p = pattern_parts[i];
		if (!p.empty() && p[0] == ':') {
			if (path_parts[i].empty()) {
				return false;
			}
			continue;
		}
		if (path_parts[i] != p) {
			return false;
		}
	}
	return true;
}

std::optional<TokenScope> RequiredScopeFor(const std::string& method_in,
                                           const std::string& path)
{
	// httplib answers HEAD from a registered GET handler without a
	// separate registration (same reason IsPublicDocPath/IsPublicApiPath
	// both explicitly accept HEAD alongside GET) - classify it the same
	// as GET, or every HEAD request would 403 as soon as scoping is
	// turned on even though the identical GET keeps working.
	const std::string& method = (method_in == "HEAD") ? "GET" : method_in;

	// See get_api_route_table()'s comment: registered outside that table
	// because its handler closes over per-instance runtime state.
	if (method == "GET" && path == "/api/v1/dosbox/info") {
		return TokenScope::Read;
	}
	for (const auto& route : get_api_route_table()) {
		const char* route_method = "GET";
		switch (route.method) {
		case HttpMethod::Get: route_method = "GET"; break;
		case HttpMethod::Post: route_method = "POST"; break;
		case HttpMethod::Put: route_method = "PUT"; break;
		case HttpMethod::Delete: route_method = "DELETE"; break;
		}
		if (method == route_method && path_matches_pattern(path, route.path)) {
			return route.required_scope;
		}
	}
	return std::nullopt;
}

static std::string strip_port(const std::string& host)
{
	// IPv6 literal: [::1]:8080
	if (host.size() > 1 && host[0] == '[') {
		const auto bracket = host.rfind(']');
		if (bracket != std::string::npos) {
			return host.substr(0, bracket + 1);
		}
		return host;
	}

	// IPv4 or hostname: 127.0.0.1:8080
	const auto colon = host.rfind(':');
	if (colon != std::string::npos) {
		return host.substr(0, colon);
	}
	return host;
}

static void fill_random_bytes(uint8_t* buf, const size_t len)
{
#if defined(WIN32)
	// BCryptGenRandom is the Windows CSPRNG. std::random_device on
	// MinGW has been deterministic on some toolchains.
	const auto status = BCryptGenRandom(nullptr,
	                                    buf,
	                                    static_cast<ULONG>(len),
	                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (!BCRYPT_SUCCESS(status)) {
		E_Exit("WEBSERVER: BCryptGenRandom failed (0x%08lx)", status);
	}
#else
	// On Linux/macOS, /dev/urandom is the standard CSPRNG source.
	auto* f = fopen("/dev/urandom", "rb");
	if (!f || fread(buf, 1, len, f) != len) {
		if (f) {
			fclose(f);
		}
		E_Exit("WEBSERVER: Failed to read /dev/urandom");
	}
	fclose(f);
#endif
}

static std::string to_hex(const uint8_t* buf, const size_t len)
{
	constexpr char hex[] = "0123456789abcdef";
	std::string s;
	s.reserve(len * 2);
	for (size_t i = 0; i < len; ++i) {
		s += hex[(buf[i] >> 4) & 0xF];
		s += hex[buf[i] & 0xF];
	}
	return s;
}

static std::string generate_api_token()
{
	uint8_t buf[32] = {};
	fill_random_bytes(buf, sizeof(buf));
	return to_hex(buf, sizeof(buf));
}

// 128 bits: enough to make a collision between two instances started in
// the same second astronomically unlikely, without the token's larger
// size (this identifies a process, it does not authenticate anything).
static std::string generate_instance_id()
{
	uint8_t buf[16] = {};
	fill_random_bytes(buf, sizeof(buf));
	return to_hex(buf, sizeof(buf));
}

static std::filesystem::path token_file_path = {};

static std::filesystem::path get_token_file_dir()
{
	return get_config_dir() / DefaultWebserverDir;
}

static bool write_token_file(const std::string& token)
{
	namespace fs = std::filesystem;

	const auto dir  = get_token_file_dir();
	const auto path = dir / "api_token";

	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		LOG_WARNING("WEBSERVER: Cannot create token dir '%s': %s",
		            dir.string().c_str(),
		            ec.message().c_str());
		return false;
	}

	const auto tmp = dir / "api_token.tmp";

	{
		auto out = std::ofstream(tmp, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) {
			LOG_WARNING("WEBSERVER: Cannot write token file '%s'",
			            tmp.string().c_str());
			return false;
		}
		out << token;
	}

#if !defined(WIN32)
	fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, ec);
	if (ec) {
		LOG_WARNING("WEBSERVER: Cannot set permissions on '%s'",
		            tmp.string().c_str());
		fs::remove(tmp, ec);
		return false;
	}
#endif

	fs::rename(tmp, path, ec);
	if (ec) {
		LOG_WARNING("WEBSERVER: Cannot rename token file: %s",
		            ec.message().c_str());
		fs::remove(tmp, ec);
		return false;
	}

	token_file_path = path;
	return true;
}

static void remove_token_file()
{
	if (token_file_path.empty()) {
		return;
	}
	std::error_code ec;
	std::filesystem::remove(token_file_path, ec);
	token_file_path.clear();
}

static bool is_valid_hex_token(const std::string& s)
{
	if (s.size() != 64) {
		return false;
	}
	for (const char c : s) {
		if (!std::isxdigit(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	return true;
}

static std::string extract_bearer_token(const std::string& auth_header)
{
	constexpr auto prefix = std::string_view("Bearer ");
	if (auth_header.size() > prefix.size() &&
	    auth_header.compare(0, prefix.size(), prefix) == 0) {
		return auth_header.substr(prefix.size());
	}
	return {};
}

static void setup_security(const std::string& addr, int port,
                           const std::string& api_token,
                           const std::optional<std::set<TokenScope>>& granted_scopes)
{
	std::set<std::string> allowed_hosts;

	const auto port_str = ":" + std::to_string(port);

	auto add = [&](const std::string& hostname) {
		allowed_hosts.emplace(hostname);
		allowed_hosts.emplace(hostname + port_str);
	};

	add(addr);

	if (addr == "127.0.0.1" || addr == "0.0.0.0") {
		add("localhost");
	}
	if (addr == "::1" || addr == "::") {
		add("localhost");
		add("[::1]");
	}

	server.set_pre_routing_handler([allowed_hosts = std::move(allowed_hosts),
	                                api_token,
	                                granted_scopes](const httplib::Request& req,
	                                                httplib::Response& res) {
		const auto host = strip_port(req.get_header_value("Host"));

		if (allowed_hosts.find(host) == allowed_hosts.end()) {
			LOG_WARNING("WEBSERVER: Rejected request with Host header '%s'",
			            req.get_header_value("Host").c_str());
			send_error(res,
			           httplib::StatusCode::Forbidden_403,
			           "Forbidden",
			           "forbidden_host");
			return httplib::Server::HandlerResponse::Handled;
		}

		// Documentation assets and the GET /api/v1/hello handshake are
		// browsable/callable without a token (Host check still
		// applies). Every other /api/v1 endpoint is not.
		if (IsPublicDocPath(req.method, req.path) ||
		    IsPublicApiPath(req.method, req.path)) {
			return httplib::Server::HandlerResponse::Unhandled;
		}

		const auto token = extract_bearer_token(
		        req.get_header_value("Authorization"));

		if (!ConstantTimeEquals(token, api_token)) {
			LOG_WARNING("WEBSERVER: Rejected request with invalid token");
			send_error(res,
			           httplib::StatusCode::Unauthorized_401,
			           "Unauthorized",
			           "unauthorized");
			return httplib::Server::HandlerResponse::Handled;
		}

		// webserver_token_scopes (4.7): unset (nullopt) means no
		// restriction was configured - one all-powerful token, today's
		// default behavior. When set, a route this server doesn't
		// recognize (RequiredScopeFor returning nullopt) denies rather
		// than allows, so a route someone forgot to classify fails
		// closed instead of silently bypassing the check.
		if (granted_scopes.has_value()) {
			const auto required = RequiredScopeFor(req.method, req.path);
			const bool ok = required.has_value() &&
			                granted_scopes->count(*required) > 0;
			if (!ok) {
				LOG_WARNING(
				        "WEBSERVER: Rejected %s %s - token lacks "
				        "required scope",
				        req.method.c_str(),
				        req.path.c_str());
				send_error(res,
				           httplib::StatusCode::Forbidden_403,
				           required.has_value()
				                   ? "Token is missing the '" +
				                             TokenScopeName(*required) +
				                             "' scope"
				                   : "Token scope not configured for this route",
				           "insufficient_scope");
				return httplib::Server::HandlerResponse::Handled;
			}
		}

		return httplib::Server::HandlerResponse::Unhandled;
	});

	server.set_default_headers({
	        {"X-Content-Type-Options", "nosniff"},
	});

	server.Options(".*", [](const httplib::Request&, httplib::Response& res) {
		res.status = httplib::StatusCode::Forbidden_403;
	});

	server.set_payload_max_length(MaxRequestBodyBytes);
}

static void run(const std::string addr, const int port,
                const std::string resource_home, const bool use_token_file,
                const std::string token_scopes_setting)
{
	const auto config_home = (get_config_dir() / DefaultWebserverDir).string();
	const auto granted_scopes = ParseTokenScopes(token_scopes_setting);
	// Also published for the Lua API (lua_api.cpp), which never passes
	// through the pre-routing handler below - see ScopeAllowed's comment.
	SetGrantedTokenScopes(granted_scopes);

	// Channel A: a launcher can supply the token via env var so it
	// never needs to scrape stderr or read a file.
	std::string api_token;
	bool token_from_env = false;

	const char* env_token = std::getenv("DOSBOX_API_TOKEN");
	if (env_token) {
		std::string candidate(env_token);
		if (is_valid_hex_token(candidate)) {
			api_token      = std::move(candidate);
			token_from_env = true;
		} else {
			LOG_WARNING(
			        "WEBSERVER: DOSBOX_API_TOKEN set but invalid "
			        "(need 64 hex chars), generating token");
		}
	}

	if (api_token.empty()) {
		api_token = generate_api_token();
	}

	// Identity for restart detection (3.6): a bridge that re-attaches
	// after a 401 can tell "same process, stale token" apart from
	// "this is a different process" by comparing instance_id, instead
	// of blindly replaying a possibly-mutating request into a fresh
	// guest session.
	const auto instance_id = generate_instance_id();
	const auto pid =
#if defined(WIN32)
	        static_cast<int64_t>(GetCurrentProcessId());
#else
	        static_cast<int64_t>(getpid());
#endif
	const auto started_at_unix =
	        std::chrono::duration_cast<std::chrono::seconds>(
	                std::chrono::system_clock::now().time_since_epoch())
	                .count();
	const auto start_steady = std::chrono::steady_clock::now();

	server.set_mount_point("/", config_home);
	server.set_mount_point("/", resource_home);

	setup_api_handlers();
	setup_security(addr, port, api_token, granted_scopes);

	server.set_exception_handler(error_handler);

	server.Get("/api/v1/dosbox/info",
	           [instance_id, pid, started_at_unix, start_steady](
	                   const httplib::Request&, httplib::Response& res) {
		           const auto capabilities = BuildCapabilitiesBlock();
		           const auto uptime_ms =
		                   std::chrono::duration_cast<std::chrono::milliseconds>(
		                           std::chrono::steady_clock::now() - start_steady)
		                           .count();

		           json j;
		           j["name"]         = EngineName;
		           j["mcp_protocol"] = McpProtocol;
		           j["version"]      = DOSBOX_GetDetailedVersion();
		           j["features"]     = FeaturesProjection(capabilities);
		           j["capabilities"] = capabilities;
		           j["limits"]       = BuildServerLimits();
		           j["instance_id"]  = instance_id;
		           j["pid"]          = pid;
		           j["started_at_unix"] = started_at_unix;
		           j["uptime_ms"]       = uptime_ms;
		           send_json(res, j);
	           });

	// Channel B: write the auto-generated token to a file so launchers
	// can read it without scraping stderr.
	if (use_token_file && !token_from_env) {
		if (write_token_file(api_token)) {
			LOG_MSG("WEBSERVER: Token written to %s",
			        token_file_path.string().c_str());
		} else {
			LOG_MSG("WEBSERVER: API token: %.8s...", api_token.c_str());
		}
	} else if (token_from_env) {
		LOG_MSG("WEBSERVER: Using API token from DOSBOX_API_TOKEN");
	} else {
		LOG_MSG("WEBSERVER: API token: %.8s...", api_token.c_str());
	}

	LOG_INFO("WEBSERVER: Starting HTTP REST API on http://%s:%d",
	         addr.c_str(),
	         port);

	auto ok = server.listen(addr, port);
	if (!ok) {
		LOG_WARNING("WEBSERVER: Failed to bind to %s:%d", addr.c_str(), port);
	}
}

static void init_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto enabled = section.AddBool("webserver_enabled", OnlyAtStart, false);
	enabled->SetHelp(
	        "Enable the HTTP REST API that exposes internal state and memory (disabled by\n"
	        "default). Open http://localhost:8386 in a browser (or use the configured port)\n"
	        "to view the API documentation.\n"
	        "\n"
	        "An API token is generated at startup and written to a token file\n"
	        "by default (see webserver_token_file); only a short preview is\n"
	        "printed to the log output. All API requests require\n"
	        "Authorization: Bearer <token>.");

	auto bind_ip = section.AddString("webserver_bind_address",
	                                 OnlyAtStart,
	                                 "127.0.0.1");
	bind_ip->SetHelp(
	        "Bind to the given IP address. By default only local connections are\n"
	        "allowed. Binding to 0.0.0.0 or :: requires webserver_allow_remote=true.");

	auto bind_port = section.AddInt("webserver_port", OnlyAtStart, 8386);
	bind_port->SetMinMax(1, 0xFFFF);
	bind_port->SetHelp("TCP port to bind to.");

	auto allow_remote = section.AddBool("webserver_allow_remote", OnlyAtStart, false);
	allow_remote->SetHelp(
	        "Allow binding to non-localhost addresses (0.0.0.0 or ::). This exposes\n"
	        "the full API to the network. Do not enable unless you understand the\n"
	        "security implications.");

	auto token_file = section.AddBool("webserver_token_file", OnlyAtStart, true);
	token_file->SetHelp(
	        "Write the full API token to a file (enabled by default); the log\n"
	        "only ever gets a short preview. The file is written to the webserver\n"
	        "config directory with restricted permissions (0600) and removed on\n"
	        "clean shutdown. Launchers and tools can read the token from this\n"
	        "file instead of scraping log output. Set to false to suppress the\n"
	        "file and rely on DOSBOX_API_TOKEN instead - with both off, the full\n"
	        "token is not obtainable. Has no effect when DOSBOX_API_TOKEN is set\n"
	        "via environment variable.");

	auto token_scopes = section.AddString("webserver_token_scopes", OnlyAtStart, "");
	token_scopes->SetHelp(
	        "Restrict what the API token can do, as a comma-separated list of\n"
	        "scopes (unset by default, meaning no restriction - the token can do\n"
	        "everything). Available scopes: read, write, input, script, media,\n"
	        "debug, control. A route this build does not recognize is refused\n"
	        "once any scope is configured, even if the list would otherwise seem\n"
	        "to cover it. Example: \"read,input,media\" for an agent that should\n"
	        "only observe the screen and send input, never write memory, load\n"
	        "scripts, use the debugger, or shut the machine down.\n"
	        "\n"
	        "A running Lua script (script/load, script/start) is held to the\n"
	        "same grant: dosbox.mem_write needs write, dosbox.key/type/\n"
	        "mouse_* need input, dosbox.screen_text/capture_start/stop need\n"
	        "media, dosbox.mount_lock needs control. Granting 'script' alone\n"
	        "only lets a script load and run - not what it does once running.\n"
	        "dosbox.osd/log/debugmsg/abort are never gated: host-side only,\n"
	        "they never reach guest memory, input, or captured output.");

	auto osd = section.AddBool("webserver_osd", OnlyAtStart, true);
	osd->SetHelp(
	        "Show on-screen indicators while automation is driving the machine\n"
	        "(script running, recording, replay, injected input). Enabled by\n"
	        "default so it is always clear when the machine is under remote\n"
	        "control. Set to false to hide the overlay.");

	// The mount policy reads these two settings straight from the primary
	// config file (mount_policy.cpp) so that -conf files and command line
	// overrides cannot widen the mount roots. They are registered here so
	// the config system knows them: otherwise every config parse logs an
	// unknown-setting warning, and they would be missing from the
	// generated config and the setting documentation.
	auto mount_bases = section.AddString("mount_allowed_bases", OnlyAtStart, "");
	mount_bases->SetHelp(
	        "Additional base directories that MOUNT may expose to the guest, as\n"
	        "a semicolon-separated list (unset by default). Paths with symlink\n"
	        "components are rejected. For security, this setting is only honored\n"
	        "in the primary config file; -conf files and command line overrides\n"
	        "are ignored.");

	auto mount_image_roots = section.AddString("mount_allowed_image_roots",
	                                           OnlyAtStart,
	                                           "");
	mount_image_roots->SetHelp(
	        "Directories that floppy and CD images may be mounted or swapped\n"
	        "from, as a semicolon-separated list (unset by default). Follows the\n"
	        "same rules as mount_allowed_bases: only the primary config file is\n"
	        "honored.");
}

} // namespace Webserver

static bool is_webserver_enabled = false;

static bool is_remote_address(const std::string& addr)
{
	return addr == "0.0.0.0" || addr == "::";
}

void WEBSERVER_Init()
{
	MountPolicy::InitPolicyConfig(get_primary_config_path());

	auto section = get_section("webserver");

	if (section->GetBool("webserver_enabled")) {
		const auto addr = section->GetString("webserver_bind_address");

		if (is_remote_address(addr) &&
		    !section->GetBool("webserver_allow_remote")) {
			LOG_WARNING(
			        "WEBSERVER: Refusing to bind to %s without "
			        "webserver_allow_remote=true",
			        addr.c_str());
			return;
		}

		if (is_remote_address(addr)) {
			LOG_WARNING(
			        "WEBSERVER: Binding to %s — API is exposed "
			        "to the network",
			        addr.c_str());
		}

		is_webserver_enabled = true;

		OSD::OsdManager::Instance().SetEnabled(
		        section->GetBool("webserver_osd"));

		// Guest-side OSD access (osd.com) is part of the
		// automation surface, so it comes and goes with it
		OSDPORT_Init();

		const auto port = section->GetInt("webserver_port");
		const auto resource_home = get_resource_path("webserver").string();
		const auto use_token_file = section->GetBool("webserver_token_file");
		const auto token_scopes = section->GetString("webserver_token_scopes");

		Webserver::InputRecording::InstallHooks();

		std::thread thread(Webserver::run,
		                   addr,
		                   port,
		                   resource_home,
		                   use_token_file,
		                   token_scopes);

		thread.detach();
	}
}

void WEBSERVER_Destroy()
{
	// Wake any in-flight POST /api/v1/wait or GET /api/v1/debug/wait
	// requests first: they block an httplib worker thread on a condvar
	// that only the frame hook, DEBUG_Loop or an SDL pause loop would
	// otherwise notify, none of which run once shutdown starts -
	// server.stop() would hang waiting for that worker to join.
	Webserver::WaitRegistry::Instance().DrainAll();
	Webserver::DebugEvents::Instance().DrainAll();
	Webserver::SnapshotRegistry::Instance().Clear();
	OSDPORT_Destroy();
	Webserver::server.stop();
	Webserver::remove_token_file();
}

void WEBSERVER_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	auto section = conf->AddSection("webserver");

	Webserver::init_config_settings(*section);
}

bool WEBSERVER_IsEnabled()
{
	return is_webserver_enabled;
}
