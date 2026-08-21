// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "wait.h"
#include "input.h"
#include "video.h"
#include "webserver.h"

#include "dosbox.h"
#if C_DEBUGGER
#include "debugger/debugger.h"
#endif

#include "gui/private/common.h"
#include "gui/render/render_shared.h"
#include "gui/titlebar.h"
#include "hardware/memory.h"
#include "lua/lua_api.h"
#include "lua/lua_bridge_commands.h"
#include "utils/fnv_hash.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <unordered_map>

using json = nlohmann::json;

namespace Webserver {

namespace {

std::string_view WaitConditionName(const WaitCondition condition)
{
	switch (condition) {
	case WaitCondition::Text: return "text";
	case WaitCondition::ScreenChange: return "screen_change";
	case WaitCondition::Frames: return "frames";
	case WaitCondition::ReplayDone: return "replay_done";
	case WaitCondition::Memory: return "memory";
	case WaitCondition::Stopped: return "stopped";
	case WaitCondition::ScriptDone: return "script_done";
	case WaitCondition::Program: return "program";
	}
	return "unknown";
}

bool CompareMemoryValue(const uint64_t current, const uint64_t expected,
                        const MemoryCompareOp op)
{
	switch (op) {
	case MemoryCompareOp::Eq: return current == expected;
	case MemoryCompareOp::Ne: return current != expected;
	case MemoryCompareOp::Lt: return current < expected;
	case MemoryCompareOp::Gt: return current > expected;
	case MemoryCompareOp::Le: return current <= expected;
	case MemoryCompareOp::Ge: return current >= expected;
	}
	return false;
}

std::string RequireString(const json& body, const char* field)
{
	if (!body.contains(field) || !body[field].is_string()) {
		throw std::invalid_argument(std::string("Missing or invalid required "
		                                        "field: ") +
		                            field);
	}
	auto value = body[field].get<std::string>();
	if (value.size() > MaxPatternLen) {
		throw std::invalid_argument(std::string(field) + " exceeds " +
		                            std::to_string(MaxPatternLen) + " bytes");
	}
	return value;
}

std::string OptionalPattern(const json& body, const char* field)
{
	if (!body.contains(field)) {
		return {};
	}
	return RequireString(body, field);
}

// nlohmann only tags a parsed number as the unsigned representation when
// the JSON *text* had no leading '-'; a value built from a C++ integer
// literal (as tests do) is always the signed representation regardless
// of its value. is_number_integer() covers both representations, so
// validate the value itself rather than relying on which one a given
// json object happens to carry.
uint64_t RequireNonNegative(const json& body, const char* field)
{
	if (!body.contains(field) || !body[field].is_number_integer()) {
		throw std::invalid_argument(std::string("memory wait requires an "
		                                        "integer ") +
		                            field);
	}
	const int64_t raw = body[field].get<int64_t>();
	if (raw < 0) {
		throw std::invalid_argument(std::string(field) +
		                            " must not be negative");
	}
	return static_cast<uint64_t>(raw);
}

} // namespace

WaitCondition ParseWaitCondition(const std::string& value)
{
	static const std::unordered_map<std::string, WaitCondition> lookup = {
	        {         "text",         WaitCondition::Text},
	        {"screen_change", WaitCondition::ScreenChange},
	        {       "frames",       WaitCondition::Frames},
	        {  "replay_done",   WaitCondition::ReplayDone},
	        {       "memory",       WaitCondition::Memory},
	        {      "stopped",      WaitCondition::Stopped},
	        {  "script_done",   WaitCondition::ScriptDone},
	        {      "program",      WaitCondition::Program},
	};
	const auto it = lookup.find(value);
	if (it == lookup.end()) {
		throw std::invalid_argument("Unknown wait condition: " + value);
	}
	return it->second;
}

uint64_t ParseHexHash(const std::string& value)
{
	if (value.size() != 16) {
		throw std::invalid_argument(
		        "baseline_hash must be exactly 16 hex characters");
	}
	uint64_t result      = 0;
	const auto [ptr, ec] = std::from_chars(value.data(),
	                                       value.data() + value.size(),
	                                       result,
	                                       16);
	if (ec != std::errc() || ptr != value.data() + value.size()) {
		throw std::invalid_argument("baseline_hash must be valid hexadecimal");
	}
	return result;
}

WaitSpec ParseWaitRequest(const json& body, const uint64_t current_frame,
                          const std::string& current_program)
{
	if (!body.is_object()) {
		throw std::invalid_argument("Request body must be a JSON object");
	}

	WaitSpec spec;
	spec.condition = ParseWaitCondition(RequireString(body, "for"));

	if (body.contains("timeout_ms")) {
		if (!body["timeout_ms"].is_number_integer()) {
			throw std::invalid_argument("timeout_ms must be an integer");
		}
		const int64_t raw = body["timeout_ms"].get<int64_t>();
		if (raw < MinWaitTimeoutMs || raw > MaxWaitTimeoutMs) {
			throw std::invalid_argument(
			        "timeout_ms must be between " +
			        std::to_string(MinWaitTimeoutMs) + " and " +
			        std::to_string(MaxWaitTimeoutMs));
		}
		spec.timeout_ms = static_cast<uint32_t>(raw);
	}

	switch (spec.condition) {
	case WaitCondition::Text:
		spec.pattern = OptionalPattern(body, "pattern");
		if (spec.pattern.empty()) {
			throw std::invalid_argument(
			        "text wait requires a non-empty pattern");
		}
		spec.ignore_case = body.value("ignore_case", false);
		break;

	case WaitCondition::ScreenChange: {
		spec.baseline_hash = ParseHexHash(
		        RequireString(body, "baseline_hash"));
		const auto source = body.value("source", std::string("text"));
		if (source == "text") {
			spec.hash_source = HashSource::Text;
		} else if (source == "frame") {
			spec.hash_source = HashSource::Frame;
		} else {
			throw std::invalid_argument("source must be 'text' or 'frame'");
		}
		break;
	}

	case WaitCondition::Frames: {
		if (!body.contains("count") || !body["count"].is_number_integer()) {
			throw std::invalid_argument(
			        "frames wait requires an integer count");
		}
		const int64_t count = body["count"].get<int64_t>();
		if (count < 1 || count > 100000) {
			throw std::invalid_argument(
			        "count must be between 1 and 100000");
		}
		spec.target_frame = current_frame + static_cast<uint64_t>(count);
		break;
	}

	case WaitCondition::ReplayDone: break;

	case WaitCondition::Memory: {
		const uint64_t addr = RequireNonNegative(body, "addr");
		if (addr > std::numeric_limits<uint32_t>::max()) {
			throw std::invalid_argument("addr out of range");
		}
		spec.mem_addr = static_cast<uint32_t>(addr);

		const int width = body.value("width", 1);
		if (width != 1 && width != 2 && width != 4) {
			throw std::invalid_argument("width must be 1, 2 or 4");
		}
		spec.mem_width = width;

		spec.mem_value = RequireNonNegative(body, "value");

		static const std::unordered_map<std::string, MemoryCompareOp> ops = {
		        {"eq", MemoryCompareOp::Eq},
		        {"ne", MemoryCompareOp::Ne},
		        {"lt", MemoryCompareOp::Lt},
		        {"gt", MemoryCompareOp::Gt},
		        {"le", MemoryCompareOp::Le},
		        {"ge", MemoryCompareOp::Ge},
		};
		const auto op_str = body.value("op", std::string("eq"));
		const auto it     = ops.find(op_str);
		if (it == ops.end()) {
			throw std::invalid_argument(
			        "op must be one of eq, ne, lt, gt, le, ge");
		}
		spec.mem_op = it->second;
		break;
	}

	case WaitCondition::Stopped: break;

	case WaitCondition::ScriptDone: break;

	case WaitCondition::Program:
		spec.pattern = OptionalPattern(body, "pattern");
		if (spec.pattern.empty()) {
			spec.has_baseline_program = true;
			spec.baseline_program     = current_program;
		}
		break;
	}

	return spec;
}

WaitRegistry& WaitRegistry::Instance()
{
	static WaitRegistry instance;
	return instance;
}

WaitOutcome WaitRegistry::WaitFor(const WaitSpec& spec)
{
	Waiter w;
	w.spec = spec;

	std::unique_lock<std::mutex> lock(mtx);

	if (waiters.size() >= MaxWaiters) {
		throw TooManyWaiters("Too many concurrent /api/v1/wait requests (max " +
		                     std::to_string(MaxWaiters) + ")");
	}
	waiters.push_back(&w);

	cv.wait_for(lock, std::chrono::milliseconds(spec.timeout_ms), [&] {
		return w.done;
	});

	const auto it = std::find(waiters.begin(), waiters.end(), &w);
	if (it != waiters.end()) {
		waiters.erase(it);
	}

	return w.outcome;
}

void WaitRegistry::Tick(const bool frames_flowing)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (waiters.empty()) {
		return;
	}

	// Computed at most once per Tick() call, only if a waiter actually
	// needs it - ReadScreenText() is ~2000 mem_readb calls, paid once
	// here rather than once per waiter.
	std::optional<std::string> screen_text;
	auto get_screen_text = [&]() -> const std::string& {
		if (!screen_text) {
			screen_text = Lua::ReadScreenText();
		}
		return *screen_text;
	};

	bool any_done = false;

	for (auto* w : waiters) {
		if (w->done) {
			continue;
		}

		// text, screen_change and frames are frame-hook-driven; replay
		// dispatch also only advances from the frame hook. None of
		// them can progress while stopped in the debugger or paused
		// in an SDL pause loop - report that plainly instead of
		// waiting out the full timeout to learn the same thing.
		const bool needs_frame_progress =
		        w->spec.condition == WaitCondition::Text ||
		        w->spec.condition == WaitCondition::ScreenChange ||
		        w->spec.condition == WaitCondition::Frames ||
		        w->spec.condition == WaitCondition::ReplayDone;

		if (!frames_flowing && needs_frame_progress) {
			w->outcome = {false, "emulator_stopped", json::object()};
			w->done  = true;
			any_done = true;
			continue;
		}

		switch (w->spec.condition) {
		case WaitCondition::Text: {
			const auto& text = get_screen_text();
			if (Lua::MatchSubstring(text,
			                        w->spec.pattern,
			                        w->spec.ignore_case)) {
				w->outcome = {true,
				              "matched",
				              {{"text_hash",
				                FormatEtag(Fnv1aHash(text))}}};
				w->done    = true;
			}
			break;
		}
		case WaitCondition::ScreenChange: {
			const uint64_t current = w->spec.hash_source == HashSource::Text
			                               ? Fnv1aHash(get_screen_text())
			                               : RENDER_GetSharedFrameHash();
			if (current != w->spec.baseline_hash) {
				w->outcome = {true,
				              "matched",
				              {{"hash", FormatEtag(current)}}};
				w->done    = true;
			}
			break;
		}
		case WaitCondition::Frames: {
			const auto frame = GFX_GetRenderedFrameCount();
			if (frame >= w->spec.target_frame) {
				w->outcome = {true, "matched", {{"frame", frame}}};
				w->done = true;
			}
			break;
		}
		case WaitCondition::ReplayDone: {
			if (!InputReplay::IsActive()) {
				w->outcome = {true, "matched", json::object()};
				w->done    = true;
			}
			break;
		}
		case WaitCondition::Memory: {
			const uint64_t mem_total = static_cast<uint64_t>(
			                                   MEM_TotalPages()) *
			                           MemPageSize;
			if (static_cast<uint64_t>(w->spec.mem_addr) + w->spec.mem_width >
			    mem_total) {
				w->outcome = {false,
				              "error",
				              {{"error", "address out of range"}}};
				w->done = true;
				break;
			}
			uint64_t current = 0;
			switch (w->spec.mem_width) {
			case 1:
				current = mem_readb<MemOpMode::SkipBreakpoints>(
				        w->spec.mem_addr);
				break;
			case 2:
				current = mem_readw<MemOpMode::SkipBreakpoints>(
				        w->spec.mem_addr);
				break;
			case 4:
				current = mem_readd<MemOpMode::SkipBreakpoints>(
				        w->spec.mem_addr);
				break;
			}
			if (CompareMemoryValue(current,
			                       w->spec.mem_value,
			                       w->spec.mem_op)) {
				w->outcome = {true, "matched", {{"value", current}}};
				w->done = true;
			}
			break;
		}
		case WaitCondition::Stopped: {
#if C_DEBUGGER
			if (DEBUG_IsDebugging()) {
				w->outcome = {true, "matched", json::object()};
				w->done    = true;
			}
#endif
			break;
		}
		case WaitCondition::ScriptDone: {
			const auto state =
			        Lua::ScriptManager::Instance().Coroutine().State();
			if (state != Lua::ScriptState::Running &&
			    state != Lua::ScriptState::Loaded &&
			    state != Lua::ScriptState::Yielded) {
				w->outcome = {true,
				              "matched",
				              {{"state",
				                Lua::ScriptStateName(state)}}};
				w->done    = true;
			}
			break;
		}
		case WaitCondition::Program: {
			const auto name = TITLEBAR_GetSegmentName();
			const bool matched = w->spec.has_baseline_program
			                           ? name != w->spec.baseline_program
			                           : Lua::MatchSubstring(name,
			                                                 w->spec.pattern,
			                                                 false);
			if (matched) {
				w->outcome = {true, "matched", {{"program", name}}};
				w->done = true;
			}
			break;
		}
		}

		if (w->done) {
			any_done = true;
		}
	}

	if (any_done) {
		cv.notify_all();
	}
}

void WaitRegistry::DrainAll()
{
	std::lock_guard<std::mutex> lock(mtx);
	for (auto* w : waiters) {
		w->outcome = {false, "shutting_down", json::object()};
		w->done    = true;
	}
	cv.notify_all();
}

void EvaluateWaits(const bool frames_flowing)
{
	WaitRegistry::Instance().Tick(frames_flowing);
}

void WaitHandlers::Post(const httplib::Request& req, httplib::Response& res)
{
	const auto body = json::parse(req.body);
	const auto spec = ParseWaitRequest(body,
	                                   GFX_GetRenderedFrameCount(),
	                                   TITLEBAR_GetSegmentName());

#if !C_DEBUGGER
	if (spec.condition == WaitCondition::Stopped) {
		res.status = httplib::StatusCode::NotImplemented_501;
		json err;
		err["error"] =
		        "wait_for stopped: debugger capability not built in "
		        "this binary";
		send_json(res, err);
		return;
	}
#endif

	const auto outcome = WaitRegistry::Instance().WaitFor(spec);

	json j;
	j["satisfied"] = outcome.satisfied;
	j["reason"]    = outcome.reason;
	j["for"]       = std::string(WaitConditionName(spec.condition));
	j.update(outcome.observation);
	send_json(res, j);
}

} // namespace Webserver
