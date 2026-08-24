// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/webserver.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "json/json.h"

using Webserver::RegisteredApiRoutes;

namespace {

// httplib path parameters are ":name"; OpenAPI path parameters are
// "{name}". Converts one segment style to the other so the two route
// tables (the engine's and the spec's) can be compared directly.
std::string to_openapi_path(const std::string& httplib_path)
{
	std::string result;
	std::istringstream stream(httplib_path);
	std::string segment;
	bool first = true;
	while (std::getline(stream, segment, '/')) {
		if (!first) {
			result += '/';
		}
		first = false;
		if (!segment.empty() && segment[0] == ':') {
			result += '{' + segment.substr(1) + '}';
		} else {
			result += segment;
		}
	}
	return result;
}

std::string to_upper(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	return s;
}

nlohmann::json load_openapi_spec()
{
	std::ifstream file("resources/webserver/openapi.json");
	EXPECT_TRUE(file.is_open())
	        << "expected to run from the repo root (CMAKE_SOURCE_DIR)";
	return nlohmann::json::parse(file);
}

} // namespace

// Guards against the drift 4.1 found: the spec documented 36 of 48
// registered routes at the time, silently missing the entire debug
// group. Every (method, path) the server actually registers must appear
// as a key in the spec's `paths` object, under the matching HTTP verb.
TEST(WebserverOpenApi, EveryRegisteredRouteIsDocumented)
{
	const auto spec = load_openapi_spec();
	ASSERT_TRUE(spec.contains("paths"));
	const auto& paths = spec.at("paths");

	for (const auto& [method, httplib_path] : RegisteredApiRoutes()) {
		const auto openapi_path = to_openapi_path(httplib_path);
		ASSERT_TRUE(paths.contains(openapi_path))
		        << method << " " << httplib_path
		        << " is registered but missing from openapi.json (looked "
		           "for path key '"
		        << openapi_path << "')";

		const auto& operations = paths.at(openapi_path);
		bool found_method      = false;
		for (const auto& [verb, op] : operations.items()) {
			(void)op;
			if (to_upper(verb) == method) {
				found_method = true;
				break;
			}
		}
		EXPECT_TRUE(found_method)
		        << method << " " << httplib_path << " (spec path '"
		        << openapi_path << "') is documented, but not for the "
		        << method << " verb";
	}
}

// The inverse check: a spec entry for a route the server no longer
// registers is stale documentation, exactly the kind of drift this item
// exists to prevent from recurring in the other direction.
TEST(WebserverOpenApi, EveryDocumentedRouteIsRegistered)
{
	const auto spec = load_openapi_spec();
	ASSERT_TRUE(spec.contains("paths"));

	std::set<std::pair<std::string, std::string>> registered;
	for (const auto& [method, httplib_path] : RegisteredApiRoutes()) {
		registered.emplace(method, to_openapi_path(httplib_path));
	}

	for (const auto& [openapi_path, operations] : spec.at("paths").items()) {
		for (const auto& [verb, op] : operations.items()) {
			(void)op;
			EXPECT_TRUE(registered.count({to_upper(verb),
			                              openapi_path}) == 1)
			        << to_upper(verb) << " " << openapi_path
			        << " is documented in openapi.json but the server "
			           "does not register it";
		}
	}
}

TEST(WebserverOpenApi, RegisteredApiRoutesIsNonEmptyAndIncludesKnownRoutes)
{
	const auto routes = RegisteredApiRoutes();
	// Sanity floor, not an exact count - the whole point of this item is
	// that the exact count is expected to keep growing as routes are
	// added, without anyone having to remember to update a hardcoded
	// number here too.
	EXPECT_GT(routes.size(), 60u);

	const std::set<std::pair<std::string, std::string>> as_set(routes.begin(),
	                                                           routes.end());
	EXPECT_EQ(as_set.count({"GET", "/api/v1/status"}), 1u);
	EXPECT_EQ(as_set.count({"GET", "/api/v1/dosbox/info"}), 1u);
	EXPECT_EQ(as_set.count({"GET", "/api/v1/debug/status"}), 1u);
}
