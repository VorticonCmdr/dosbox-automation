// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/cpu.h"

#include <gtest/gtest.h>

#include "json/json.h"

using Webserver::RegClass;
using Webserver::RegisterKind;
using Webserver::WriteRegisterCommand;

namespace {

TEST(CpuWrite, GeneralRegistersRecognized)
{
	EXPECT_EQ(RegisterKind("eax").reg_class, RegClass::General);
	EXPECT_EQ(RegisterKind("ebx").reg_class, RegClass::General);
	EXPECT_EQ(RegisterKind("ecx").reg_class, RegClass::General);
	EXPECT_EQ(RegisterKind("edx").reg_class, RegClass::General);
	EXPECT_EQ(RegisterKind("esi").reg_class, RegClass::General);
	EXPECT_EQ(RegisterKind("edi").reg_class, RegClass::General);
	EXPECT_EQ(RegisterKind("esp").reg_class, RegClass::General);
	EXPECT_EQ(RegisterKind("ebp").reg_class, RegClass::General);
}

TEST(CpuWrite, SegmentRegistersRecognized)
{
	EXPECT_EQ(RegisterKind("cs").reg_class, RegClass::Segment);
	EXPECT_EQ(RegisterKind("ds").reg_class, RegClass::Segment);
	EXPECT_EQ(RegisterKind("es").reg_class, RegClass::Segment);
	EXPECT_EQ(RegisterKind("ss").reg_class, RegClass::Segment);
	EXPECT_EQ(RegisterKind("fs").reg_class, RegClass::Segment);
	EXPECT_EQ(RegisterKind("gs").reg_class, RegClass::Segment);
}

TEST(CpuWrite, UnknownRejected)
{
	EXPECT_EQ(RegisterKind("banana").reg_class, RegClass::Unknown);
	EXPECT_EQ(RegisterKind("eip").reg_class, RegClass::Unknown);
	EXPECT_EQ(RegisterKind("flags").reg_class, RegClass::Unknown);
}

TEST(CpuWrite, IndicesAreDistinct)
{
	auto eax = RegisterKind("eax");
	auto ebx = RegisterKind("ebx");
	EXPECT_NE(eax.index, ebx.index);
	EXPECT_EQ(eax.reg_class, ebx.reg_class);
}

// -- WriteRegisterCommand::Put: HTTP-level request handling --
//
// The unknown-register and out-of-range-segment-value checks respond
// directly and return before constructing a Command or touching the
// Bridge, so they're safe to exercise here. The success path needs a
// live Bridge pump, which no webserver test currently sets up - same
// gap noted for the debugger route group (webserver_debug_tests.cpp).

TEST(WriteRegisterHandler, RejectsUnknownRegister)
{
	httplib::Request req;
	req.body = R"({"register": "banana", "value": 1})";
	httplib::Response res;
	WriteRegisterCommand::Put(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("error").get<std::string>(), "Unknown register: banana");
}

TEST(WriteRegisterHandler, RejectsSegmentValueAbove0xFFFF)
{
	httplib::Request req;
	req.body = R"({"register": "cs", "value": 65536})";
	httplib::Response res;
	WriteRegisterCommand::Put(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("error").get<std::string>(),
	          "Segment register value must be 0..0xFFFF");
}

TEST(WriteRegisterHandler, RejectsMissingRequiredFields)
{
	httplib::Request req;
	req.body = R"({"register": "eax"})";
	httplib::Response res;
	EXPECT_THROW(WriteRegisterCommand::Put(req, res), std::exception);
}

TEST(WriteRegisterHandler, RejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(WriteRegisterCommand::Put(req, res), std::exception);
}

} // namespace
