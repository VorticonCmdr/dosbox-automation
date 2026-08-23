// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "input.h"
#include "bridge.h"
#include "webserver.h"

#include "gui/osd/osd.h"
#include "gui/private/common.h"
#include "gui/titlebar.h"
#include "hardware/input/keyboard.h" // KBD_KEYS, KEYBOARD_AddKey, keyboard_input_hook
#include "hardware/input/mouse.h" // MOUSE_Event*, mouse_*_hook
#include "hardware/pic.h"
#include "lua/text_input.h"
#include "utils/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "libs/json/json.h"

#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>

using json = nlohmann::json;

namespace Webserver {

bool IsValidEventTimeMs(const double t_ms)
{
	return t_ms >= 0 && t_ms <= MaxEventTimeMs;
}

bool IsValidEventFrame(const int64_t frame)
{
	return frame >= 0 && static_cast<uint64_t>(frame) <= MaxEventFrame;
}

bool IsValidTypingCps(const double cps)
{
	return cps >= MinTypingCps && cps <= MaxTypingCps;
}

static const std::unordered_map<std::string, KBD_KEYS> key_name_map = {
        {        "KBD_NONE",         KBD_NONE},
        {           "KBD_1",            KBD_1},
        {           "KBD_2",            KBD_2},
        {           "KBD_3",            KBD_3},
        {           "KBD_4",            KBD_4},
        {           "KBD_5",            KBD_5},
        {           "KBD_6",            KBD_6},
        {           "KBD_7",            KBD_7},
        {           "KBD_8",            KBD_8},
        {           "KBD_9",            KBD_9},
        {           "KBD_0",            KBD_0},
        {           "KBD_q",            KBD_q},
        {           "KBD_w",            KBD_w},
        {           "KBD_e",            KBD_e},
        {           "KBD_r",            KBD_r},
        {           "KBD_t",            KBD_t},
        {           "KBD_y",            KBD_y},
        {           "KBD_u",            KBD_u},
        {           "KBD_i",            KBD_i},
        {           "KBD_o",            KBD_o},
        {           "KBD_p",            KBD_p},
        {           "KBD_a",            KBD_a},
        {           "KBD_s",            KBD_s},
        {           "KBD_d",            KBD_d},
        {           "KBD_f",            KBD_f},
        {           "KBD_g",            KBD_g},
        {           "KBD_h",            KBD_h},
        {           "KBD_j",            KBD_j},
        {           "KBD_k",            KBD_k},
        {           "KBD_l",            KBD_l},
        {           "KBD_z",            KBD_z},
        {           "KBD_x",            KBD_x},
        {           "KBD_c",            KBD_c},
        {           "KBD_v",            KBD_v},
        {           "KBD_b",            KBD_b},
        {           "KBD_n",            KBD_n},
        {           "KBD_m",            KBD_m},
        {          "KBD_f1",           KBD_f1},
        {          "KBD_f2",           KBD_f2},
        {          "KBD_f3",           KBD_f3},
        {          "KBD_f4",           KBD_f4},
        {          "KBD_f5",           KBD_f5},
        {          "KBD_f6",           KBD_f6},
        {          "KBD_f7",           KBD_f7},
        {          "KBD_f8",           KBD_f8},
        {          "KBD_f9",           KBD_f9},
        {         "KBD_f10",          KBD_f10},
        {         "KBD_f11",          KBD_f11},
        {         "KBD_f12",          KBD_f12},
        {         "KBD_esc",          KBD_esc},
        {         "KBD_tab",          KBD_tab},
        {   "KBD_backspace",    KBD_backspace},
        {       "KBD_enter",        KBD_enter},
        {       "KBD_space",        KBD_space},
        {     "KBD_leftalt",      KBD_leftalt},
        {    "KBD_rightalt",     KBD_rightalt},
        {    "KBD_leftctrl",     KBD_leftctrl},
        {   "KBD_rightctrl",    KBD_rightctrl},
        {     "KBD_leftgui",      KBD_leftgui},
        {    "KBD_rightgui",     KBD_rightgui},
        {   "KBD_leftshift",    KBD_leftshift},
        {  "KBD_rightshift",   KBD_rightshift},
        {    "KBD_capslock",     KBD_capslock},
        {  "KBD_scrolllock",   KBD_scrolllock},
        {     "KBD_numlock",      KBD_numlock},
        {       "KBD_grave",        KBD_grave},
        {       "KBD_minus",        KBD_minus},
        {      "KBD_equals",       KBD_equals},
        {   "KBD_backslash",    KBD_backslash},
        { "KBD_leftbracket",  KBD_leftbracket},
        {"KBD_rightbracket", KBD_rightbracket},
        {   "KBD_semicolon",    KBD_semicolon},
        {       "KBD_quote",        KBD_quote},
        {      "KBD_oem102",       KBD_oem102},
        {      "KBD_period",       KBD_period},
        {       "KBD_comma",        KBD_comma},
        {       "KBD_slash",        KBD_slash},
        {       "KBD_abnt1",        KBD_abnt1},
        { "KBD_printscreen",  KBD_printscreen},
        {       "KBD_pause",        KBD_pause},
        {      "KBD_insert",       KBD_insert},
        {        "KBD_home",         KBD_home},
        {      "KBD_pageup",       KBD_pageup},
        {      "KBD_delete",       KBD_delete},
        {         "KBD_end",          KBD_end},
        {    "KBD_pagedown",     KBD_pagedown},
        {        "KBD_left",         KBD_left},
        {          "KBD_up",           KBD_up},
        {        "KBD_down",         KBD_down},
        {       "KBD_right",        KBD_right},
        {         "KBD_kp1",          KBD_kp1},
        {         "KBD_kp2",          KBD_kp2},
        {         "KBD_kp3",          KBD_kp3},
        {         "KBD_kp4",          KBD_kp4},
        {         "KBD_kp5",          KBD_kp5},
        {         "KBD_kp6",          KBD_kp6},
        {         "KBD_kp7",          KBD_kp7},
        {         "KBD_kp8",          KBD_kp8},
        {         "KBD_kp9",          KBD_kp9},
        {         "KBD_kp0",          KBD_kp0},
        {    "KBD_kpdivide",     KBD_kpdivide},
        {  "KBD_kpmultiply",   KBD_kpmultiply},
        {     "KBD_kpminus",      KBD_kpminus},
        {      "KBD_kpplus",       KBD_kpplus},
        {     "KBD_kpenter",      KBD_kpenter},
        {    "KBD_kpperiod",     KBD_kpperiod},
};

static const std::unordered_map<std::string, MouseButtonId> button_name_map = {
        {  "left",   MouseButtonId::Left},
        { "right",  MouseButtonId::Right},
        {"middle", MouseButtonId::Middle},
};

static std::mutex pending_mutex;
static std::queue<InputEvent> pending_events;
static bool in_replay_dispatch = false;

static void dispatch_input_event(const InputEvent& ev)
{
	in_replay_dispatch = true;

	switch (ev.type) {
	case InputEvent::Type::Key:
		KEYBOARD_AddKey(static_cast<KBD_KEYS>(ev.key), ev.pressed);
		break;
	case InputEvent::Type::MouseMove:
		MOUSE_InjectMoved(ev.x_rel, ev.y_rel);
		break;
	case InputEvent::Type::MouseButton: {
		auto it = button_name_map.find(ev.button);
		if (it != button_name_map.end()) {
			MOUSE_InjectButton(it->second, ev.pressed);
		}
		break;
	}
	case InputEvent::Type::MouseWheel:
		MOUSE_InjectWheel(ev.wheel_delta);
		break;
	}

	in_replay_dispatch = false;
}

static size_t pending_total       = 0;
static size_t pending_dispatched  = 0;
static double replay_start_pic_ms = 0;
static std::chrono::steady_clock::time_point replay_start_wall;

// Wall elapsed/drift as of the last dispatched event - kept after the
// chain empties or self-aborts so a status check right after still
// reports the finished run's numbers instead of zero. While the chain
// is active, InputReplay::GetStatus() computes elapsed live instead
// (more accurate between dispatches); drift only has meaning at a
// dispatch, so it's always read from here.
static double pending_elapsed_ms = 0;
static double pending_drift_ms   = 0;

// Set the first time the front event hits keyboard backpressure, reset
// the moment it (or a different front event) doesn't. See
// ReplayStallThresholdMs.
static std::optional<std::chrono::steady_clock::time_point> pending_backpressure_since;

static std::mutex frame_replay_mutex;
static std::queue<InputEvent> frame_pending_events;
static bool frame_replay_active        = false;
static uint64_t frame_replay_start     = 0;
static size_t frame_pending_total      = 0;
static size_t frame_pending_dispatched = 0;
static std::chrono::steady_clock::time_point frame_replay_start_wall;
static double frame_replay_first_t_ms = 0;

static double frame_elapsed_ms = 0;
static double frame_drift_ms   = 0;
static std::optional<std::chrono::steady_clock::time_point> frame_backpressure_since;

// Which engine most recently had *any* state-changing event - armed a
// chain, dispatched, finished, stalled-out, or was cancelled - read
// only once neither chain is active, to attribute a finished/aborted
// run's stats to the right one instead of reporting all-zero, and to
// prefer whichever engine's status is actually fresh over one that
// happened to start first but already finished. Its own small mutex:
// touched from both engines' code paths (each already under a
// different lock) and from GetStatus(), so it can't safely piggyback
// on pending_mutex or frame_replay_mutex without entangling their lock
// ordering. Callers must already hold pending_mutex or
// frame_replay_mutex before calling this - never the reverse - so
// last_engine_mutex is always the innermost lock.
static std::mutex last_engine_mutex;
static std::string last_engine = "none";

static void mark_last_engine(const char* engine)
{
	std::lock_guard<std::mutex> lock(last_engine_mutex);
	last_engine = engine;
}

static constexpr double backpressure_retry_ms = 1.0;

static void pic_input_handler(uint32_t)
{
	std::lock_guard<std::mutex> lock(pending_mutex);
	if (pending_events.empty()) {
		return;
	}

	const auto& ev = pending_events.front();

	// Key events need buffer space. A press/release pair uses two
	// slots. If there's no room, retry after a short delay instead
	// of dispatching into the overflow latch.
	if (ev.type == InputEvent::Type::Key && KEYBOARD_GetBufferFreeSlots() < 2) {
		const auto now = std::chrono::steady_clock::now();
		if (!pending_backpressure_since) {
			pending_backpressure_since = now;
		} else if (std::chrono::duration<double, std::milli>(
		                   now - *pending_backpressure_since)
		                   .count() >= ReplayStallThresholdMs) {
			const auto dropped = pending_events.size();
			std::queue<InputEvent>().swap(pending_events);
			pending_backpressure_since.reset();
			pending_elapsed_ms = std::chrono::duration<double, std::milli>(
			                             now - replay_start_wall)
			                             .count();
			mark_last_engine("pic");
			TITLEBAR_NotifyApiReplayStatus(false);
			LOG_WARNING(
			        "REPLAY (PIC): chain aborted, stuck %.0fms waiting "
			        "for keyboard buffer space - %zu event(s) never "
			        "dispatched (%zu/%zu total)",
			        ReplayStallThresholdMs,
			        dropped,
			        pending_dispatched,
			        pending_total);
			return;
		}
		PIC_AddEvent(pic_input_handler, backpressure_retry_ms);
		return;
	}
	pending_backpressure_since.reset();

	auto dispatched_ev = ev;
	pending_events.pop();
	++pending_dispatched;

	const double pic_ms     = PIC_FullIndex() - replay_start_pic_ms;
	const double wall_ms    = std::chrono::duration<double, std::milli>(
	                                  std::chrono::steady_clock::now() -
	                                  replay_start_wall)
	                                  .count();
	const double pic_drift  = pic_ms - dispatched_ev.t_ms;
	const double wall_drift = wall_ms - dispatched_ev.t_ms;

	pending_elapsed_ms = wall_ms;
	pending_drift_ms   = wall_drift;

	if (pending_dispatched % 100 == 0 || pending_dispatched == pending_total) {
		LOG_MSG("REPLAY [%zu/%zu] t=%.1fms pic=%+.2fms wall=%+.2fms (pic-wall=%+.2fms)",
		        pending_dispatched,
		        pending_total,
		        dispatched_ev.t_ms,
		        pic_drift,
		        wall_drift,
		        pic_ms - wall_ms);
	}

	dispatch_input_event(dispatched_ev);

	if (!pending_events.empty()) {
		const auto& next = pending_events.front();
		const auto delay = std::max(next.t_ms - dispatched_ev.t_ms, 0.0);
		PIC_AddEvent(pic_input_handler, delay);
	} else {
		mark_last_engine("pic");
		TITLEBAR_NotifyApiReplayStatus(false);
		LOG_MSG("REPLAY chain complete: %zu/%zu events dispatched, pic=%+.2fms wall=%+.2fms",
		        pending_dispatched,
		        pending_total,
		        pic_drift,
		        wall_drift);
	}
}

InputSequenceCommand::InputSequenceCommand(std::vector<InputEvent> events,
                                           bool has_frame_data)
        : events(std::move(events)),
          has_frame_data(has_frame_data)
{}

void InputSequenceCommand::Execute()
{
	if (has_frame_data) {
		ExecuteFrameBased();
	} else {
		ExecutePicBased();
	}
}

void InputSequenceCommand::ExecutePicBased()
{
	std::lock_guard<std::mutex> lock(pending_mutex);

	if (!pending_events.empty()) {
		error = "Replay already in progress";
		return;
	}

	for (auto& ev : events) {
		pending_events.push(ev);
	}
	pending_total      = pending_events.size();
	pending_dispatched = 0;
	// Otherwise a status query landing after arming but before the
	// first dispatch would report a leftover drift figure from
	// whatever chain last dispatched, misattributed to this new one.
	pending_drift_ms = 0;
	pending_backpressure_since.reset();
	LOG_DEBUG("REPLAY (PIC) starting chain: %zu timed events", pending_total);
	if (!pending_events.empty()) {
		replay_start_pic_ms = PIC_FullIndex();
		replay_start_wall   = std::chrono::steady_clock::now();
		TITLEBAR_NotifyApiReplayStatus(true);
		PIC_AddEvent(pic_input_handler, pending_events.front().t_ms);
		mark_last_engine("pic");
	}
}

void InputSequenceCommand::ExecuteFrameBased()
{
	std::lock_guard<std::mutex> lock(frame_replay_mutex);

	if (frame_replay_active) {
		error = "Replay already in progress";
		return;
	}

	/* Normalize frames and timestamps so the first event is at frame 0,
	   t=0. The recording stores values relative to recording start, but
	   there's dead time between "start recording" and the first actual
	   input. Stripping that offset makes replay independent of when the API
	   call lands relative to the game boot sequence. */
	uint64_t frame_base = 0;
	double t_base       = 0;
	for (const auto& ev : events) {
		if (ev.frame > 0 || ev.t_ms > 0) {
			frame_base = ev.frame;
			t_base     = ev.t_ms;
			break;
		}
	}

	for (auto& ev : events) {
		ev.frame = (ev.frame >= frame_base) ? ev.frame - frame_base : 0;
		ev.t_ms  = (ev.t_ms >= t_base) ? ev.t_ms - t_base : 0;
		frame_pending_events.push(ev);
	}
	frame_pending_total      = frame_pending_events.size();
	frame_pending_dispatched = 0;
	frame_drift_ms           = 0;
	frame_backpressure_since.reset();

	if (!frame_pending_events.empty()) {
		frame_replay_start      = GFX_GetRenderedFrameCount();
		frame_replay_start_wall = std::chrono::steady_clock::now();
		frame_replay_first_t_ms = 0;
		frame_replay_active     = true;
		TITLEBAR_NotifyApiReplayStatus(true);
		OSD::OsdManager::Instance().SetIcon(OSD::IconId::ReplayActive, true);
		LOG_MSG("REPLAY (frame) starting: %zu events, normalized from frame %llu (%.1fms offset removed)",
		        frame_pending_total,
		        static_cast<unsigned long long>(frame_base),
		        t_base);
		mark_last_engine("frame");
	}
}

void InputSequenceCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto body = json::parse(req.body);

	if (!body.contains("events") || !body["events"].is_array()) {
		res.status = 400;
		json err;
		err["error"] = "Missing or invalid 'events' array";
		send_json(res, err);
		return;
	}

	if (body["events"].size() > MaxInputEvents) {
		res.status = 400;
		json err;
		err["error"] = "Too many events (max " +
		               std::to_string(MaxInputEvents) + ")";
		send_json(res, err);
		return;
	}

	// Every event field must be recognized. Silently ignoring a
	// misnamed field (say 'x' instead of 'x_rel') injects zero-motion
	// events and leaves the caller diagnosing ghosts; the first field
	// test drive lost an hour to exactly that.
	static const std::unordered_map<std::string, std::vector<std::string>> allowed_fields = {
	        {         "key",                   {"key", "pressed"}},
	        {  "mouse_move", {"x_rel", "y_rel", "x_abs", "y_abs"}},
	        {"mouse_button",                {"button", "pressed"}},
	        { "mouse_wheel",                            {"delta"}},
	};
	static const std::vector<std::string> common_fields = {"type",
	                                                       "t",
	                                                       "delay_ms",
	                                                       "frame"};

	auto is_allowed = [](const std::vector<std::string>& fields,
	                     const std::string& name) {
		return std::find(fields.begin(), fields.end(), name) != fields.end();
	};

	std::vector<InputEvent> events;

	// Running timeline position for 'delay_ms' (relative) timing
	double cumulative_t_ms = 0.0;

	for (const auto& jev : body["events"]) {
		InputEvent ev = {};

		const auto type_str = jev.value("type", "key");

		const auto af = allowed_fields.find(type_str);
		if (af != allowed_fields.end()) {
			for (const auto& [field_name, field_value] : jev.items()) {
				if (is_allowed(common_fields, field_name) ||
				    is_allowed(af->second, field_name)) {
					continue;
				}
				res.status = 400;
				json err;
				auto allowed = common_fields;
				allowed.insert(allowed.end(),
				               af->second.begin(),
				               af->second.end());
				err["error"] = "Unknown field '" + field_name +
				               "' for " + type_str +
				               " event. Allowed fields: " +
				               join(allowed, ", ", "", "");
				send_json(res, err);
				return;
			}
		}

		// Two timing forms: 't' places the event at an absolute
		// position on the sequence timeline (recording format);
		// 'delay_ms' waits relative to the previous event, which is
		// the natural form for hand-written sequences. Mixing them in
		// one event is ambiguous.
		if (jev.contains("t") && jev.contains("delay_ms")) {
			res.status = 400;
			json err;
			err["error"] =
			        "Use either 't' (absolute ms) or "
			        "'delay_ms' (relative ms), not both";
			send_json(res, err);
			return;
		}
		if (jev.contains("t")) {
			ev.t_ms = jev["t"].get<double>();
			if (!IsValidEventTimeMs(ev.t_ms)) {
				res.status = 400;
				json err;
				err["error"] = "'t' must be between 0 and " +
				               std::to_string(MaxEventTimeMs) + " ms";
				send_json(res, err);
				return;
			}
			cumulative_t_ms = ev.t_ms;
		} else if (jev.contains("delay_ms")) {
			const auto delay_ms = jev["delay_ms"].get<double>();
			if (!IsValidEventTimeMs(delay_ms)) {
				res.status = 400;
				json err;
				err["error"] = "'delay_ms' must be between 0 and " +
				               std::to_string(MaxEventTimeMs) + " ms";
				send_json(res, err);
				return;
			}
			cumulative_t_ms += delay_ms;
			// Bounds the accumulated position, not just this one
			// delay: many individually-valid delays can still sum
			// past the limit.
			if (!IsValidEventTimeMs(cumulative_t_ms)) {
				res.status = 400;
				json err;
				err["error"] = "Accumulated event timeline exceeds " +
				               std::to_string(MaxEventTimeMs) + " ms";
				send_json(res, err);
				return;
			}
			ev.t_ms = cumulative_t_ms;
		} else {
			// No timing given: fire at the current timeline position
			ev.t_ms = cumulative_t_ms;
		}
		if (jev.contains("frame")) {
			// nlohmann silently reinterprets a negative literal as
			// its unsigned bit pattern on a direct get<uint64_t>(),
			// so the sign has to be checked before that conversion.
			const auto frame_val = jev["frame"].get<int64_t>();
			if (!IsValidEventFrame(frame_val)) {
				res.status = 400;
				json err;
				err["error"] = "'frame' must be between 0 and " +
				               std::to_string(MaxEventFrame);
				send_json(res, err);
				return;
			}
			ev.frame = static_cast<uint64_t>(frame_val);
		}

		if (type_str == "key") {
			ev.type             = InputEvent::Type::Key;
			const auto key_name = jev.value("key", "KBD_NONE");
			auto it             = key_name_map.find(key_name);
			if (it == key_name_map.end()) {
				res.status = 400;
				json err;
				err["error"] = "Unknown key: " + key_name;
				send_json(res, err);
				return;
			}
			ev.key     = static_cast<int>(it->second);
			ev.pressed = jev.value("pressed", true);
		} else if (type_str == "mouse_move") {
			ev.type  = InputEvent::Type::MouseMove;
			ev.x_rel = jev.value("x_rel", 0.0f);
			ev.y_rel = jev.value("y_rel", 0.0f);
			ev.x_abs = jev.value("x_abs", 0.0f);
			ev.y_abs = jev.value("y_abs", 0.0f);
		} else if (type_str == "mouse_button") {
			ev.type    = InputEvent::Type::MouseButton;
			ev.button  = jev.value("button", "left");
			ev.pressed = jev.value("pressed", true);
			if (button_name_map.find(ev.button) == button_name_map.end()) {
				res.status = 400;
				json err;
				err["error"] = "Unknown button: " + ev.button;
				send_json(res, err);
				return;
			}
		} else if (type_str == "mouse_wheel") {
			ev.type        = InputEvent::Type::MouseWheel;
			ev.wheel_delta = jev.value("delta", 0.0f);
		} else {
			res.status = 400;
			json err;
			err["error"] = "Unknown event type: " + type_str;
			send_json(res, err);
			return;
		}

		events.push_back(std::move(ev));
	}

	bool has_frame_data = false;
	for (const auto& jev : body["events"]) {
		if (jev.contains("frame")) {
			has_frame_data = true;
			break;
		}
	}

	InputSequenceCommand cmd(std::move(events), has_frame_data);
	cmd.WaitForCompletion(5000);

	if (!cmd.error.empty()) {
		res.status = 409;
		json err;
		err["error"] = cmd.error;
		send_json(res, err);
		return;
	}

	json result;
	result["status"]           = "ok";
	result["events_scheduled"] = body["events"].size();
	send_json(res, result);
}

// --- Input Recording ---

static std::mutex rec_mutex;
static bool rec_active = false;
static bool rec_paused = false;
static std::vector<InputEvent> rec_buffer;
static double rec_start_pic_ms;
static uint64_t rec_start_frame = 0;

static const std::unordered_map<int, std::string> button_id_to_name = {
        {0,   "left"},
        {1,  "right"},
        {2, "middle"},
};

static const std::unordered_map<int, std::string> key_id_to_name = [] {
	std::unordered_map<int, std::string> m;
	for (const auto& [name, key] : key_name_map) {
		m[static_cast<int>(key)] = name;
	}
	return m;
}();

static double rec_elapsed_ms_precise()
{
	return PIC_FullIndex() - rec_start_pic_ms;
}

static double rec_elapsed_ms_approx()
{
	return PIC_AtomicIndex() - rec_start_pic_ms;
}

void InputRecording::StartOnEmulationThread()
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	rec_buffer.clear();
	rec_active       = true;
	rec_paused       = false;
	rec_start_pic_ms = PIC_FullIndex();
	rec_start_frame  = GFX_GetRenderedFrameCount();
	TITLEBAR_NotifyApiRecordingStatus(true);
	OSD::OsdManager::Instance().SetIcon(OSD::IconId::RecordingActive, true);
}

void StartRecordingCommand::Execute()
{
	InputRecording::StartOnEmulationThread();
}

void PauseRecordingCommand::Execute()
{
	// Re-check on the emulation thread rather than trusting a
	// pre-check made on the httplib worker: recording could stop
	// between that check and this Command actually running.
	was_recording = InputRecording::IsRecording();
	if (!was_recording) {
		return;
	}
	InputRecording::Pause();
	is_paused = InputRecording::IsPaused();
}

void StopRecordingCommand::Execute()
{
	was_recording = InputRecording::Stop(events);
}

void InputRecording::Pause()
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	if (rec_active) {
		rec_paused = !rec_paused;
	}
}

bool InputRecording::Stop(std::vector<InputEvent>& out_events)
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	if (!rec_active) {
		return false;
	}
	rec_active = false;
	rec_paused = false;
	out_events = std::move(rec_buffer);
	rec_buffer.clear();
	TITLEBAR_NotifyApiRecordingStatus(false);
	OSD::OsdManager::Instance().SetIcon(OSD::IconId::RecordingActive, false);
	return true;
}

bool InputRecording::IsRecording()
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	return rec_active;
}

bool InputRecording::IsPaused()
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	return rec_paused;
}

size_t InputRecording::EventCount()
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	return rec_buffer.size();
}

double InputRecording::DurationMs()
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	if (!rec_active) {
		return 0;
	}
	return rec_elapsed_ms_approx();
}

void InputRecording::OnKeyEvent(int key, bool pressed)
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	if (!rec_active || rec_paused) {
		return;
	}
	InputEvent ev;
	ev.t_ms    = rec_elapsed_ms_precise();
	ev.frame   = GFX_GetRenderedFrameCount() - rec_start_frame;
	ev.type    = InputEvent::Type::Key;
	ev.key     = key;
	ev.pressed = pressed;
	rec_buffer.push_back(std::move(ev));
}

void InputRecording::OnMouseMove(float x_rel, float y_rel, float x_abs, float y_abs)
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	if (!rec_active || rec_paused) {
		return;
	}
	InputEvent ev;
	ev.t_ms  = rec_elapsed_ms_precise();
	ev.frame = GFX_GetRenderedFrameCount() - rec_start_frame;
	ev.type  = InputEvent::Type::MouseMove;
	ev.x_rel = x_rel;
	ev.y_rel = y_rel;
	ev.x_abs = x_abs;
	ev.y_abs = y_abs;
	rec_buffer.push_back(std::move(ev));
}

void InputRecording::OnMouseButton(const std::string& button, bool pressed)
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	if (!rec_active || rec_paused) {
		return;
	}
	InputEvent ev;
	ev.t_ms    = rec_elapsed_ms_precise();
	ev.frame   = GFX_GetRenderedFrameCount() - rec_start_frame;
	ev.type    = InputEvent::Type::MouseButton;
	ev.button  = button;
	ev.pressed = pressed;
	rec_buffer.push_back(std::move(ev));
}

void InputRecording::OnMouseWheel(float delta)
{
	std::lock_guard<std::mutex> lock(rec_mutex);
	if (!rec_active || rec_paused) {
		return;
	}
	InputEvent ev;
	ev.t_ms        = rec_elapsed_ms_precise();
	ev.frame       = GFX_GetRenderedFrameCount() - rec_start_frame;
	ev.type        = InputEvent::Type::MouseWheel;
	ev.wheel_delta = delta;
	rec_buffer.push_back(std::move(ev));
}

static void hook_keyboard(int key, bool pressed)
{
	if (in_replay_dispatch) {
		return;
	}
	InputRecording::OnKeyEvent(key, pressed);
}

static void hook_mouse_move(float x_rel, float y_rel, float x_abs, float y_abs)
{
	if (in_replay_dispatch) {
		return;
	}
	InputRecording::OnMouseMove(x_rel, y_rel, x_abs, y_abs);
}

static void hook_mouse_button(int button_id, bool pressed)
{
	if (in_replay_dispatch) {
		return;
	}
	auto it = button_id_to_name.find(button_id);
	if (it != button_id_to_name.end()) {
		InputRecording::OnMouseButton(it->second, pressed);
	}
}

static void hook_mouse_wheel(float delta)
{
	if (in_replay_dispatch) {
		return;
	}
	InputRecording::OnMouseWheel(delta);
}

void InputRecording::InstallHooks()
{
	keyboard_input_hook = hook_keyboard;
	mouse_move_hook     = hook_mouse_move;
	mouse_button_hook   = hook_mouse_button;
	mouse_wheel_hook    = hook_mouse_wheel;
}

static json event_to_json(const InputEvent& ev)
{
	json j;
	j["t"]     = ev.t_ms;
	j["frame"] = ev.frame;

	switch (ev.type) {
	case InputEvent::Type::Key: {
		j["type"] = "key";
		auto it   = key_id_to_name.find(ev.key);
		j["key"] = (it != key_id_to_name.end()) ? it->second : "KBD_NONE";
		j["pressed"] = ev.pressed;
		break;
	}
	case InputEvent::Type::MouseMove:
		j["type"]  = "mouse_move";
		j["x_rel"] = ev.x_rel;
		j["y_rel"] = ev.y_rel;
		j["x_abs"] = ev.x_abs;
		j["y_abs"] = ev.y_abs;
		break;
	case InputEvent::Type::MouseButton:
		j["type"]    = "mouse_button";
		j["button"]  = ev.button;
		j["pressed"] = ev.pressed;
		break;
	case InputEvent::Type::MouseWheel:
		j["type"]  = "mouse_wheel";
		j["delta"] = ev.wheel_delta;
		break;
	}
	return j;
}

void RecordingHandlers::PostStart(const httplib::Request&, httplib::Response& res)
{
	if (InputRecording::IsRecording()) {
		res.status = 409;
		json err;
		err["error"] = "Already recording";
		send_json(res, err);
		return;
	}
	StartRecordingCommand cmd;
	cmd.WaitForCompletion(1000);
	json j;
	j["status"] = "recording";
	send_json(res, j);
}

void RecordingHandlers::PostPause(const httplib::Request&, httplib::Response& res)
{
	PauseRecordingCommand cmd;
	cmd.WaitForCompletion(1000);
	if (!cmd.was_recording) {
		res.status = 409;
		json err;
		err["error"] = "Not recording";
		send_json(res, err);
		return;
	}
	json j;
	j["status"] = cmd.is_paused ? "paused" : "recording";
	send_json(res, j);
}

void RecordingHandlers::PostStop(const httplib::Request&, httplib::Response& res)
{
	StopRecordingCommand cmd;
	cmd.WaitForCompletion(1000);
	if (!cmd.was_recording) {
		res.status = 409;
		json err;
		err["error"] = "Not recording";
		send_json(res, err);
		return;
	}

	json j;
	j["event_count"] = cmd.events.size();
	j["events"]      = json::array();
	double duration  = 0;
	for (const auto& ev : cmd.events) {
		j["events"].push_back(event_to_json(ev));
		if (ev.t_ms > duration) {
			duration = ev.t_ms;
		}
	}
	j["duration_ms"] = duration;
	send_json(res, j);
}

void RecordingHandlers::GetStatus(const httplib::Request&, httplib::Response& res)
{
	json j;
	j["recording"]   = InputRecording::IsRecording();
	j["paused"]      = InputRecording::IsPaused();
	j["event_count"] = InputRecording::EventCount();
	j["duration_ms"] = InputRecording::DurationMs();
	send_json(res, j);
}

void ReplayHandlers::GetStatus(const httplib::Request&, httplib::Response& res)
{
	const auto status = InputReplay::GetStatus();

	json j;
	j["active"]        = status.active;
	j["engine"]        = status.engine;
	j["total"]         = status.total;
	j["dispatched"]    = status.dispatched;
	j["remaining"]     = status.total - status.dispatched;
	j["elapsed_ms"]    = status.elapsed_ms;
	j["drift_ms"]      = status.drift_ms;
	j["current_frame"] = status.current_frame;
	send_json(res, j);
}

void ReplayCancelCommand::Execute()
{
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (!pending_events.empty()) {
			cancelled_pic = true;
			std::queue<InputEvent>().swap(pending_events);
			// Freeze elapsed_ms at the actual wall-clock duration
			// up to this cancellation, not the time of the last
			// dispatch - which would understate it by however long
			// the chain had already been idly waiting for its next
			// scheduled event.
			pending_elapsed_ms = std::chrono::duration<double, std::milli>(
			                             std::chrono::steady_clock::now() -
			                             replay_start_wall)
			                             .count();
			mark_last_engine("pic");
		}
		pending_backpressure_since.reset();
	}
	// Cancels any still-scheduled pic_input_handler invocation (the
	// delayed dispatch of the next event, or a pending backpressure
	// retry). Safe to call unconditionally even if nothing was
	// scheduled. Must run here, on the emulation thread - PIC_RemoveEvents
	// walks the raw PIC event list with no locking of its own.
	PIC_RemoveEvents(pic_input_handler);

	{
		std::lock_guard<std::mutex> lock(frame_replay_mutex);
		if (frame_replay_active) {
			cancelled_frame     = true;
			frame_replay_active = false;
			std::queue<InputEvent>().swap(frame_pending_events);
			frame_elapsed_ms = std::chrono::duration<double, std::milli>(
			                           std::chrono::steady_clock::now() -
			                           frame_replay_start_wall)
			                           .count();
			mark_last_engine("frame");
		}
		frame_backpressure_since.reset();
	}

	if (cancelled_pic || cancelled_frame) {
		TITLEBAR_NotifyApiReplayStatus(false);
		OSD::OsdManager::Instance().SetIcon(OSD::IconId::ReplayActive, false);
		LOG_MSG("REPLAY: cancelled by API (%s%s%s)",
		        cancelled_pic ? "pic" : "",
		        (cancelled_pic && cancelled_frame) ? "+" : "",
		        cancelled_frame ? "frame" : "");
	}
}

void ReplayCancelCommand::Delete(const httplib::Request&, httplib::Response& res)
{
	ReplayCancelCommand cmd;
	cmd.WaitForCompletion(1000);

	if (!cmd.error.empty()) {
		res.status = 500;
		json err;
		err["error"] = cmd.error;
		send_json(res, err);
		return;
	}

	json j;
	j["cancelled"] = cmd.cancelled_pic || cmd.cancelled_frame;
	send_json(res, j);
}

bool InputReplay::IsActive()
{
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (!pending_events.empty()) {
			return true;
		}
	}
	std::lock_guard<std::mutex> lock(frame_replay_mutex);
	return frame_replay_active;
}

ReplayStatus InputReplay::GetStatus()
{
	// Both locks held together for the whole snapshot below, not
	// released between the two engines' reads: every mutator in this
	// file only ever locks one of these two at a time (never both), so
	// holding both here blocks every mutator out for the duration and
	// makes the combined snapshot atomic. Without this, a chain could
	// arm or finish in the gap between two separately-scoped locks,
	// producing a combined result (e.g. active:false with a stale
	// engine attribution) that was never true at any single instant.
	std::lock_guard<std::mutex> pic_lock(pending_mutex);
	std::lock_guard<std::mutex> frame_lock(frame_replay_mutex);

	const bool pic_active       = !pending_events.empty();
	const size_t pic_total      = pending_total;
	const size_t pic_dispatched = pending_dispatched;
	const double pic_drift_ms   = pending_drift_ms;
	const double pic_elapsed_ms =
	        pic_active ? std::chrono::duration<double, std::milli>(
	                             std::chrono::steady_clock::now() - replay_start_wall)
	                             .count()
	                   : pending_elapsed_ms;

	const bool fr_active       = frame_replay_active;
	const size_t fr_total      = frame_pending_total;
	const size_t fr_dispatched = frame_pending_dispatched;
	const double fr_drift_ms   = frame_drift_ms;
	const double fr_elapsed_ms = fr_active
	                                   ? std::chrono::duration<double, std::milli>(
	                                             std::chrono::steady_clock::now() -
	                                             frame_replay_start_wall)
	                                             .count()
	                                   : frame_elapsed_ms;

	ReplayStatus status;

	if (pic_active && fr_active) {
		// Two independent POST /input/sequence calls (one with frame
		// data, one without) can each arm their own chain - each
		// engine only checks its own state, not the other's. Merge
		// rather than pick one arbitrarily, so an agent that manages
		// to hit this edge case at least sees both chains' progress
		// instead of one going silently unreported.
		status.active     = true;
		status.engine     = "mixed";
		status.total      = pic_total + fr_total;
		status.dispatched = pic_dispatched + fr_dispatched;
		status.elapsed_ms = std::max(pic_elapsed_ms, fr_elapsed_ms);
		// Whichever chain is further off schedule, not arbitrarily the
		// PIC one - a caller checking drift wants the worse-behaving
		// chain, same reasoning as elapsed_ms's max() above.
		status.drift_ms = std::abs(pic_drift_ms) >= std::abs(fr_drift_ms)
		                        ? pic_drift_ms
		                        : fr_drift_ms;
	} else if (pic_active) {
		status.active     = true;
		status.engine     = "pic";
		status.total      = pic_total;
		status.dispatched = pic_dispatched;
		status.elapsed_ms = pic_elapsed_ms;
		status.drift_ms   = pic_drift_ms;
	} else if (fr_active) {
		status.active     = true;
		status.engine     = "frame";
		status.total      = fr_total;
		status.dispatched = fr_dispatched;
		status.elapsed_ms = fr_elapsed_ms;
		status.drift_ms   = fr_drift_ms;
	} else {
		// Neither chain is active: attribute to whichever one last
		// ran, so a status check right after completion (or a stall
		// abort) still reports the finished run's numbers instead of
		// an all-zero result that looks like nothing ever happened.
		std::lock_guard<std::mutex> lock(last_engine_mutex);
		status.engine = last_engine;
		if (last_engine == "frame") {
			status.total      = fr_total;
			status.dispatched = fr_dispatched;
			status.elapsed_ms = fr_elapsed_ms;
			status.drift_ms   = fr_drift_ms;
		} else if (last_engine == "pic") {
			status.total      = pic_total;
			status.dispatched = pic_dispatched;
			status.elapsed_ms = pic_elapsed_ms;
			status.drift_ms   = pic_drift_ms;
		}
	}

	status.current_frame = GFX_GetRenderedFrameCount();
	return status;
}

void ReplayDispatchFrame(uint64_t current_frame)
{
	if (!frame_replay_active) {
		return;
	}

	std::lock_guard<std::mutex> lock(frame_replay_mutex);

	const auto relative_frame   = current_frame - frame_replay_start;
	int dispatched_this_frame   = 0;
	double last_dispatched_t_ms = 0;
	bool stuck_on_backpressure  = false;

	while (!frame_pending_events.empty() &&
	       frame_pending_events.front().frame <= relative_frame &&
	       dispatched_this_frame < 500) {
		const auto& ev = frame_pending_events.front();

		// Hold key events until the keyboard buffer has room
		if (ev.type == InputEvent::Type::Key &&
		    KEYBOARD_GetBufferFreeSlots() < 2) {
			stuck_on_backpressure = true;
			break;
		}

		auto dispatched_ev = ev;
		frame_pending_events.pop();
		++frame_pending_dispatched;
		++dispatched_this_frame;
		last_dispatched_t_ms = dispatched_ev.t_ms;

		dispatch_input_event(dispatched_ev);

		// OSD feedback for visible replay events
		if (dispatched_ev.type == InputEvent::Type::Key &&
		    dispatched_ev.pressed) {
			auto it = key_id_to_name.find(dispatched_ev.key);
			if (it != key_id_to_name.end()) {
				OSD_ShowCommand("replay: " + it->second,
				                current_frame);
			}
		} else if (dispatched_ev.type == InputEvent::Type::MouseButton) {
			std::string label = dispatched_ev.pressed
			                          ? "replay: click "
			                          : "replay: release ";
			OSD_ShowCommand(label + dispatched_ev.button, current_frame);
		}
	}

	if (dispatched_this_frame > 0) {
		const double wall_ms = std::chrono::duration<double, std::milli>(
		                               std::chrono::steady_clock::now() -
		                               frame_replay_start_wall)
		                               .count();
		const double expected_ms = last_dispatched_t_ms -
		                           frame_replay_first_t_ms;
		const double wall_drift  = wall_ms - expected_ms;

		frame_elapsed_ms = wall_ms;
		frame_drift_ms   = wall_drift;

		if (frame_pending_dispatched % 100 == 0 ||
		    dispatched_this_frame >= 10) {
			LOG_MSG("REPLAY frame %llu: %d events (%zu/%zu), "
			        "wall=%.1fms expected=%.1fms drift=%+.1fms",
			        static_cast<unsigned long long>(relative_frame),
			        dispatched_this_frame,
			        frame_pending_dispatched,
			        frame_pending_total,
			        wall_ms,
			        expected_ms,
			        wall_drift);
		}
	}

	// dispatched_this_frame > 0 means the front event blocking now (if
	// any) had genuine progress dispatch ahead of it, so it isn't the
	// same stall a previous call may have started timing - start its
	// clock fresh rather than inheriting an unrelated event's elapsed
	// stuck time. Without this, a chain throttled to ~1 event per frame
	// by keyboard backpressure - genuine, continuous progress - would
	// look stuck on every single call (some events dispatch, then the
	// next due one blocks) and self-abort after ReplayStallThresholdMs
	// despite never actually being wedged.
	if (dispatched_this_frame > 0) {
		frame_backpressure_since.reset();
	}

	if (stuck_on_backpressure) {
		const auto now = std::chrono::steady_clock::now();
		if (!frame_backpressure_since) {
			frame_backpressure_since = now;
		} else if (std::chrono::duration<double, std::milli>(
		                   now - *frame_backpressure_since)
		                   .count() >= ReplayStallThresholdMs) {
			const auto dropped = frame_pending_events.size();
			std::queue<InputEvent>().swap(frame_pending_events);
			frame_replay_active = false;
			frame_backpressure_since.reset();
			frame_elapsed_ms = std::chrono::duration<double, std::milli>(
			                           now - frame_replay_start_wall)
			                           .count();
			mark_last_engine("frame");
			TITLEBAR_NotifyApiReplayStatus(false);
			OSD::OsdManager::Instance().SetIcon(OSD::IconId::ReplayActive,
			                                    false);
			LOG_WARNING(
			        "REPLAY (frame): chain aborted, stuck %.0fms "
			        "waiting for keyboard buffer space - %zu event(s) "
			        "never dispatched (%zu/%zu total)",
			        ReplayStallThresholdMs,
			        dropped,
			        frame_pending_dispatched,
			        frame_pending_total);
			return;
		}
	}

	if (dispatched_this_frame >= 500) {
		LOG_WARNING("WEBSERVER: Replay frame %llu hit 500-event cap",
		            static_cast<unsigned long long>(relative_frame));
	}

	if (frame_pending_events.empty()) {
		frame_replay_active = false;
		TITLEBAR_NotifyApiReplayStatus(false);
		OSD::OsdManager::Instance().SetIcon(OSD::IconId::ReplayActive, false);
		const double wall_ms = std::chrono::duration<double, std::milli>(
		                               std::chrono::steady_clock::now() -
		                               frame_replay_start_wall)
		                               .count();
		const double expected_ms = last_dispatched_t_ms -
		                           frame_replay_first_t_ms;
		const double wall_drift  = wall_ms - expected_ms;
		frame_elapsed_ms         = wall_ms;
		frame_drift_ms           = wall_drift;
		mark_last_engine("frame");
		LOG_MSG("REPLAY (frame) complete: %zu/%zu events, %llu frames, "
		        "wall=%.1fms expected=%.1fms drift=%+.1fms",
		        frame_pending_dispatched,
		        frame_pending_total,
		        static_cast<unsigned long long>(relative_frame),
		        wall_ms,
		        expected_ms,
		        wall_drift);
	}
}

// --- Input Type (text injection over REST) ---

std::vector<InputEvent> ExpandTextToEvents(const std::string_view text,
                                           const double cps)
{
	const double rate_cps = (cps > 0.0) ? cps : 30.0;
	const double step_ms  = 1000.0 / rate_cps;

	std::vector<InputEvent> events;
	double t_ms = 0;

	for (const auto stroke : Lua::TextToStrokes(text)) {
		auto push = [&](KBD_KEYS key, bool pressed) {
			InputEvent ev = {};
			ev.t_ms       = t_ms;
			ev.type       = InputEvent::Type::Key;
			ev.key        = static_cast<int>(key);
			ev.pressed    = pressed;
			events.push_back(ev);
		};

		if (stroke.shift) {
			push(KBD_leftshift, true);
		}
		push(stroke.key, true);
		push(stroke.key, false);
		if (stroke.shift) {
			push(KBD_leftshift, false);
		}
		t_ms += step_ms;
	}
	return events;
}

void InputTypeCommand::Execute()
{
	InputSequenceCommand seq(std::move(events), /*has_frame_data=*/false);
	seq.Execute();
	error = seq.error;
}

void InputTypeCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto body = json::parse(req.body);

	if (!body.contains("text") || !body["text"].is_string()) {
		res.status = 400;
		json err;
		err["error"] = "Missing or invalid 'text' string";
		send_json(res, err);
		return;
	}

	const auto text = body["text"].get<std::string>();

	if (text.size() > MaxTypedTextChars) {
		res.status = 400;
		json err;
		err["error"] = "Text too long (max " +
		               std::to_string(MaxTypedTextChars) + " chars)";
		send_json(res, err);
		return;
	}

	const double cps = body.value("cps", 30.0);

	if (!IsValidTypingCps(cps)) {
		res.status = 400;
		json err;
		err["error"] = "'cps' must be between " +
		               std::to_string(MinTypingCps) + " and " +
		               std::to_string(MaxTypingCps);
		send_json(res, err);
		return;
	}

	InputTypeCommand cmd(ExpandTextToEvents(text, cps));
	cmd.WaitForCompletion(5000);

	if (!cmd.error.empty()) {
		res.status = 409;
		json err;
		err["error"] = cmd.error;
		send_json(res, err);
		return;
	}

	json result;
	result["status"] = "ok";
	result["chars"]  = text.size();
	send_json(res, result);
}

} // namespace Webserver
