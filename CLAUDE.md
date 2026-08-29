# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

dosbox-automation is a DOSBox variant (based on DOSBox Staging 0.84) for Linux and
Windows with a local HTTP REST API for automated game installation, deterministic
input recording/replay, frame capture, and programmatic emulator control. It also
embeds a Lua scripting engine for install automation and testing. See README.md
for the feature list.

This repo is VorticonCmdr's fork of upstream
[dosbox-automation/dosbox-automation](https://github.com/dosbox-automation/dosbox-automation),
adding the headless debugger REST/Lua area and both web UIs
(`/debugger.html`, `/control.html`). Don't assume upstream's docs, issue
tracker, or release builds apply to this fork's own additions.

Project rules live in `.claude/rules/` (code style, docs style, versioning,
commits) and are mirrored in `docs/CONTRIBUTING.md`, which is also what external
contributors and their LLM tooling read. Keep both in sync if either changes.

## Build

Configure/build with CMake presets (see `CMakePresets.json`). Common ones:

```
cmake --preset=debug-linux            # Linux, system libs
cmake --build --preset=debug-linux
cmake --preset=debug-linux-vcpkg      # Linux, vcpkg deps
cmake --preset=debug-macos            # macOS, Xcode generator
cmake --preset=debug-windows-vs2022   # Windows
```

Release presets follow the same naming (`release-linux`, `release-macos`, etc).
Full platform instructions: `docs/build-linux.md`, `docs/build-macos.md`,
`docs/build-windows.md`.

macOS uses system Homebrew Lua 5.5 (`src/lua/*.h` are symlinks into
`/opt/homebrew/opt/lua/include/lua5.5/`), not vcpkg's Lua port.

## Tests

Unit tests use the CMake/ctest integration (doctest-based, `tests/*.cpp`):

```
ctest -j 8 --preset debug-linux                                   # all tests
ctest -j 8 --preset debug-linux -R DOS_FilesTest                  # one suite
ctest -j 8 --preset debug-linux -R DOS_FilesTest.DOS_MakeName_Basic_Failures  # one case
ctest -j 8 --preset debug-linux -R DOS_FilesTest.DOS_MakeName_Basic_Failures -V  # verbose
```

Build with `-DOPT_TESTS=OFF` to skip compiling tests entirely.

End-to-end tests drive a live headless DOSBox instance over the HTTP API:

```
python3 tests/run-e2e.py                       # everything
python3 tests/run-e2e.py --category api        # API contract tests
python3 tests/run-e2e.py --category lua        # Lua REST endpoint + function tests
python3 tests/run-e2e.py --category capture    # ZMBV capture endpoint tests
python3 tests/run-e2e.py --category e2e        # manifest-driven installer automation
python3 tests/run-e2e.py --list                # list available tests
python3 tests/run-e2e.py --list-games          # list game manifests / disk availability
```
`DOSBOX_BIN` env var points at the binary (default `build/debug-linux/dosbox`).

Before compiling, verify all commits in a PR build individually:
`scripts/tools/compile-commits.sh` (required for bisectability).

## Formatting and linting

Format only what you touched, not whole files:

```
./scripts/tools/format-commit.sh     # formats C/C++ touched by the latest commit
clang-format -i <file>               # manual, targeted line ranges preferred
```

`./scripts/ci/count-warnings.py` summarizes compiler warnings from a build log
(used for quality gating).

`.clang-tidy`, `.mdl-styles` (Markdown), and `.pylint` configs also apply;
respect them for the relevant file types.

## Architecture

### Two trust domains, one bridge

The codebase splits into the emulated machine (trusted, single-threaded on the
main/emulation thread) and the webserver (untrusted input, runs on its own
thread(s) via httplib). They never touch shared state directly.

- `src/webserver/bridge.{h,cpp}` — `Webserver::Bridge` is the only crossing
  point. The webserver thread builds a `Command`, calls
  `cmd.WaitForCompletion(timeout_ms)`, which blocks until the emulation thread's
  main loop calls `Bridge::Instance().ProcessRequests()` and executes queued
  commands synchronously. `Command::Execute()` runs on the emulation thread and
  sets `Command::error` instead of throwing (the webserver thread checks it
  after and throws there, turning it into an HTTP error response).
- Anything crossing the bridge must already be validated. Validate HTTP input
  (ranges, sizes, path traversal, etc.) in the webserver handler *before*
  constructing the `Command` — see `.claude/rules/code-style.md` "Security
  posture" and the same section in `docs/CONTRIBUTING.md`.
- `src/webserver/webserver.cpp` wires up all `/api/v1/...` routes and defines
  `IsPublicDocPath()`, the exact-match allowlist of paths that bypass bearer
  token auth (landing page, API explorer, openapi.json, vendored Swagger UI
  assets). Keep it exact-match; anything looser risks leaking the token file
  path.
- Each API area has a `<name>.cpp`/`<name>.h` (or `private/<name>.h`) pair
  under `src/webserver/`: `cpu`, `debug`, `dos`, `drive`, `freeze`, `input`,
  `io_port`, `memory`, `video`, `capture`, `control`, `frame_tap`. Follow this
  pattern for new endpoint groups: a `*Command` (or `*Handlers`) struct with
  static handler methods, backed by a `Command` subclass that does the actual
  emulator-side work via the Bridge.

### Lua scripting (`src/lua/`)

`LuaEngine` (`lua_engine.{h,cpp}`) wraps the embedded Lua 5.5 interpreter.
`lua_bridge_commands.{h,cpp}` exposes Bridge-mediated emulator control to Lua
scripts (mirrors what the REST API exposes, for in-script automation).
`lua_coroutine.{h,cpp}` implements coroutine-based scripting so long-running
scripts can yield across emulated frames. `script_validator.{h,cpp}` checks
scripts before execution — treat script content as untrusted input, same as
HTTP input. `text_input.{h,cpp}` handles text injection into DOS programs.

### Debugger (`src/debugger/`)

Interactive debugger (`debugger.cpp`, `debugger_gui.cpp`, `debugger_disasm.cpp`)
plus a headless automation API (breakpoints, pause/step, memory/register
access) reachable over the webserver Bridge for scripted debugging and
CI-driven modding workflows.

### Emulator core

`src/hardware/`, `src/cpu/`, `src/dos/`, `src/ints/`, `src/fpu/` are the
DOSBox Staging-derived emulator core: avoid exceptions here (see language
rules), this code is on the hot path and interfaces with legacy C-style
DOSBox internals.

### Vendored code

`src/libs/` is vendored third-party code with its own conventions; project
style/formatting rules do not apply there (see `.claude/rules/code-style.md`
Scope section).

## Documentation

The user manual and website live in upstream's separate repository
(dosbox-automation.org) and don't cover this fork's own additions. This repo
only carries developer docs under `docs/`.
If a change alters user-visible behavior, a config setting, an API route, or a
Lua function, flag it in the PR description so the manual gets updated —
config/API/Lua tables in this file (if added later) and the manual's setting
descriptions are copied verbatim from code comments/help text.

## Commit and versioning conventions

See `.claude/rules/commits.md` and `.claude/rules/versioning-guide.md`:
commits are unprefixed, terse, natural language, one logical change each
(formatting/doc changes that accompany a fix stay in the same commit); no
Co-Authored-By trailers; commits require explicit human approval before being
made. Version format is `<upstream-base>-vc<release-number>`, e.g.
`0.84-vc1`; full product name is `dosbox-automation 0.84-vc1`. The `vc`
suffix (this fork's own scheme) is deliberately distinct from upstream's own
`da<release-number>` numbering, so a version string can't be mistaken for
an upstream release.
