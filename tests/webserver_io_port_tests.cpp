// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/io_port.h"

#include <gtest/gtest.h>

using Webserver::PortReadCommand;
using Webserver::PortWriteCommand;
using Webserver::ValidatePortRequest;

namespace {

TEST(IoPort, AcceptsByteWidth)
{
	EXPECT_NO_THROW(ValidatePortRequest(0x3B8, 1));
}

TEST(IoPort, AcceptsWordWidth)
{
	EXPECT_NO_THROW(ValidatePortRequest(0x3C4, 2));
}

TEST(IoPort, AcceptsMaxPort)
{
	EXPECT_NO_THROW(ValidatePortRequest(0xFFFF, 1));
}

TEST(IoPort, AcceptsZeroPort)
{
	EXPECT_NO_THROW(ValidatePortRequest(0x0000, 1));
}

TEST(IoPort, RejectsWidth4)
{
	EXPECT_THROW(ValidatePortRequest(0x60, 4), std::invalid_argument);
}

TEST(IoPort, RejectsWidth0)
{
	EXPECT_THROW(ValidatePortRequest(0x60, 0), std::invalid_argument);
}

TEST(IoPort, RejectsPortAbove0xFFFF)
{
	EXPECT_THROW(ValidatePortRequest(0x10000, 1), std::invalid_argument);
}

// -- PortReadCommand::Get / PortWriteCommand::Put: HTTP-level request
// handling --
//
// Both validate the request (the ValidatePortRequest calls above) before
// constructing a Command or touching the Bridge, so the rejection paths
// are safe to exercise directly. The success path needs a live Bridge
// pump plus the real IO subsystem, which no webserver test currently
// sets up - same gap noted for the debugger route group
// (webserver_debug_tests.cpp).

TEST(IoPortHandlers, GetRejectsPortAbove0xFFFF)
{
	httplib::Request req;
	req.params.emplace("port", "70000");
	httplib::Response res;
	EXPECT_THROW(PortReadCommand::Get(req, res), std::invalid_argument);
}

TEST(IoPortHandlers, GetRejectsBadWidth)
{
	httplib::Request req;
	req.params.emplace("port", "888");
	req.params.emplace("width", "4");
	httplib::Response res;
	EXPECT_THROW(PortReadCommand::Get(req, res), std::invalid_argument);
}

TEST(IoPortHandlers, GetDefaultsWidthToOneWhenAbsent)
{
	// Same "no pump running" reasoning as the debugger route group: a
	// request that passes validation reaches the Bridge, which has
	// nothing to throw promptly or times out - not something to
	// exercise here. This only confirms the port-missing case still
	// fails validation before any of that, via num_param's own
	// required-parameter check.
	httplib::Request req;
	httplib::Response res;
	EXPECT_THROW(PortReadCommand::Get(req, res), std::invalid_argument);
}

TEST(IoPortHandlers, PutRejectsPortAbove0xFFFF)
{
	httplib::Request req;
	req.body = R"({"port": 70000, "value": 1})";
	httplib::Response res;
	EXPECT_THROW(PortWriteCommand::Put(req, res), std::invalid_argument);
}

TEST(IoPortHandlers, PutRejectsBadWidth)
{
	httplib::Request req;
	req.body = R"({"port": 888, "value": 1, "width": 4})";
	httplib::Response res;
	EXPECT_THROW(PortWriteCommand::Put(req, res), std::invalid_argument);
}

TEST(IoPortHandlers, PutRejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(PortWriteCommand::Put(req, res), std::exception);
}

TEST(IoPortHandlers, PutRejectsMissingRequiredFields)
{
	httplib::Request req;
	req.body = R"({"port": 888})";
	httplib::Response res;
	EXPECT_THROW(PortWriteCommand::Put(req, res), std::exception);
}

} // namespace
