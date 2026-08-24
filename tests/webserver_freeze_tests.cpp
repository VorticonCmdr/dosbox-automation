// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/freeze.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

#include "json/json.h"

using Webserver::FreezeHandlers;
using Webserver::FreezeRegistry;
using Webserver::ValidateFreezeRange;

namespace {

class FreezeTest : public ::testing::Test {
protected:
	FreezeRegistry reg;
};

TEST_F(FreezeTest, AddAndList)
{
	EXPECT_TRUE(reg.Add(0x1000, 99, 1));
	EXPECT_TRUE(reg.Add(0x2000, 5, 2));
	const auto entries = reg.List();
	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0].address, 0x1000u);
	EXPECT_EQ(entries[0].value, 99u);
	EXPECT_EQ(entries[0].width, 1);
	EXPECT_EQ(entries[1].address, 0x2000u);
	EXPECT_EQ(entries[1].width, 2);
}

TEST_F(FreezeTest, AddSameAddressUpdatesInPlace)
{
	reg.Add(0x1000, 1, 1);
	reg.Add(0x1000, 250, 1);
	ASSERT_EQ(reg.List().size(), 1u);
	EXPECT_EQ(reg.List()[0].value, 250u);
}

TEST_F(FreezeTest, RejectsBeyondCap)
{
	for (int i = 0; i < FreezeRegistry::MaxEntries; ++i) {
		EXPECT_TRUE(reg.Add(0x1000 + i * 4, 0, 1));
	}
	EXPECT_FALSE(reg.Add(0x9000, 0, 1));
}

TEST_F(FreezeTest, RemoveOne)
{
	reg.Add(0x1000, 1, 1);
	reg.Add(0x2000, 2, 1);
	EXPECT_TRUE(reg.Remove(0x1000));
	EXPECT_EQ(reg.List().size(), 1u);
	EXPECT_EQ(reg.List()[0].address, 0x2000u);
}

TEST_F(FreezeTest, RemoveNonexistentReturnsFalse)
{
	EXPECT_FALSE(reg.Remove(0x9999));
}

TEST_F(FreezeTest, ClearAll)
{
	reg.Add(0x1000, 1, 1);
	reg.Add(0x2000, 2, 1);
	reg.Clear();
	EXPECT_TRUE(reg.List().empty());
}

TEST_F(FreezeTest, RejectsBadWidth)
{
	EXPECT_FALSE(reg.Add(0x1000, 0, 3));
	EXPECT_FALSE(reg.Add(0x1000, 0, 0));
}

TEST_F(FreezeTest, AcceptsAllValidWidths)
{
	EXPECT_TRUE(reg.Add(0x1000, 0, 1));
	EXPECT_TRUE(reg.Add(0x2000, 0, 2));
	EXPECT_TRUE(reg.Add(0x3000, 0, 4));
}

TEST(FreezeRangeValidation, AcceptsInRangeAddress)
{
	EXPECT_TRUE(ValidateFreezeRange(0x1000, 4, 16 * 1024 * 1024));
}

TEST(FreezeRangeValidation, AcceptsExactUpperBoundary)
{
	EXPECT_TRUE(ValidateFreezeRange(1000 - 4, 4, 1000));
}

TEST(FreezeRangeValidation, RejectsOneBytePastTheEnd)
{
	EXPECT_FALSE(ValidateFreezeRange(1000 - 3, 4, 1000));
}

TEST(FreezeRangeValidation, RejectsAddressNearUint32MaxWithout32BitWraparound)
{
	// address=0xFFFFFFFE, width=4: address + width in 32-bit arithmetic
	// wraps to 2, which is < any real mem_total and would wrongly pass.
	// The 64-bit sum is far beyond any real mem_total and must fail.
	EXPECT_FALSE(ValidateFreezeRange(std::numeric_limits<uint32_t>::max() - 1,
	                                 4,
	                                 16 * 1024 * 1024));
}

TEST(FreezeRangeValidation, RejectsMaxUint32AddressAtAnyWidth)
{
	EXPECT_FALSE(ValidateFreezeRange(std::numeric_limits<uint32_t>::max(),
	                                 1,
	                                 16 * 1024 * 1024));
}

// -- FreezeHandlers: HTTP-level request handling --
//
// FreezeHandlers::Post/Get/Delete are plain static handlers over
// FreezeRegistry::Instance() - never a Command, never crosses the
// Bridge - so they're safe to call directly, like ControlHandlers::
// GetHello. FreezeRegistry::Instance() is a process-wide singleton
// shared with every other test in this binary, so each test clears it
// first rather than assuming it starts empty.

class FreezeHandlersTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		FreezeRegistry::Instance().Clear();
	}
	void TearDown() override
	{
		FreezeRegistry::Instance().Clear();
	}
};

TEST_F(FreezeHandlersTest, PostRejectsBadWidth)
{
	httplib::Request req;
	req.body = R"({"address": 4096, "value": 1, "width": 3})";
	httplib::Response res;
	FreezeHandlers::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("error").get<std::string>(), "width must be 1, 2, or 4");
	EXPECT_TRUE(FreezeRegistry::Instance().List().empty());
}

TEST_F(FreezeHandlersTest, PostRejectsAddressOutOfRange)
{
	// FreezeRegistry::Instance() and MEM_TotalPages() are both
	// process-wide globals shared with every other test in this
	// binary - MEM_TotalPages() is 0 unless some earlier-running
	// DOSBoxTestFixture-based test already called MEM_Init() (test
	// order isn't controlled here), so this can't assume 0. An address
	// near UINT32_MAX is out of range regardless: no real DOS memory
	// config comes anywhere close to 4 GiB. The success path (a real
	// memory-backed address) needs a booted MEM_Init(), which no
	// webserver test currently sets up on purpose - same gap noted for
	// the debugger route group (webserver_debug_tests.cpp).
	httplib::Request req;
	req.body = R"({"address": 4294967280, "value": 1})";
	httplib::Response res;
	FreezeHandlers::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("error").get<std::string>(), "address out of range");
	EXPECT_TRUE(FreezeRegistry::Instance().List().empty());
}

TEST_F(FreezeHandlersTest, PostRejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(FreezeHandlers::Post(req, res), std::exception);
}

TEST_F(FreezeHandlersTest, GetReportsEmptyRegistry)
{
	httplib::Request req;
	httplib::Response res;
	FreezeHandlers::Get(req, res);

	const auto j = nlohmann::json::parse(res.body);
	EXPECT_TRUE(j.at("freezes").empty());
	EXPECT_EQ(j.at("count").get<int>(), 0);
}

TEST_F(FreezeHandlersTest, GetReportsSeededEntries)
{
	FreezeRegistry::Instance().Add(0x1000, 42, 2);
	FreezeRegistry::Instance().Add(0x2000, 7, 1);

	httplib::Request req;
	httplib::Response res;
	FreezeHandlers::Get(req, res);

	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("count").get<int>(), 2);
	ASSERT_EQ(j.at("freezes").size(), 2u);
	EXPECT_EQ(j["freezes"][0].at("address").get<uint32_t>(), 0x1000u);
	EXPECT_EQ(j["freezes"][0].at("value").get<uint32_t>(), 42u);
	EXPECT_EQ(j["freezes"][0].at("width").get<int>(), 2);
}

TEST_F(FreezeHandlersTest, DeleteWithEmptyBodyClearsAll)
{
	FreezeRegistry::Instance().Add(0x1000, 1, 1);
	FreezeRegistry::Instance().Add(0x2000, 2, 1);

	httplib::Request req;
	httplib::Response res;
	FreezeHandlers::Delete(req, res);

	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("status").get<std::string>(), "cleared");
	EXPECT_TRUE(FreezeRegistry::Instance().List().empty());
}

TEST_F(FreezeHandlersTest, DeleteWithAddressRemovesJustThatOne)
{
	FreezeRegistry::Instance().Add(0x1000, 1, 1);
	FreezeRegistry::Instance().Add(0x2000, 2, 1);

	httplib::Request req;
	req.body = R"({"address": 4096})";
	httplib::Response res;
	FreezeHandlers::Delete(req, res);

	// The success path never sets res.status explicitly (send_json only
	// sets the body) - a real server defaults an unset status to 200 on
	// dispatch, but that default doesn't apply when calling the handler
	// directly like this, so only the error paths (which do set it) are
	// asserted on status.
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("status").get<std::string>(), "removed");
	EXPECT_EQ(j.at("address").get<uint32_t>(), 4096u);

	const auto remaining = FreezeRegistry::Instance().List();
	ASSERT_EQ(remaining.size(), 1u);
	EXPECT_EQ(remaining[0].address, 0x2000u);
}

TEST_F(FreezeHandlersTest, DeleteWithAddressNotFoundReturns404)
{
	httplib::Request req;
	req.body = R"({"address": 4096})";
	httplib::Response res;
	FreezeHandlers::Delete(req, res);

	EXPECT_EQ(res.status, 404);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("error").get<std::string>(), "no freeze at that address");
}

} // namespace
