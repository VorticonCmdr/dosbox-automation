# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""Integration tests for the Lua scripting REST endpoints."""

import time

import pytest


def test_script_load_valid(dosbox):
    r = dosbox.script_load("dosbox.log('hello')", name="test-load")
    assert r.status_code == 200
    data = r.json()
    assert data["status"] == "loaded"
    assert data["name"] == "test-load"


def test_script_load_empty_body(dosbox):
    time.sleep(2.1)
    r = dosbox.script_load("")
    assert r.status_code == 400


def test_script_load_invalid_content_type(dosbox):
    time.sleep(2.1)
    r = dosbox._post(
        "/api/v1/script/load",
        data="print('hi')",
        headers={"Content-Type": "application/octet-stream"},
    )
    assert r.status_code == 415


def test_script_start_after_completion(dosbox):
    # After a script completes, starting again without a fresh load
    # should re-run the already-loaded script (not error).
    # This tests the contract: "start" on a completed script state.
    time.sleep(2.1)
    dosbox.script_load("dosbox.output.x = 1", name="rerun")
    dosbox.script_start()
    data = dosbox.wait_script_done(timeout=5)
    assert data["state"] == "completed"


def test_script_lifecycle(dosbox):
    time.sleep(2.1)  # rate limiter cooldown
    r = dosbox.script_load(
        "dosbox.output.result = 'done'\n",
        name="lifecycle-test",
    )
    assert r.status_code == 200

    r = dosbox.script_start()
    assert r.status_code == 200

    data = dosbox.wait_script_done(timeout=10)
    assert data["state"] == "completed"
    assert data["output"]["result"] == "done"


def test_script_stop_while_running(dosbox):
    time.sleep(2.1)
    r = dosbox.script_load(
        "while true do dosbox.wait_frames(1) end",
        name="infinite",
    )
    assert r.status_code == 200
    dosbox.script_start()
    time.sleep(0.5)

    r = dosbox.script_stop()
    assert r.status_code == 200


def test_script_status_idle(dosbox):
    # After stop, state should be idle.
    time.sleep(0.3)
    r = dosbox.script_status()
    assert r.status_code == 200
    assert r.json()["state"] == "idle"


def test_script_seed_determinism(dosbox):
    results = []
    for i in range(2):
        time.sleep(2.1)
        dosbox.script_load(
            "dosbox.output.val = tostring(math.random(1, 10000))\n",
            name=f"seed-test-{i}",
            seed=42,
        )
        dosbox.script_start()
        data = dosbox.wait_script_done()
        results.append(data["output"]["val"])
    assert results[0] == results[1]


def test_script_error_reported(dosbox):
    time.sleep(2.1)
    dosbox.script_load("error('intentional')", name="err-test")
    dosbox.script_start()
    data = dosbox.wait_script_done()
    assert data["state"] == "error"
    assert "intentional" in data.get("error", "")


def test_script_double_start_rejected(dosbox):
    time.sleep(2.1)
    dosbox.script_load(
        "while true do dosbox.wait_frames(1) end",
        name="double-start",
    )
    dosbox.script_start()
    time.sleep(0.3)

    r = dosbox.script_start()
    assert r.status_code == 400
    assert "already" in r.json()["error"].lower()

    dosbox.script_stop()
    time.sleep(0.3)


def test_script_rate_limit(dosbox):
    # Wait for any prior cooldown to expire, then do two loads in
    # quick succession. The second one must be rejected.
    time.sleep(2.5)

    r1 = dosbox.script_load("dosbox.log('a')", name="rate-a")
    assert r1.status_code == 200

    # Immediate second load should be rate-limited.
    r2 = dosbox.script_load("dosbox.log('b')", name="rate-b")
    assert r2.status_code == 429
    assert "Retry-After" in r2.headers


def test_script_load_bad_content_type_does_not_consume_the_rate_limit_slot(dosbox):
    # Regression: the rate limiter used to be checked before
    # Content-Type/body/param validation, so a request that was always
    # going to 415 still burned the 2-second slot.
    time.sleep(2.1)

    r1 = dosbox._post(
        "/api/v1/script/load",
        data="print('hi')",
        headers={"Content-Type": "application/octet-stream"},
    )
    assert r1.status_code == 415

    # Immediately following, well inside the 2s window - must still
    # succeed since the 415 above never touched the rate limiter.
    r2 = dosbox.script_load("dosbox.log('ok')", name="after-415")
    assert r2.status_code == 200


def test_script_log_requires_a_debug_load(dosbox):
    time.sleep(2.1)
    dosbox.script_load("dosbox.output.x = 1", name="no-debug")
    dosbox.script_start()
    dosbox.wait_script_done()

    r = dosbox.script_log()
    assert r.status_code == 400


def test_script_log_returns_content_and_status_reports_log_path(dosbox):
    time.sleep(2.1)
    dosbox.script_load("dosbox.log('hi from debug log')", name="log-test",
                       debug=True)

    status = dosbox.script_status().json()
    assert "log_path" in status

    r = dosbox.script_log()
    assert r.status_code == 200
    data = r.json()
    assert "log-test" in data["content"]
    assert data["truncated"] is False


def test_script_status_omits_log_path_when_not_debug(dosbox):
    time.sleep(2.1)
    dosbox.script_load("dosbox.output.x = 1", name="not-debug", debug=False)
    assert "log_path" not in dosbox.script_status().json()


def test_script_log_survives_after_the_script_completes(dosbox):
    # Regression: DebugLog::Close() used to clear FilePath(), which
    # would have made script/log 400 right after the script finishes -
    # exactly when a caller most wants to read what happened.
    time.sleep(2.1)
    dosbox.script_load("dosbox.output.done = true", name="survives",
                       debug=True)
    dosbox.script_start()
    dosbox.wait_script_done()

    status = dosbox.script_status().json()
    assert "log_path" in status

    r = dosbox.script_log()
    assert r.status_code == 200
    assert "completed" in r.json()["content"]


def test_script_log_dropped_after_a_failed_debug_reload(dosbox):
    # Regression: a reload that fails to compile used to leave
    # mgr.Params().debug true (committed before LoadScript is even
    # attempted) with mgr.Log() still pointing at the PREVIOUS
    # successful debug run, so script/log kept serving a completely
    # unrelated script's stale content as if it belonged to the failed
    # reload.
    time.sleep(2.1)
    dosbox.script_load("dosbox.output.a = 1", name="first", debug=True)
    dosbox.script_start()
    dosbox.wait_script_done()
    assert dosbox.script_log().status_code == 200

    time.sleep(2.1)
    r = dosbox.script_load("this is not valid lua (((", name="second",
                           debug=True)
    assert r.status_code == 400

    assert dosbox.script_log().status_code == 400
    assert "log_path" not in dosbox.script_status().json()
