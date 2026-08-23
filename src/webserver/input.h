// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_INPUT_H
#define DOSBOX_WEBSERVER_INPUT_H

#include "bridge.h"
#include "libs/http/http.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Webserver {

struct InputEvent {
	double t_ms    = 0;
	uint64_t frame = 0;
	enum class Type {
		Key,
		MouseMove,
		MouseButton,
		MouseWheel
	} type = Type::Key;

	// Key params
	int key      = 0;
	bool pressed = false;

	// Mouse move params
	float x_rel = 0, y_rel = 0;
	float x_abs = 0, y_abs = 0;

	// Mouse button params
	std::string button = {};

	// Mouse wheel params
	float wheel_delta = 0;
};

// Upper bound for an event's position on the replay timeline ('t',
// 'delay_ms' after accumulation, and 'cps'-derived timing). PIC_AddEvent
// (hardware/pic.h) turns this delay into `CPU_CycleMax * delay_ms` CPU
// cycles and asserts the result fits an int32_t; an unbounded value from
// the request body reaches that assert directly (or is undefined
// behaviour with asserts compiled out). No legitimate automation
// sequence needs a timeline longer than this.
inline constexpr double MaxEventTimeMs = 24.0 * 60 * 60 * 1000; // 24 hours

// Generous upper bound for a frame-relative event's target frame (a
// billion frames is months of continuous playback at any real frame
// rate). Guards against the same class of "hangs the replay state
// forever" input as MaxEventTimeMs, for the frame-based scheduler.
inline constexpr uint64_t MaxEventFrame = 1'000'000'000;

// A cps near zero makes ExpandTextToEvents' step_ms (1000/cps) huge:
// accumulated across up to MaxTypedTextChars characters, that reaches the
// same PIC_AddEvent overflow MaxEventTimeMs guards against. MinTypingCps
// is already an unrealistically slow "one keystroke every 10 seconds";
// MaxTypedTextChars characters at that rate stays comfortably under
// MaxEventTimeMs.
inline constexpr double MinTypingCps = 0.1;
inline constexpr double MaxTypingCps = 1000.0;

// Max events in one /api/v1/input/sequence body, and max characters in
// one /api/v1/input/type body. Also the cap on a single recording's
// rec_buffer (see InputRecording::OnKeyEvent et al.): the same limit
// either way, since a recording that couldn't fit in one /input/sequence
// body couldn't be replayed by it either.
inline constexpr size_t MaxInputEvents    = 32000;
inline constexpr size_t MaxTypedTextChars = 4096;

// Named recording store limits. MaxRecordingNameLength matches
// Lua::ScriptValidator::ValidateParams' rule for script names -
// [A-Za-z0-9_-], <=64 chars - the same "safe as a filename or map key"
// bar. MaxStoredRecordings bounds worst-case memory: each recording can
// hold up to MaxInputEvents events, so the store's ceiling is deliberately
// modest (roughly the same order of magnitude as SnapshotRegistry's
// 32 MB total-bytes cap, in spirit if not in exact accounting).
inline constexpr size_t MaxRecordingNameLength = 64;
inline constexpr size_t MaxStoredRecordings    = 20;

// A key event that can't dispatch (keyboard buffer full) retries every
// backpressure_retry_ms (PIC engine) or every frame (frame engine)
// indefinitely - correct for a guest that's briefly slow to drain its
// buffer, but a guest that never will (hung, crashed, or never reading
// input) would otherwise wedge the chain forever and leave every later
// POST /input/sequence refused with 409. Once the *same* front event has
// been stuck this long, the chain self-aborts rather than waiting
// indefinitely. Deliberately not tied to "time since the last dispatch"
// in general: a legitimate sequence can have long, intentional gaps
// between events (e.g. a 10s wait for a menu to load), and those must
// never be mistaken for a stall.
inline constexpr double ReplayStallThresholdMs = 5000.0;

// Bounds for an event's position on the replay timeline and a
// frame-relative event's target frame. PIC_AddEvent (hardware/pic.h)
// turns an event's time into CPU cycles and asserts the result fits an
// int32_t; these reject anything that would reach that assert (or be
// undefined behaviour with asserts compiled out) before it does.
// Exposed for testing.
bool IsValidEventTimeMs(double t_ms);
bool IsValidEventFrame(int64_t frame);

// Typing rate bounds for InputTypeCommand::Post's 'cps'. A rate near
// zero derives event timing far past IsValidEventTimeMs' bound just as
// surely as an out-of-range 't' or 'delay_ms' does. Exposed for testing.
bool IsValidTypingCps(double cps);

class InputSequenceCommand : public Command {
public:
	InputSequenceCommand(std::vector<InputEvent> events, bool has_frame_data);
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

private:
	void ExecuteFrameBased();
	void ExecutePicBased();

	std::vector<InputEvent> events = {};
	bool has_frame_data            = false;
};

// Expand text into timed key press/release events on the US layout,
// paced at `cps` characters per second. Shifted characters wrap the
// base key in a shift down/up pair. Exposed for testing.
std::vector<InputEvent> ExpandTextToEvents(std::string_view text, double cps);

class InputTypeCommand : public Command {
public:
	InputTypeCommand(std::vector<InputEvent> events)
	        : events(std::move(events))
	{}
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

private:
	std::vector<InputEvent> events = {};
};

class StartRecordingCommand : public Command {
public:
	void Execute() override;
};

// InputRecording::Pause/Stop must run on the emulation thread: Stop
// touches OsdManager (no mutex, read by Render on the emulation thread),
// so calling it directly from the httplib worker thread is a data race.
class PauseRecordingCommand : public Command {
public:
	void Execute() override;

	bool was_recording = false;
	bool is_paused     = false;
};

class StopRecordingCommand : public Command {
public:
	void Execute() override;

	bool was_recording             = false;
	std::vector<InputEvent> events = {};
	// Whether MaxInputEvents was hit and further events were dropped
	// during this recording (see RecordingHandlers::PostStop).
	bool truncated = false;
};

namespace InputRecording {
void StartOnEmulationThread();
void Pause();
bool Stop(std::vector<InputEvent>& out_events, bool& out_truncated);
bool IsRecording();
bool IsPaused();
size_t EventCount();
double DurationMs();
bool IsTruncated();

void OnKeyEvent(int key, bool pressed);
void OnMouseMove(float x_rel, float y_rel, float x_abs, float y_abs);
void OnMouseButton(const std::string& button, bool pressed);
void OnMouseWheel(float delta);

void InstallHooks();
} // namespace InputRecording

struct RecordingHandlers {
	static void PostStart(const httplib::Request&, httplib::Response& res);
	static void PostPause(const httplib::Request&, httplib::Response& res);
	static void PostStop(const httplib::Request& req, httplib::Response& res);
	static void GetStatus(const httplib::Request&, httplib::Response& res);
};

// Process-lifetime, in-memory named recording store - zero filesystem
// surface. Populated only from RecordingHandlers::PostStop (after a
// StopRecordingCommand has already completed) and read from
// InputSequenceCommand::Post (resolving a {"recording":"<name>"} body
// into an already-validated event vector before the Command is even
// constructed). Every function here is web-thread-only: the emulation
// thread never touches this store.
namespace RecordingStore {
// <=MaxRecordingNameLength chars, [A-Za-z0-9_-] - same rule as
// Lua::ScriptValidator::ValidateParams uses for script names.
bool IsValidName(const std::string& name);

// False only if `name` doesn't already exist and the store is at
// MaxStoredRecordings capacity - callers should check this before
// stopping a recording they intend to name, so a full store fails the
// request up front rather than after the recording has already ended.
bool HasRoom(const std::string& name);

// Saves (or overwrites) a named recording.
void Save(const std::string& name, std::vector<InputEvent> events,
          bool truncated, double duration_ms);

struct Entry {
	size_t event_count = 0;
	double duration_ms = 0;
	bool truncated     = false;
};

// Metadata for every stored recording, name paired with its Entry.
std::vector<std::pair<std::string, Entry>> List();

// A copy of the stored events for `name`, or nullopt if no recording
// by that name exists. A copy, not a move: a named recording is meant
// to be replayed repeatedly, so resolving it must never consume or
// invalidate the stored original.
std::optional<std::vector<InputEvent>> Get(const std::string& name);

// True if a recording named `name` existed and was removed.
bool Delete(const std::string& name);
} // namespace RecordingStore

struct RecordingStoreHandlers {
	static void GetList(const httplib::Request&, httplib::Response& res);
	static void Delete(const httplib::Request& req, httplib::Response& res);
};

void ReplayDispatchFrame(uint64_t current_frame);

// Snapshot for GET /api/v1/input/replay/status. `engine` is "pic",
// "frame", "mixed" (both chains happen to be active at once - two
// separate POST /input/sequence calls, one with frame data and one
// without, can each start their own chain independently; rare, but not
// prevented), or "none" (no replay has run yet this session). A
// finished or self-aborted chain keeps reporting its final
// total/dispatched/elapsed_ms/drift_ms rather than zeroing them out -
// checking status right after a replay ends (or a stall aborts it) is
// the normal sequence.
struct ReplayStatus {
	bool active            = false;
	std::string engine     = "none";
	size_t total           = 0;
	size_t dispatched      = 0;
	double elapsed_ms      = 0;
	double drift_ms        = 0;
	uint64_t current_frame = 0;
};

namespace InputReplay {
// True while either the PIC-timed or frame-timed replay chain still has
// events left to dispatch. Safe to call from any thread - locks the
// same mutexes the dispatch paths already use.
bool IsActive();

// Safe to call from any thread, same as IsActive() - reads emulation
// state through the same mutexes the dispatch paths use, the same
// precedent GET /api/v1/status already sets for GFX_GetRenderedFrameCount:
// a Bridge Command here would block or time out in exactly the stalled
// case this is meant to report.
ReplayStatus GetStatus();
} // namespace InputReplay

// DELETE /api/v1/input/replay: drain both chains, cancel any pending PIC
// event for the PIC-timed one (PIC_RemoveEvents must run on the
// emulation thread - pic_input_handler already takes pending_mutex
// there, so this is not a new lock-ordering risk), and clear the
// titlebar/OSD replay flags regardless of which chain (if either) was
// actually active.
class ReplayCancelCommand : public Command {
public:
	void Execute() override;
	static void Delete(const httplib::Request& req, httplib::Response& res);

	bool cancelled_pic   = false;
	bool cancelled_frame = false;
};

struct ReplayHandlers {
	static void GetStatus(const httplib::Request&, httplib::Response& res);
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_INPUT_H
