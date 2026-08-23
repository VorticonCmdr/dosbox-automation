# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""Integration tests for the ZMBV video capture REST endpoints."""

import time

import pytest


def test_capture_status_when_idle(dosbox):
    r = dosbox.capture_status()
    assert r.status_code == 200
    body = r.json()
    assert body["capturing"] is False
    assert body["frames"] == 0
    assert body["bytes_written"] == 0
    assert body["elapsed_ms"] == 0
    assert "path" not in body


def test_capture_start_stop(dosbox):
    r = dosbox.capture_start()
    assert r.status_code == 200

    r = dosbox.capture_status()
    assert r.status_code == 200
    assert r.json()["capturing"] is True

    r = dosbox.capture_stop()
    assert r.status_code == 200

    r = dosbox.capture_status()
    assert r.status_code == 200
    assert r.json()["capturing"] is False


def test_capture_status_reports_path_frames_and_elapsed_after_a_run(dosbox):
    r = dosbox.capture_start()
    assert r.status_code == 200

    time.sleep(0.5)

    r = dosbox.capture_status()
    assert r.status_code == 200
    body = r.json()
    assert body["path"].endswith(".avi")
    assert body["frames"] > 0
    assert body["bytes_written"] > 0
    assert body["elapsed_ms"] > 0

    dosbox.capture_stop()

    # Retained past the stop, since checking status right after stopping
    # is the normal sequence.
    r = dosbox.capture_status()
    assert r.status_code == 200
    stopped_body = r.json()
    assert stopped_body["capturing"] is False
    assert stopped_body["path"] == body["path"]
    assert stopped_body["frames"] > 0

    # elapsed_ms is frozen at the recording's real duration, not still
    # climbing with wall-clock time since the stop.
    time.sleep(0.5)
    r = dosbox.capture_status()
    assert r.json()["elapsed_ms"] == stopped_body["elapsed_ms"]


def test_capture_start_with_mode_and_compression(dosbox):
    r = dosbox.capture_start(mode="raw", compression=3)
    assert r.status_code == 200
    assert r.json()["compression"] == 3

    r = dosbox.capture_status()
    assert r.json()["compression_level"] == 3

    dosbox.capture_stop()

    # Restore the default so this test doesn't leak state into others.
    dosbox.capture_start(mode="raw", compression=9)
    dosbox.capture_stop()


def test_capture_start_with_compression_rejected_while_recording(dosbox):
    r = dosbox.capture_start()
    assert r.status_code == 200

    r = dosbox.capture_start(compression=5)
    assert r.status_code == 409

    dosbox.capture_stop()


def test_capture_double_start(dosbox):
    dosbox.capture_start()
    r = dosbox.capture_start()
    # Should not fail, just keep recording.
    assert r.status_code == 200

    dosbox.capture_stop()


def test_capture_stop_when_not_recording(dosbox):
    r = dosbox.capture_stop()
    assert r.status_code == 200


def test_capture_from_lua(dosbox):
    time.sleep(2.1)  # rate limiter
    r = dosbox.script_load(
        "dosbox.capture_start()\n"
        "dosbox.wait_frames(10)\n"
        "dosbox.capture_stop()\n"
        "dosbox.output.done = 'yes'\n",
        name="capture-lua",
    )
    assert r.status_code == 200

    dosbox.script_start()
    data = dosbox.wait_script_done()
    assert data["state"] == "completed"
    assert data["output"]["done"] == "yes"
