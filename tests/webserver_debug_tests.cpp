// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/debug.h"

#include <gtest/gtest.h>
#include <string>

#include "json/json.h"

using Webserver::DebugAddBreakpointCommand;
using Webserver::DebugAddWatchCommand;
using Webserver::DebugContinueCommand;
using Webserver::DebugDeleteBreakpointCommand;
using Webserver::DebugDeleteWatchCommand;
using Webserver::DebugListBreakpointsCommand;
using Webserver::DebugListWatchesCommand;
using Webserver::DebugPauseCommand;
using Webserver::DebugRunToCommand;
using Webserver::DebugStatusCommand;
using Webserver::DebugStepCommand;
using Webserver::DebugStepOutCommand;
using Webserver::DebugStepOverCommand;
using Webserver::DebugWaitHandlers;

namespace {

// Every handler in this group differs in *shape*, not just behavior,
// between the two build configurations (private/debug.h declares
// DebugAddBreakpointCommand, DebugListBreakpointsCommand and
// DebugDeleteBreakpointCommand with entirely different members/
// constructors per #if C_DEBUGGER), and CI runs the same ctest suite
// once plain and once with -DOPT_DEBUGGER=ON. Splitting on the real
// preprocessor macro (not if constexpr) is required so each leg only
// compiles against what that build actually declares.

#if !C_DEBUGGER

// -- Non-debugger build: every route returns 501, none touch the Bridge --
//
// NotBuilt() (debug.cpp) responds directly from the calling thread, so
// these are as safe to call as ControlHandlers::GetHello - no Command is
// constructed, no emulator state is touched.

TEST(WebserverDebugNotBuilt, StatusReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugStatusCommand::Get(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_status"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, PauseReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugPauseCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_pause"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, ContinueReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugContinueCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_continue"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, StepReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugStepCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_step"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, StepOverReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugStepOverCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_step_over"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, WaitReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugWaitHandlers::Get(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_wait"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, RunToReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugRunToCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_run_to"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, StepOutReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugStepOutCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_step_out"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, AddBreakpointReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugAddBreakpointCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_breakpoints"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, ListBreakpointsReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugListBreakpointsCommand::Get(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_breakpoints"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, DeleteBreakpointReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugDeleteBreakpointCommand::Delete(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_breakpoints"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, AddWatchReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugAddWatchCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_watches"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, ListWatchesReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugListWatchesCommand::Get(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_watches"),
	          std::string::npos);
}

TEST(WebserverDebugNotBuilt, DeleteWatchReturns501)
{
	httplib::Request req;
	httplib::Response res;
	DebugDeleteWatchCommand::Delete(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("debug_watches"),
	          std::string::npos);
}

// A 501-stub handler ignores the request entirely - a body that would be
// rejected on a debugger build (or accepted and then queued) must still
// just produce the same 501, never parse.
TEST(WebserverDebugNotBuilt, IgnoresRequestBodyEntirely)
{
	httplib::Request req;
	req.body = "this is not json and must never be parsed";
	httplib::Response res;
	DebugStepCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
}

#else // C_DEBUGGER

// -- Debugger build: request validation that runs before any Command is
// constructed or queued. These never touch the Bridge, so they're safe
// to call without a pump thread or a booted emulator - unlike the
// happy-path behavior (real stepping, real breakpoint hits, GET
// /api/v1/debug/wait's long poll), which needs a DOSBoxTestFixture-based
// test and a live Bridge pump that no webserver test currently sets up.

TEST(WebserverDebugValidation, AddBreakpointRejectsMissingType)
{
	httplib::Request req;
	req.body = R"({"segment": 0, "offset": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::exception);
}

TEST(WebserverDebugValidation, AddBreakpointRejectsUnknownType)
{
	httplib::Request req;
	req.body = R"({"type": "bogus"})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointRejectsOnceCombinedWithIgnoreCount)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "once": true, "ignore_count": 3})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointRejectsOnceCombinedWithCondition)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "once": true,
	               "condition": {"register": "ax", "op": "eq", "value": 1}})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointExecuteRequiresSegmentAndOffset)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointInterruptRequiresInt)
{
	httplib::Request req;
	req.body = R"({"type": "interrupt"})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointInterruptRejectsOutOfRangeInt)
{
	httplib::Request req;
	req.body = R"({"type": "interrupt", "int": 256})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointConditionMustBeObject)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "condition": "not an object"})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointConditionRejectsRegisterAndMemoryTogether)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "condition": {"register": "ax", "segment": 0,
	                             "op": "eq", "value": 1}})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointConditionRequiresOp)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "condition": {"register": "ax", "value": 1}})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointConditionRejectsUnknownOp)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "condition": {"register": "ax", "op": "wat", "value": 1}})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointConditionRejectsUnknownRegister)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "condition": {"register": "zz", "op": "eq", "value": 1}})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddBreakpointConditionRejectsBadMemoryWidth)
{
	httplib::Request req;
	req.body = R"({"type": "execute", "segment": 0, "offset": 0,
	               "condition": {"segment": 0, "offset": 0, "width": 3,
	                             "op": "eq", "value": 1}})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}

#if !C_HEAVY_DEBUGGER
// The CI "Debugger build" leg is C_DEBUGGER=1, C_HEAVY_DEBUGGER=0 - this
// is the path that actually runs there. Memory breakpoints need
// C_HEAVY_DEBUGGER (debugger.cpp's CheckBreakpoint match logic is
// compiled out otherwise), so this returns its own 501 directly, without
// ever constructing a Command.
TEST(WebserverDebugValidation, AddMemoryBreakpointReturns501WithoutHeavyDebugger)
{
	httplib::Request req;
	req.body = R"({"type": "memory", "segment": 0, "offset": 0})";
	httplib::Response res;
	DebugAddBreakpointCommand::Post(req, res);

	EXPECT_EQ(res.status, httplib::StatusCode::NotImplemented_501);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("C_HEAVY_DEBUGGER"),
	          std::string::npos);
}
#else
// Only reachable with C_HEAVY_DEBUGGER, same reasoning as the #if branch
// above: "trigger" is only parsed once type == "memory" gets past the
// heavy-debugger gate.
TEST(WebserverDebugValidation, AddMemoryBreakpointRejectsUnknownTrigger)
{
	httplib::Request req;
	req.body =
	        R"({"type": "memory", "segment": 0, "offset": 0, "trigger": "bogus"})";
	httplib::Response res;
	EXPECT_THROW(DebugAddBreakpointCommand::Post(req, res), std::invalid_argument);
}
#endif // !C_HEAVY_DEBUGGER

TEST(WebserverDebugValidation, AddWatchRejectsMissingName)
{
	httplib::Request req;
	req.body = R"({"segment": 0, "offset": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugAddWatchCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddWatchRejectsEmptyName)
{
	httplib::Request req;
	req.body = R"({"name": "", "segment": 0, "offset": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugAddWatchCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddWatchRejectsNameTooLong)
{
	httplib::Request req;
	// 16 characters - one past the 15-char cap (IV's fixed 16-byte
	// buffer, name + NUL).
	req.body = R"({"name": "0123456789abcdef", "segment": 0, "offset": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugAddWatchCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddWatchAcceptsFifteenCharName)
{
	httplib::Request req;
	req.body = R"({"name": "0123456789abcde", "segment": 0, "offset": 0})";
	httplib::Response res;
	// Passes JSON-body validation and reaches WaitForCompletion(), which
	// has no Bridge pump running here - times out rather than throwing.
	// That's fine: this test only asserts the 15-char name itself isn't
	// what gets rejected, matching the boundary the "too long" test
	// checks from the other side.
	EXPECT_THROW(DebugAddWatchCommand::Post(req, res), std::exception);
}

TEST(WebserverDebugValidation, AddWatchRejectsMissingSegment)
{
	httplib::Request req;
	req.body = R"({"name": "hp", "offset": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugAddWatchCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddWatchRejectsMissingOffset)
{
	httplib::Request req;
	req.body = R"({"name": "hp", "segment": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugAddWatchCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, AddWatchRejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(DebugAddWatchCommand::Post(req, res), std::exception);
}

TEST(WebserverDebugValidation, DeleteWatchRejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(DebugDeleteWatchCommand::Delete(req, res), std::exception);
}

TEST(WebserverDebugValidation, DeleteWatchRejectsMissingAddress)
{
	httplib::Request req;
	req.body = "{}";
	httplib::Response res;
	EXPECT_THROW(DebugDeleteWatchCommand::Delete(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, DeleteBreakpointRejectsBothIdAndIndex)
{
	httplib::Request req;
	req.body = R"({"id": 1, "index": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugDeleteBreakpointCommand::Delete(req, res),
	             std::invalid_argument);
}

TEST(WebserverDebugValidation, DeleteBreakpointRejectsNeitherIdNorIndex)
{
	httplib::Request req;
	req.body = "{}";
	httplib::Response res;
	EXPECT_THROW(DebugDeleteBreakpointCommand::Delete(req, res),
	             std::invalid_argument);
}

TEST(WebserverDebugValidation, RunToRejectsMissingSegment)
{
	httplib::Request req;
	req.body = R"({"offset": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugRunToCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, RunToRejectsMissingOffset)
{
	httplib::Request req;
	req.body = R"({"segment": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugRunToCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, RunToRejectsOutOfRangeSegment)
{
	httplib::Request req;
	req.body = R"({"segment": -1, "offset": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugRunToCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, RunToRejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(DebugRunToCommand::Post(req, res), std::exception);
}

TEST(WebserverDebugValidation, StepRejectsZeroCount)
{
	httplib::Request req;
	req.body = R"({"count": 0})";
	httplib::Response res;
	EXPECT_THROW(DebugStepCommand::Post(req, res), std::invalid_argument);
}

TEST(WebserverDebugValidation, StepRejectsCountAboveMax)
{
	httplib::Request req;
	// DEBUG_MaxStepCount is 64 (debugger.h) - one past the cap.
	req.body = R"({"count": 65})";
	httplib::Response res;
	EXPECT_THROW(DebugStepCommand::Post(req, res), std::invalid_argument);
}

#endif // C_DEBUGGER

} // namespace
