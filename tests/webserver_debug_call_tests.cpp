// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/debug_call.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "dosbox.h"
#include "json/json.h"

#if C_DEBUGGER

using DebugCall::DescribeUnrestored;
using DebugCall::DiffBytes;
using DebugCall::InterruptKind;
using Webserver::ParseDebugCallRequest;
using json = nlohmann::json;

namespace {

json minimal()
{
	return {
	        {   "kind", "far16"},
                {"segment",  0x1000},
                { "offset",   0x100}
        };
}

// "AQI=" is base64 for {0x01, 0x02}
constexpr const char* TwoBytes = "AQI=";

TEST(DebugCallDiff, NoDifferences)
{
	const std::vector<uint8_t> a(64, 7);
	const auto d = DiffBytes(a.data(), a.data(), 64, 0x1000, 8, 1 << 20);
	EXPECT_TRUE(d.diffs.empty());
	EXPECT_EQ(d.changed_bytes, 0);
	EXPECT_FALSE(d.truncated);
}

TEST(DebugCallDiff, MergesRunsWithinGap)
{
	std::vector<uint8_t> before(64, 0);
	auto after = before;
	after[2]   = 1;
	after[5]   = 1; // 3 bytes later: merged with byte 2
	after[30]  = 1; // far away: its own run
	const auto d = DiffBytes(before.data(), after.data(), 64, 0x1000, 8, 1 << 20);
	ASSERT_EQ(d.diffs.size(), 2u);
	EXPECT_EQ(d.diffs[0].address, 0x1002u);
	EXPECT_EQ(d.diffs[0].bytes, (std::vector<uint8_t>{1, 0, 0, 1}));
	EXPECT_EQ(d.diffs[1].address, 0x101eu);
	EXPECT_EQ(d.changed_bytes, 3);
}

TEST(DebugCallDiff, TruncatesButStillCountsEveryChange)
{
	std::vector<uint8_t> before(256, 0);
	std::vector<uint8_t> after(256, 1);
	const auto d = DiffBytes(before.data(), after.data(), 256, 0, 8, 100);
	EXPECT_TRUE(d.truncated);
	EXPECT_TRUE(d.diffs.empty()); // the one run is longer than the cap
	EXPECT_EQ(d.changed_bytes, 256);
}

TEST(DebugCallDiff, ChangeAtLastByte)
{
	std::vector<uint8_t> before(16, 0);
	auto after = before;
	after[15]  = 9;
	const auto d = DiffBytes(before.data(), after.data(), 16, 0, 8, 1 << 20);
	ASSERT_EQ(d.diffs.size(), 1u);
	EXPECT_EQ(d.diffs[0].address, 15u);
	EXPECT_EQ(d.diffs[0].bytes, (std::vector<uint8_t>{9}));
}

TEST(DebugCallUnrestored, CleanCallReportsNothing)
{
	const std::vector<DebugCall::PortUse> no_ports = {};
	EXPECT_TRUE(DescribeUnrestored(no_ports,
	                               {
	                                       {0x21, 0x2c, InterruptKind::Software, 1}
        },
	                               {{"DOS Int 21", 0x2c, 1}},
	                               0)
	                    .empty());
}

TEST(DebugCallUnrestored, HandlersAreAnAllowList)
{
	EXPECT_TRUE(DebugCall::IsRestorableHandler("DOS Int 21", 0x2c));
	EXPECT_TRUE(DebugCall::IsRestorableHandler("Int 11 Equipment", 0x99));
	// chdir lives in Drives[]->curdir, set drive in the DOS block
	EXPECT_FALSE(DebugCall::IsRestorableHandler("DOS Int 21", 0x3b));
	EXPECT_FALSE(DebugCall::IsRestorableHandler("DOS Int 21", 0x0e));
	EXPECT_FALSE(DebugCall::IsRestorableHandler("Int 67 ems", 0x44));
	EXPECT_FALSE(DebugCall::IsRestorableHandler("something new", 0));
	const std::vector<DebugCall::PortUse> no_ports     = {};
	const std::vector<DebugCall::InterruptUse> no_ints = {};
	const auto u = DescribeUnrestored(no_ports,
	                                  no_ints,
	                                  {
	                                          {"DOS Int 21", 0x3b, 1}
        },
	                                  0);
	ASSERT_EQ(u.size(), 1u);
	EXPECT_NE(u[0].find("'DOS Int 21' (AH=3Bh)"), std::string::npos);
}

TEST(DebugCallUnrestored, ReportsEveryCategory)
{
	const std::vector<DebugCall::PortUse> ports = {
	        {0x3da, 5, 0}
        };
	const auto u = DescribeUnrestored(
	        ports,
	        {
	                {0x00, 0x00, InterruptKind::Exception, 1}
        },
	        {{"DOS Int 21", 0x3d, 1},
		 {"Int 67 ems", 0x44, 1},
		 {"IRQ 0 Clock", 0, 3}},
	        3);
	ASSERT_EQ(u.size(), 5u);
	EXPECT_NE(u[0].find("port I/O"), std::string::npos);
	EXPECT_NE(u[1].find("AH=3Dh"), std::string::npos);
	EXPECT_NE(u[2].find("Int 67 ems"), std::string::npos);
	EXPECT_NE(u[3].find("exception"), std::string::npos);
	EXPECT_NE(u[4].find("machine time"), std::string::npos);
}

TEST(DebugCallParse, MinimalRequestIsOneEmptyCase)
{
	const auto r = ParseDebugCallRequest(minimal());
	EXPECT_EQ(r.kind, DebugCall::CallKind::Far16);
	ASSERT_EQ(r.cases.size(), 1u);
	EXPECT_TRUE(r.cases[0].registers.empty());
	EXPECT_FALSE(r.timers);
}

TEST(DebugCallParse, CasesRegistersMemoryRanges)
{
	auto j       = minimal();
	j["cases"]   = json::array({
	        {{"registers", {{"eax", 10}, {"ds", 0x2000}}},
		 {"memory", json::array({{{"address", 0x22000}, {"data", TwoBytes}}})}},
	        {{"registers", {{"ecx", 0xffffffff}}}}
        });
	j["ranges"]  = json::array({
	        {{"space", "vram"}, {"start", 0}, {"end", 0x10000}}
        });
	const auto r = ParseDebugCallRequest(j);
	ASSERT_EQ(r.cases.size(), 2u);
	EXPECT_EQ(r.cases[0].registers.size(), 2u);
	EXPECT_EQ(r.cases[0].memory[0].bytes, (std::vector<uint8_t>{1, 2}));
	EXPECT_EQ(r.cases[1].registers[0].value, 0xffffffffu);
	EXPECT_EQ(r.ranges[0].space, DebugCall::Space::Vram);
}

TEST(DebugCallParse, RejectsMalformedInput)
{
	const auto bad = [](const json& j) {
		EXPECT_THROW(ParseDebugCallRequest(j), std::exception) << j.dump();
	};
	bad(json::array());
	bad({
	        {"kind", "far16"}
        }); // no segment/offset
	auto j    = minimal();
	j["kind"] = "far32";
	bad(j); // unknown kind
	j           = minimal();
	j["offset"] = 0x10000;
	bad(j); // 16-bit offset
	j             = minimal();
	j["surprise"] = 1;
	bad(j); // unknown field
	j                = minimal();
	j["stub_offset"] = 0;
	bad(j); // stub for far16
	j              = minimal();
	j["registers"] = {
	        {"eip", 1}
        };
	bad(j); // not writable
	j              = minimal();
	j["registers"] = {
	        {"eax", -1}
        };
	bad(j);
	j              = minimal();
	j["registers"] = {
	        {"eax", 1}
        };
	j["cases"] = json::array({json::object()});
	bad(j);
	j          = minimal();
	j["cases"] = json::array();
	bad(j);
	j           = minimal();
	j["memory"] = json::array({
	        {{"address", 0}, {"data", "!!"}}
        });
	bad(j);
	j           = minimal();
	j["memory"] = json::array({
	        {{"address", 0}, {"data", ""}}
        });
	bad(j);
	j           = minimal();
	j["ranges"] = json::array({
	        {{"start", 10}, {"end", 10}}
        });
	bad(j);
	j           = minimal();
	j["ranges"] = json::array({
	        {{"start", 0}, {"end", (16 << 20) + 1}}
        });
	bad(j);
	j               = minimal();
	j["timeout_ms"] = 60001;
	bad(j);
	j               = minimal();
	j["max_cycles"] = 0;
	bad(j);
	j           = minimal();
	j["timers"] = 1;
	bad(j);
	j           = minimal();
	j["return"] = "jump";
	bad(j);
	j           = minimal();
	j["return"] = 1;
	bad(j);
}

TEST(DebugCallParse, ReturnMode)
{
	EXPECT_EQ(ParseDebugCallRequest(minimal()).return_mode,
	          DebugCall::ReturnMode::Push);
	auto j      = minimal();
	j["return"] = "replace";
	EXPECT_EQ(ParseDebugCallRequest(j).return_mode,
	          DebugCall::ReturnMode::Replace);
}

TEST(DebugCallParse, CapsCaseCountAndPatchVolume)
{
	auto j     = minimal();
	j["cases"] = json::array();
	for (size_t i = 0; i <= Webserver::MaxCallCases; ++i) {
		j["cases"].push_back(json::object());
	}
	EXPECT_THROW(ParseDebugCallRequest(j), std::invalid_argument);

	// 2 MiB of patches in one case
	j = minimal();
	const std::string big_b64((1 << 20) / 3 * 4, 'A'); // ~768 KiB decoded
	j["memory"] = json::array({
	        {{"address", 0}, {"data", big_b64}},
	        {{"address", 0}, {"data", big_b64}}
        });
	EXPECT_THROW(ParseDebugCallRequest(j), std::invalid_argument);
}

TEST(DebugCallParse, CapsResponseSize)
{
	auto j     = minimal();
	j["cases"] = json::array();
	for (int i = 0; i < 64; ++i) {
		j["cases"].push_back(json::object());
	}
	j["ranges"]         = json::array({
	        {{"start", 0}, {"end", 0x100000}},
	        {{"start", 0}, {"end", 0x100000}}
        });
	j["max_diff_bytes"] = 1 << 20; // 64 x 2 x 1 MiB > 64 MiB
	EXPECT_THROW(ParseDebugCallRequest(j), std::invalid_argument);
	j["max_diff_bytes"] = 1 << 19;
	EXPECT_NO_THROW(ParseDebugCallRequest(j));
}

} // namespace

#endif
