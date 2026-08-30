// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_H
#define DOSBOX_WEBSERVER_H

#include <charconv>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "http/http.h"
#include "json/json.h"

#include "config/config.h"

namespace Webserver {

constexpr auto TypeJson   = "application/json";
constexpr auto TypeBinary = "application/octet-stream";

// Engine identity and the highest MCP protocol minor this build's API
// surface implements. The single source both GET /api/v1/hello
// (pre-auth) and GET /api/v1/dosbox/info report it from, so the two
// routes can never disagree - see dosbox_mcp/protocol.py on the bridge
// side for what a minor bump is expected to mean (additive only,
// negotiated as min(bridge minor, engine minor)).
constexpr auto EngineName = "dosbox-automation";
// PROTOCOL.md's changelog already documents route/field additions
// through 1.13.0 (draft) as things this engine ships - the debug group
// (1.1.0), memory/search limit+total (1.2.0), memory/scan (1.3.0),
// memory/snapshot+diff (1.4.0), the breakpoint address bounds check
// (1.5.0), memory/allocate+free+allocations (1.6.0), drive/mount.policy/
// mount.images (1.7.0), capture/video/start compression+status fields
// (1.8.0), input/replay/status+cancel (1.9.0), input/record store
// (1.10.0), batch (1.11.0), input/mouse (1.12.0), script/log +
// dosbox/info's instance_id/pid/started_at_unix/uptime_ms (1.13.0). This
// item (4.2, closing the negotiation loop itself: dosbox/info finally
// sends this field, GET /api/v1/hello is implemented for the first time)
// is 1.14.0 - see PROTOCOL.md's changelog entry of the same number.
constexpr auto McpProtocol = "1.14";

// httplib::Server::set_payload_max_length's cap, applied to every request
// body regardless of route. Named so the capability descriptor
// (capabilities.cpp) can report the same number it enforces.
constexpr size_t MaxRequestBodyBytes = 10 * 1024 * 1024;

enum class Source {
	Param, // get_param_value
	Path,  // get_path_value
	Header // get_header_value
};

template <typename T>
static T num_param(const httplib::Request& req, Source src, const std::string& name,
                   const T min = std::numeric_limits<T>::lowest(),
                   const T max = std::numeric_limits<T>::max())
{
	std::string str;
	if (src == Source::Param) {
		str = req.get_param_value(name);
	} else if (src == Source::Path) {
		str = req.path_params.at(name);
	} else if (src == Source::Header) {
		str = req.get_header_value(name);
	}

	if (str.empty()) {
		throw std::invalid_argument(
		        "Missing or empty required parameter: " + name);
	}

	T value           = 0;
	int base          = 10;
	const char* first = str.data();
	const char* last  = str.data() + str.size();
	if (str.size() >= 2 && str[0] == '0' &&
	    std::tolower(static_cast<unsigned char>(str[1])) == 'x') {
		first += 2;
		base = 16;
	}

	auto [ptr, ec] = std::from_chars(first, last, value, base);
	if (ec == std::errc::invalid_argument || ptr != str.data() + str.size()) {
		throw std::invalid_argument("Invalid argument for " + name +
		                            ": " + str);
	} else if (ec == std::errc::result_out_of_range || value < min ||
	           value > max) {
		throw std::invalid_argument("Invalid argument for " + name +
		                            ": " + str + " (out of range)");
	}
	return value;
}

void send_json(httplib::Response& res, const nlohmann::json& j);

// True when a request may bypass the bearer-token check: a GET/HEAD for one of
// the fixed documentation assets (landing page, API explorer, openapi spec,
// vendored Swagger UI). Exact-match only, so no traversal or token file leaks.
bool IsPublicDocPath(const std::string& method, const std::string& path);

// True when a request may bypass the bearer-token check because it's the
// pre-auth protocol handshake, GET /api/v1/hello - not a doc asset.
// Deliberately kept separate from IsPublicDocPath rather than folded
// into its allowlist: that predicate's own comment explains why it's
// exact-match against one fixed small set, and widening it is exactly
// the kind of change that risks a path toward the token file (see the
// config_home mount comment in run()). This predicate exists for one
// reason only - handing out {name, version, mcp_protocol} before a
// bridge has a token, so it can tell whether it's even talking to a
// compatible engine before trying to authenticate. Same discipline:
// exact-match, GET/HEAD only. The Host allowlist check in the
// pre-routing handler still runs before either predicate is consulted.
bool IsPublicApiPath(const std::string& method, const std::string& path);

// True when addr is one of the fixed loopback forms this server
// recognizes as "definitely local" - exact match only, same discipline
// as IsPublicDocPath/IsPublicApiPath above, so a LAN address or a
// hostname that merely happens to resolve locally never counts. Used to
// gate webserver_allow_remote (binding anywhere else needs it) and
// webserver_require_auth (disabling the token check needs it too).
bool IsLoopbackBindAddress(const std::string& addr);

// Every (method, path) the server actually registers under /api/v1,
// including GET /api/v1/dosbox/info. Exposed for testing: a test walks
// this against resources/webserver/openapi.json so the spec can't drift
// out of sync with the real route table again (4.1).
std::vector<std::pair<std::string, std::string>> RegisteredApiRoutes();

// Least-privilege grouping for webserver_token_scopes (4.7): every
// registered route requires exactly one of these. The engine still
// issues one token per instance - this restricts what that token can
// do, it does not create per-scope tokens.
enum class TokenScope { Read, Write, Input, Script, Media, Debug, Control };

// Human-readable scope name (lowercase, matches the config setting's
// vocabulary) - for error messages and the config-value parser below.
std::string TokenScopeName(TokenScope scope);

// The scope a registered route requires. Looked up against the same
// table setup_api_handlers() registers from, so it cannot drift from
// the real route set the way openapi.json once did (4.1) - there is no
// separate list to keep in sync. Returns nullopt only for a path/method
// this server does not recognize at all; the pre-routing handler
// treats that as a deny whenever scoping is active, never as an
// implicit allow, so a route someone forgot to classify fails closed
// instead of silently bypassing the check.
std::optional<TokenScope> RequiredScopeFor(const std::string& method,
                                           const std::string& path);

// Parses webserver_token_scopes (comma-separated scope names, e.g.
// "read,input,media") into the set of granted scopes. An empty string
// - the default - means "no restriction configured" and returns
// nullopt; the pre-routing handler skips the scope check entirely in
// that case, preserving today's single-all-powerful-token behavior.
// An unrecognized name is logged and dropped rather than rejected
// outright: a typo can only narrow what the token can do, never widen
// it beyond what was written in the config file.
std::optional<std::set<TokenScope>> ParseTokenScopes(const std::string& value);

// Set once at startup (run()) from webserver_token_scopes - nullopt for
// unrestricted. Read from two places that never share a call path: the
// pre-routing handler for HTTP requests, and the Lua API
// (lua_api.cpp) for a running script, which never passes through HTTP
// routing at all and would otherwise bypass scoping entirely. Mutex-
// guarded rather than relying on startup ordering to make this safe by
// construction, not by argument.
void SetGrantedTokenScopes(std::optional<std::set<TokenScope>> scopes);

// True when `scope` is currently permitted: always true if scoping was
// never configured (SetGrantedTokenScopes(nullopt), the default), else
// true only if `scope` is in the granted set. The Lua API's
// guest-reaching functions (key/type/mouse_*, mem_write, mount_lock,
// capture_start/stop, screen_text and friends) each call this with
// their own required scope before doing anything, the same way an
// equivalent REST route would be checked by RequiredScopeFor.
bool ScopeAllowed(TokenScope scope);

// The JSON error shape and HTTP status a caught exception maps to.
// `message` stays a plain, caller-facing string; `code` and `retryable`
// are additive fields a caller can key logic off without parsing it.
struct ErrorInfo {
	httplib::StatusCode status = httplib::StatusCode::InternalServerError_500;
	std::string message = "Internal server error";
	std::string code    = "internal_error";
	bool retryable      = false;
};

// Classifies an exception (from std::current_exception() in a catch-all
// handler, or std::make_exception_ptr() in a test) into the response
// shape error_handler sends. A null ep classifies as the generic
// internal_error default. Exposed for testing.
ErrorInfo ClassifyException(std::exception_ptr ep);

} // namespace Webserver

void WEBSERVER_Init();
void WEBSERVER_Destroy();
void WEBSERVER_AddConfigSection(const ConfigPtr& conf);
bool WEBSERVER_IsEnabled();

#endif // DOSBOX_WEBSERVER_H
