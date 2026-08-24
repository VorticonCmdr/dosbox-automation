// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "lua/lua_bridge_commands.h"
#include "lua/lua_debug_log.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

fs::path MakeTempDir()
{
	std::random_device rd = {};
	auto dist             = std::uniform_int_distribution<uint64_t>();
	for (int attempt = 0; attempt < 16; ++attempt) {
		const auto name = "lua_script_log_" + std::to_string(dist(rd));
		const auto candidate = fs::temp_directory_path() / name;
		std::error_code ec   = {};
		if (fs::create_directory(candidate, ec) && !ec) {
			return candidate;
		}
	}
	return {};
}

// -- DebugLog --

class DebugLogTest : public testing::Test {
protected:
	fs::path tmp_dir = {};
	Lua::DebugLog log;

	void SetUp() override
	{
		tmp_dir = MakeTempDir();
		ASSERT_FALSE(tmp_dir.empty());
	}

	void TearDown() override
	{
		if (!tmp_dir.empty() && fs::exists(tmp_dir)) {
			fs::remove_all(tmp_dir);
		}
	}
};

TEST_F(DebugLogTest, OpenReportsOpenAndAPath)
{
	ASSERT_TRUE(log.Open(tmp_dir.string(), "myscript"));
	EXPECT_TRUE(log.IsOpen());
	EXPECT_FALSE(log.FilePath().empty());
	EXPECT_NE(log.FilePath().find("myscript"), std::string::npos);
}

TEST_F(DebugLogTest, CloseStopsWritingButKeepsReportingTheSamePath)
{
	ASSERT_TRUE(log.Open(tmp_dir.string(), "myscript"));
	const auto path = log.FilePath();

	log.Close();

	EXPECT_FALSE(log.IsOpen());
	EXPECT_EQ(log.FilePath(), path);
}

TEST_F(DebugLogTest, OpenAfterCloseReplacesThePathWithAFreshOne)
{
	ASSERT_TRUE(log.Open(tmp_dir.string(), "first"));
	const auto first_path = log.FilePath();
	log.Close();

	ASSERT_TRUE(log.Open(tmp_dir.string(), "second"));
	EXPECT_NE(log.FilePath(), first_path);
	EXPECT_NE(log.FilePath().find("second"), std::string::npos);
}

TEST_F(DebugLogTest, NeverOpenedReportsNoPath)
{
	EXPECT_FALSE(log.IsOpen());
	EXPECT_TRUE(log.FilePath().empty());
}

// -- LuaLogCommand::Execute --

class LuaLogCommandTest : public testing::Test {
protected:
	fs::path tmp_dir = {};

	void SetUp() override
	{
		tmp_dir = MakeTempDir();
		ASSERT_FALSE(tmp_dir.empty());
	}

	void TearDown() override
	{
		// ScriptManager::Instance() is a process-wide singleton -
		// leaving it pointed at a directory this test is about to
		// delete would break whichever test runs next and touches it.
		auto& mgr = Lua::ScriptManager::Instance();
		mgr.Log().Close();
		mgr.Params() = Lua::ScriptParams{};
		mgr.Engine().Reset();

		if (!tmp_dir.empty() && fs::exists(tmp_dir)) {
			fs::remove_all(tmp_dir);
		}
	}

	// Real, successful compile - the gate this item's fix added
	// (mgr.Engine().HasLoadedScript()) needs a genuinely loaded script,
	// not just Params()/Log() poked directly. Never touches
	// get_config_dir() (unlike a debug=true LuaLoadCommand::Execute),
	// so it's safe to call outside a full engine/webserver bring-up.
	void LoadValidScript(const std::string& name)
	{
		auto& mgr = Lua::ScriptManager::Instance();
		const auto r = mgr.Engine().LoadScript("dosbox.output.ok = 1", name);
		ASSERT_TRUE(r.ok) << r.error;
	}
};

TEST_F(LuaLogCommandTest, ReportsNoLogWhenNoScriptIsLoaded)
{
	Lua::LuaLogCommand cmd;
	cmd.Execute();
	EXPECT_TRUE(cmd.log_path.empty());
	EXPECT_FALSE(cmd.debug_script_loaded);
}

TEST_F(LuaLogCommandTest, ReportsNoLogWhenTheLoadedScriptIsNotADebugScript)
{
	LoadValidScript("test");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = false;

	Lua::LuaLogCommand cmd;
	cmd.Execute();
	EXPECT_TRUE(cmd.log_path.empty());
}

TEST_F(LuaLogCommandTest, ReportsThePathWhileTheDebugLogIsOpen)
{
	LoadValidScript("test");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "test"));

	Lua::LuaLogCommand cmd;
	cmd.Execute();
	EXPECT_EQ(cmd.log_path, mgr.Log().FilePath());
	EXPECT_FALSE(cmd.log_path.empty());
	EXPECT_TRUE(cmd.debug_script_loaded);
}

TEST_F(LuaLogCommandTest, StillReportsThePathAfterTheScriptFinishesAndCloses)
{
	// Reproduces the exact scenario this item exists for: an agent
	// polling script/status after a debug script completes or errors,
	// then wanting to read the log through script/log.
	LoadValidScript("test");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "test"));
	const auto path = mgr.Log().FilePath();

	mgr.Log().Close();

	Lua::LuaLogCommand cmd;
	cmd.Execute();
	EXPECT_EQ(cmd.log_path, path);
}

TEST_F(LuaLogCommandTest, StopsReportingThePathOnceANonDebugScriptReplacesIt)
{
	LoadValidScript("old");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "old"));
	mgr.Log().Close();

	// A fresh load with debug=false: LuaLoadCommand::Execute does this
	// exact sequence (params.debug false -> mgr.Log().Close(), no Open).
	LoadValidScript("new");
	mgr.Params().debug = false;
	mgr.Log().Close();

	Lua::LuaLogCommand cmd;
	cmd.Execute();
	EXPECT_TRUE(cmd.log_path.empty());
}

TEST_F(LuaLogCommandTest, DropsThePathWhenAReloadResetsTheEngineWithoutLoadingAnything)
{
	// mgr.Engine().Reset() is what LuaLoadCommand::Execute calls before
	// every load attempt, success or failure - this isolates that half
	// of the real bug (see the next test for the full reload-failure
	// reproduction via the real LuaLoadCommand).
	LoadValidScript("old");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "old"));
	mgr.Log().Close();
	ASSERT_TRUE(mgr.Engine().HasLoadedScript());

	mgr.Engine().Reset();
	ASSERT_FALSE(mgr.Engine().HasLoadedScript());

	Lua::LuaLogCommand cmd;
	cmd.Execute();
	EXPECT_TRUE(cmd.log_path.empty());
}

TEST_F(LuaLogCommandTest, FailedDebugReloadDoesNotServeThePreviousRunsStaleLog)
{
	// Regression: LuaLoadCommand::Execute used to commit mgr.Params()
	// (and reset the engine) before LoadScript was known to succeed. A
	// reload that failed to compile left mgr.Params().debug true with
	// mgr.Log() still pointing at a completely different, previously
	// completed script's log - script/log would keep serving that
	// stale content as if it belonged to the failed reload.
	LoadValidScript("first");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "first"));
	mgr.Log().Close();

	// A failing reload never reaches the debug-log-open code (it
	// returns right after LoadScript fails), so this is safe to call
	// directly - see ReadLogTailTest/DebugLogTest's own comments on why
	// a *successful* debug=true LuaLoadCommand::Execute is not.
	Lua::LuaLoadCommand cmd("this is not valid lua (((",
	                        Lua::ScriptParams{.name = "second", .debug = true});
	cmd.Execute();
	ASSERT_FALSE(cmd.error.empty());

	Lua::LuaLogCommand logCmd;
	logCmd.Execute();
	EXPECT_TRUE(logCmd.log_path.empty());
	EXPECT_FALSE(logCmd.debug_script_loaded);
}

// -- LuaStatusCommand::Execute (log_path gate only) --

class LuaStatusCommandLogPathTest : public testing::Test {
protected:
	fs::path tmp_dir = {};

	void SetUp() override
	{
		tmp_dir = MakeTempDir();
		ASSERT_FALSE(tmp_dir.empty());
	}

	void TearDown() override
	{
		auto& mgr = Lua::ScriptManager::Instance();
		mgr.Log().Close();
		mgr.Params() = Lua::ScriptParams{};
		mgr.Engine().Reset();

		if (!tmp_dir.empty() && fs::exists(tmp_dir)) {
			fs::remove_all(tmp_dir);
		}
	}

	void LoadValidScript(const std::string& name)
	{
		auto& mgr = Lua::ScriptManager::Instance();
		const auto r = mgr.Engine().LoadScript("dosbox.output.ok = 1", name);
		ASSERT_TRUE(r.ok) << r.error;
	}
};

TEST_F(LuaStatusCommandLogPathTest, OmitsLogPathWhenNoScriptIsLoaded)
{
	Lua::LuaStatusCommand cmd;
	cmd.Execute();
	EXPECT_TRUE(cmd.result.log_path.empty());
}

TEST_F(LuaStatusCommandLogPathTest, ReportsLogPathWhileTheDebugLogIsOpen)
{
	LoadValidScript("test");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "test"));

	Lua::LuaStatusCommand cmd;
	cmd.Execute();
	EXPECT_EQ(cmd.result.log_path, mgr.Log().FilePath());
}

TEST_F(LuaStatusCommandLogPathTest, StillReportsLogPathAfterTheScriptFinishesAndCloses)
{
	LoadValidScript("test");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "test"));
	const auto path = mgr.Log().FilePath();
	mgr.Log().Close();

	Lua::LuaStatusCommand cmd;
	cmd.Execute();
	EXPECT_EQ(cmd.result.log_path, path);
}

TEST_F(LuaStatusCommandLogPathTest,
       AgreesWithLuaLogCommandOnAvailabilityAfterAFailedReload)
{
	// The specific property an adversarial review flagged as untested:
	// script/status and script/log must never disagree about whether a
	// log is currently available.
	LoadValidScript("first");
	auto& mgr          = Lua::ScriptManager::Instance();
	mgr.Params().debug = true;
	ASSERT_TRUE(mgr.Log().Open(tmp_dir.string(), "first"));
	mgr.Log().Close();

	Lua::LuaLoadCommand loadCmd("this is not valid lua (((",
	                            Lua::ScriptParams{.name  = "second",
	                                              .debug = true});
	loadCmd.Execute();
	ASSERT_FALSE(loadCmd.error.empty());

	Lua::LuaStatusCommand statusCmd;
	statusCmd.Execute();
	Lua::LuaLogCommand logCmd;
	logCmd.Execute();

	EXPECT_EQ(statusCmd.result.log_path.empty(), logCmd.log_path.empty());
	EXPECT_TRUE(statusCmd.result.log_path.empty());
}

// -- ReadLogTail --

class ReadLogTailTest : public testing::Test {
protected:
	fs::path tmp_dir = {};

	void SetUp() override
	{
		tmp_dir = MakeTempDir();
		ASSERT_FALSE(tmp_dir.empty());
	}

	void TearDown() override
	{
		if (!tmp_dir.empty() && fs::exists(tmp_dir)) {
			fs::remove_all(tmp_dir);
		}
	}

	fs::path WriteFile(const std::string& name, const std::string& content)
	{
		const auto path = tmp_dir / name;
		std::ofstream f(path, std::ios::binary);
		f << content;
		return path;
	}
};

TEST_F(ReadLogTailTest, NonexistentFileReturnsNullopt)
{
	bool truncated    = false;
	const auto result = Lua::ReadLogTail((tmp_dir / "missing.log").string(),
	                                     truncated);
	EXPECT_FALSE(result.has_value());
}

TEST_F(ReadLogTailTest, SmallFileReadsInFullAndReportsNotTruncated)
{
	const auto path = WriteFile("small.log", "hello debug log\n");

	bool truncated    = false;
	const auto result = Lua::ReadLogTail(path.string(), truncated);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, "hello debug log\n");
	EXPECT_FALSE(truncated);
}

TEST_F(ReadLogTailTest, EmptyFileReadsAsEmptyAndNotTruncated)
{
	const auto path = WriteFile("empty.log", "");

	bool truncated    = false;
	const auto result = Lua::ReadLogTail(path.string(), truncated);
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->empty());
	EXPECT_FALSE(truncated);
}

TEST_F(ReadLogTailTest, FileExactlyAtTheCapIsNotTruncated)
{
	const std::string content(Lua::MaxLogTailBytes, 'x');
	const auto path = WriteFile("exact.log", content);

	bool truncated    = false;
	const auto result = Lua::ReadLogTail(path.string(), truncated);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), Lua::MaxLogTailBytes);
	EXPECT_FALSE(truncated);
}

TEST_F(ReadLogTailTest, OversizedFileKeepsOnlyTheTailAndReportsTruncated)
{
	// Distinct prefix/suffix bytes so a boundary-arithmetic error (off
	// by one on the seek offset) shows up as wrong content, not just a
	// wrong length.
	const std::string prefix(1000, 'a');
	const std::string tail(Lua::MaxLogTailBytes, 'b');
	const auto path = WriteFile("oversized.log", prefix + tail);

	bool truncated    = false;
	const auto result = Lua::ReadLogTail(path.string(), truncated);
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(truncated);
	EXPECT_EQ(result->size(), Lua::MaxLogTailBytes);
	EXPECT_EQ(*result, tail);
}

} // namespace
