"""Contract tests for the dosbox-automation webserver API.

Every endpoint is exercised against a live headless DOSBox instance.
These tests define the API contract — any failure after a rebase
indicates cherry-pick damage.
"""

import base64
import json
import struct
import time
from io import BytesIO

import pytest
from PIL import Image


# ---------------------------------------------------------------------------
# Status & info endpoints
# ---------------------------------------------------------------------------

def test_dosbox_is_running(dosbox):
    r = dosbox.status()
    assert r.status_code == 200
    data = r.json()
    assert data["running"] is True


def test_status_shape(dosbox):
    r = dosbox.status()
    assert r.status_code == 200
    data = r.json()
    assert "running" in data
    assert "shutdown_requested" in data
    assert "is_booted" in data
    assert "program" in data
    assert "canonical_name" in data
    assert "is_shell" in data
    assert data["shutdown_requested"] is False
    assert data["is_shell"] is True


def test_program_state_at_shell(dosbox):
    r = dosbox.program_state()
    assert r.status_code == 200
    data = r.json()
    assert "segment_name" in data
    assert "canonical_name" in data
    assert "is_shell" in data
    assert "is_booted" in data
    assert data["is_shell"] is True


def test_dosbox_info(dosbox):
    r = dosbox.dosbox_info()
    assert r.status_code == 200
    data = r.json()
    assert "version" in data
    assert len(data["version"]) > 0
    assert "configHome" not in data
    assert "configWebserver" not in data


def test_dosbox_info_reports_features(dosbox):
    r = dosbox.dosbox_info()
    assert r.status_code == 200
    data = r.json()
    assert "features" in data
    features = data["features"]
    for key in ("memory", "input", "cpu_registers", "port_io", "freeze"):
        assert features.get(key) is True, f"{key} should be true"
    assert features["cpu_control"] is True
    assert features["debugger"] is False


# ---------------------------------------------------------------------------
# CPU & DOS internals
# ---------------------------------------------------------------------------

def test_cpu_state(dosbox):
    r = dosbox.cpu_state()
    assert r.status_code == 200
    data = r.json()
    regs = data["registers"]
    for name in ("eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp",
                 "eip", "flags", "cs", "ds", "es", "ss", "fs", "gs"):
        assert name in regs, f"Missing register: {name}"
        assert isinstance(regs[name], int)


def test_dos_internals(dosbox):
    r = dosbox.dos_internals()
    assert r.status_code == 200
    data = r.json()
    assert "listOfLists" in data
    assert "dosSwappableArea" in data
    assert "firstShell" in data
    assert data["listOfLists"] > 0
    assert data["dosSwappableArea"] > 0
    assert data["firstShell"] > 0


# ---------------------------------------------------------------------------
# Input injection — valid inputs
# ---------------------------------------------------------------------------

def test_input_keypress(dosbox):
    r = dosbox.press_key("KBD_a")
    assert r.status_code == 200
    data = r.json()
    assert data["status"] == "ok"
    assert data["events_scheduled"] == 1


def test_input_empty_events(dosbox):
    r = dosbox.input_sequence([])
    assert r.status_code == 200
    assert r.json()["events_scheduled"] == 0


def test_input_mouse_move(dosbox):
    r = dosbox.input_sequence([{
        "type": "mouse_move", "x_rel": 5.0, "y_rel": -3.0,
    }])
    assert r.status_code == 200
    assert r.json()["status"] == "ok"


def test_input_mouse_button(dosbox):
    r = dosbox.input_sequence([{
        "type": "mouse_button", "button": "left", "pressed": True,
    }])
    assert r.status_code == 200


def test_input_mouse_wheel(dosbox):
    r = dosbox.input_sequence([{
        "type": "mouse_wheel", "delta": 1.0,
    }])
    assert r.status_code == 200


def test_input_timed_events(dosbox):
    r = dosbox.input_sequence([
        {"t": 0, "type": "key", "key": "KBD_h", "pressed": True},
        {"t": 50, "type": "key", "key": "KBD_h", "pressed": False},
        {"t": 100, "type": "key", "key": "KBD_i", "pressed": True},
        {"t": 150, "type": "key", "key": "KBD_i", "pressed": False},
    ])
    assert r.status_code == 200
    assert r.json()["events_scheduled"] == 4


def test_input_delay_ms_relative_timing(dosbox):
    # delay_ms is the hand-written form: wait relative to the previous
    # event instead of an absolute timeline position. A 409 means an
    # earlier test's timed chain is still draining; that is the
    # documented busy contract, so retry briefly.
    events = [
        {"type": "mouse_move", "x_rel": -4000, "y_rel": -4000},
        {"type": "mouse_move", "x_rel": 160, "y_rel": 105, "delay_ms": 100},
        {"type": "mouse_button", "button": "left", "pressed": True,
         "delay_ms": 100},
        {"type": "mouse_button", "button": "left", "pressed": False,
         "delay_ms": 50},
    ]
    deadline = time.monotonic() + 5.0
    while True:
        r = dosbox.input_sequence(events)
        if r.status_code != 409 or time.monotonic() > deadline:
            break
        time.sleep(0.1)
    assert r.status_code == 200
    assert r.json()["events_scheduled"] == 4


def test_input_recording_fields_still_accepted(dosbox):
    # Recorded replays carry x_abs/y_abs metadata alongside the deltas;
    # they must keep replaying
    r = dosbox.input_sequence([{
        "type": "mouse_move",
        "t": 0, "frame": 1,
        "x_rel": 2.0, "y_rel": 3.0, "x_abs": 100.0, "y_abs": 50.0,
    }])
    assert r.status_code == 200


# ---------------------------------------------------------------------------
# Input injection — validation errors
# ---------------------------------------------------------------------------

def test_input_missing_events_key(dosbox):
    r = dosbox.input_sequence_raw(json.dumps({"wrong": []}))
    assert r.status_code == 400
    assert "events" in r.json()["error"].lower()


def test_input_events_not_array(dosbox):
    r = dosbox.input_sequence_raw(json.dumps({"events": "not-array"}))
    assert r.status_code == 400


def test_input_unknown_key(dosbox):
    r = dosbox.input_sequence([{"type": "key", "key": "KBD_NONEXISTENT"}])
    assert r.status_code == 400
    assert "KBD_NONEXISTENT" in r.json()["error"]


def test_input_unknown_event_type(dosbox):
    r = dosbox.input_sequence([{"type": "teleport"}])
    assert r.status_code == 400
    assert "teleport" in r.json()["error"]


def test_input_unknown_button(dosbox):
    r = dosbox.input_sequence([{"type": "mouse_button", "button": "extra"}])
    assert r.status_code == 400
    assert "extra" in r.json()["error"]


def test_input_unknown_field_rejected(dosbox):
    # 'x'/'y' instead of 'x_rel'/'y_rel' must fail loudly, not inject
    # silent zero-motion events (found on the first MCP field test)
    r = dosbox.input_sequence([{"type": "mouse_move", "x": 160, "y": 105}])
    assert r.status_code == 400
    error = r.json()["error"]
    assert "'x'" in error
    assert "x_rel" in error  # the error must name the correct field


def test_input_unknown_field_rejected_for_key_event(dosbox):
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_a", "presed": True},  # typo
    ])
    assert r.status_code == 400
    assert "'presed'" in r.json()["error"]


def test_input_t_and_delay_ms_conflict(dosbox):
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_a", "t": 100, "delay_ms": 100},
    ])
    assert r.status_code == 400
    assert "not both" in r.json()["error"]


def test_input_negative_delay_rejected(dosbox):
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_a", "delay_ms": -5},
    ])
    assert r.status_code == 400
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_a", "t": -5},
    ])
    assert r.status_code == 400


# ---------------------------------------------------------------------------
# Replay status and cancel
# ---------------------------------------------------------------------------

def test_replay_status_when_idle(dosbox):
    dosbox.replay_cancel()  # in case an earlier test in this module left one running
    r = dosbox.replay_status()
    assert r.status_code == 200
    data = r.json()
    assert data["active"] is False
    assert data["total"] == 0
    assert data["dispatched"] == 0
    assert data["remaining"] == 0
    assert "current_frame" in data


def test_replay_status_reports_progress_for_pic_sequence(dosbox):
    # Two events a second apart (no 'frame' field -> PIC-timed engine),
    # so there's a real window to observe mid-flight progress in.
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_a", "pressed": True, "delay_ms": 0},
        {"type": "key", "key": "KBD_a", "pressed": False, "delay_ms": 1000},
    ])
    assert r.status_code == 200

    time.sleep(0.2)
    r = dosbox.replay_status()
    assert r.status_code == 200
    data = r.json()
    assert data["active"] is True
    assert data["engine"] == "pic"
    assert data["total"] == 2
    assert data["dispatched"] == 1
    assert data["remaining"] == 1
    assert data["elapsed_ms"] > 0

    # Let it finish, then confirm the finished run's numbers are
    # retained (not zeroed) and frozen (not still climbing).
    time.sleep(1.2)
    r = dosbox.replay_status()
    data = r.json()
    assert data["active"] is False
    assert data["dispatched"] == 2
    assert data["total"] == 2
    frozen_elapsed = data["elapsed_ms"]

    time.sleep(0.3)
    r = dosbox.replay_status()
    assert r.json()["elapsed_ms"] == frozen_elapsed


def test_replay_status_reports_progress_for_frame_sequence(dosbox):
    # A 'frame' field on any event routes the whole sequence through the
    # frame-timed engine instead of the PIC-timed one.
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_b", "pressed": True, "frame": 0, "t": 0},
        {"type": "key", "key": "KBD_b", "pressed": False, "frame": 200, "t": 3000},
    ])
    assert r.status_code == 200

    time.sleep(0.1)
    r = dosbox.replay_status()
    data = r.json()
    assert data["active"] is True
    assert data["engine"] == "frame"

    dosbox.replay_cancel()


def test_replay_cancel_stops_a_running_sequence(dosbox):
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_c", "pressed": True, "delay_ms": 0},
        {"type": "key", "key": "KBD_c", "pressed": False, "delay_ms": 5000},
    ])
    assert r.status_code == 200
    time.sleep(0.1)

    r = dosbox.replay_cancel()
    assert r.status_code == 200
    assert r.json()["cancelled"] is True

    r = dosbox.replay_status()
    data = r.json()
    assert data["active"] is False
    assert data["dispatched"] < data["total"]

    # The queue was actually drained, not just marked inactive - a new
    # sequence must be free to start right away rather than 409ing.
    r = dosbox.input_sequence([
        {"type": "key", "key": "KBD_c", "pressed": True},
        {"type": "key", "key": "KBD_c", "pressed": False},
    ])
    assert r.status_code == 200


def test_replay_cancel_when_nothing_active(dosbox):
    r = dosbox.replay_cancel()
    assert r.status_code == 200
    assert r.json()["cancelled"] is False


# ---------------------------------------------------------------------------
# Recording lifecycle
# ---------------------------------------------------------------------------

def test_recording_lifecycle(dosbox):
    # Start
    r = dosbox.recording_start()
    assert r.status_code == 200
    assert r.json()["status"] == "recording"

    # Status while recording
    r = dosbox.recording_status()
    assert r.status_code == 200
    assert r.json()["recording"] is True
    assert r.json()["paused"] is False

    # Pause
    r = dosbox.recording_pause()
    assert r.status_code == 200
    assert r.json()["status"] == "paused"

    # Status while paused
    r = dosbox.recording_status()
    assert r.json()["paused"] is True
    assert r.json()["recording"] is True

    # Resume
    r = dosbox.recording_pause()
    assert r.status_code == 200
    assert r.json()["status"] == "recording"

    # Stop (empty)
    r = dosbox.recording_stop()
    assert r.status_code == 200
    data = r.json()
    assert "event_count" in data
    assert "duration_ms" in data
    assert "events" in data
    assert isinstance(data["events"], list)


def test_recording_error_stop_when_not_recording(dosbox):
    r = dosbox.recording_stop()
    assert r.status_code == 409
    assert "not recording" in r.json()["error"].lower()


def test_recording_error_pause_when_not_recording(dosbox):
    r = dosbox.recording_pause()
    assert r.status_code == 409
    assert "not recording" in r.json()["error"].lower()


def test_recording_error_start_when_already_recording(dosbox):
    dosbox.recording_start()
    try:
        r = dosbox.recording_start()
        assert r.status_code == 409
        assert "already" in r.json()["error"].lower()
    finally:
        dosbox.recording_stop()


def test_recording_round_trip(dosbox):
    """Start recording, inject keys, stop, verify the lifecycle works.

    API-injected keys are excluded from recording by design (the
    in_replay_dispatch flag prevents a replay from re-recording
    itself). So this test verifies the recording lifecycle and
    data shape, not that injected keys appear in the capture.
    """
    dosbox.recording_start()
    time.sleep(0.3)

    dosbox.press_key("KBD_a", pressed=True)
    time.sleep(0.1)
    dosbox.press_key("KBD_a", pressed=False)
    time.sleep(0.3)

    r = dosbox.recording_stop()
    assert r.status_code == 200
    data = r.json()
    assert "event_count" in data
    assert "duration_ms" in data
    assert "events" in data
    assert isinstance(data["events"], list)
    assert data["duration_ms"] >= 0


def test_recording_status_and_stop_report_truncated_field(dosbox):
    dosbox.recording_start()
    r = dosbox.recording_status()
    assert r.json()["truncated"] is False

    r = dosbox.recording_stop()
    assert r.json()["truncated"] is False


# ---------------------------------------------------------------------------
# Named recording store
# ---------------------------------------------------------------------------

def test_named_recording_save_list_replay_delete(dosbox):
    # API-injected keys aren't recorded (see test_recording_round_trip),
    # so this exercises the store's plumbing - not that real captured
    # events survive the round trip, which is covered by construction
    # (the store only ever holds what StopRecordingCommand handed it).
    dosbox.recording_start()
    r = dosbox.recording_stop(name="it-named-1")
    assert r.status_code == 200
    data = r.json()
    assert data["name"] == "it-named-1"

    r = dosbox.recordings_list()
    assert r.status_code == 200
    names = [rec["name"] for rec in r.json()["recordings"]]
    assert "it-named-1" in names

    r = dosbox.input_sequence_from_recording("it-named-1")
    assert r.status_code == 200
    assert r.json()["status"] == "ok"

    # Not consumed by replay - still there for a second replay.
    r = dosbox.input_sequence_from_recording("it-named-1")
    assert r.status_code == 200

    r = dosbox.recording_delete("it-named-1")
    assert r.status_code == 200
    assert r.json()["status"] == "deleted"

    r = dosbox.recording_delete("it-named-1")
    assert r.status_code == 404


def test_named_recording_replay_unknown_name_is_404(dosbox):
    r = dosbox.input_sequence_from_recording("does-not-exist")
    assert r.status_code == 404


def test_named_recording_delete_unknown_name_is_404(dosbox):
    r = dosbox.recording_delete("does-not-exist")
    assert r.status_code == 404


def test_input_sequence_rejects_events_and_recording_together(dosbox):
    r = dosbox.input_sequence_raw(
        json.dumps({"events": [], "recording": "whatever"}))
    assert r.status_code == 400


def test_recording_stop_invalid_name_leaves_recording_running(dosbox):
    dosbox.recording_start()
    try:
        r = dosbox.recording_stop(name="bad name with spaces")
        assert r.status_code == 400

        r = dosbox.recording_status()
        assert r.json()["recording"] is True
    finally:
        dosbox.recording_stop()


def test_recording_stop_include_events_false_omits_events(dosbox):
    dosbox.recording_start()
    r = dosbox.recording_stop(include_events=False)
    assert r.status_code == 200
    assert "events" not in r.json()


def test_named_recording_store_capacity_rejects_new_name_when_full(dosbox):
    limits = dosbox.dosbox_info().json()["capabilities"]["input"]["limits"]
    cap = limits["max_stored_recordings"]

    names = []
    try:
        for i in range(cap):
            name = f"it-cap-{i}"
            dosbox.recording_start()
            r = dosbox.recording_stop(name=name)
            assert r.status_code == 200
            names.append(name)

        dosbox.recording_start()
        r = dosbox.recording_stop(name="it-cap-overflow")
        assert r.status_code == 503
        assert r.json()["error_code"] == "registry_full"

        # Recording must still be running - the refusal happens before
        # the stop, not after losing the data.
        assert dosbox.recording_status().json()["recording"] is True
        dosbox.recording_stop()

        # Overwriting an existing name is still allowed at capacity.
        dosbox.recording_start()
        r = dosbox.recording_stop(name=names[0])
        assert r.status_code == 200
    finally:
        for name in names:
            dosbox.recording_delete(name)


# ---------------------------------------------------------------------------
# Video frame capture
# ---------------------------------------------------------------------------

def test_frame_jpeg(dosbox):
    r = dosbox.frame(fmt="jpeg")
    assert r.status_code == 200
    assert "image/jpeg" in r.headers.get("Content-Type", "")
    img = Image.open(BytesIO(r.content))
    assert img.width > 0
    assert img.height > 0
    assert img.mode == "RGB"


def test_frame_png(dosbox):
    r = dosbox.frame(fmt="png")
    assert r.status_code == 200
    assert "image/png" in r.headers.get("Content-Type", "")
    assert r.content[:4] == b"\x89PNG"
    img = Image.open(BytesIO(r.content))
    assert img.width > 0
    assert img.height > 0


def test_frame_raw(dosbox):
    r = dosbox.frame(fmt="raw")
    assert r.status_code == 200
    assert "application/octet-stream" in r.headers.get("Content-Type", "")
    header = struct.unpack_from("<IIiB H", r.content)
    width, height, pitch, pf, pal_count = header
    assert width > 0
    assert height > 0
    assert abs(pitch) >= width


def test_frame_info_shape(dosbox):
    r = dosbox.frame_info()
    assert r.status_code == 200
    data = r.json()
    assert "width" in data
    assert "height" in data
    assert "pixel_format" in data
    assert "pitch" in data
    assert "is_paletted" in data
    assert data["width"] > 0
    assert data["height"] > 0


def test_frame_quality_parameter(dosbox):
    r_low = dosbox.frame(fmt="jpeg", quality=10)
    r_high = dosbox.frame(fmt="jpeg", quality=98)
    assert r_low.status_code == 200
    assert r_high.status_code == 200
    assert len(r_low.content) < len(r_high.content)


def test_frame_accept_header_png(dosbox):
    r = dosbox.session.get(
        dosbox._url("/api/v1/video/frame"),
        headers={"Accept": "image/png", "Host": "127.0.0.1"},
        timeout=dosbox.timeout,
    )
    assert r.status_code == 200
    assert "image/png" in r.headers.get("Content-Type", "")


def test_frame_capture_to_file(dosbox, tmp_path):
    path = dosbox.capture_frame(tmp_path / "shot.jpg")
    assert path.exists()
    assert path.stat().st_size > 1000
    img = Image.open(path)
    assert img.width == 720
    assert img.height == 400


def test_frame_mode_rendered(dosbox):
    # Post-shader capture: a different pipeline stage than the default
    # (raw/native DOS framebuffer), reachable even headless since the
    # present loop still runs.
    r = dosbox.frame(fmt="png", mode="rendered")
    assert r.status_code == 200
    img = Image.open(BytesIO(r.content))
    assert img.width > 0
    assert img.height > 0


def test_frame_mode_rejects_unknown_value(dosbox):
    r = dosbox.frame(fmt="png", mode="bogus")
    assert r.status_code == 400
    assert r.json()["error_code"] == "invalid_argument"


def test_frame_png_level_parameter(dosbox):
    r_fast = dosbox.frame(fmt="png", png_level=0)
    r_small = dosbox.frame(fmt="png", png_level=9)
    assert r_fast.status_code == 200
    assert r_small.status_code == 200
    assert r_fast.content[:4] == b"\x89PNG"
    assert len(r_small.content) < len(r_fast.content)


def test_frame_png_level_default_is_smaller_than_level_one(dosbox):
    # The old hardcoded default (level 1); the new default (6, no
    # explicit png_level) must actually be smaller, not just accepted.
    r_old_default = dosbox.frame(fmt="png", png_level=1)
    r_new_default = dosbox.frame(fmt="png")
    assert len(r_new_default.content) < len(r_old_default.content)


def test_frame_scale_downsamples_by_the_exact_divisor(dosbox):
    info = dosbox.frame_info().json()
    r = dosbox.frame(fmt="png", scale=2)
    assert r.status_code == 200
    img = Image.open(BytesIO(r.content))
    assert img.width == info["width"] // 2
    assert img.height == info["height"] // 2


def test_frame_scale_rejects_a_non_divisor_value(dosbox):
    r = dosbox.frame(fmt="png", scale=3)
    assert r.status_code == 400
    assert r.json()["error_code"] == "invalid_argument"


def test_frame_scale_rejects_raw_format(dosbox):
    r = dosbox.frame(fmt="raw", scale=2)
    assert r.status_code == 400
    assert r.json()["error_code"] == "invalid_argument"


def test_frame_scale_one_is_accepted_for_raw_format(dosbox):
    # scale=1 is a no-op regardless of format - only a real scale
    # request is incompatible with raw's unconverted pixel data.
    r = dosbox.frame(fmt="raw", scale=1)
    assert r.status_code == 200


def test_frame_crop_returns_exactly_the_requested_rectangle(dosbox):
    r = dosbox.frame(fmt="png", crop=(10, 5, 100, 50))
    assert r.status_code == 200
    img = Image.open(BytesIO(r.content))
    assert img.width == 100
    assert img.height == 50


def test_frame_crop_rejects_a_rectangle_that_does_not_fit(dosbox):
    info = dosbox.frame_info().json()
    r = dosbox.frame(fmt="png", crop=(info["width"] - 1, 0, 100, 10))
    assert r.status_code == 400
    body = r.json()
    assert body["error_code"] == "invalid_argument"
    assert "does not fit" in body["error"]


def test_frame_crop_applies_to_raw_format_too(dosbox):
    r = dosbox.frame(fmt="raw", crop=(0, 0, 8, 4))
    assert r.status_code == 200
    header_size = struct.calcsize("<IIiBH")
    width, height, pitch, pf, pal_count = struct.unpack_from("<IIiBH", r.content)
    assert width == 8
    assert height == 4
    # Tightly packed now (no source row padding carried through): pitch
    # is exactly width * bytes-per-pixel for whichever pixel format this
    # frame actually is, and the body's exact byte count follows from
    # that - not just bounded above by the widest possible format.
    bytes_per_pixel = -(-pf // 8)  # ceil(bits / 8), matching get_bits_per_pixel
    assert pitch == width * bytes_per_pixel
    palette_size = pal_count * 3
    data_size = height * pitch
    assert len(r.content) == header_size + palette_size + data_size


def test_frame_crop_and_scale_compose(dosbox):
    r = dosbox.frame(fmt="png", crop=(0, 0, 200, 100), scale=2)
    assert r.status_code == 200
    img = Image.open(BytesIO(r.content))
    assert img.width == 100
    assert img.height == 50


# ---------------------------------------------------------------------------
# Video capture compression levels
# ---------------------------------------------------------------------------

def test_capture_compression_get_shape(dosbox):
    r = dosbox.capture_compression()
    assert r.status_code == 200
    data = r.json()
    assert isinstance(data["raw"], int)
    assert isinstance(data["rendered"], int)
    assert 0 <= data["raw"] <= 9
    assert 0 <= data["rendered"] <= 9


def test_capture_compression_set_and_restore(dosbox):
    before = dosbox.capture_compression().json()

    r = dosbox.capture_compression_set(raw=3, rendered=1)
    assert r.status_code == 200
    data = r.json()
    assert data["raw"] == 3
    assert data["rendered"] == 1

    # Partial update touches only the named mode
    r = dosbox.capture_compression_set(rendered=5)
    assert r.status_code == 200
    data = r.json()
    assert data["raw"] == 3
    assert data["rendered"] == 5

    r = dosbox.capture_compression_set_raw(before)
    assert r.status_code == 200
    assert r.json() == before


def test_capture_compression_set_validation(dosbox):
    r = dosbox.capture_compression_set_raw({})
    assert r.status_code == 400

    r = dosbox.capture_compression_set(raw=10)
    assert r.status_code == 400

    r = dosbox.capture_compression_set(raw=-1)
    assert r.status_code == 400

    r = dosbox.capture_compression_set(rendered="fast")
    assert r.status_code == 400


def test_capture_compression_set_rejected_while_recording(dosbox):
    r = dosbox.capture_video_start(mode="raw")
    assert r.status_code == 200
    try:
        r = dosbox.capture_compression_set(raw=1)
        assert r.status_code == 409
    finally:
        r = dosbox.capture_video_stop()
        assert r.status_code == 200

    # After stopping, setting works again
    before = dosbox.capture_compression().json()
    r = dosbox.capture_compression_set_raw(before)
    assert r.status_code == 200


# ---------------------------------------------------------------------------
# Drive swap validation
# ---------------------------------------------------------------------------

def test_drive_swap_missing_drive(dosbox):
    r = dosbox.drive_swap_raw(json.dumps({"image": "/tmp/fake.img"}))
    assert r.status_code == 400


def test_drive_swap_missing_image(dosbox):
    r = dosbox.drive_swap_raw(json.dumps({"drive": "A"}))
    assert r.status_code == 400


def test_drive_swap_invalid_drive_letter(dosbox):
    r = dosbox.drive_swap("1", "/tmp/fake.img")
    assert r.status_code == 400


def test_drive_swap_nonexistent_file(dosbox):
    r = dosbox.drive_swap("A", "/tmp/does-not-exist-ever.img")
    assert r.status_code == 400


# ---------------------------------------------------------------------------
# Memory operations
# ---------------------------------------------------------------------------

def test_memory_read_binary(dosbox):
    r = dosbox.memory_read(0, 16)
    assert r.status_code == 200
    assert len(r.content) == 16


def test_memory_read_json(dosbox):
    r = dosbox.memory_read_json(0, 16)
    assert r.status_code == 200
    data = r.json()
    assert "registers" in data
    assert "memory" in data
    assert "addr" in data["memory"]
    assert "data" in data["memory"]
    decoded = base64.b64decode(data["memory"]["data"])
    assert len(decoded) == 16


def test_memory_read_segment_by_register_name(dosbox):
    regs = dosbox.cpu_state().json()["registers"]
    r = dosbox.memory_read_segment_json("cs", 0, 16)
    assert r.status_code == 200
    data = r.json()
    assert data["memory"]["addr"] == regs["cs"] * 16


def test_memory_read_segment_by_numeric_paragraph(dosbox):
    r = dosbox.memory_read_segment_json(0x1000, 0x50, 16)
    assert r.status_code == 200
    data = r.json()
    assert data["memory"]["addr"] == 0x1000 * 16 + 0x50


def test_memory_write_segment_resolves_to_the_same_address_the_linear_route_reads(dosbox):
    regs = dosbox.cpu_state().json()["registers"]
    r = dosbox.memory_write_segment("ss", 0, b"\x99")
    assert r.status_code == 200

    linear_addr = regs["ss"] * 16
    readback = dosbox.memory_read(linear_addr, 1)
    assert readback.content == b"\x99"


def test_memory_write_if_match_succeeds_when_bytes_match(dosbox):
    offset = 0x9000  # well clear of anything the BIOS/DOS touches at boot
    original = dosbox.memory_read(offset, 4).content
    new_bytes = bytes((b + 1) % 256 for b in original)

    r = dosbox.memory_write(offset, new_bytes, if_match=original)
    assert r.status_code == 200

    assert dosbox.memory_read(offset, 4).content == new_bytes


def test_memory_write_if_match_conflicts_when_bytes_differ(dosbox):
    offset = 0x9010
    original = dosbox.memory_read(offset, 4).content
    wrong_expected = bytes((b + 1) % 256 for b in original)
    attempted = bytes((b + 2) % 256 for b in original)

    r = dosbox.memory_write(offset, attempted, if_match=wrong_expected)
    assert r.status_code == 412
    data = r.json()
    assert base64.b64decode(data["memory"]["data"]) == original

    # The mismatch must have blocked the write entirely.
    assert dosbox.memory_read(offset, 4).content == original


def test_memory_allocate_and_free(dosbox):
    r = dosbox.memory_allocate(256, area="CONV")
    assert r.status_code == 200
    data = r.json()
    assert "addr" in data
    assert data["addr"] > 0

    r = dosbox.memory_free(data["addr"])
    assert r.status_code == 200


def test_memory_allocate_invalid_area(dosbox):
    r = dosbox.session.post(
        dosbox._url("/api/v1/memory/allocate"),
        json={"size": 256, "area": "FAKE"},
        timeout=dosbox.timeout,
    )
    assert r.status_code == 400


def test_memory_allocate_xms_non_bestfit(dosbox):
    r = dosbox.session.post(
        dosbox._url("/api/v1/memory/allocate"),
        json={"size": 256, "area": "XMS", "strategy": "FIRST_FIT"},
        timeout=dosbox.timeout,
    )
    assert r.status_code == 400


# ---------------------------------------------------------------------------
# Memory snapshot / diff
# ---------------------------------------------------------------------------

def test_memory_snapshot_returns_handle_and_size(dosbox):
    r = dosbox.memory_snapshot(0x9020, 0x9024)
    assert r.status_code == 200
    data = r.json()
    assert data["start"] == 0x9020
    assert data["end"] == 0x9024
    assert data["bytes"] == 4
    assert "handle" in data


def test_memory_snapshot_span_too_large_rejected(dosbox):
    r = dosbox.memory_snapshot(0, 16 * 1024 * 1024 + 1)
    assert r.status_code == 400


def test_memory_diff_changed_finds_modified_byte(dosbox):
    offset = 0x9030
    dosbox.memory_write(offset, b"\x00\x00\x00\x00")
    handle = dosbox.memory_snapshot(offset, offset + 4).json()["handle"]

    dosbox.memory_write(offset + 1, b"\x05")
    r = dosbox.memory_diff(handle, "changed")
    assert r.status_code == 200
    data = r.json()
    assert data["total"] == 1
    assert data["matches"][0]["addr"] == offset + 1
    assert data["matches"][0]["value"] == 5


def test_memory_diff_equals_is_a_synonym_for_unchanged(dosbox):
    offset = 0x9040
    dosbox.memory_write(offset, b"\x07\x07\x07\x07")
    handle = dosbox.memory_snapshot(offset, offset + 4).json()["handle"]

    r = dosbox.memory_diff(handle, "equals")
    assert r.status_code == 200
    assert r.json()["total"] == 4


def test_memory_diff_refine_narrows_across_rounds(dosbox):
    offset = 0x9050
    dosbox.memory_write(offset, b"\x00\x00\x00\x00")
    handle = dosbox.memory_snapshot(offset, offset + 4).json()["handle"]

    # Round 1: bytes 0 and 2 change.
    dosbox.memory_write(offset, b"\x05\x00\x09\x00")
    round1 = dosbox.memory_diff(handle, "changed").json()
    assert round1["candidates"] == 2

    # Round 2: only byte 0 increases further.
    dosbox.memory_write(offset, b"\x0a\x00\x09\x00")
    round2 = dosbox.memory_diff(handle, "increased").json()
    assert round2["total"] == 1
    assert round2["matches"][0]["addr"] == offset


def test_memory_diff_width_locks_in_after_first_round(dosbox):
    offset = 0x9060
    dosbox.memory_write(offset, b"\x00\x00\x00\x00")
    handle = dosbox.memory_snapshot(offset, offset + 4).json()["handle"]

    dosbox.memory_diff(handle, "unchanged", width=2)
    r = dosbox.memory_diff(handle, "unchanged", width=4)
    assert r.status_code == 400


def test_memory_diff_unknown_handle_is_404(dosbox):
    r = dosbox.memory_diff(999999999, "changed")
    assert r.status_code == 404


def test_memory_diff_bad_op_rejected(dosbox):
    offset = 0x9070
    dosbox.memory_write(offset, b"\x00\x00")
    handle = dosbox.memory_snapshot(offset, offset + 2).json()["handle"]
    r = dosbox.memory_diff(handle, "bogus")
    assert r.status_code == 400


def test_memory_diff_handle_removed_once_exhausted(dosbox):
    offset = 0x9080
    dosbox.memory_write(offset, b"\x00\x00")
    handle = dosbox.memory_snapshot(offset, offset + 2).json()["handle"]

    dosbox.memory_write(offset, b"\x01\x01")
    dosbox.memory_diff(handle, "changed")  # narrows to 2 candidates

    # Nothing decreases - the candidate set drops to zero and the
    # handle is removed.
    r = dosbox.memory_diff(handle, "decreased")
    assert r.status_code == 200
    assert r.json()["candidates"] == 0

    r = dosbox.memory_diff(handle, "changed")
    assert r.status_code == 404


# ---------------------------------------------------------------------------
# Host validation
# ---------------------------------------------------------------------------

def test_host_validation_rejects_bad_host(dosbox):
    r = dosbox.get_with_host("/api/v1/status", "evil.example.com")
    assert r.status_code == 403


def test_host_validation_accepts_localhost(dosbox):
    r = dosbox.get_with_host("/api/v1/status", "localhost")
    assert r.status_code == 200


# ---------------------------------------------------------------------------
# Token authentication
# ---------------------------------------------------------------------------

def test_token_auth_rejects_no_token(dosbox):
    r = dosbox.get_without_token("/api/v1/status")
    assert r.status_code == 401


def test_token_auth_rejects_wrong_token(dosbox):
    import requests as req
    r = req.get(
        dosbox._url("/api/v1/status"),
        headers={
            "Host": "127.0.0.1",
            "Authorization": "Bearer 0000000000000000000000000000000000000000000000000000000000000000",
        },
        timeout=dosbox.timeout,
    )
    assert r.status_code == 401


def test_security_headers_present(dosbox):
    r = dosbox.status()
    assert r.status_code == 200
    assert r.headers.get("X-Content-Type-Options") == "nosniff"


def test_options_preflight_rejected(dosbox):
    import requests as req
    r = req.options(
        dosbox._url("/api/v1/status"),
        headers={
            "Host": "127.0.0.1",
            "Authorization": f"Bearer {dosbox.session.headers.get('Authorization', '').replace('Bearer ', '')}",
        },
        timeout=dosbox.timeout,
    )
    assert r.status_code == 403


def test_event_array_size_cap(dosbox):
    giant = [{"type": "key", "key": "KBD_a", "pressed": True}] * 32001
    r = dosbox.input_sequence(giant)
    assert r.status_code == 400
    assert "max" in r.json()["error"].lower()


def test_event_array_at_limit(dosbox):
    events = [{"type": "key", "key": "KBD_a", "pressed": True}] * 32000
    r = dosbox.input_sequence(events)
    assert r.status_code == 200


def test_drive_swap_no_path_leak(dosbox):
    r = dosbox.drive_swap("A", "/tmp/does-not-exist-ever.img")
    assert r.status_code == 400
    assert "/tmp" not in r.json().get("image", "")
