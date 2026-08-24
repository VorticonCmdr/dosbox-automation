// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/webserver.h"

#include <optional>
#include <set>
#include <string>

#include <gtest/gtest.h>

using Webserver::ParseTokenScopes;
using Webserver::RegisteredApiRoutes;
using Webserver::RequiredScopeFor;
using Webserver::TokenScope;
using Webserver::TokenScopeName;

namespace {

// Guards against the same drift 4.1 fixed for openapi.json: every route
// the server actually registers must classify to a scope, or a scoped
// token would either wrongly deny it (if RequiredScopeFor is fixed to
// fail closed, as it is) or - had it been implemented the other way -
// silently bypass the check for a route someone forgot to add to the
// table.
TEST(WebserverScope, EveryRegisteredRouteHasARequiredScope)
{
	for (const auto& [method, path] : RegisteredApiRoutes()) {
		EXPECT_TRUE(RequiredScopeFor(method, path).has_value())
		        << method << " " << path << " has no scope classification";
	}
}

TEST(WebserverScope, UnrecognizedRouteHasNoScope)
{
	EXPECT_FALSE(RequiredScopeFor("GET", "/api/v1/nonexistent").has_value());
	EXPECT_FALSE(RequiredScopeFor("PATCH", "/api/v1/status").has_value());
}

// httplib answers HEAD from a registered GET handler with no separate
// registration - a HEAD request must classify the same as the matching
// GET, or turning scoping on breaks a HEAD request that worked before
// (a real regression this caught during live verification: an
// unscoped instance answered 200 to HEAD /api/v1/cpu/state, a scoped
// one with 'read' granted answered 403, because only "GET"/"POST"/
// "PUT"/"DELETE" were ever compared).
TEST(WebserverScope, HeadClassifiesTheSameAsTheMatchingGet)
{
	EXPECT_EQ(RequiredScopeFor("HEAD", "/api/v1/cpu/state"),
	          RequiredScopeFor("GET", "/api/v1/cpu/state"));
	EXPECT_EQ(RequiredScopeFor("HEAD", "/api/v1/dosbox/info"), TokenScope::Read);
	// HEAD must not leak into a route only registered for a write verb.
	EXPECT_FALSE(RequiredScopeFor("HEAD", "/api/v1/cpu/register").has_value());
}

TEST(WebserverScope, DosboxInfoIsClassifiedDespiteNotBeingInTheRouteTable)
{
	const auto scope = RequiredScopeFor("GET", "/api/v1/dosbox/info");
	ASSERT_TRUE(scope.has_value());
	EXPECT_EQ(*scope, TokenScope::Read);
}

TEST(WebserverScope, SpotChecksMatchTheDocumentedVocabulary)
{
	EXPECT_EQ(RequiredScopeFor("GET", "/api/v1/cpu/state"), TokenScope::Read);
	EXPECT_EQ(RequiredScopeFor("PUT", "/api/v1/cpu/register"), TokenScope::Write);
	EXPECT_EQ(RequiredScopeFor("POST", "/api/v1/input/sequence"),
	          TokenScope::Input);
	EXPECT_EQ(RequiredScopeFor("POST", "/api/v1/script/load"), TokenScope::Script);
	EXPECT_EQ(RequiredScopeFor("GET", "/api/v1/video/frame"), TokenScope::Media);
	EXPECT_EQ(RequiredScopeFor("POST", "/api/v1/debug/pause"), TokenScope::Debug);
	EXPECT_EQ(RequiredScopeFor("POST", "/api/v1/dosbox/shutdown"),
	          TokenScope::Control);
}

// The pre-routing handler matches scopes against the request's concrete
// path, not the httplib ":name" pattern - these are the routes with path
// parameters, so this is the part most likely to be silently wrong.
TEST(WebserverScope, MatchesParameterizedRoutesByConcretePath)
{
	EXPECT_EQ(RequiredScopeFor("GET", "/api/v1/memory/1000/16"),
	          TokenScope::Read);
	EXPECT_EQ(RequiredScopeFor("GET", "/api/v1/memory/0040/1000/16"),
	          TokenScope::Read);
	EXPECT_EQ(RequiredScopeFor("PUT", "/api/v1/memory/1000"), TokenScope::Write);
	EXPECT_EQ(RequiredScopeFor("PUT", "/api/v1/memory/0040/1000"),
	          TokenScope::Write);
	EXPECT_EQ(RequiredScopeFor("GET", "/api/v1/debug/disassemble/0040/1000/16"),
	          TokenScope::Debug);
	EXPECT_EQ(RequiredScopeFor("DELETE", "/api/v1/input/recordings/mine"),
	          TokenScope::Input);

	// A segment count mismatch must not accidentally match a shorter or
	// longer pattern.
	EXPECT_FALSE(RequiredScopeFor("GET", "/api/v1/memory/1000").has_value());
	// "/api/v1/memory/1000/16/extra" is not actually a mismatch: it has
	// the same segment count as "/api/v1/memory/:segment/:offset/:len"
	// and legitimately matches that pattern instead - a real, expected
	// ambiguity between the two memory-read routes, not a matcher bug.
	// One segment further than either pattern is the real mismatch case.
	EXPECT_FALSE(
	        RequiredScopeFor("GET", "/api/v1/memory/1000/16/extra/more").has_value());
	// An empty path parameter segment (a trailing/doubled slash) must not
	// match either - :name requires a non-empty segment.
	EXPECT_FALSE(RequiredScopeFor("PUT", "/api/v1/memory/").has_value());
}

TEST(WebserverScope, TokenScopeNameCoversEveryEnumerator)
{
	for (const auto scope : {TokenScope::Read,
	                         TokenScope::Write,
	                         TokenScope::Input,
	                         TokenScope::Script,
	                         TokenScope::Media,
	                         TokenScope::Debug,
	                         TokenScope::Control}) {
		EXPECT_FALSE(TokenScopeName(scope).empty());
	}
}

TEST(WebserverParseTokenScopes, EmptyStringMeansUnrestricted)
{
	EXPECT_EQ(ParseTokenScopes(""), std::nullopt);
}

TEST(WebserverParseTokenScopes, ParsesEachKnownScopeName)
{
	for (const auto scope : {TokenScope::Read,
	                         TokenScope::Write,
	                         TokenScope::Input,
	                         TokenScope::Script,
	                         TokenScope::Media,
	                         TokenScope::Debug,
	                         TokenScope::Control}) {
		const auto granted = ParseTokenScopes(TokenScopeName(scope));
		ASSERT_TRUE(granted.has_value());
		EXPECT_EQ(*granted, std::set<TokenScope>{scope});
	}
}

TEST(WebserverParseTokenScopes, ParsesCommaSeparatedListWithWhitespace)
{
	const auto granted = ParseTokenScopes("read, input,  media");
	ASSERT_TRUE(granted.has_value());
	EXPECT_EQ(*granted,
	          (std::set<TokenScope>{TokenScope::Read,
	                                TokenScope::Input,
	                                TokenScope::Media}));
}

TEST(WebserverParseTokenScopes, DuplicateEntriesCollapse)
{
	const auto granted = ParseTokenScopes("read,read,read");
	ASSERT_TRUE(granted.has_value());
	EXPECT_EQ(*granted, std::set<TokenScope>{TokenScope::Read});
}

// A typo can only narrow what a configured token can do, never widen it:
// an unrecognized entry is dropped, not treated as "grant everything" or
// as a parse failure that falls back to unrestricted.
TEST(WebserverParseTokenScopes, UnknownEntryIsDroppedNotFatal)
{
	const auto granted = ParseTokenScopes("read,bogus,write");
	ASSERT_TRUE(granted.has_value());
	EXPECT_EQ(*granted,
	          (std::set<TokenScope>{TokenScope::Read, TokenScope::Write}));
}

// Every entry unrecognized still returns a present-but-empty set, not
// nullopt - the operator explicitly configured scoping, so the token
// grants nothing rather than silently falling back to unrestricted.
TEST(WebserverParseTokenScopes, AllUnknownEntriesYieldsEmptyGrantedSet)
{
	const auto granted = ParseTokenScopes("bogus,also-bogus");
	ASSERT_TRUE(granted.has_value());
	EXPECT_TRUE(granted->empty());
}

} // namespace
