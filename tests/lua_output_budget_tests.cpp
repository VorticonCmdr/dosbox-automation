// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "lua/lua_bridge_commands.h"
#include "lua/lua_coroutine.h"
#include "lua/lua_engine.h"
#include "webserver/bridge.h"

#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

extern "C" {
#include <lua.h>
}

namespace {

// Runs `script` to completion (the dosbox API, including dosbox.output,
// only exists on a coroutine that has actually started - see
// LuaCoroutine::Start/RegisterApi) and returns the resulting
// LuaOutputNode for dosbox.output, exactly the walk
// LuaStatusCommand::Execute performs.
Lua::LuaOutputNode SerializeOutputOf(const std::string& script,
                                     Lua::LuaOutputBudget& budget)
{
	Lua::LuaEngine engine;
	Lua::LuaCoroutine coroutine{engine};

	auto load_result = engine.LoadScript(script, "test");
	EXPECT_TRUE(load_result.ok) << load_result.error;
	EXPECT_TRUE(coroutine.Start());

	auto state = Lua::ScriptState::Running;
	for (uint64_t i = 1; i <= 50; ++i) {
		state = coroutine.DispatchFrame(i);
		if (state == Lua::ScriptState::Completed ||
		    state == Lua::ScriptState::Error) {
			break;
		}
	}
	EXPECT_EQ(state, Lua::ScriptState::Completed) << coroutine.ErrorMessage();

	auto* L = engine.GetState();
	lua_getglobal(L, "dosbox");
	lua_getfield(L, -1, "output");
	auto node = Lua::LuaTableToNode(L, lua_absindex(L, -1), 0, budget);
	lua_pop(L, 2);
	return node;
}

const Lua::LuaOutputNode* Find(const Lua::LuaOutputNode& object,
                               const std::string& key)
{
	for (const auto& [k, v] : object.object_value) {
		if (k == key) {
			return &v;
		}
	}
	return nullptr;
}

// -- LuaOutputNode::IsEmpty --

TEST(LuaOutputNode, DefaultConstructedIsEmpty)
{
	Lua::LuaOutputNode node;
	EXPECT_TRUE(node.IsEmpty());
}

TEST(LuaOutputNode, ObjectWithNoEntriesIsEmpty)
{
	Lua::LuaOutputNode node;
	node.kind = Lua::LuaOutputNode::Kind::Object;
	EXPECT_TRUE(node.IsEmpty());
}

TEST(LuaOutputNode, ObjectWithAnEntryIsNotEmpty)
{
	Lua::LuaOutputNode node;
	node.kind = Lua::LuaOutputNode::Kind::Object;
	node.object_value.emplace_back("x", Lua::LuaOutputNode{});
	EXPECT_FALSE(node.IsEmpty());
}

// -- LuaTableToNode: basic shapes --

TEST(LuaTableToNode, SerializesScalarTypes)
{
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(
	        "dosbox.output.b = true\n"
	        "dosbox.output.i = 42\n"
	        "dosbox.output.d = 3.5\n"
	        "dosbox.output.s = 'hi'\n",
	        budget);

	ASSERT_EQ(node.kind, Lua::LuaOutputNode::Kind::Object);
	ASSERT_NE(Find(node, "b"), nullptr);
	EXPECT_EQ(Find(node, "b")->kind, Lua::LuaOutputNode::Kind::Bool);
	EXPECT_EQ(Find(node, "b")->bool_value, true);

	ASSERT_NE(Find(node, "i"), nullptr);
	EXPECT_EQ(Find(node, "i")->kind, Lua::LuaOutputNode::Kind::Int);
	EXPECT_EQ(Find(node, "i")->int_value, 42);

	ASSERT_NE(Find(node, "d"), nullptr);
	EXPECT_EQ(Find(node, "d")->kind, Lua::LuaOutputNode::Kind::Double);
	EXPECT_DOUBLE_EQ(Find(node, "d")->double_value, 3.5);

	ASSERT_NE(Find(node, "s"), nullptr);
	EXPECT_EQ(Find(node, "s")->kind, Lua::LuaOutputNode::Kind::String);
	EXPECT_EQ(Find(node, "s")->string_value, "hi");

	EXPECT_FALSE(budget.truncated);
}

TEST(LuaTableToNode, SerializesNestedTables)
{
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf("dosbox.output.a = {b = {c = 1}}", budget);

	const auto* a = Find(node, "a");
	ASSERT_NE(a, nullptr);
	ASSERT_EQ(a->kind, Lua::LuaOutputNode::Kind::Object);
	const auto* b = Find(*a, "b");
	ASSERT_NE(b, nullptr);
	ASSERT_EQ(b->kind, Lua::LuaOutputNode::Kind::Object);
	const auto* c = Find(*b, "c");
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(c->kind, Lua::LuaOutputNode::Kind::Int);
	EXPECT_EQ(c->int_value, 1);

	EXPECT_FALSE(budget.truncated);
}

TEST(LuaTableToNode, DropsUnsupportedValueTypesWithoutTruncating)
{
	// Functions (and userdata/threads) have no JSON representation.
	// The pre-3.8 code silently omitted the key rather than erroring
	// or emitting a placeholder - preserved exactly.
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(
	        "dosbox.output.f = function() end\n"
	        "dosbox.output.x = 1\n",
	        budget);

	EXPECT_EQ(Find(node, "f"), nullptr);
	ASSERT_NE(Find(node, "x"), nullptr);
	EXPECT_EQ(Find(node, "x")->int_value, 1);
	EXPECT_FALSE(budget.truncated);
}

TEST(LuaTableToNode, EmptyTableProducesAnEmptyObjectNode)
{
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf("-- dosbox.output left untouched", budget);
	EXPECT_TRUE(node.IsEmpty());
	EXPECT_FALSE(budget.truncated);
}

// -- Depth cap --

TEST(LuaTableToNode, DepthBeyondTheCapIsReplacedWithAPlaceholder)
{
	// Builds a chain 15 tables deep: t = {n = {n = {n = ... = 1}}}.
	std::string script =
	        "local t = {n = 1}\n"
	        "for i = 1, 15 do t = {n = t} end\n"
	        "dosbox.output.chain = t\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_TRUE(budget.truncated);

	// Walk down until we hit the placeholder; MaxOutputDepth (10)
	// guarantees we find one well before the chain's actual depth (15).
	const Lua::LuaOutputNode* cur = Find(node, "chain");
	bool found_placeholder        = false;
	for (int i = 0; i < 20 && cur; ++i) {
		if (cur->kind == Lua::LuaOutputNode::Kind::String) {
			EXPECT_EQ(cur->string_value, "...");
			found_placeholder = true;
			break;
		}
		cur = Find(*cur, "n");
	}
	EXPECT_TRUE(found_placeholder);
}

// -- Node budget --

TEST(LuaTableToNode, NodeBudgetTruncatesAFlatTableWithManyEntries)
{
	std::string script = "for i = 1, " +
	                     std::to_string(Lua::MaxOutputNodes + 500) +
	                     " do dosbox.output[tostring(i)] = i end\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_TRUE(budget.truncated);
	EXPECT_LE(node.object_value.size(), Lua::MaxOutputNodes);
	EXPECT_LE(budget.nodes_used, Lua::MaxOutputNodes);
}

TEST(LuaTableToNode, StaysUnderTheNodeBudgetWhenTheTableFitsWithinIt)
{
	std::string script = "for i = 1, 10 do dosbox.output[tostring(i)] = i end\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_FALSE(budget.truncated);
	EXPECT_EQ(node.object_value.size(), 10u);
}

// -- Byte budget --

TEST(LuaTableToNode, ByteBudgetTruncatesManyLargeStrings)
{
	// Each value is 8 KiB; enough entries to blow well past
	// MaxOutputBytes (64 KiB) if nothing capped it.
	std::string script =
	        "local big = string.rep('x', 8192)\n"
	        "for i = 1, 32 do dosbox.output[tostring(i)] = big end\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_TRUE(budget.truncated);
	EXPECT_LE(budget.bytes_used, Lua::MaxOutputBytes);
	// Truncation must have actually dropped entries, not merely flagged
	// while still keeping all 32 (which would defeat the whole point).
	EXPECT_LT(node.object_value.size(), 32u);
}

TEST(LuaTableToNode, RejectingAnOversizedStringNeverPaysForCopyingIt)
{
	// Regression for a real bug the adversarial review caught: an
	// earlier version of the byte budget checked
	// budget.bytes_used + added_bytes AFTER already copying the full
	// Lua string into value.string_value. A single huge string
	// referenced from many small, distinct (non-shared, so the
	// visited-set dedup doesn't help) sibling tables meant paying for
	// that full copy on every single occurrence before the rejection
	// discarded it - the exact O(n) work per rejection this budget
	// exists to avoid. The fix defers the copy until after the size
	// check (lua_tolstring is an O(1) peek, not a copy). This test
	// mainly guards correctness (only the first copy, if any, should
	// ever land) - see the plan writeup for the wall-clock evidence.
	std::string script =
	        "local big = string.rep('x', 200000)\n" // ~195 KiB, over
	                                                // MaxOutputBytes alone
	        "for i = 1, 50 do dosbox.output[tostring(i)] = {big} end\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_TRUE(budget.truncated);
	EXPECT_LE(budget.bytes_used, Lua::MaxOutputBytes);
	// No entry ever actually held the full ~195 KiB string - each
	// {big} sub-table's own entry was rejected by the byte budget
	// before the copy, so it comes back as an empty Object, not a
	// node holding the giant string.
	for (const auto& [key, child] : node.object_value) {
		for (const auto& [inner_key, inner_value] : child.object_value) {
			EXPECT_NE(inner_value.string_value.size(), 200000u);
		}
	}
}

TEST(LuaTableToNode, PairsExaminedCapsATableOfEntirelyUnsupportedValues)
{
	// Regression: the "skip unsupported key/value" continue paths
	// used to never touch the budget at all, so a table made entirely
	// of unsupported-type values (here, function literals) iterated
	// in full regardless of size - bounded only by the much larger
	// instruction limit, not by this budget.
	std::string script = "for i = 1, " +
	                     std::to_string(Lua::MaxOutputNodes + 500) +
	                     " do dosbox.output[tostring(i)] = function() end end\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_TRUE(node.object_value.empty()); // every value was unsupported
	EXPECT_TRUE(budget.truncated);
	EXPECT_LE(budget.pairs_examined, Lua::MaxOutputNodes);
}

TEST(LuaTableToNode, EmbeddedNulByteInAStringValueIsNotTruncated)
{
	// Regression: lua_tostring relies on strlen, which stops at the
	// first NUL; the fix reads the value via lua_tolstring's explicit
	// length instead.
	std::string script = "dosbox.output.s = 'ab\\0cd'";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	const auto* s = Find(node, "s");
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->string_value.size(), 5u);
	EXPECT_EQ(s->string_value, std::string("ab\0cd", 5));
}

TEST(LuaTableToNode, EmbeddedNulByteInAStringKeyIsNotTruncated)
{
	std::string script = "dosbox.output['ab\\0cd'] = 1";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	ASSERT_EQ(node.object_value.size(), 1u);
	EXPECT_EQ(node.object_value[0].first, std::string("ab\0cd", 5));
}

// -- Shared references and cycles --

TEST(LuaTableToNode, SharedSubtablesDoNotExpandExponentially)
{
	// The scenario from the plan: N tables, each holding M references
	// to the previous one - ~N*M Lua slots, but naive recursion would
	// emit M^N nodes. 8 levels of 10 references each is 10^8 without
	// the visited-set dedup; this test times out long before that if
	// the dedup regresses.
	std::string script =
	        "local t = {}\n"
	        "for level = 1, 8 do\n"
	        "  local next_level = {}\n"
	        "  for i = 1, 10 do next_level[i] = t end\n"
	        "  t = next_level\n"
	        "end\n"
	        "dosbox.output.shared = t\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_TRUE(budget.truncated);
	EXPECT_LE(budget.nodes_used, Lua::MaxOutputNodes);
}

TEST(LuaTableToNode, SelfReferentialTableTerminatesInsteadOfHanging)
{
	std::string script =
	        "local t = {}\n"
	        "t.self = t\n"
	        "dosbox.output.cyclic = t\n";
	Lua::LuaOutputBudget budget;
	auto node = SerializeOutputOf(script, budget);

	EXPECT_TRUE(budget.truncated);
	const auto* cyclic = Find(node, "cyclic");
	ASSERT_NE(cyclic, nullptr);
	const auto* self = Find(*cyclic, "self");
	ASSERT_NE(self, nullptr);
	EXPECT_EQ(self->kind, Lua::LuaOutputNode::Kind::String);
	EXPECT_EQ(self->string_value, "...");
}

// -- LuaOutputNodeToJson --

TEST(LuaOutputNodeToJson, ConvertsEveryKind)
{
	Lua::LuaOutputNode obj;
	obj.kind = Lua::LuaOutputNode::Kind::Object;

	Lua::LuaOutputNode b;
	b.kind       = Lua::LuaOutputNode::Kind::Bool;
	b.bool_value = true;
	obj.object_value.emplace_back("b", b);

	Lua::LuaOutputNode i;
	i.kind      = Lua::LuaOutputNode::Kind::Int;
	i.int_value = 7;
	obj.object_value.emplace_back("i", i);

	Lua::LuaOutputNode d;
	d.kind         = Lua::LuaOutputNode::Kind::Double;
	d.double_value = 1.25;
	obj.object_value.emplace_back("d", d);

	Lua::LuaOutputNode s;
	s.kind         = Lua::LuaOutputNode::Kind::String;
	s.string_value = "hi";
	obj.object_value.emplace_back("s", s);

	const auto j = Lua::LuaOutputNodeToJson(obj);
	EXPECT_EQ(j["b"], true);
	EXPECT_EQ(j["i"], 7);
	EXPECT_DOUBLE_EQ(j["d"].get<double>(), 1.25);
	EXPECT_EQ(j["s"], "hi");
}

TEST(LuaOutputNodeToJson, NullNodeConvertsToJsonNull)
{
	Lua::LuaOutputNode node;
	EXPECT_TRUE(Lua::LuaOutputNodeToJson(node).is_null());
}

// -- LuaStatusCommand integration --

class LuaStatusCommandOutputTest : public testing::Test {
protected:
	void TearDown() override
	{
		auto& mgr = Lua::ScriptManager::Instance();
		mgr.Log().Close();
		mgr.Params() = Lua::ScriptParams{};
		mgr.Engine().Reset();
	}

	Lua::ScriptState RunToCompletion(const std::string& script)
	{
		auto& mgr   = Lua::ScriptManager::Instance();
		auto result = mgr.Engine().LoadScript(script, "test");
		EXPECT_TRUE(result.ok) << result.error;
		if (!mgr.Coroutine().Start()) {
			return mgr.Coroutine().State();
		}
		Lua::ScriptState state = Lua::ScriptState::Running;
		for (uint64_t i = 1; i <= 50; ++i) {
			state = mgr.Coroutine().DispatchFrame(i);
			if (state == Lua::ScriptState::Completed ||
			    state == Lua::ScriptState::Error) {
				break;
			}
		}
		return state;
	}
};

TEST_F(LuaStatusCommandOutputTest, ReportsOutputAndLeavesTruncatedFalseWhenSmall)
{
	ASSERT_EQ(RunToCompletion("dosbox.output.result = 'done'"),
	          Lua::ScriptState::Completed);

	Lua::LuaStatusCommand cmd;
	cmd.Execute();

	ASSERT_FALSE(cmd.result.output.IsEmpty());
	const auto* result = Find(cmd.result.output, "result");
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->string_value, "done");
	EXPECT_FALSE(cmd.result.output_truncated);
}

TEST_F(LuaStatusCommandOutputTest, ReportsOutputTruncatedWhenTheBudgetIsExceeded)
{
	std::string script = "for i = 1, " +
	                     std::to_string(Lua::MaxOutputNodes + 500) +
	                     " do dosbox.output[tostring(i)] = i end\n";
	ASSERT_EQ(RunToCompletion(script), Lua::ScriptState::Completed);

	Lua::LuaStatusCommand cmd;
	cmd.Execute();

	EXPECT_TRUE(cmd.result.output_truncated);
	EXPECT_LE(cmd.result.output.object_value.size(), Lua::MaxOutputNodes);
}

TEST_F(LuaStatusCommandOutputTest, GetIncludesOutputTruncatedInTheJsonResponse)
{
	// Regression: earlier coverage only checked the pre-JSON
	// cmd.result.output_truncated field via Execute() directly -
	// nothing exercised LuaStatusCommand::Get() itself, which is what
	// actually puts output_truncated on the wire.
	ASSERT_EQ(RunToCompletion("dosbox.output.result = 'done'"),
	          Lua::ScriptState::Completed);

	httplib::Request req;
	httplib::Response res;
	std::thread getter([&] { Lua::LuaStatusCommand::Get(req, res); });
	while (Webserver::Bridge::Instance().QueueDepth() < 1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Webserver::Bridge::Instance().ProcessRequests();
	getter.join();

	// Get() never sets res.status on the success path (httplib itself
	// defaults an unset status to 200 when a response actually goes
	// out over a socket) - only the body matters when calling Get()
	// directly like this, bypassing httplib entirely.
	const auto body = nlohmann::json::parse(res.body);
	ASSERT_TRUE(body.contains("output_truncated"));
	EXPECT_EQ(body["output_truncated"], false);
	ASSERT_TRUE(body.contains("output"));
	EXPECT_EQ(body["output"]["result"], "done");
}

} // namespace
