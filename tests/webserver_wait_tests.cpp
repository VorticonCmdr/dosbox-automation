// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/wait.h"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Webserver::HashSource;
using Webserver::MaxWaiters;
using Webserver::MaxWaitTimeoutMs;
using Webserver::MemoryCompareOp;
using Webserver::MinWaitTimeoutMs;
using Webserver::ParseHexHash;
using Webserver::ParseWaitCondition;
using Webserver::ParseWaitRequest;
using Webserver::TooManyWaiters;
using Webserver::WaitCondition;
using Webserver::WaitRegistry;

using json = nlohmann::json;

namespace {

// ---- ParseWaitCondition -----------------------------------------------

TEST(ParseWaitConditionTest, ParsesEveryDocumentedCondition)
{
	EXPECT_EQ(ParseWaitCondition("text"), WaitCondition::Text);
	EXPECT_EQ(ParseWaitCondition("screen_change"), WaitCondition::ScreenChange);
	EXPECT_EQ(ParseWaitCondition("frames"), WaitCondition::Frames);
	EXPECT_EQ(ParseWaitCondition("replay_done"), WaitCondition::ReplayDone);
	EXPECT_EQ(ParseWaitCondition("memory"), WaitCondition::Memory);
	EXPECT_EQ(ParseWaitCondition("stopped"), WaitCondition::Stopped);
	EXPECT_EQ(ParseWaitCondition("script_done"), WaitCondition::ScriptDone);
	EXPECT_EQ(ParseWaitCondition("program"), WaitCondition::Program);
}

TEST(ParseWaitConditionTest, RejectsUnknownValue)
{
	EXPECT_THROW(ParseWaitCondition("bogus"), std::invalid_argument);
}

TEST(ParseWaitConditionTest, IsCaseSensitive)
{
	EXPECT_THROW(ParseWaitCondition("Text"), std::invalid_argument);
}

// ---- ParseHexHash -------------------------------------------------------

TEST(ParseHexHashTest, RoundTripsAKnownValue)
{
	EXPECT_EQ(ParseHexHash("00000000000000ff"), 0xffULL);
	EXPECT_EQ(ParseHexHash("ffffffffffffffff"), 0xffffffffffffffffULL);
}

TEST(ParseHexHashTest, RejectsWrongLength)
{
	EXPECT_THROW(ParseHexHash("ff"), std::invalid_argument);
	EXPECT_THROW(ParseHexHash("00000000000000ffff"), std::invalid_argument);
}

TEST(ParseHexHashTest, RejectsNonHexCharacters)
{
	EXPECT_THROW(ParseHexHash("000000000000000z"), std::invalid_argument);
}

// ---- ParseWaitRequest -----------------------------------------------------
//
// Bodies are built via sequential j["field"] = value assignment, matching
// how every response-building function in webserver/*.cpp already
// constructs json - not aggregate-initializer syntax, which clang-format
// wraps one-entry-per-line for anything past a single key here.

TEST(ParseWaitRequestTest, RejectsMissingFor)
{
	json body = json::object();
	EXPECT_THROW(ParseWaitRequest(body, 0, ""), std::invalid_argument);
}

TEST(ParseWaitRequestTest, DefaultsTimeoutWhenAbsent)
{
	json body   = {};
	body["for"] = "replay_done";

	const auto spec = ParseWaitRequest(body, 0, "");
	EXPECT_EQ(spec.timeout_ms, Webserver::DefaultWaitTimeoutMs);
}

TEST(ParseWaitRequestTest, ClampsTimeoutToTheDocumentedRange)
{
	json too_low          = {};
	too_low["for"]        = "replay_done";
	too_low["timeout_ms"] = 0;
	EXPECT_THROW(ParseWaitRequest(too_low, 0, ""), std::invalid_argument);

	json too_high          = {};
	too_high["for"]        = "replay_done";
	too_high["timeout_ms"] = MaxWaitTimeoutMs + 1;
	EXPECT_THROW(ParseWaitRequest(too_high, 0, ""), std::invalid_argument);

	json at_min          = {};
	at_min["for"]        = "replay_done";
	at_min["timeout_ms"] = MinWaitTimeoutMs;
	const auto spec      = ParseWaitRequest(at_min, 0, "");
	EXPECT_EQ(spec.timeout_ms, MinWaitTimeoutMs);
}

TEST(ParseWaitRequestTest, TextRequiresANonEmptyPattern)
{
	json missing   = {};
	missing["for"] = "text";
	EXPECT_THROW(ParseWaitRequest(missing, 0, ""), std::invalid_argument);

	json empty       = {};
	empty["for"]     = "text";
	empty["pattern"] = "";
	EXPECT_THROW(ParseWaitRequest(empty, 0, ""), std::invalid_argument);

	json ok         = {};
	ok["for"]       = "text";
	ok["pattern"]   = "C:\\>";
	const auto spec = ParseWaitRequest(ok, 0, "");
	EXPECT_EQ(spec.pattern, "C:\\>");
	EXPECT_FALSE(spec.ignore_case);
}

TEST(ParseWaitRequestTest, TextRejectsAnOversizedPattern)
{
	json body       = {};
	body["for"]     = "text";
	body["pattern"] = std::string(257, 'a');
	EXPECT_THROW(ParseWaitRequest(body, 0, ""), std::invalid_argument);
}

TEST(ParseWaitRequestTest, TextHonoursIgnoreCase)
{
	json body           = {};
	body["for"]         = "text";
	body["pattern"]     = "ok";
	body["ignore_case"] = true;

	const auto spec = ParseWaitRequest(body, 0, "");
	EXPECT_TRUE(spec.ignore_case);
}

TEST(ParseWaitRequestTest, ScreenChangeRequiresBaselineHash)
{
	json missing   = {};
	missing["for"] = "screen_change";
	EXPECT_THROW(ParseWaitRequest(missing, 0, ""), std::invalid_argument);

	json ok             = {};
	ok["for"]           = "screen_change";
	ok["baseline_hash"] = "00000000000000ff";
	const auto spec     = ParseWaitRequest(ok, 0, "");
	EXPECT_EQ(spec.baseline_hash, 0xffULL);
	EXPECT_EQ(spec.hash_source, HashSource::Text);
}

TEST(ParseWaitRequestTest, ScreenChangeParsesFrameSource)
{
	json body             = {};
	body["for"]           = "screen_change";
	body["baseline_hash"] = "0000000000000000";
	body["source"]        = "frame";

	const auto spec = ParseWaitRequest(body, 0, "");
	EXPECT_EQ(spec.hash_source, HashSource::Frame);
}

TEST(ParseWaitRequestTest, ScreenChangeRejectsUnknownSource)
{
	json body             = {};
	body["for"]           = "screen_change";
	body["baseline_hash"] = "0000000000000000";
	body["source"]        = "bogus";

	EXPECT_THROW(ParseWaitRequest(body, 0, ""), std::invalid_argument);
}

TEST(ParseWaitRequestTest, FramesResolvesARelativeCountToAnAbsoluteTarget)
{
	json body     = {};
	body["for"]   = "frames";
	body["count"] = 5;

	const auto spec = ParseWaitRequest(body, 1000, "");
	EXPECT_EQ(spec.target_frame, 1005u);
}

TEST(ParseWaitRequestTest, FramesRejectsOutOfRangeCount)
{
	json too_low     = {};
	too_low["for"]   = "frames";
	too_low["count"] = 0;
	EXPECT_THROW(ParseWaitRequest(too_low, 0, ""), std::invalid_argument);

	json too_high     = {};
	too_high["for"]   = "frames";
	too_high["count"] = 100001;
	EXPECT_THROW(ParseWaitRequest(too_high, 0, ""), std::invalid_argument);
}

TEST(ParseWaitRequestTest, MemoryParsesAddrWidthValueAndOp)
{
	json body     = {};
	body["for"]   = "memory";
	body["addr"]  = 0x1000;
	body["width"] = 2;
	body["value"] = 42;
	body["op"]    = "ge";

	const auto spec = ParseWaitRequest(body, 0, "");
	EXPECT_EQ(spec.mem_addr, 0x1000u);
	EXPECT_EQ(spec.mem_width, 2);
	EXPECT_EQ(spec.mem_value, 42u);
	EXPECT_EQ(spec.mem_op, MemoryCompareOp::Ge);
}

TEST(ParseWaitRequestTest, MemoryDefaultsWidthAndOp)
{
	json body     = {};
	body["for"]   = "memory";
	body["addr"]  = 0;
	body["value"] = 0;

	const auto spec = ParseWaitRequest(body, 0, "");
	EXPECT_EQ(spec.mem_width, 1);
	EXPECT_EQ(spec.mem_op, MemoryCompareOp::Eq);
}

TEST(ParseWaitRequestTest, MemoryRejectsBadWidth)
{
	json body     = {};
	body["for"]   = "memory";
	body["addr"]  = 0;
	body["width"] = 3;
	body["value"] = 0;

	EXPECT_THROW(ParseWaitRequest(body, 0, ""), std::invalid_argument);
}

TEST(ParseWaitRequestTest, MemoryRejectsAddrPastUint32)
{
	json body     = {};
	body["for"]   = "memory";
	body["addr"]  = 0x100000000ULL;
	body["value"] = 0;

	EXPECT_THROW(ParseWaitRequest(body, 0, ""), std::invalid_argument);
}

TEST(ParseWaitRequestTest, MemoryRejectsUnknownOp)
{
	json body     = {};
	body["for"]   = "memory";
	body["addr"]  = 0;
	body["value"] = 0;
	body["op"]    = "bogus";

	EXPECT_THROW(ParseWaitRequest(body, 0, ""), std::invalid_argument);
}

TEST(ParseWaitRequestTest, ProgramWithPatternDoesNotCaptureABaseline)
{
	json body       = {};
	body["for"]     = "program";
	body["pattern"] = "DOOM";

	const auto spec = ParseWaitRequest(body, 0, "COMMAND");
	EXPECT_EQ(spec.pattern, "DOOM");
	EXPECT_FALSE(spec.has_baseline_program);
}

TEST(ParseWaitRequestTest, ProgramWithoutPatternCapturesTheCurrentProgram)
{
	json body   = {};
	body["for"] = "program";

	const auto spec = ParseWaitRequest(body, 0, "COMMAND");
	EXPECT_TRUE(spec.has_baseline_program);
	EXPECT_EQ(spec.baseline_program, "COMMAND");
}

TEST(ParseWaitRequestTest, ConditionsWithNoExtraParamsParseFromForAlone)
{
	json replay_done   = {};
	replay_done["for"] = "replay_done";
	EXPECT_NO_THROW(ParseWaitRequest(replay_done, 0, ""));

	json stopped   = {};
	stopped["for"] = "stopped";
	EXPECT_NO_THROW(ParseWaitRequest(stopped, 0, ""));

	json script_done   = {};
	script_done["for"] = "script_done";
	EXPECT_NO_THROW(ParseWaitRequest(script_done, 0, ""));
}

// ---- WaitRegistry mechanics ------------------------------------------------
//
// WaitRegistry::Instance() is a process-wide singleton shared by every test
// below (and, in the real binary, by the frame hook / DEBUG_Loop / SDL pause
// loops). Every test registers, waits for its own waiter(s) to be resolved
// or time out, and joins its threads before returning, so no state leaks
// between tests. Tick() reads real emulator-core globals (CurMode, memory,
// the render/titlebar/script-manager singletons) that are never booted in
// this unit-test binary - tests below only exercise condition/observation
// paths that are well-defined without a running DOS session: the
// frame-hook-only "unsatisfiable while stopped" short-circuit (never
// touches emulator state), replay_done (false by construction, nothing
// ever starts a replay here), screen_change on the frame hash (a plain
// atomic, always 0 by default), and memory's own range check with an
// address that overflows any plausible emulated memory size.

TEST(WaitRegistryTest, TimesOutWithoutAnyTickCalls)
{
	json body          = {};
	body["for"]        = "frames";
	body["count"]      = 1;
	body["timeout_ms"] = 30;

	const auto spec    = ParseWaitRequest(body, 0, "");
	const auto outcome = WaitRegistry::Instance().WaitFor(spec);

	EXPECT_FALSE(outcome.satisfied);
	EXPECT_EQ(outcome.reason, "timeout");
}

TEST(WaitRegistryTest, EmulatorStoppedResolvesFrameHookOnlyConditionsFast)
{
	auto& registry = WaitRegistry::Instance();

	json body          = {};
	body["for"]        = "frames";
	body["count"]      = 1;
	body["timeout_ms"] = 5000;
	const auto spec    = ParseWaitRequest(body, 0, "");

	Webserver::WaitOutcome outcome;
	std::thread waiter([&] { outcome = registry.WaitFor(spec); });

	// Give the waiter a moment to register before ticking as "stopped".
	std::this_thread::sleep_for(20ms);
	registry.Tick(/*frames_flowing=*/false);
	waiter.join();

	EXPECT_FALSE(outcome.satisfied);
	EXPECT_EQ(outcome.reason, "emulator_stopped");
}

TEST(WaitRegistryTest, ReplayDoneSatisfiedImmediatelyWhenNothingIsReplaying)
{
	auto& registry = WaitRegistry::Instance();

	json body          = {};
	body["for"]        = "replay_done";
	body["timeout_ms"] = 5000;
	const auto spec    = ParseWaitRequest(body, 0, "");

	Webserver::WaitOutcome outcome;
	std::thread waiter([&] { outcome = registry.WaitFor(spec); });

	std::this_thread::sleep_for(20ms);
	registry.Tick(/*frames_flowing=*/true);
	waiter.join();

	EXPECT_TRUE(outcome.satisfied);
	EXPECT_EQ(outcome.reason, "matched");
}

TEST(WaitRegistryTest, ScreenChangeFrameSourceMatchesWhenHashDiffersFromBaseline)
{
	auto& registry = WaitRegistry::Instance();

	// The shared frame hash is a plain atomic, 0 until a real frame is
	// rendered - never true in this binary, so any non-zero baseline
	// already differs from "current".
	json body             = {};
	body["for"]           = "screen_change";
	body["baseline_hash"] = "00000000000000ff";
	body["source"]        = "frame";
	body["timeout_ms"]    = 5000;
	const auto spec       = ParseWaitRequest(body, 0, "");

	Webserver::WaitOutcome outcome;
	std::thread waiter([&] { outcome = registry.WaitFor(spec); });

	std::this_thread::sleep_for(20ms);
	registry.Tick(/*frames_flowing=*/true);
	waiter.join();

	EXPECT_TRUE(outcome.satisfied);
	EXPECT_EQ(outcome.reason, "matched");
	ASSERT_TRUE(outcome.observation.contains("hash"));
}

TEST(WaitRegistryTest, MemoryOutOfRangeAddressReturnsAnErrorReasonNotAHang)
{
	auto& registry = WaitRegistry::Instance();

	// No plausible emulated memory size covers this address - true
	// whether or not MEM_Init() ever ran in this binary.
	json body          = {};
	body["for"]        = "memory";
	body["addr"]       = 0xfffffff0;
	body["width"]      = 4;
	body["value"]      = 0;
	body["timeout_ms"] = 5000;
	const auto spec    = ParseWaitRequest(body, 0, "");

	Webserver::WaitOutcome outcome;
	std::thread waiter([&] { outcome = registry.WaitFor(spec); });

	std::this_thread::sleep_for(20ms);
	registry.Tick(/*frames_flowing=*/true);
	waiter.join();

	EXPECT_FALSE(outcome.satisfied);
	EXPECT_EQ(outcome.reason, "error");
}

TEST(WaitRegistryTest, FifthConcurrentWaiterIsRefusedWithoutBlocking)
{
	auto& registry = WaitRegistry::Instance();

	json body          = {};
	body["for"]        = "replay_done";
	body["timeout_ms"] = 5000;
	const auto spec    = ParseWaitRequest(body, 0, "");

	std::vector<std::thread> blockers;
	std::vector<Webserver::WaitOutcome> outcomes(MaxWaiters);
	for (size_t i = 0; i < MaxWaiters; ++i) {
		blockers.emplace_back(
		        [&, i] { outcomes[i] = registry.WaitFor(spec); });
	}

	// Give all MaxWaiters callers a chance to register before the
	// overflow attempt - registration only takes a lock, no Tick
	// needed, so a short sleep is enough in practice.
	std::this_thread::sleep_for(50ms);

	EXPECT_THROW(registry.WaitFor(spec), TooManyWaiters);

	// Let the MaxWaiters legitimate waiters resolve (matched, since
	// nothing is replaying) rather than leaving them to time out.
	registry.Tick(/*frames_flowing=*/true);
	for (auto& t : blockers) {
		t.join();
	}
	for (const auto& outcome : outcomes) {
		EXPECT_TRUE(outcome.satisfied);
	}
}

TEST(WaitRegistryTest, DrainAllWakesPendingWaitersWithAShuttingDownReason)
{
	auto& registry = WaitRegistry::Instance();

	json body          = {};
	body["for"]        = "frames";
	body["count"]      = 1;
	body["timeout_ms"] = 5000;
	const auto spec    = ParseWaitRequest(body, 0, "");

	Webserver::WaitOutcome outcome;
	std::thread waiter([&] { outcome = registry.WaitFor(spec); });

	std::this_thread::sleep_for(20ms);
	registry.DrainAll();
	waiter.join();

	EXPECT_FALSE(outcome.satisfied);
	EXPECT_EQ(outcome.reason, "shutting_down");
}

} // namespace
