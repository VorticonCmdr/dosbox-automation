# dosbox-automation

![GPL-2.0-or-later][gpl-badge]

A DOSBox variant for Linux and Windows with a local HTTP REST API for automated game installation, input recording with deterministic replay, and programmatic emulator control.

Based on DOSBox Staging 0.84. Your existing DOSBox configurations will continue to work.

This is [VorticonCmdr](https://github.com/VorticonCmdr)'s fork of
[dosbox-automation/dosbox-automation](https://github.com/dosbox-automation/dosbox-automation)
(upstream). It adds the headless debugger REST/Lua area, the web debugger
(`/debugger.html`), and the web control panel (`/control.html`) on top of
upstream's automation API; it does not carry upstream's mixer control
support. Upstream's [release builds](https://github.com/dosbox-automation/dosbox-automation/releases)
and [manual](https://www.dosbox-automation.org/) don't include any of this
fork's additions.

## Quick start

1. **Build it.** This fork isn't published as a binary release yet —
   [build from source](#build-from-source).

2. **Enable the API.** It's off by default. Add to your config file (or pass on the command line):

   ```
   [webserver]
   webserver_enabled = true
   ```

   ```
   dosbox --set webserver_enabled=true
   ```

3. **Run it.**

   ```
   dosbox
   ```

   On startup it prints the API's URL (default `http://localhost:8386`) and writes a bearer token
   to a file in the webserver config directory (only a short preview goes to the log). Every
   request needs `Authorization: Bearer <token>`, except `GET /api/v1/hello` and the browser doc
   pages below.

4. **Try it.** Open `http://localhost:8386` in a browser for the landing page, `/api.html` for a
   live Swagger explorer of every endpoint, `/control.html` for the [web control
   panel](#web-control-panel), or `/debugger.html` for the [web debugger](#web-debugger) — no
   token needed just to load these pages, only for the API calls they make.

## Configuration

Project-specific settings this fork adds on top of stock DOSBox Staging configuration (see the
[DOSBox Staging manual](https://www.dosbox-staging.org/) for everything else — sound cards,
video, mouse, etc.). Every setting also has a `SetHelp()` description in the generated config
file.

### `[webserver]`

| Setting | Default | Description |
|---|---|---|
| `webserver_enabled` | `false` | Enable the HTTP REST API. |
| `webserver_bind_address` | `127.0.0.1` | IP to bind to. Binding to `0.0.0.0`/`::` also requires `webserver_allow_remote`. |
| `webserver_port` | `8386` | TCP port. |
| `webserver_allow_remote` | `false` | Allow binding to a non-localhost address, exposing the API to the network. |
| `webserver_token_file` | `true` | Write the full bearer token to a `0600` file in the config dir (removed on clean shutdown). Set `false` to rely on the `DOSBOX_API_TOKEN` environment variable instead. |
| `webserver_token_scopes` | *(unrestricted)* | Comma-separated scope list to restrict what the token can do: `read`, `write`, `input`, `script`, `media`, `debug`, `control`. Also gates what a running Lua script may do. |
| `webserver_osd` | `true` | Show on-screen indicators while automation is driving the machine (script running, recording, replay, injected input). |
| `mount_allowed_bases` | *(none)* | Semicolon-separated extra base directories `MOUNT`/`drive/mount` may expose to the guest. Symlink components are rejected. Only honored from the primary config file. |
| `mount_allowed_image_roots` | *(none)* | Semicolon-separated directories floppy/CD images may be mounted or swapped from. Same primary-config-file-only restriction. |

### `[debug]`

| Setting | Default | Description |
|---|---|---|
| `debugger_window` | `true` | Show the interactive debugger window. Set `false` to keep the debugger fully usable over the REST API (breakpoints, stepping, memory/register access, the web debugger) without ever opening a window — the same headless fallback used when no GPU device is available. |

## Features

- **Status & control** — emulator/program state, graceful shutdown, block-until-condition waits.
- **Input** — inject keyboard/mouse event sequences or literal text; read/set the mouse cursor; cancel a running replay.
- **Recording & replay** — record keyboard and mouse input with deterministic, frame-accurate replay; list, save, and delete recordings.
- **Video & capture** — grab the current frame (image or metadata), scrape the text-mode screen, and record ZMBV video with adjustable compression.
- **Drives & mounting** — list drives, swap floppy/CD images (multi-disk installs), mount a host directory as a drive letter, lock the mount configuration, and browse or enumerate images/directories under an allow-listed root.
- **Memory** — allocate/free guest memory, search or pattern-scan a range, snapshot-and-diff to hunt down a moving value, freeze addresses to a fixed value, and raw read/write at any address.
- **CPU** — read all registers/flags, write a single register.
- **DOS internals** — walk the DOS MCB chain (PSP ownership, free memory), and inspect live EMS/XMS driver state (handles, page mappings, A20/HMA/UMB).
- **Port I/O** — read/write an x86 I/O port.
- **Headless debugger** — pause/continue/step/step-over/step-out/run-to, execute/interrupt/memory breakpoints (with conditions and ignore counts), watched variables, disassembly, and call-stack backtraces — all reachable purely over the API, with or without the interactive window (see `debugger_window` above).
- **Web debugger** — a browser-based alternative to the interactive window at `/debugger.html`: registers, live disassembly, breakpoints (including read/write memory watchpoints), watched variables, backtrace, and a memory hex-dump, driven entirely by the debug REST API.
- **Web control panel** — a dashboard at `/control.html` for everything else: live screen preview and video capture, input/typing and mouse control, recording and replay, drive mounting and image swaps, memory freezes and raw reads, CPU registers and port I/O, DOS/EMS/XMS internals, Lua script control, and a raw-JSON escape hatch for batch and wait_for.
- **Lua scripting** — sandboxed scripts run inside the emulator with a `dosbox.*` API (keyboard/mouse input, memory read/write, screen text/matching, on-screen messages, capture control), gated by the same token scopes as the REST API. Useful for install automation and in-emulator test logic that doesn't want a network round-trip per step.
- **Batch execution** — bundle multiple memory/register/port/freeze operations into one atomic emulation-thread pass.
- **Lifecycle control** — designed for launchers and CI: deterministic recording/replay, frame capture for visual verification, and graceful shutdown.

Every route also carries a token scope (`read`, `write`, `input`, `media`, `debug`, `script`,
`control`), enforced against `webserver_token_scopes`. The full endpoint list is documented at
`/api.html` (Swagger UI) once the server is running, or in `resources/webserver/openapi.json`.

### Web control panel

`/control.html` needs no token to load, same as `/debugger.html` and `/api.html` — paste the
bearer token into the page itself once it's open. It's a routine-operations dashboard, not a
Swagger replacement: mount drives, type into DOS programs, watch the live screen, freeze a memory
value, load a Lua script — all from panels instead of hand-built requests. Anything more
specialized (pattern scans, memory snapshot/diff, one-off calls to any other route) is still one
click away via the linked `/api.html`.

### Web debugger

`/debugger.html` needs no token to load — like the landing page and Swagger UI, it's a static
page. Once open, paste the bearer token (from the token file or log preview) into the page itself;
every actual debug/memory/CPU call it makes still requires it, same as any other API client.

## Security

If you open a web server, you open an attack surface. dosbox-automation ships with bearer token authentication, host header validation, mount path restrictions, and localhost-only binding. See [SECURITY.md](docs/SECURITY.md) for this fork's model and how to report an issue, or upstream's [security documentation](https://www.dosbox-automation.org/0.84-da1/automation/security/) for the base mechanism this fork inherits.

## Upstream project website

https://www.dosbox-automation.org/ is upstream's site (manual, downloads). It
does not cover anything this fork adds.

## Downloads

No binary releases are published for this fork yet — see
[Build from source](#build-from-source). Upstream's [release
builds](https://github.com/dosbox-automation/dosbox-automation/releases) are
plain dosbox-automation, without the debugger API or either web UI.

## Build from source

Configure and build with the CMake presets in `CMakePresets.json` (`cmake --list-presets` to see
them all):

```
cmake --preset=debug-linux
cmake --build --preset=debug-linux
```

The resulting binary is at `build/debug-linux/dosbox`. Swap `debug-linux` for `release-linux` for
an optimized build, or `debug-linux-vcpkg`/`release-linux-vcpkg` to build dependencies via vcpkg
instead of system packages (used for official Linux binaries, for glibc-only portability).

Full platform-specific instructions:

- [Linux](docs/build-linux.md)
- [Windows](docs/build-windows.md) (Visual Studio 2022 + Clang/LLVM + vcpkg recommended; presets
  `debug-windows-vs2022`/`release-windows-vs2022`)
- [macOS](docs/build-macos.md) (instructions taken verbatim from upstream DOSBox Staging, untested here - reports welcome)

## License

dosbox-automation is licensed under GNU GPL v2+, based on DOSBox Staging.

[gpl-badge]: https://img.shields.io/badge/license-GPL--2.0--or--later-blue

---
Built with AI-assisted development. See [CONTRIBUTING.md](docs/CONTRIBUTING.md).
