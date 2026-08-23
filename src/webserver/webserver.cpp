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

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(WIN32)
#include <bcrypt.h>
#include <windows.h>
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
	};
	return public_paths.count(path) > 0;
}

static httplib::Server server;

static void setup_api_handlers()
{
	server.Get("/api/v1/cpu/state", CpuStateCommand::Get);

	server.Get("/api/v1/dos/internals", DosInternalsCommand::Get);

	server.Post("/api/v1/dosbox/shutdown", ShutdownCommand::Post);

	server.Post("/api/v1/memory/allocate", AllocMemoryCommand::Post);
	server.Post("/api/v1/memory/free", FreeMemoryCommand::Post);
	server.Get("/api/v1/memory/allocations", MemoryAllocationsCommand::Get);
	server.Post("/api/v1/memory/search", SearchMemoryCommand::Post);
	server.Post("/api/v1/memory/scan", ScanMemoryCommand::Post);
	server.Post("/api/v1/memory/snapshot", MemorySnapshotHandlers::Post);
	server.Post("/api/v1/memory/diff", MemoryDiffHandlers::Post);
	server.Post("/api/v1/memory/freeze", FreezeHandlers::Post);
	server.Get("/api/v1/memory/freeze", FreezeHandlers::Get);
	server.Delete("/api/v1/memory/freeze", FreezeHandlers::Delete);
	server.Get("/api/v1/memory/:offset/:len", ReadMemoryCommand::Get);
	server.Get("/api/v1/memory/:segment/:offset/:len", ReadMemoryCommand::Get);
	server.Put("/api/v1/memory/:offset", WriteMemoryCommand::Put);
	server.Put("/api/v1/memory/:segment/:offset", WriteMemoryCommand::Put);

	server.Get("/api/v1/io/port", PortReadCommand::Get);
	server.Put("/api/v1/io/port", PortWriteCommand::Put);
	server.Put("/api/v1/cpu/register", WriteRegisterCommand::Put);

	server.Get("/api/v1/debug/status", DebugStatusCommand::Get);
	server.Post("/api/v1/debug/pause", DebugPauseCommand::Post);
	server.Post("/api/v1/debug/continue", DebugContinueCommand::Post);
	server.Post("/api/v1/debug/step", DebugStepCommand::Post);
	server.Post("/api/v1/debug/step_over", DebugStepOverCommand::Post);
	server.Post("/api/v1/debug/run_to", DebugRunToCommand::Post);
	server.Post("/api/v1/debug/step_out", DebugStepOutCommand::Post);
	server.Get("/api/v1/debug/wait", DebugWaitHandlers::Get);

	server.Get("/api/v1/debug/disassemble/:segment/:offset/:count",
	           DisassembleCommand::Get);
	server.Get("/api/v1/debug/backtrace", BacktraceCommand::Get);

	server.Get("/api/v1/debug/breakpoints", DebugListBreakpointsCommand::Get);
	server.Post("/api/v1/debug/breakpoints", DebugAddBreakpointCommand::Post);
	server.Delete("/api/v1/debug/breakpoints",
	              DebugDeleteBreakpointCommand::Delete);

	server.Post("/api/v1/input/sequence", InputSequenceCommand::Post);
	server.Post("/api/v1/input/type", InputTypeCommand::Post);

	server.Get("/api/v1/video/frame", VideoHandlers::GetFrame);
	server.Get("/api/v1/video/frame/info", VideoHandlers::GetFrameInfo);
	server.Get("/api/v1/video/text", ScreenTextCommand::Get);

	server.Get("/api/v1/program/state", ControlHandlers::GetProgramState);
	server.Get("/api/v1/status", ControlHandlers::GetStatus);
	server.Post("/api/v1/control/shutdown", ShutdownCommand::Post);

	server.Post("/api/v1/wait", WaitHandlers::Post);

	server.Post("/api/v1/drive/swap", DriveSwapCommand::Post);

	server.Post("/api/v1/mount/lock",
	            [](const httplib::Request&, httplib::Response& res) {
		            MountPolicy::Lock();
		            json j;
		            j["status"] = "locked";
		            send_json(res, j);
	            });
	server.Get("/api/v1/mount/lock",
	           [](const httplib::Request&, httplib::Response& res) {
		           json j;
		           j["locked"] = MountPolicy::IsLocked();
		           send_json(res, j);
	           });

	server.Post("/api/v1/input/record/start", RecordingHandlers::PostStart);
	server.Post("/api/v1/input/record/pause", RecordingHandlers::PostPause);
	server.Post("/api/v1/input/record/stop", RecordingHandlers::PostStop);
	server.Get("/api/v1/input/record/status", RecordingHandlers::GetStatus);

	server.Post("/api/v1/script/load", Lua::LuaLoadCommand::Post);
	server.Post("/api/v1/script/start", Lua::LuaStartCommand::Post);
	server.Post("/api/v1/script/stop", Lua::LuaStopCommand::Post);
	server.Get("/api/v1/script/status", Lua::LuaStatusCommand::Get);

	server.Post("/api/v1/capture/video/start", CaptureStartCommand::Post);
	server.Post("/api/v1/capture/video/stop", CaptureStopCommand::Post);
	server.Get("/api/v1/capture/video/status", CaptureStatusCommand::Get);
	server.Get("/api/v1/capture/video/compression",
	           CaptureCompressionGetCommand::Get);
	server.Put("/api/v1/capture/video/compression",
	           CaptureCompressionSetCommand::Put);
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

static std::string generate_api_token()
{
	uint8_t buf[32] = {};

#if defined(WIN32)
	// BCryptGenRandom is the Windows CSPRNG. std::random_device on
	// MinGW has been deterministic on some toolchains.
	const auto status = BCryptGenRandom(nullptr,
	                                    buf,
	                                    sizeof(buf),
	                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (!BCRYPT_SUCCESS(status)) {
		E_Exit("WEBSERVER: BCryptGenRandom failed (0x%08lx)", status);
	}
#else
	// On Linux/macOS, /dev/urandom is the standard CSPRNG source.
	auto* f = fopen("/dev/urandom", "rb");
	if (!f || fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
		if (f) {
			fclose(f);
		}
		E_Exit("WEBSERVER: Failed to read /dev/urandom");
	}
	fclose(f);
#endif

	constexpr char hex[] = "0123456789abcdef";
	std::string token;
	token.reserve(64);
	for (const auto byte : buf) {
		token += hex[(byte >> 4) & 0xF];
		token += hex[byte & 0xF];
	}
	return token;
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
                           const std::string& api_token)
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
	                                api_token](const httplib::Request& req,
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

		// Documentation assets are browsable without a token (Host
		// check still applies). The /api/v1 endpoints below are not.
		if (IsPublicDocPath(req.method, req.path)) {
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
                const std::string resource_home, const bool use_token_file)
{
	const auto config_home = (get_config_dir() / DefaultWebserverDir).string();

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

	server.set_mount_point("/", config_home);
	server.set_mount_point("/", resource_home);

	setup_api_handlers();
	setup_security(addr, port, api_token);

	server.set_exception_handler(error_handler);

	server.Get("/api/v1/dosbox/info",
	           [](const httplib::Request&, httplib::Response& res) {
		           const auto capabilities = BuildCapabilitiesBlock();

		           json j;
		           j["version"]      = DOSBOX_GetDetailedVersion();
		           j["features"]     = FeaturesProjection(capabilities);
		           j["capabilities"] = capabilities;
		           j["limits"]       = BuildServerLimits();
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
	        "An API token is generated at startup and printed to the log output.\n"
	        "All API requests require Authorization: Bearer <token>.");

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

	auto token_file = section.AddBool("webserver_token_file", OnlyAtStart, false);
	token_file->SetHelp(
	        "Write the API token to a file instead of printing it to the log.\n"
	        "The file is written to the webserver config directory with restricted\n"
	        "permissions (0600) and removed on clean shutdown. Launchers and tools\n"
	        "can read the token from this file instead of scraping log output.\n"
	        "Has no effect when DOSBOX_API_TOKEN is set via environment variable.");

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

		Webserver::InputRecording::InstallHooks();

		std::thread thread(Webserver::run, addr, port, resource_home, use_token_file);

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
