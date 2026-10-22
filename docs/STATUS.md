# Fork status and maintenance

This fork adds remote debugging and automation to dosbox-automation. The
[developer reference](index.html) and generated
[OpenAPI specification](../resources/webserver/openapi.json) describe the user-facing
surface. The [MCP development log](mcp-plan.md) retains the implementation history
and unfinished reverse-engineering work; its opening problem statements describe
historical defects, not the current release.

## Supported behavior

| Area | Current surface |
| --- | --- |
| Debugging | Pause/resume, stepping, conditional breakpoints, stop records, disassembly, backtraces, browser debugger |
| Automation | Server-side waits, batches, snapshots/refinement, signature scans, input recording/replay |
| Browser tools | General control panel and debugger; deliberate token-free loopback access remains supported |
| Permissions | Bearer authentication by default, optional token scopes, loopback-only authentication opt-out |
| Build | Bundled static Lua 5.5.0; normal, debugger, and heavy-debugger configurations |

## Build and verify

Heavy debugging is opt-in: memory watchpoints add instruction and memory-access
overhead. Use a separate build directory so a normal build cannot silently
replace the debugger binary.

| Platform | Configure/build preset | Binary relative to the build directory |
| --- | --- | --- |
| Linux, vcpkg | `debug-linux-heavy-vcpkg` | `dosbox_with_debugger` |
| macOS, vcpkg | `debug-macos-heavy` | `Debug/dosbox_with_debugger` |
| Windows, VS2022/vcpkg | `debug-windows-heavy-vs2022` | `Debug/dosbox_with_debugger.exe` |

For example, on Linux with vcpkg configured:

```sh
cmake --preset debug-linux-heavy-vcpkg
cmake --build --preset debug-linux-heavy-vcpkg --parallel 4
ctest --preset debug-linux-heavy-vcpkg --output-on-failure
python3 -m pip install -r tests/integration/requirements.txt
DOSBOX_BIN="$PWD/build/debug-linux-heavy/dosbox_with_debugger" python3 tests/run-e2e.py --category auth
DOSBOX_BIN="$PWD/build/debug-linux-heavy/dosbox_with_debugger" python3 tests/run-e2e.py --category api
```

Always set `DOSBOX_BIN` explicitly when testing a debugger build. The integration
harness's default selects the normal Linux binary.

CI builds and runs unit tests on Linux, macOS, and Windows. Linux additionally
covers normal, debugger, and heavy-debugger configurations over live HTTP. The
heavy-debugger job runs a pinned revision of this fork's MCP bridge against the
built engine. The bridge revision is recorded in `.github/workflows/ci.yml`;
update it deliberately when the two repositories' contracts change together.

## Upstream integration

The current baseline is upstream commit `b48f36921` (2026-07-20). The maintenance
changes selectively import:

- NE2000 page-register and packet-buffer hardening through `09aa05b6b`, including
  the regression tests for malformed receive rings and oversized transmit buffers.
- Config-anchor image roots (`852a0d068`), guest `MOUNT`/`BOOT` whitelist enforcement
  (`902966105`), and canonical image paths at the mount sink (`936561047`).
- Directory type validation (`5a7433485`) and policy-side rejection of claimed
  owner trust when command injection is possible.
- Bundled Lua 5.5.0 from `f5ed1a422`; the unused sol2 dependency is removed.

These are selective backports, not a full merge of upstream 0.85.1. Keep the next
integration steps focused: dependency/platform updates, emulator correctness
fixes, then optional archive mounts and TrueType rendering. Each stage needs its
own build and API validation because upstream reorganized the source tree and
changed parts of the API independently of this fork.

Before publishing a release, run hosted CI, inspect the platform results, and
retain the `-vc` version suffix so the build is identifiable as this fork.

## Verification on 2026-10-22

Both normal and heavy-debugger engines rebuilt successfully on the local macOS
host with bundled Lua. Final incremental build logs contain no compiler warnings.

| Check | Result |
| --- | --- |
| Normal unit suite | 1,470 passed, 3 skipped, 4 inherited disabled tests |
| Heavy-debugger unit suite | 1,497 passed, 3 skipped, 4 inherited disabled tests |
| Normal API contract suite | 128 passed |
| Heavy-debugger HTTP suite | 249 passed, 3 skipped |
| Pinned MCP bridge live smoke tests | 3 passed |
| Workflow validation | actionlint and YAML parsing passed |

The HTTP run includes token-free access, guest image-policy regressions,
authentication, token provisioning, Lua, and capture. Three HTTP checks skip
because mformat or Linux-specific device/filesystem fixtures are unavailable on
this host. Unit skips cover Linux-specific paths and a CPU configuration. The
four inherited disabled tests are the adjacent port-container cases.

Enabling the live CI jobs exposed two pre-existing timing assumptions: `/status`
can answer before the emulation thread processes commands, and PIC replay time
can advance more slowly than wall time on a busy host. Readiness now requires a
successful bridged CPU read, failed launches are cleaned up, and replay tests
wait for completion. The complete normal API suite and complete heavy HTTP suite
passed after those changes.

The new hosted jobs target Ubuntu 24.04, macOS 15, and Windows Server 2025.
Linux/Windows builds and the hosted vcpkg/Xcode setup still need a GitHub Actions
run; local validation does not establish that those environments pass.
