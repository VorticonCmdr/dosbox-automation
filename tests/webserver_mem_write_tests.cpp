// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/memory.h"
#include "webserver/webserver.h"

#include <gtest/gtest.h>
#include <string>

using Webserver::Segment;
using Webserver::StrToBaseSegment;
using Webserver::TypeBinary;
using Webserver::TypeJson;
using Webserver::WriteMemoryCommand;

namespace {

// -- StrToBaseSegment: pure lookup --

TEST(StrToBaseSegmentTest, RecognizesEveryRegisterCaseInsensitively)
{
	EXPECT_EQ(StrToBaseSegment("cs"), Segment::CS);
	EXPECT_EQ(StrToBaseSegment("CS"), Segment::CS);
	EXPECT_EQ(StrToBaseSegment("ss"), Segment::SS);
	EXPECT_EQ(StrToBaseSegment("ds"), Segment::DS);
	EXPECT_EQ(StrToBaseSegment("es"), Segment::ES);
	EXPECT_EQ(StrToBaseSegment("fs"), Segment::FS);
	EXPECT_EQ(StrToBaseSegment("gs"), Segment::GS);
}

TEST(StrToBaseSegmentTest, UnknownNameIsNone)
{
	// A numeric paragraph value ("1000") is also not a register name -
	// parse_mem_addr's caller relies on this same None return to know
	// to resolve it as a fixed address instead.
	EXPECT_EQ(StrToBaseSegment("1000"), Segment::None);
	EXPECT_EQ(StrToBaseSegment("nope"), Segment::None);
	EXPECT_EQ(StrToBaseSegment(""), Segment::None);
}

// -- WriteMemoryCommand::Put: HTTP-level request handling --
//
// Every check below (path param parsing, Content-Type, body shape, the
// If-Match header's unquote-then-base64-decode, and the 128 MiB size
// cap) runs before a Command is constructed or the Bridge is touched,
// so it's safe to exercise directly. The actual compare-and-swap
// (Execute()'s conflict detection, and therefore the real 412 response)
// needs a live Bridge pump plus a booted MEM_Init(), which no webserver
// test currently sets up - same gap noted for the debugger route group
// (webserver_debug_tests.cpp) and for freeze/search/cpu-write
// (webserver_freeze_tests.cpp et al). The bridge side of If-Match/412
// (dosbox-mcp's _mem_write) already has full coverage of the 412
// response shape in tests/test_memory_tools.py - this file closes the
// engine-side half of that gap, not a redundant copy of it.

TEST(WriteMemoryHandler, RejectsMissingOffsetPathParam)
{
	// A real request can't actually reach this handler without an
	// "offset" path segment (that's what routes to it), so this is a
	// defense-in-depth check, not a reachable production scenario.
	// req.path_params.at("offset") throws std::out_of_range directly,
	// not num_param's own std::invalid_argument - that one only fires
	// for a present-but-empty value.
	httplib::Request req;
	req.set_header("Content-Type", TypeJson);
	req.body = R"({"data": "AQ=="})";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::out_of_range);
}

TEST(WriteMemoryHandler, RejectsMissingContentType)
{
	httplib::Request req;
	req.path_params["offset"] = "100";
	req.body                  = R"({"data": "AQ=="})";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::invalid_argument);
}

TEST(WriteMemoryHandler, RejectsUnknownContentType)
{
	httplib::Request req;
	req.path_params["offset"] = "100";
	req.set_header("Content-Type", "text/plain");
	req.body = "AQ==";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::invalid_argument);
}

TEST(WriteMemoryHandler, RejectsMalformedJsonBody)
{
	httplib::Request req;
	req.path_params["offset"] = "100";
	req.set_header("Content-Type", TypeJson);
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::exception);
}

TEST(WriteMemoryHandler, RejectsJsonBodyMissingData)
{
	httplib::Request req;
	req.path_params["offset"] = "100";
	req.set_header("Content-Type", TypeJson);
	req.body = "{}";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::exception);
}

TEST(WriteMemoryHandler, RejectsMalformedBase64Data)
{
	httplib::Request req;
	req.path_params["offset"] = "100";
	req.set_header("Content-Type", TypeJson);
	req.body = R"({"data": "not valid base64!!"})";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::exception);
}

TEST(WriteMemoryHandler, RejectsDataOverTheSizeCap)
{
	// 128 MiB + 1 byte of raw (binary Content-Type, so no base64
	// inflation to account for) payload.
	httplib::Request req;
	req.path_params["offset"] = "100";
	req.set_header("Content-Type", TypeBinary);
	req.body = std::string(128 * 1024 * 1024 + 1, 'A');
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::invalid_argument);
}

TEST(WriteMemoryHandler, RejectsSegmentOffsetOverflowPastAddressableRange)
{
	// A numeric paragraph segment resolves eagerly in parse_mem_addr
	// (not deferred to Execute() like a register name), so this
	// overflow throws before any Command exists.
	httplib::Request req;
	req.path_params["offset"]  = "4294967295"; // UINT32_MAX
	req.path_params["segment"] = "65535"; // 0xFFFF -> paragraph 0xFFFF0
	req.set_header("Content-Type", TypeJson);
	req.body = R"({"data": "AQ=="})";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::invalid_argument);
}

TEST(WriteMemoryHandler, RejectsMalformedBase64IfMatchHeader)
{
	httplib::Request req;
	req.path_params["offset"] = "100";
	req.set_header("Content-Type", TypeJson);
	req.set_header("If-Match", "not valid base64!!");
	req.body = R"({"data": "AQ=="})";
	httplib::Response res;
	EXPECT_THROW(WriteMemoryCommand::Put(req, res), std::exception);
}

} // namespace
