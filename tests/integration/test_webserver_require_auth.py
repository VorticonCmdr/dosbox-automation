# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""webserver_require_auth integration tests.

Covers: an unauthenticated request succeeds once auth is disabled (and
still fails by default); the token file is not exposed over HTTP while
auth is disabled; and the server refuses to disable auth unless bound
to a loopback address with webserver_allow_remote=false.
"""

import os
import subprocess
from pathlib import Path

import pytest
import requests

from conftest import DOSBOX_BIN, StderrCapture, expected_config_dir, find_free_port


# -----------------------------------------------------------------------
# Unauthenticated requests succeed once auth is disabled, still fail by
# default
# -----------------------------------------------------------------------

def test_no_auth_allows_unauthenticated_requests(dosbox_e2e):
    instance = dosbox_e2e(extra_sets=["webserver_require_auth=false"])
    r = instance.client.get_without_token("/api/v1/status")
    assert r.status_code == 200


def test_auth_required_by_default_rejects_unauthenticated_requests(dosbox_e2e):
    instance = dosbox_e2e()
    r = instance.client.get_without_token("/api/v1/status")
    assert r.status_code == 401


# -----------------------------------------------------------------------
# The token file is not served over HTTP while auth is disabled
# -----------------------------------------------------------------------

def start_with_require_auth_disabled(work_dir):
    """Start DOSBox with webserver_require_auth=false and no env token,
    so the engine generates its own token and writes it to a file - the
    scenario that would leak the file over HTTP if config_home were
    still mounted."""
    port = find_free_port()
    work_dir.mkdir(parents=True, exist_ok=True)

    config_dir = expected_config_dir(work_dir)
    config_dir.mkdir(parents=True, exist_ok=True)
    dosbox_bin = Path(DOSBOX_BIN).resolve()
    resource_dir = dosbox_bin.parent / "resources"
    (config_dir / "dosbox-automation.conf").write_text(
        f"[webserver]\nmount_allowed_bases = {resource_dir}\n"
    )

    env = {
        **os.environ,
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "HOME": str(work_dir),
        "XDG_CONFIG_HOME": str(work_dir / ".config"),
    }
    env.pop("DOSBOX_API_TOKEN", None)

    cmd = [
        DOSBOX_BIN,
        "--noprimaryconf",
        "--nolocalconf",
        "--set", "webserver_enabled=true",
        "--set", f"webserver_port={port}",
        "--set", "webserver_require_auth=false",
    ]

    proc = subprocess.Popen(
        cmd, env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        cwd=str(work_dir),
    )

    capture = StderrCapture(proc.stderr)
    capture.start()

    if not capture.ready.wait(timeout=15):
        proc.kill()
        capture.join(timeout=2)
        raise RuntimeError(
            f"DOSBox did not start:\n{capture.get_output()[:2000]}"
        )

    return proc, capture, port, config_dir


def test_no_auth_does_not_expose_token_file(tmp_path):
    work_dir = tmp_path / "no-auth-token-leak"
    proc, capture, port, config_dir = start_with_require_auth_disabled(work_dir)

    try:
        token_path = config_dir / "webserver" / "api_token"
        assert token_path.exists(), (
            f"Token file not created at {token_path}\n"
            f"stderr: {capture.get_output()[:2000]}"
        )

        r = requests.get(
            f"http://127.0.0.1:{port}/api_token",
            headers={"Host": "127.0.0.1"},
            timeout=5,
        )
        assert r.status_code == 404, (
            "GET /api_token must not serve the token file, even with "
            f"auth disabled - got {r.status_code}: {r.text[:200]!r}"
        )
    finally:
        # No token is required to shut this instance down with auth off.
        requests.post(
            f"http://127.0.0.1:{port}/api/v1/control/shutdown",
            headers={"Host": "127.0.0.1"},
            timeout=5,
        )
        proc.wait(timeout=5)
        capture.join(timeout=2)


# -----------------------------------------------------------------------
# Refuses to disable auth unless bound to loopback with
# webserver_allow_remote=false
# -----------------------------------------------------------------------

def test_no_auth_refuses_with_allow_remote(tmp_path):
    work_dir = tmp_path / "no-auth-refusal"
    work_dir.mkdir(parents=True, exist_ok=True)
    port = find_free_port()

    config_dir = expected_config_dir(work_dir)
    config_dir.mkdir(parents=True, exist_ok=True)
    dosbox_bin = Path(DOSBOX_BIN).resolve()
    resource_dir = dosbox_bin.parent / "resources"
    (config_dir / "dosbox-automation.conf").write_text(
        f"[webserver]\nmount_allowed_bases = {resource_dir}\n"
    )

    env = {
        **os.environ,
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "HOME": str(work_dir),
        "XDG_CONFIG_HOME": str(work_dir / ".config"),
    }
    env.pop("DOSBOX_API_TOKEN", None)

    cmd = [
        DOSBOX_BIN,
        "--noprimaryconf",
        "--nolocalconf",
        "--set", "webserver_enabled=true",
        "--set", f"webserver_port={port}",
        "--set", "webserver_require_auth=false",
        "--set", "webserver_allow_remote=true",
    ]

    proc = subprocess.Popen(
        cmd, env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        cwd=str(work_dir),
    )

    capture = StderrCapture(proc.stderr)
    capture.start()

    try:
        # The webserver thread is never spawned on this path, so the
        # ready line (only ever logged from inside Webserver::run) never
        # appears - a short bounded wait is enough to observe that.
        assert not capture.ready.wait(timeout=3), (
            "webserver started despite the unsafe require_auth=false + "
            f"allow_remote=true combination:\n{capture.get_output()[:2000]}"
        )

        output = capture.get_output()
        assert "Refusing to disable authentication" in output, (
            f"Expected refusal warning not found:\n{output[:2000]}"
        )
        assert "Starting HTTP REST API" not in output

        with pytest.raises(requests.exceptions.ConnectionError):
            requests.get(
                f"http://127.0.0.1:{port}/api/v1/status",
                headers={"Host": "127.0.0.1"},
                timeout=2,
            )
    finally:
        proc.kill()
        proc.wait(timeout=5)
        capture.join(timeout=2)
