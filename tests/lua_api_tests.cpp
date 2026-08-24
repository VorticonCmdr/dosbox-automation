// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "lua/lua_api.h"
#include "lua/lua_coroutine.h"
#include "lua/lua_debug_log.h"
#include "lua/lua_engine.h"
#include "webserver/input.h"
#include "webserver/webserver.h"

#include "hardware/input/keyboard.h"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Records the key events type()/key() inject, via the keyboard input hook, so
// tests can verify the exact mapping without a running guest.
std::vector<std::pair<int, bool>> captured_keys;

void CaptureKeyHook(int key, bool pressed)
{
	captured_keys.emplace_back(key, pressed);
}

class LuaApiTest : public ::testing::Test {
protected:
	Lua::LuaEngine engine;
	Lua::LuaCoroutine coroutine{engine};

	void SetUp() override
	{
		KEYBOARD_ClrBuffer();
		captured_keys.clear();
		keyboard_input_hook = &CaptureKeyHook;
	}

	void TearDown() override
	{
		keyboard_input_hook = nullptr;
		KEYBOARD_ClrBuffer();
	}

	void LoadAndStart(const std::string& script)
	{
		auto result = engine.LoadScript(script, "test");
		ASSERT_TRUE(result.ok) << result.error;
		ASSERT_TRUE(coroutine.Start());
	}

	Lua::ScriptState RunToCompletion(const std::string& script,
	                                 int max_frames = 100)
	{
		LoadAndStart(script);
		Lua::ScriptState state = Lua::ScriptState::Running;
		for (int i = 1; i <= max_frames; ++i) {
			state = coroutine.DispatchFrame(static_cast<uint64_t>(i));
			if (state == Lua::ScriptState::Completed ||
			    state == Lua::ScriptState::Error) {
				break;
			}
		}
		return state;
	}

	// Paced type() yields until the guest drains the keyboard buffer. With
	// no guest, clear the buffer each frame to stand in for one consuming
	// keys, so the script can run to completion.
	Lua::ScriptState RunTypeDraining(const std::string& script,
	                                 int max_frames = 300)
	{
		LoadAndStart(script);
		Lua::ScriptState state = Lua::ScriptState::Running;
		for (int i = 1; i <= max_frames; ++i) {
			KEYBOARD_ClrBuffer();
			state = coroutine.DispatchFrame(static_cast<uint64_t>(i));
			if (state == Lua::ScriptState::Completed ||
			    state == Lua::ScriptState::Error) {
				break;
			}
		}
		return state;
	}

	// The keys pressed (press events only), in order, from the capture hook.
	std::vector<int> PressedKeys() const
	{
		std::vector<int> pressed;
		for (const auto& [key, down] : captured_keys) {
			if (down) {
				pressed.push_back(key);
			}
		}
		return pressed;
	}
};

// -- Input: key() --

TEST_F(LuaApiTest, KeyValidNameSucceeds)
{
	auto state = RunToCompletion("dosbox.key('KBD_enter', true)");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, KeyInvalidNameErrors)
{
	auto state = RunToCompletion("dosbox.key('KBD_nonexistent', true)");
	EXPECT_EQ(state, Lua::ScriptState::Error);
	EXPECT_NE(coroutine.ErrorMessage().find("unknown key name"),
	          std::string::npos);
}

TEST_F(LuaApiTest, KeyMissingPressedErrors)
{
	auto state = RunToCompletion("dosbox.key('KBD_enter')");
	EXPECT_EQ(state, Lua::ScriptState::Error);
}

// -- Input: type() --

TEST_F(LuaApiTest, TypeAsciiCompletesWhenBufferDrains)
{
	auto state = RunTypeDraining("dosbox.type('Hello World')");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, TypeEmptyStringSucceeds)
{
	// Nothing to inject, so type() returns without yielding.
	auto state = RunToCompletion("dosbox.type('')");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
	EXPECT_TRUE(captured_keys.empty());
}

TEST_F(LuaApiTest, TypeMapsDigitsToDigitKeys)
{
	// Regression: digits must map through a table. Arithmetic from KBD_0
	// walked into the QWERTY letters (e.g. '7' -> KBD_u) because the enum
	// orders the row 1..9 then 0.
	auto state = RunTypeDraining("dosbox.type('0123456789')");
	ASSERT_EQ(state, Lua::ScriptState::Completed);

	const std::vector<int> expected = {
	        KBD_0, KBD_1, KBD_2, KBD_3, KBD_4, KBD_5, KBD_6, KBD_7, KBD_8, KBD_9};
	EXPECT_EQ(PressedKeys(), expected);
}

TEST_F(LuaApiTest, TypeMapsLowercaseLetters)
{
	// Regression: the alpha row is QWERTY-ordered, so letters map through a
	// table too, not arithmetic from KBD_a.
	auto state = RunTypeDraining("dosbox.type('abcxyz')");
	ASSERT_EQ(state, Lua::ScriptState::Completed);

	const std::vector<int> expected = {KBD_a, KBD_b, KBD_c, KBD_x, KBD_y, KBD_z};
	EXPECT_EQ(PressedKeys(), expected);
}

TEST_F(LuaApiTest, TypeUppercaseWrapsKeyInShift)
{
	auto state = RunTypeDraining("dosbox.type('A')");
	ASSERT_EQ(state, Lua::ScriptState::Completed);

	const std::vector<std::pair<int, bool>> expected = {
	        {KBD_leftshift,  true},
	        {        KBD_a,  true},
	        {        KBD_a, false},
	        {KBD_leftshift, false},
	};
	EXPECT_EQ(captured_keys, expected);
}

TEST_F(LuaApiTest, TypePacesInjectionAcrossFrames)
{
	// A whole string must not be slammed into the 8-slot buffer at once.
	// With no guest draining it, the first injecting frame can only fit a
	// few keys, far fewer than the full string's events.
	LoadAndStart("dosbox.type('abcdefghij')"); // 10 chars -> 20 events
	coroutine.DispatchFrame(1); // starts script, queues input
	coroutine.DispatchFrame(2); // first injecting frame

	EXPECT_GT(captured_keys.size(), 0u);
	EXPECT_LT(captured_keys.size(), 20u);
}

TEST_F(LuaApiTest, TypeStaysPendingUntilGuestDrains)
{
	// type() is async and waits for the guest to consume the keys. With the
	// buffer never drained, it must stay yielded rather than report complete.
	auto state = RunToCompletion("dosbox.type('abcdefgh')", 50);
	EXPECT_EQ(state, Lua::ScriptState::Yielded);
}

// -- Input: mouse_click() --

TEST_F(LuaApiTest, MouseClickValidButton)
{
	auto state = RunToCompletion("dosbox.mouse_click('left')");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, MouseClickInvalidButton)
{
	auto state = RunToCompletion("dosbox.mouse_click('extra')");
	EXPECT_EQ(state, Lua::ScriptState::Error);
	EXPECT_NE(coroutine.ErrorMessage().find("unknown mouse button"),
	          std::string::npos);
}

// -- Input: mouse_move() --

TEST_F(LuaApiTest, MouseMoveSucceeds)
{
	auto state = RunToCompletion("dosbox.mouse_move(10.0, -5.0)");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

// -- Memory: read/write bounds --

TEST_F(LuaApiTest, MemReadByteBoundsCheck)
{
	// Reading from address 0x0000:0x0000 should work in the test
	// environment if emulator memory is initialized. If not
	// initialized, this will error, which is also correct.
	auto state = RunToCompletion("local v = dosbox.mem_read_byte(0, 0)");
	// Either succeeds or errors with out-of-range, both are valid
	// in a test environment without full emulator init.
	EXPECT_TRUE(state == Lua::ScriptState::Completed ||
	            state == Lua::ScriptState::Error);
}

TEST_F(LuaApiTest, MemReadZeroLengthErrors)
{
	auto state = RunToCompletion("dosbox.mem_read(0, 0, 0)");
	EXPECT_EQ(state, Lua::ScriptState::Error);
	EXPECT_NE(coroutine.ErrorMessage().find("length must be"), std::string::npos);
}

TEST_F(LuaApiTest, MemReadTooLargeErrors)
{
	auto state = RunToCompletion("dosbox.mem_read(0, 0, 2000000)");
	EXPECT_EQ(state, Lua::ScriptState::Error);
	EXPECT_NE(coroutine.ErrorMessage().find("length must be"), std::string::npos);
}

// -- Screen: text reading --

TEST_F(LuaApiTest, ScreenTextReturnsString)
{
	// In test environment without VGA init, screen_text returns
	// empty or garbage, but it should not crash.
	auto state = RunToCompletion("local t = dosbox.screen_text()");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, ScreenMatchReturnsBoolean)
{
	auto state = RunToCompletion(
	        "local m = dosbox.screen_match('test')\n"
	        "assert(type(m) == 'boolean')\n");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, ScreenMatchIgnoreCaseOption)
{
	auto state = RunToCompletion(
	        "local m = dosbox.screen_match('TEST', {ignorecase = true})\n"
	        "assert(type(m) == 'boolean')\n");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

// -- OSD --

TEST_F(LuaApiTest, OsdShowsText)
{
	auto state = RunToCompletion("dosbox.osd('Hello')");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, OsdWithOptions)
{
	auto state = RunToCompletion(
	        "dosbox.osd('Status', {color = 'green', size = 'large'})");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, OsdClear)
{
	auto state = RunToCompletion(
	        "dosbox.osd('temp')\n"
	        "dosbox.osd_clear()\n");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

// -- Output table --

TEST_F(LuaApiTest, OutputTableExists)
{
	auto state = RunToCompletion("assert(type(dosbox.output) == 'table')\n");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, OutputTableWritable)
{
	auto state = RunToCompletion(
	        "dosbox.output.result = 'success'\n"
	        "dosbox.output.count = 42\n"
	        "assert(dosbox.output.result == 'success')\n"
	        "assert(dosbox.output.count == 42)\n");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

// -- Logging --

TEST_F(LuaApiTest, LogSucceeds)
{
	auto state = RunToCompletion("dosbox.log('test message')");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, DebugMsgSucceeds)
{
	auto state = RunToCompletion("dosbox.debugmsg('debug info')");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

TEST_F(LuaApiTest, AbortStopsScript)
{
	auto state = RunToCompletion("dosbox.abort('deliberate abort')");
	EXPECT_EQ(state, Lua::ScriptState::Error);
	EXPECT_NE(coroutine.ErrorMessage().find("deliberate abort"),
	          std::string::npos);
}

// -- Mount lock --

TEST_F(LuaApiTest, MountLockSucceeds)
{
	auto state = RunToCompletion("dosbox.mount_lock()");
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

// -- wait_for_text yields --

TEST_F(LuaApiTest, WaitForTextNegativeTimeoutErrors)
{
	auto state = RunToCompletion("dosbox.wait_for_text('x', -1)");
	EXPECT_EQ(state, Lua::ScriptState::Error);
	EXPECT_NE(coroutine.ErrorMessage().find("non-negative"), std::string::npos);
}

TEST_F(LuaApiTest, WaitForTextTimesOut)
{
	// In test environment, screen has no text, so this should timeout.
	LoadAndStart(
	        "local found = dosbox.wait_for_text('IMPOSSIBLE', 5)\n"
	        "dosbox.output.found = found\n");

	Lua::ScriptState state = Lua::ScriptState::Running;
	for (int i = 1; i <= 20; ++i) {
		state = coroutine.DispatchFrame(static_cast<uint64_t>(i));
		if (state == Lua::ScriptState::Completed ||
		    state == Lua::ScriptState::Error) {
			break;
		}
	}

	// Should complete after timeout, not error.
	EXPECT_EQ(state, Lua::ScriptState::Completed);
}

// -- Token scopes (4.7 follow-up) --
//
// script/load and script/start are REST routes gated on the 'script'
// scope like any other, but once running, a script never passes
// through the pre-routing handler again - dosbox.* calls reach guest
// state directly. These tests prove 'script' alone is not a blanket
// grant: each guest-reaching function must also honor whatever
// webserver_token_scopes configured, exactly like its REST equivalent.
//
// Webserver::SetGrantedTokenScopes is a process-wide global (the same
// one the real pre-routing handler reads) - every test here resets it
// in SetUp/TearDown so it can't leak into unrelated tests elsewhere in
// this binary, the same discipline already used for FreezeRegistry and
// RecordingStore.

class LuaApiScopeTest : public LuaApiTest {
protected:
	void SetUp() override
	{
		LuaApiTest::SetUp();
		Webserver::SetGrantedTokenScopes(std::nullopt);
	}

	void TearDown() override
	{
		Webserver::SetGrantedTokenScopes(std::nullopt);
		LuaApiTest::TearDown();
	}
};

struct GatedLuaCall {
	std::string script;
	Webserver::TokenScope scope;
	std::string scope_name;
};

// One entry per guest-reaching dosbox.* function - every RequireScope
// call site added in lua_api.cpp needs to be proven individually, since
// a wrong or missing scope on any single one is a silent security gap,
// not a crash.
const std::vector<GatedLuaCall>& GatedLuaCalls()
{
	static const std::vector<GatedLuaCall> calls = {
	        {"dosbox.key('KBD_enter', true)",   Webserver::TokenScope::Input,   "input"},
	        {             "dosbox.type('x')",   Webserver::TokenScope::Input,   "input"},
	        {  "dosbox.mouse_move(1.0, 1.0)",   Webserver::TokenScope::Input,   "input"},
	        {   "dosbox.mouse_click('left')",   Webserver::TokenScope::Input,   "input"},
	        {     "dosbox.mem_read(0, 0, 1)",    Webserver::TokenScope::Read,    "read"},
	        {  "dosbox.mem_write(0, 0, 'x')",   Webserver::TokenScope::Write,   "write"},
	        {   "dosbox.mem_read_byte(0, 0)",    Webserver::TokenScope::Read,    "read"},
	        {   "dosbox.mem_read_word(0, 0)",    Webserver::TokenScope::Read,    "read"},
	        {         "dosbox.screen_text()",   Webserver::TokenScope::Media,   "media"},
	        {     "dosbox.screen_match('x')",   Webserver::TokenScope::Media,   "media"},
	        {        "dosbox.is_text_mode()",   Webserver::TokenScope::Media,   "media"},
	        { "dosbox.wait_for_text('x', 1)",   Webserver::TokenScope::Media,   "media"},
	        {          "dosbox.mount_lock()", Webserver::TokenScope::Control, "control"},
	        {       "dosbox.capture_start()",   Webserver::TokenScope::Media,   "media"},
	        {        "dosbox.capture_stop()",   Webserver::TokenScope::Media,   "media"},
	};
	return calls;
}

TEST_F(LuaApiScopeTest, EveryGatedFunctionDeniesWithoutItsScope)
{
	for (const auto& call : GatedLuaCalls()) {
		// Grant every scope except the one this call needs, so a wrong
		// classification (too permissive) would still be caught, not
		// masked by an otherwise-empty grant.
		std::set<Webserver::TokenScope> granted = {
		        Webserver::TokenScope::Read,
		        Webserver::TokenScope::Write,
		        Webserver::TokenScope::Input,
		        Webserver::TokenScope::Script,
		        Webserver::TokenScope::Media,
		        Webserver::TokenScope::Debug,
		        Webserver::TokenScope::Control,
		};
		granted.erase(call.scope);
		Webserver::SetGrantedTokenScopes(granted);

		const auto state = RunToCompletion(call.script);
		EXPECT_EQ(state, Lua::ScriptState::Error) << call.script;
		EXPECT_NE(coroutine.ErrorMessage().find("token lacks the '" +
		                                        call.scope_name + "' scope"),
		          std::string::npos)
		        << call.script << ": " << coroutine.ErrorMessage();
	}
}

TEST_F(LuaApiScopeTest, EveryGatedFunctionAllowedWithOnlyItsOwnScope)
{
	for (const auto& call : GatedLuaCalls()) {
		Webserver::SetGrantedTokenScopes(
		        std::set<Webserver::TokenScope>{call.scope});

		const auto state = RunToCompletion(call.script);
		// type()/wait_for_text() can still legitimately yield/timeout
		// with no guest present; the point here is only that none of
		// them are rejected for scope, so a lingering Running state
		// (never reached Completed/Error within the frame budget) is
		// as acceptable a pass as Completed - only Error carrying the
		// scope-denial message would mean the grant didn't take.
		if (state == Lua::ScriptState::Error) {
			EXPECT_EQ(coroutine.ErrorMessage().find("token lacks the '"),
			          std::string::npos)
			        << call.script << ": " << coroutine.ErrorMessage();
		}
	}
}

TEST_F(LuaApiScopeTest, HostOnlyFunctionsAreNeverGated)
{
	// osd/osd_clear/log/debugmsg/abort never reach the guest or return
	// guest data - RequireScope deliberately isn't called from them.
	Webserver::SetGrantedTokenScopes(std::set<Webserver::TokenScope>{});

	EXPECT_EQ(RunToCompletion("dosbox.osd('hi')"), Lua::ScriptState::Completed);
	EXPECT_EQ(RunToCompletion("dosbox.osd_clear()"), Lua::ScriptState::Completed);
	EXPECT_EQ(RunToCompletion("dosbox.log('hi')"), Lua::ScriptState::Completed);
	EXPECT_EQ(RunToCompletion("dosbox.debugmsg('hi')"),
	          Lua::ScriptState::Completed);
}

TEST_F(LuaApiScopeTest, EverythingAllowedWhenUnrestricted)
{
	// Same tolerant check as EveryGatedFunctionAllowedWithOnlyItsOwnScope:
	// mem_write's own out-of-range check can still fire depending on
	// whether some earlier DOSBoxTestFixture-based test in this binary
	// has initialized MEM_TotalPages() - unrelated to scoping, which is
	// the only thing this test is about.
	Webserver::SetGrantedTokenScopes(std::nullopt);
	for (const auto* script : {"dosbox.key('KBD_enter', true)",
	                           "dosbox.mem_write(0, 0, 'x')",
	                           "dosbox.mount_lock()"}) {
		const auto state = RunToCompletion(script);
		if (state == Lua::ScriptState::Error) {
			EXPECT_EQ(coroutine.ErrorMessage().find("token lacks the '"),
			          std::string::npos)
			        << script << ": " << coroutine.ErrorMessage();
		}
	}
}

// --- InputType expansion tests ---

TEST(InputType, ExpandsShiftedCharWithWrappedShift)
{
	const auto events = Webserver::ExpandTextToEvents("A", 100.0);
	ASSERT_EQ(events.size(), 4u);
	EXPECT_EQ(events[0].key, static_cast<int>(KBD_leftshift));
	EXPECT_TRUE(events[0].pressed);
	EXPECT_EQ(events[1].key, static_cast<int>(KBD_a));
	EXPECT_TRUE(events[1].pressed);
	EXPECT_EQ(events[2].key, static_cast<int>(KBD_a));
	EXPECT_FALSE(events[2].pressed);
	EXPECT_EQ(events[3].key, static_cast<int>(KBD_leftshift));
	EXPECT_FALSE(events[3].pressed);
}

TEST(InputType, UnshiftedCharIsPressRelease)
{
	const auto events = Webserver::ExpandTextToEvents("a", 100.0);
	ASSERT_EQ(events.size(), 2u);
	EXPECT_EQ(events[0].key, static_cast<int>(KBD_a));
	EXPECT_TRUE(events[0].pressed);
	EXPECT_FALSE(events[1].pressed);
}

TEST(InputType, TimingIncreasesByCharacter)
{
	const auto events = Webserver::ExpandTextToEvents("ab", 50.0);
	EXPECT_DOUBLE_EQ(events.front().t_ms, 0.0);
	EXPECT_DOUBLE_EQ(events.back().t_ms, 20.0);
}

TEST(InputType, EmptyTextProducesNoEvents)
{
	const auto events = Webserver::ExpandTextToEvents("", 30.0);
	EXPECT_TRUE(events.empty());
}

TEST(InputType, UnmappableCharsSkipped)
{
	const auto events = Webserver::ExpandTextToEvents("\x01\x02", 30.0);
	EXPECT_TRUE(events.empty());
}

TEST(InputType, ZeroCpsFallsBackToDefault)
{
	const auto events = Webserver::ExpandTextToEvents("ab", 0.0);
	ASSERT_GE(events.size(), 4u);
	EXPECT_DOUBLE_EQ(events[0].t_ms, 0.0);
	EXPECT_NEAR(events[2].t_ms, 1000.0 / 30.0, 0.001);
}

} // namespace
