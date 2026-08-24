// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/capabilities.h"

#include "webserver/bridge.h"
#include "webserver/input.h"
#include "webserver/private/batch.h"
#include "webserver/private/dos.h"
#include "webserver/private/freeze.h"
#include "webserver/private/memory.h"
#include "webserver/wait.h"
#include "webserver/webserver.h"

#include "lua/lua_bridge_commands.h"
#include "lua/script_validator.h"

#include <gtest/gtest.h>

#include "libs/json/json.h"

using Webserver::AllocationRegistry;
using Webserver::BatchBaseTimeoutMs;
using Webserver::BatchMaxTimeoutMs;
using Webserver::BatchPerOpTimeoutMs;
using Webserver::BuildCapabilitiesBlock;
using Webserver::BuildServerLimits;
using Webserver::CapabilityState;
using Webserver::CapabilityStateName;
using Webserver::ComputeDebuggerCapability;
using Webserver::DefaultBridgeTimeoutMs;
using Webserver::FeaturesProjection;
using Webserver::FreezeRegistry;
using Webserver::MaxBatchOps;
using Webserver::MaxBatchReadBytes;
using Webserver::MaxBatchWriteBytes;
using Webserver::MaxInputEvents;
using Webserver::MaxMemoryTransferBytes;
using Webserver::MaxMouseCoordinate;
using Webserver::MaxPatternLen;
using Webserver::MaxRequestBodyBytes;
using Webserver::MaxSearchSpanBytes;
using Webserver::MaxTypedTextChars;
using Webserver::MaxWaiters;
using Webserver::MaxWaitTimeoutMs;

namespace {

TEST(CapabilityStateNameTest, MapsAllThreeStates)
{
	EXPECT_EQ(CapabilityStateName(CapabilityState::On), "on");
	EXPECT_EQ(CapabilityStateName(CapabilityState::Off), "off");
	EXPECT_EQ(CapabilityStateName(CapabilityState::Degraded), "degraded");
}

TEST(ComputeDebuggerCapabilityTest, NotBuiltIsOffRegardlessOfHeavyOrCore)
{
	const auto info = ComputeDebuggerCapability(false, true, CoreKind::Dynamic);
	EXPECT_EQ(info.state, CapabilityState::Off);
	EXPECT_NE(info.reason.find("not built"), std::string::npos);
	EXPECT_EQ(info.limits["built"], false);
}

TEST(ComputeDebuggerCapabilityTest, BuiltWithHeavyIsFullyOn)
{
	const auto info = ComputeDebuggerCapability(true, true, CoreKind::Normal);
	EXPECT_EQ(info.state, CapabilityState::On);
	EXPECT_NE(info.reason.find("heavy"), std::string::npos);
	EXPECT_EQ(info.limits["heavy_debugger_built"], true);
}

TEST(ComputeDebuggerCapabilityTest, LightOnlyOnNormalCoreIsDegradedNotOff)
{
	const auto info = ComputeDebuggerCapability(true, false, CoreKind::Normal);
	EXPECT_EQ(info.state, CapabilityState::Degraded);
	EXPECT_NE(info.reason.find("memory breakpoints unavailable"),
	          std::string::npos);
	EXPECT_NE(info.reason.find("normal"), std::string::npos);
	EXPECT_EQ(info.limits["effective_core"], "normal");
}

TEST(ComputeDebuggerCapabilityTest, LightOnlyOnSimpleCoreNamesSimpleCore)
{
	const auto info = ComputeDebuggerCapability(true, false, CoreKind::Simple);
	EXPECT_EQ(info.state, CapabilityState::Degraded);
	EXPECT_NE(info.reason.find("simple"), std::string::npos);
	EXPECT_EQ(info.limits["effective_core"], "simple");
}

TEST(ComputeDebuggerCapabilityTest, LightOnlyOnDynamicCoreExplainsTheFallback)
{
	const auto info = ComputeDebuggerCapability(true, false, CoreKind::Dynamic);
	EXPECT_EQ(info.state, CapabilityState::Degraded);
	EXPECT_NE(info.reason.find("interpreter fallback"), std::string::npos);
	EXPECT_EQ(info.limits["effective_core"], "dynamic");
}

TEST(ComputeDebuggerCapabilityTest, FullCoreIsNamedDistinctlyFromNormal)
{
	const auto info = ComputeDebuggerCapability(true, false, CoreKind::Full);
	EXPECT_EQ(info.limits["effective_core"], "full");
}

TEST(BuildCapabilitiesBlockTest, CoversEveryServedGroup)
{
	const auto capabilities = BuildCapabilitiesBlock();
	for (const std::string group : {"memory",
	                                "input",
	                                "cpu_registers",
	                                "cpu_control",
	                                "port_io",
	                                "freeze",
	                                "debugger",
	                                "script",
	                                "drive",
	                                "capture",
	                                "wait",
	                                "batch"}) {
		ASSERT_TRUE(capabilities.contains(group)) << group;
		EXPECT_TRUE(capabilities[group].contains("state")) << group;
		EXPECT_TRUE(capabilities[group].contains("reason")) << group;
		EXPECT_TRUE(capabilities[group]["limits"].is_object()) << group;
	}
}

TEST(BuildCapabilitiesBlockTest, LimitsMatchTheSameNamedConstantsTheValidatorsUse)
{
	const auto capabilities = BuildCapabilitiesBlock();

	EXPECT_EQ(capabilities["memory"]["limits"]["max_transfer_bytes"],
	          MaxMemoryTransferBytes);
	EXPECT_EQ(capabilities["memory"]["limits"]["max_search_span_bytes"],
	          MaxSearchSpanBytes);
	EXPECT_EQ(capabilities["memory"]["limits"]["max_dos_allocations"],
	          AllocationRegistry::MaxEntries);

	EXPECT_EQ(capabilities["input"]["limits"]["max_events"], MaxInputEvents);
	EXPECT_EQ(capabilities["input"]["limits"]["max_typed_text_chars"],
	          MaxTypedTextChars);
	EXPECT_EQ(capabilities["input"]["limits"]["max_mouse_coordinate"],
	          MaxMouseCoordinate);

	EXPECT_EQ(capabilities["freeze"]["limits"]["max_entries"],
	          FreezeRegistry::MaxEntries);

	EXPECT_EQ(capabilities["script"]["limits"]["max_body_bytes"],
	          Lua::ScriptValidator::MaxBodySize);
	EXPECT_EQ(capabilities["script"]["limits"]["max_log_tail_bytes"],
	          Lua::MaxLogTailBytes);
	EXPECT_EQ(capabilities["script"]["limits"]["max_output_nodes"],
	          Lua::MaxOutputNodes);
	EXPECT_EQ(capabilities["script"]["limits"]["max_output_bytes"],
	          Lua::MaxOutputBytes);

	EXPECT_EQ(capabilities["wait"]["limits"]["max_timeout_ms"], MaxWaitTimeoutMs);
	EXPECT_EQ(capabilities["wait"]["limits"]["max_pattern_len"], MaxPatternLen);
	EXPECT_EQ(capabilities["wait"]["limits"]["max_waiters"], MaxWaiters);

	EXPECT_EQ(capabilities["batch"]["limits"]["max_ops"], MaxBatchOps);
	EXPECT_EQ(capabilities["batch"]["limits"]["max_read_bytes"],
	          MaxBatchReadBytes);
	EXPECT_EQ(capabilities["batch"]["limits"]["max_write_bytes"],
	          MaxBatchWriteBytes);
	EXPECT_EQ(capabilities["batch"]["limits"]["base_timeout_ms"],
	          BatchBaseTimeoutMs);
	EXPECT_EQ(capabilities["batch"]["limits"]["per_op_timeout_ms"],
	          BatchPerOpTimeoutMs);
	EXPECT_EQ(capabilities["batch"]["limits"]["max_timeout_ms"],
	          BatchMaxTimeoutMs);
}

TEST(BuildCapabilitiesBlockTest, DebuggerStateReflectsThisBuildsMacros)
{
	const auto capabilities = BuildCapabilitiesBlock();
	const std::string state = capabilities["debugger"]["state"];
	if constexpr (!static_cast<bool>(C_DEBUGGER)) {
		EXPECT_EQ(state, "off");
	} else if constexpr (static_cast<bool>(C_HEAVY_DEBUGGER)) {
		EXPECT_EQ(state, "on");
	} else {
		EXPECT_EQ(state, "degraded");
	}
}

TEST(FeaturesProjectionTest, OnAndDegradedProjectTrueOffProjectsFalse)
{
	nlohmann::json capabilities;
	capabilities["a"]["state"] = "on";
	capabilities["b"]["state"] = "degraded";
	capabilities["c"]["state"] = "off";

	const auto features = FeaturesProjection(capabilities);
	EXPECT_EQ(features["a"], true);
	EXPECT_EQ(features["b"], true);
	EXPECT_EQ(features["c"], false);
}

TEST(FeaturesProjectionTest, MatchesEveryKeyBuildCapabilitiesBlockProduces)
{
	const auto capabilities = BuildCapabilitiesBlock();
	const auto features     = FeaturesProjection(capabilities);
	for (const auto& [group, info] : capabilities.items()) {
		ASSERT_TRUE(features.contains(group)) << group;
		EXPECT_EQ(features[group].get<bool>(), info["state"] != "off")
		        << group;
	}
}

TEST(BuildServerLimitsTest, MatchesTheEnforcedConstants)
{
	const auto limits = BuildServerLimits();
	EXPECT_EQ(limits["max_request_body_bytes"], MaxRequestBodyBytes);
	EXPECT_EQ(limits["bridge_default_timeout_ms"], DefaultBridgeTimeoutMs);
}

} // namespace
