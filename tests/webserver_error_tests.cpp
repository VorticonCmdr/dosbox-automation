// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/bridge.h"
#include "webserver/webserver.h"

#include <gtest/gtest.h>
#include <stdexcept>

#include "libs/json/json.h"

using Webserver::BridgeNotPumping;
using Webserver::BridgeQueueFull;
using Webserver::BridgeTimeout;
using Webserver::ClassifyException;

namespace {

TEST(ClassifyExceptionTest, NullPointerIsGenericInternalError)
{
	const auto info = ClassifyException(nullptr);
	EXPECT_EQ(info.status, httplib::StatusCode::InternalServerError_500);
	EXPECT_EQ(info.code, "internal_error");
	EXPECT_FALSE(info.retryable);
}

TEST(ClassifyExceptionTest, InvalidArgumentEchoesMessageAsBadRequest)
{
	auto ep = std::make_exception_ptr(
	        std::invalid_argument("port must be 0x0000..0xFFFF"));
	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::BadRequest_400);
	EXPECT_EQ(info.message, "port must be 0x0000..0xFFFF");
	EXPECT_EQ(info.code, "invalid_argument");
	EXPECT_FALSE(info.retryable);
}

TEST(ClassifyExceptionTest, OutOfRangeEchoesMessageAsBadRequest)
{
	auto ep = std::make_exception_ptr(
	        std::out_of_range("Address range exceeds emulated memory size"));
	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::BadRequest_400);
	EXPECT_EQ(info.message, "Address range exceeds emulated memory size");
	EXPECT_EQ(info.code, "out_of_range");
	EXPECT_FALSE(info.retryable);
}

TEST(ClassifyExceptionTest, MalformedJsonEchoesMessageAsBadRequest)
{
	auto ep = [] {
		try {
			[[maybe_unused]] auto parsed = nlohmann::json::parse(
			        "{not valid json");
			return std::exception_ptr();
		} catch (...) {
			return std::current_exception();
		}
	}();
	ASSERT_TRUE(static_cast<bool>(ep));

	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::BadRequest_400);
	EXPECT_EQ(info.code, "malformed_body");
	EXPECT_FALSE(info.message.empty());
	EXPECT_FALSE(info.retryable);
}

TEST(ClassifyExceptionTest, BridgeTimeoutIsRetryableServiceUnavailable)
{
	auto ep = std::make_exception_ptr(
	        BridgeTimeout("Command execution timed out - the emulator may "
	                      "be paused, minimized, or unresponsive"));
	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::ServiceUnavailable_503);
	EXPECT_EQ(info.code, "bridge_timeout");
	EXPECT_TRUE(info.retryable);
	EXPECT_NE(info.message.find("paused"), std::string::npos);
}

TEST(ClassifyExceptionTest, NotPumpingIsRetryableServiceUnavailable)
{
	auto ep         = std::make_exception_ptr(BridgeNotPumping(
	        "The emulation thread has not processed requests recently - "
	        "it may be paused, minimized, or unresponsive"));
	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::ServiceUnavailable_503);
	EXPECT_EQ(info.code, "not_pumping");
	EXPECT_TRUE(info.retryable);
	EXPECT_NE(info.message.find("paused"), std::string::npos);
}

TEST(ClassifyExceptionTest, QueueFullIsRetryableTooManyRequests)
{
	auto ep         = std::make_exception_ptr(BridgeQueueFull(
	        "Too many commands are already queued for the emulation thread"));
	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::TooManyRequests_429);
	EXPECT_EQ(info.code, "queue_full");
	EXPECT_TRUE(info.retryable);
}

TEST(ClassifyExceptionTest, GenericStdExceptionStaysGenericNotEchoed)
{
	// A plain std::runtime_error is deliberately NOT given its own
	// catch clause: an unclassified exception from deep in the
	// emulator could carry a filesystem path or other detail that
	// must not reach an HTTP caller. It must fall through to the
	// generic internal_error default, not echo e.what().
	auto ep = std::make_exception_ptr(
	        std::runtime_error("/Users/someone/secret/path/leaked"));
	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::InternalServerError_500);
	EXPECT_EQ(info.code, "internal_error");
	EXPECT_EQ(info.message, "Internal server error");
	EXPECT_EQ(info.message.find("secret"), std::string::npos);
	EXPECT_FALSE(info.retryable);
}

TEST(ClassifyExceptionTest, NonStdExceptionStaysGeneric)
{
	auto ep = std::make_exception_ptr(42); // an arbitrary
	                                       // non-std::exception throw
	const auto info = ClassifyException(ep);
	EXPECT_EQ(info.status, httplib::StatusCode::InternalServerError_500);
	EXPECT_EQ(info.code, "internal_error");
	EXPECT_FALSE(info.retryable);
}

} // namespace
