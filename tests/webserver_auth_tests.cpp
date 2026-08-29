// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/control.h"
#include "webserver/private/auth.h"
#include "webserver/webserver.h"

#include <string>

#include <gtest/gtest.h>

#include "json/json.h"

using Webserver::ConstantTimeEquals;
using Webserver::ControlHandlers;
using Webserver::EngineName;
using Webserver::IsPublicApiPath;
using Webserver::IsPublicDocPath;
using Webserver::McpProtocol;

namespace {

const std::string token =
        "8f3a1c5e9b2d4f6a8c0e2a4c6e8f0a1b"
        "3d5f7a9c1e3b5d7f9a1c3e5b7d9f1a3c";

TEST(WebserverAuth, EqualTokensMatch)
{
	EXPECT_TRUE(ConstantTimeEquals(token, token));
	EXPECT_TRUE(ConstantTimeEquals(std::string(token), std::string(token)));
}

TEST(WebserverAuth, EmptyStringsMatch)
{
	EXPECT_TRUE(ConstantTimeEquals("", ""));
}

// A request without an Authorization header yields an empty candidate
// token; it must never compare equal to the real one.
TEST(WebserverAuth, EmptyCandidateRejected)
{
	EXPECT_FALSE(ConstantTimeEquals("", token));
	EXPECT_FALSE(ConstantTimeEquals(token, ""));
}

TEST(WebserverAuth, DifferentLengthsRejected)
{
	EXPECT_FALSE(ConstantTimeEquals(token, token + "0"));
	EXPECT_FALSE(ConstantTimeEquals(token, token.substr(0, 63)));
}

TEST(WebserverAuth, FirstByteDifferenceRejected)
{
	auto candidate = token;
	candidate[0]   = '0';
	ASSERT_NE(candidate, token);
	EXPECT_FALSE(ConstantTimeEquals(candidate, token));
}

TEST(WebserverAuth, MiddleByteDifferenceRejected)
{
	auto candidate = token;
	candidate[31]  = '0';
	ASSERT_NE(candidate, token);
	EXPECT_FALSE(ConstantTimeEquals(candidate, token));
}

TEST(WebserverAuth, LastByteDifferenceRejected)
{
	auto candidate = token;
	candidate[63]  = '0';
	ASSERT_NE(candidate, token);
	EXPECT_FALSE(ConstantTimeEquals(candidate, token));
}

// All bytes different must reject just like one byte different. The
// constant-time property itself is structural (no early exit in the
// loop); a wall-clock assertion would be flaky, so it is not tested.
TEST(WebserverAuth, CompletelyDifferentRejected)
{
	const std::string candidate(token.size(), 'z');
	EXPECT_FALSE(ConstantTimeEquals(candidate, token));
}

// Embedded null bytes must participate in the comparison, not
// terminate it.
TEST(WebserverAuth, EmbeddedNullBytesCompared)
{
	std::string a = "ab";
	std::string b = "ab";
	a += '\0';
	b += '\0';
	a += "cd";
	b += "ce";
	EXPECT_FALSE(ConstantTimeEquals(a, b));

	auto c = a;
	EXPECT_TRUE(ConstantTimeEquals(a, c));
}

// -- Documentation path allowlist (token bypass) --

TEST(WebserverDocPath, AllowsDocAssetsForGet)
{
	for (const auto* p : {"/",
	                      "/index.html",
	                      "/style-index.css",
	                      "/api.html",
	                      "/openapi.json",
	                      "/swagger-ui.css",
	                      "/swagger-ui-bundle.js",
	                      "/debugger.html",
	                      "/control.html"}) {
		EXPECT_TRUE(IsPublicDocPath("GET", p)) << p;
		EXPECT_TRUE(IsPublicDocPath("HEAD", p)) << p;
	}
}

TEST(WebserverDocPath, RejectsNonReadMethods)
{
	EXPECT_FALSE(IsPublicDocPath("POST", "/openapi.json"));
	EXPECT_FALSE(IsPublicDocPath("PUT", "/index.html"));
	EXPECT_FALSE(IsPublicDocPath("DELETE", "/"));
	EXPECT_FALSE(IsPublicDocPath("OPTIONS", "/api.html"));
}

TEST(WebserverDocPath, NeverExposesTokenFile)
{
	EXPECT_FALSE(IsPublicDocPath("GET", "/api_token"));
	EXPECT_FALSE(IsPublicDocPath("GET", "/.dosbox/api_token"));
}

TEST(WebserverDocPath, RejectsApiEndpoints)
{
	EXPECT_FALSE(IsPublicDocPath("GET", "/api/v1/status"));
	EXPECT_FALSE(IsPublicDocPath("GET", "/api/v1/cpu/state"));
}

TEST(WebserverDocPath, RejectsTraversalAndTrickyVariants)
{
	// httplib hands us the already-decoded path; an exact-match allowlist
	// fails closed on anything that is not byte-for-byte a known asset.
	EXPECT_FALSE(IsPublicDocPath("GET", "/openapi.json/../api_token"));
	EXPECT_FALSE(IsPublicDocPath("GET", "/../api_token"));
	EXPECT_FALSE(IsPublicDocPath("GET", "/./index.html"));
	EXPECT_FALSE(IsPublicDocPath("GET", "//index.html"));
	EXPECT_FALSE(IsPublicDocPath("GET", "/index.html "));
	EXPECT_FALSE(IsPublicDocPath("GET", "/INDEX.HTML"));
	EXPECT_FALSE(IsPublicDocPath("GET", "/openapi.json?x=1"));
	EXPECT_FALSE(IsPublicDocPath("GET", ""));
}

// -- Pre-auth API path allowlist (4.2: GET /api/v1/hello) --

TEST(WebserverApiPath, AllowsHelloForGetAndHead)
{
	EXPECT_TRUE(IsPublicApiPath("GET", "/api/v1/hello"));
	EXPECT_TRUE(IsPublicApiPath("HEAD", "/api/v1/hello"));
}

TEST(WebserverApiPath, RejectsNonReadMethods)
{
	EXPECT_FALSE(IsPublicApiPath("POST", "/api/v1/hello"));
	EXPECT_FALSE(IsPublicApiPath("PUT", "/api/v1/hello"));
	EXPECT_FALSE(IsPublicApiPath("DELETE", "/api/v1/hello"));
}

TEST(WebserverApiPath, RejectsEveryOtherApiEndpoint)
{
	EXPECT_FALSE(IsPublicApiPath("GET", "/api/v1/status"));
	EXPECT_FALSE(IsPublicApiPath("GET", "/api/v1/dosbox/info"));
	EXPECT_FALSE(IsPublicApiPath("GET", "/api/v1/cpu/state"));
}

TEST(WebserverApiPath, RejectsTraversalAndTrickyVariants)
{
	EXPECT_FALSE(IsPublicApiPath("GET", "/api/v1/hello/../dosbox/info"));
	EXPECT_FALSE(IsPublicApiPath("GET", "/api/v1/hello/"));
	EXPECT_FALSE(IsPublicApiPath("GET", "/api/v1/hello?x=1"));
	EXPECT_FALSE(IsPublicApiPath("GET", "/API/V1/HELLO"));
	EXPECT_FALSE(IsPublicApiPath("GET", ""));
}

TEST(WebserverApiPath, NeverOverlapsWithDocPathAllowlist)
{
	// The two predicates are deliberately separate (see IsPublicApiPath's
	// header comment) - confirm neither allowlist secretly covers the
	// other's one path.
	EXPECT_FALSE(IsPublicDocPath("GET", "/api/v1/hello"));
	for (const auto* p : {"/",
	                      "/index.html",
	                      "/style-index.css",
	                      "/api.html",
	                      "/openapi.json",
	                      "/swagger-ui.css",
	                      "/swagger-ui-bundle.js",
	                      "/debugger.html",
	                      "/control.html"}) {
		EXPECT_FALSE(IsPublicApiPath("GET", p)) << p;
	}
}

// -- GET /api/v1/hello response shape --
//
// Not a Command/Bridge test: ControlHandlers::GetHello never touches
// emulator state or crosses the Bridge (that's the whole point of it
// being pre-auth), so it's safe and meaningful to call directly.

TEST(WebserverHello, ReportsExactlyNameVersionAndProtocol)
{
	httplib::Request req;
	httplib::Response res;
	ControlHandlers::GetHello(req, res);

	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("name").get<std::string>(), EngineName);
	EXPECT_EQ(j.at("mcp_protocol").get<std::string>(), McpProtocol);
	EXPECT_TRUE(j.at("version").is_string());
	EXPECT_FALSE(j.at("version").get<std::string>().empty());

	// Exactly these three fields - no pid, uptime, or anything else that
	// would need per-instance state or a Bridge crossing.
	EXPECT_EQ(j.size(), 3u);
}

} // namespace
