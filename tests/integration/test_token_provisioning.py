# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#

"""Token provisioning integration tests for the dosbox-automation REST API.

Tests Channel B: when webserver_token_file is enabled and no env var
token is supplied, the auto-generated token is written to a file at
<config_dir>/webserver/api_token with restricted permissions. The file
is removed on clean shutdown.
"""

import os
import stat
from pathlib import Path

import pytest
import requests

from conftest import find_free_port, start_dosbox_instance, WORKSPACE


def start_with_token_file(work_dir, use_env_token=False, token_file_setting=True):
    """Start DOSBox with webserver_token_file enabled.

    When use_env_token is False, no DOSBOX_API_TOKEN is set, so the
    server auto-generates a token and writes it to the file.

    token_file_setting selects what's passed on the command line:
    True/False pass an explicit --set webserver_token_file=..., None
    omits the flag entirely so the engine's own default applies.
    """
    import secrets
    import subprocess
    import threading

    from conftest import DOSBOX_BIN, StderrCapture, DosboxInstance
    from dosbox_client import DosboxClient

    port = find_free_port()
    work_dir.mkdir(parents=True, exist_ok=True)

    # Write primary config
    config_dir = work_dir / ".config" / "dosbox-automation"
    config_dir.mkdir(parents=True, exist_ok=True)
    dosbox_bin = Path(DOSBOX_BIN).resolve()
    resource_dir = dosbox_bin.parent / "resources"
    primary_conf = config_dir / "dosbox-automation.conf"
    primary_conf.write_text(
        f"[webserver]\nmount_allowed_bases = {resource_dir}\n"
    )

    env = {
        **os.environ,
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "HOME": str(work_dir),
        "XDG_CONFIG_HOME": str(work_dir / ".config"),
    }

    if use_env_token:
        token = secrets.token_hex(32)
        env["DOSBOX_API_TOKEN"] = token
    else:
        env.pop("DOSBOX_API_TOKEN", None)
        token = None

    cmd = [
        DOSBOX_BIN,
        "--noprimaryconf",
        "--nolocalconf",
        "--set", "webserver_enabled=true",
        "--set", f"webserver_port={port}",
    ]
    if token_file_setting is not None:
        cmd += ["--set", f"webserver_token_file={'true' if token_file_setting else 'false'}"]

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

    # If no env token, read it from the file
    if token is None:
        token_path = config_dir / "webserver" / "api_token"
        if token_path.exists():
            token = token_path.read_text().strip()
        else:
            proc.kill()
            capture.join(timeout=2)
            raise RuntimeError(
                f"Token file not created at {token_path}\n"
                f"stderr: {capture.get_output()[:2000]}"
            )

    client = DosboxClient(f"http://127.0.0.1:{port}", token=token)
    client.wait_ready(timeout=10)

    return DosboxInstance(client, proc, capture, work_dir), token


# -----------------------------------------------------------------------
# Token file is written when enabled
# -----------------------------------------------------------------------

def test_token_file_created(tmp_path):
    """With webserver_token_file=true, the token file is created."""
    work_dir = tmp_path / "token-test"
    inst, token = start_with_token_file(work_dir)

    token_path = work_dir / ".config" / "dosbox-automation" / "webserver" / "api_token"

    try:
        assert token_path.exists(), f"Token file not found at {token_path}"
        content = token_path.read_text().strip()
        assert len(content) == 64, f"Token length {len(content)}, expected 64"
        assert content == token
    finally:
        inst.shutdown()


# -----------------------------------------------------------------------
# Token file has restricted permissions
# -----------------------------------------------------------------------

@pytest.mark.skipif(os.name == "nt", reason="Unix permissions only")
def test_token_file_permissions(tmp_path):
    """The token file must have 0600 permissions."""
    work_dir = tmp_path / "perm-test"
    inst, _ = start_with_token_file(work_dir)

    token_path = work_dir / ".config" / "dosbox-automation" / "webserver" / "api_token"

    try:
        mode = stat.S_IMODE(token_path.stat().st_mode)
        assert mode == 0o600, f"Expected 0600, got {oct(mode)}"
    finally:
        inst.shutdown()


# -----------------------------------------------------------------------
# Token from file authenticates API calls
# -----------------------------------------------------------------------

def test_token_from_file_authenticates(tmp_path):
    """A token read from the file can authenticate API requests."""
    work_dir = tmp_path / "auth-test"
    inst, token = start_with_token_file(work_dir)

    try:
        r = inst.client.status()
        assert r.status_code == 200

        # Wrong token should fail
        bad = requests.get(
            f"{inst.client.base_url}/api/v1/status",
            headers={"Authorization": "Bearer " + "0" * 64,
                     "Host": "127.0.0.1"},
            timeout=5,
        )
        assert bad.status_code == 401
    finally:
        inst.shutdown()


# -----------------------------------------------------------------------
# Token file removed on clean shutdown
# -----------------------------------------------------------------------

def test_token_file_removed_on_shutdown(tmp_path):
    """The token file is removed when DOSBox shuts down cleanly."""
    work_dir = tmp_path / "shutdown-test"
    inst, _ = start_with_token_file(work_dir)

    token_path = work_dir / ".config" / "dosbox-automation" / "webserver" / "api_token"
    assert token_path.exists()

    inst.shutdown()

    assert not token_path.exists(), "Token file still exists after shutdown"


# -----------------------------------------------------------------------
# No token file when env var is supplied
# -----------------------------------------------------------------------

def test_no_token_file_with_env_var(tmp_path):
    """When DOSBOX_API_TOKEN is set, no file is written even if enabled."""
    work_dir = tmp_path / "env-test"
    inst, _ = start_with_token_file(work_dir, use_env_token=True)

    token_path = work_dir / ".config" / "dosbox-automation" / "webserver" / "api_token"

    try:
        assert not token_path.exists(), (
            "Token file should not be written when env var is set"
        )
    finally:
        inst.shutdown()


# -----------------------------------------------------------------------
# webserver_token_file defaults to on, so the out-of-the-box token is
# actually obtainable (see docs/mcp-plan.md item 4.6).
# -----------------------------------------------------------------------

def test_token_file_written_by_default(tmp_path):
    """With no --set for webserver_token_file, the engine's own default
    still writes the full token to a file."""
    work_dir = tmp_path / "default-test"
    inst, token = start_with_token_file(work_dir, token_file_setting=None)

    token_path = work_dir / ".config" / "dosbox-automation" / "webserver" / "api_token"

    try:
        assert token_path.exists(), (
            f"Token file not found at {token_path} - webserver_token_file "
            "default may have regressed to off"
        )
        assert token_path.read_text().strip() == token
    finally:
        inst.shutdown()


def test_no_full_token_obtainable_when_explicitly_disabled(tmp_path):
    """With webserver_token_file=false and no DOSBOX_API_TOKEN, the full
    token is genuinely unobtainable: no file, and only a short preview
    in the log."""
    import subprocess

    from conftest import DOSBOX_BIN, StderrCapture

    work_dir = tmp_path / "disabled-test"
    work_dir.mkdir(parents=True, exist_ok=True)
    port = find_free_port()

    config_dir = work_dir / ".config" / "dosbox-automation"
    config_dir.mkdir(parents=True, exist_ok=True)
    dosbox_bin = Path(DOSBOX_BIN).resolve()
    resource_dir = dosbox_bin.parent / "resources"
    (config_dir / "dosbox-automation.conf").write_text(
        f"[webserver]\nmount_allowed_bases = {resource_dir}\n"
    )

    env = {
        **os.environ,
        "SDL_VIDEODRIVER": "offscreen",
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
        "--set", "webserver_token_file=false",
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
        assert capture.ready.wait(timeout=15), (
            f"DOSBox did not start:\n{capture.get_output()[:2000]}"
        )

        token_path = config_dir / "webserver" / "api_token"
        assert not token_path.exists(), (
            "No token file should exist with webserver_token_file=false"
        )

        preview_lines = [
            line for line in capture.lines
            if "WEBSERVER:" in line and "API token:" in line
        ]
        assert preview_lines, "Expected a log preview line for the token"
        # "API token: xxxxxxxx..." - always the 8-char preview, never
        # the full 64-char token, with no other channel to get it from.
        preview = preview_lines[0].split("API token:", 1)[1].strip()
        assert preview == "..." or len(preview) <= len("xxxxxxxx..."), (
            f"Full token appears to have leaked into the log: {preview!r}"
        )
    finally:
        # No valid token exists anywhere to shut down cleanly through
        # the API - this configuration is exactly what's under test.
        proc.kill()
        proc.wait(timeout=5)
        capture.join(timeout=2)
