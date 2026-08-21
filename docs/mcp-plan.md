# dosbox-automation: MCP capability plan

Three things make the current surface weak for an agent, and almost everything else is downstream of them:

1. **No way to wait.** Every "is it done yet" is a poll loop. Each iteration costs a turn, a round trip, and a full result block in the transcript.
2. **No way to tell why something failed.** Bridge timeouts, bad parameters and genuine crashes all arrive as `500 Internal server error`, and the bridge reports refusals as successes while reporting 4xx as errors.
3. **Several advertised capabilities do not work at all.** `script_run` raises AttributeError on every call. `bridge_start` fails on macOS. `type: "memory"` breakpoints return 200 and never fire. Absolute mouse coordinates are validated, recorded, and then discarded.

Fix those before adding surface. Ordering below is impact per effort within each tier.

Effort scale: S = under a day, M = a few days, L = a week or more, XL = multi-week.

**Progress: 1.1-1.8 and 2.1 done. Rest of tiers 2-4 outstanding.**

---

## Tier 1: foundations

Nothing above this tier is worth building until these land. Items 1.1 through 1.4 are defect repair; 1.5 through 1.8 are the substrate every later tool inherits.

### 1.1 Fix script_run
**Bridge. Effort: S. Status: done.**

`tools/script.py:54` calls `client.post_text(...)`. In production `client` is the `Connection` object (`server.py:87-89`), which defines only get/post/put/delete (`connection.py:178-188`). `post_text` exists only on `DosboxClient`. Every `script_run` call raises AttributeError. The unit test passes a hand-rolled fake that has the method, so it ships green.

Add the delegator, with the signature the verifier corrected (`Connection.call` is `call(self, method, path, **kwargs)`, so a third positional raises TypeError):

```python
def post_text(self, path, text, **kw):
    return self.call("post_text", path, text=text, **kw)
```

Add `params` passthrough. Write the regression test through a real `Connection` wired to `httpx.MockTransport` (the pattern exists in `tests/test_connection.py:25,37-51`), never another fake, because a fake is what hid the bug.

**Why the agent is better off:** this one line restores every blocking primitive the REST surface lacks. `dosbox.wait_for_text`, buffer-gated `dosbox.type`, `screen_match`, `wait_frames` all become reachable immediately, as a stopgap until 1.8 lands.

**Risk:** none technical. But note that fixing it opens the capability-mode hole described in 3.1: `script` is in `_INTERACT_GROUPS` while Lua exposes `mem_write`, so interact mode will start granting the memory writes it claims to withhold.

**Shipped:** exactly as proposed. `Connection.post_text()` delegator added; regression test goes through a real `Connection` wired to `httpx.MockTransport` in `tests/test_server.py`, confirmed it reproduces the original `AttributeError` on the pre-fix code.

### 1.2 Fix bridge_start on macOS
**Bridge. Effort: S. Status: done, two deviations from the proposal below, both verified against the real macOS build.**

`lifecycle.py:121-131` writes a conf file and points the child at it with `XDG_CONFIG_HOME` only. `src/misc/cross.cpp:54-64` shows macOS resolves the config dir through `HOME` and never reads `XDG_CONFIG_HOME`. So on Darwin the conf is ignored, `webserver_enabled` stays false, no token file appears, and `_wait_for_token` stalls 30s then kills the child with a message blaming the engine. This is the documented way an agent obtains a machine.

Stop depending on config discovery. `build_argv()` gains `--noprimaryconf --nolocalconf --set webserver_enabled=true --set webserver_port=N --set webserver_bind_address=127.0.0.1 --set webserver_token_file=true`. All three flags exist (`config.cpp:571,572,608`). Set `HOME` as well as `XDG_CONFIG_HOME`. Compute `token_path` with the same per-platform rule `default_token_path()` implements, parameterized on the overridden home rather than `os.environ`. Keep writing the conf file for `mount_allowed_bases` / `mount_allowed_image_roots`, which are read only from the primary config by design.

**Verify before shipping:** `lifecycle.py:129` sets `SDL_VIDEODRIVER=dummy`; the engine's own harness (`conftest.py:69`) uses `offscreen`. If the dummy driver never reaches `GFX_EndUpdate`, then under `bridge_start` freezes stop being re-asserted, frame-numbered sequences never dispatch, and every Lua script hangs at its first wait, all with 200 OK. Match the harness or prove `dummy` presents. Add an assertion that `GFX_GetRenderedFrameCount()` advances.

**Test fix required:** `tests/test_lifecycle.py`'s fake engine implements the Windows/Linux token path on every platform, which is why CI cannot see this. Make it mirror the real per-platform rule.

**Shipped, deviating on two points found while implementing:**
1. Dropped the token-file approach entirely in favor of `DOSBOX_API_TOKEN` (webserver.cpp's "channel A": a launcher-supplied token via env var, already supported precisely so a launcher never has to read a file or scrape logs). `InstanceManager` now generates the token itself and retries the authenticated attach until the child is reachable, instead of polling for a token file. This sidesteps the `default_token_path()` platform-parity work entirely, not just on macOS.
2. Tested both `dummy` and `offscreen` against the real macOS binary: `offscreen` (the plan's suggestion, matching the engine's Linux e2e harness) **aborts** on this SDL3 build (`SDL: Could not initialize SDL render backend`). Kept `dummy`, which starts cleanly and serves the webserver correctly — the plan had the risk assessment backwards for this platform.

`build_argv()` now matches the engine's own working e2e harness pattern: `--noprimaryconf --nolocalconf --set webserver_enabled=true --set webserver_port=N`, with both `HOME` and `XDG_CONFIG_HOME` redirected. Verified end-to-end: spawned the real binary, authenticated attach, feature negotiation, and a real `/api/v1/status` call, all through the production `Connection`/`_make_attach` path.

### 1.3 video/frame: validate before acquiring, RAII the frame
**Engine. Effort: S. Status: done.**

`VideoHandlers::GetFrame` (`video.cpp:211-270`) deep-copies the framebuffer at line 231 or 240 and frees it with a single unguarded statement at 269. `num_param<int>(req, Source::Param, "quality", 1, 100)` at line 263 throws on `quality=0`, `quality=101` or `quality=abc`, unwinding past the free. Up to 1.2 MB leaked per malformed request, loopable from a shell. `screen_capture` is the most-called tool in an agent loop and therefore the most likely to carry a typo'd parameter.

Parse mode, format and quality first, then acquire, then encode. Wrap the `RenderedImage` in a scope guard so both the shared-frame and rendered-tap paths free on every exit. While in there, override libjpeg's `error_exit` (`video.cpp:96` leaves the default, which calls `exit()`).

**Why the agent is better off:** a long session stops slowly leaking the emulator to death from its own typos, and a bad parameter fails in microseconds instead of after the 2000 ms rendered-frame wait.

**Prerequisite for:** 3.3 (screen_capture parameters). Do not add more parameters to that handler before this restructure.

**Shipped**, plus a second bug found while implementing the libjpeg `error_exit` fix: `buf`/`buf_size` (from `jpeg_mem_dest`) have to be declared *before* the `setjmp` call, not after, or the longjmp error path can't reach them to free a partially-encoded buffer. `FrameGuard` (a small local RAII wrapper) applied to both `GetFrame` and `GetFrameInfo`.

### 1.4 Trust-boundary validation sweep
**Engine. Effort: M to L. Status: done.**

Four concrete defects on the boundary the project's own rules call the trust boundary, plus the structural reason they were not caught.

- **Remote heap corruption.** `FreeMemoryCommand::Post` (`dos.cpp:273-284`) reads `addr` with no range check. Above `XMS_START*MEM_PAGE_SIZE` it calls `MEM_ReleasePages(addr / MEM_PAGE_SIZE)`, whose loop (`memory.cpp:346-352`) indexes `memory.mhandles` (4096 entries at 16 MB) with no bounds check. `addr=0xFFFFFFFF` writes megabytes past the vector, then follows the garbage it read.
- **Silent truncation.** `AllocMemoryCommand`'s ctor takes `uint16_t bytes` (`private/dos.h:55`) while `Post` reads a `uint32_t size` (`dos.cpp:191`) and passes it at `:241`. A request for 65536 allocates 0 and returns a valid-looking address.
- **Freeze range check wraps.** `freeze.cpp:103` is `address + width > mem_total` in 32-bit arithmetic. `address=0xFFFFFFFE, width=4` passes, and `ApplyFreezes` then writes past the end every rendered frame. `memory.cpp:87` does the same check correctly with a 64-bit cast.
- **Replay timing is unbounded doubles.** `input.cpp:418-438` checks only `< 0`; `cps` (`input.cpp:983`) only `<= 0`. `{"t": 1e308}` reaches `PIC_AddEvent` and `PIC_MakeCycles`, which asserts in debug (one request kills the process) and is UB in release. `frame` (`input.cpp:443`) is an unbounded `uint64_t` and a negative JSON integer becomes `0xFFFF...`.

Fix each as a **pure validator** in the module's `private/*.h`, not inline in the handler, which is why `tests/webserver_freeze_tests.cpp` cannot reach the freeze bug today. Add `ValidateMemoryRange(addr, len, mem_total)` with 64-bit math and use it from read, write, search and freeze. Gate `memory/free` behind an allocation registry (mutex-guarded, capped, modelled on `FreezeRegistry`) so `MEM_ReleasePages` is only ever reached with a handle the engine minted, and add a bounds guard inside `MEM_ReleasePages` itself. Replace `io_port.cpp:45-46`'s `std::stoul/stoi` with `num_param`. Add `#include "util/checks.h"` and `CHECK_NARROWING()` to every `.cpp` in `src/webserver/`, file by file; that is what would have caught the alloc truncation.

**Deliverable is the test suite:** `tests/integration/test_malformed_input.py`, a fixed hostile corpus (negative, 2^31, 2^32-1, 1e308, empty body, wrong type, unknown field, 12 MiB body, wrong Content-Type, deep nesting) applied to every registered route, asserting the response is JSON, the status is in `{400,401,403,404,409,413,415,501}`, never 500, and `/api/v1/status` still answers afterwards. Wire it into `tests/run-e2e.py`, which currently reaches only 5 of 13 suites.

**Risk:** `CHECK_NARROWING` surfaces existing implicit conversions that each need a decision, not a blanket cast. Freeing an address the API did not allocate now fails instead of silently mangling state.

**Shipped, one deliberate scope cut:** did not add `CHECK_NARROWING()` project-wide, or even to the touched files. The macro's own header comment defines it as declaring a file free of narrowing issues; `dos.cpp` still has unrelated pre-existing narrowing conversions (e.g. `DosInternalsCommand::Execute`'s `PhysPt`->`uint16_t` assignments) that are out of scope for this fix, so adding the macro would have misrepresented the file's state. All four bugs fixed with explicit runtime validation instead (`AllocationRegistry` gating `FreeMemoryCommand`/`AllocMemoryCommand`, `ValidateFreezeRange`, bounded `t`/`delay_ms`/`frame`/`cps` in `input.cpp`, `num_param`-based parsing in `io_port.cpp`). Also found and fixed a bug the plan missed: `cps` near zero in `InputTypeCommand` derives the same unbounded-timeline crash via `step_ms = 1000/cps`, not just direct `t`/`delay_ms` injection.

20 new unit tests added (`webserver_alloc_tests.cpp`, `webserver_input_validation_tests.cpp`, extended `webserver_freeze_tests.cpp`), extracting pure validators for testability rather than testing the HTTP handlers directly, matching the codebase's existing convention (`ValidatePortRequest`, `ScanBufferForValue`). Verified end-to-end against the real binary: every fixed input (oversized alloc, bogus free address, the freeze wraparound address, malformed port query, zero-quality frame, `1e308`/near-zero `cps`) now returns 400 instead of corrupting state, leaking, or crashing.

### 1.5 Typed error envelope and correct isError polarity
**Engine + bridge + protocol. Effort: M. Status: done.**

`error_handler` (`webserver.cpp:62-90`) discards `e.what()` in every branch. The precise message `num_param` builds never reaches the caller, and `Bridge::ExecuteCommand`'s timeout (`bridge.cpp:41`) is indistinguishable from a crash. That timeout is the *routine* failure: neither SDL pause loop pumps `ProcessRequests`, so a minimized or unfocused window fails every bridge-backed route with an opaque 500. On the bridge, `client._handle` flattens everything to `RuntimeError(f"{status}: {msg}")` that no handler catches; `httpx.ReadTimeout` is not caught at all (`connection.py:167` catches only `ConnectError`/`RemoteProtocolError`), producing the literal two-word result `timed out` and leaving a dead client attached. And nothing in `dosbox_mcp/` ever sets `isError`, so guard refusals and lifecycle failures return as successes while genuine 4xx return as errors. The signal is inverted for the common cases.

Engine: keep throwing `std::invalid_argument` / `std::out_of_range` (do not plumb a new exception through `num_param`, which is instantiated in every handler). Change `error_handler` to emit `{"error": {"code", "message": e.what(), "retryable"}}` with the code derived from the caught type, keeping a top-level `error` string alias for one release. Add one dedicated `Webserver::BridgeTimeout` thrown from `ExecuteCommand`, mapped to **503 + Retry-After + retryable:true**, not 500. Unify the two pre-routing text/plain rejections (`Forbidden` at `:367`, `Unauthorized` at `:383`) onto the same JSON shape. **Keep the terminal `catch (const std::exception&)` generic**: invalid_argument/out_of_range messages are parameter names and address ranges and are safe to echo; an arbitrary exception from deeper in the emulator may carry paths.

Bridge: a `DosboxError` hierarchy carrying `.status` and `.code`; replace the `"401" in str(e)` substring match (`connection.py:130,171`) with a status check on the typed exception; catch `httpx.TimeoutException` and `TransportError` in both `call` and `_try_connect`, detaching on timeout; add a per-call timeout override (required by 1.8); wrap handlers so failures return `CallToolResult(isError=True)` with tool, route, code, message and hint; flip guard and NotConnected to isError.

**Why the agent is better off:** recovery hinges on "is this worth retrying, is it my fault, or is the machine wrong". A stable code plus `retryable` answers that in one field. `retryable:true` with "the emulator may be paused or minimized" turns today's mystery 500 into an instruction.

**Blocks:** 1.8 (client timeout handling), 3.1 (`outputSchema` cannot land while error paths return bare content lists).

**Shipped**, close to the proposal, plus a few corrections found while implementing:

- **Kept `error` as a plain string, added `error_code`/`retryable` as sibling fields**, not `{"error": {code, message, retryable}}`. Nesting would have been an actual breaking type change to an existing field (string -> object), not additive - PROTOCOL.md's own versioning rule treats that as a MAJOR break. `{error, error_code, retryable}` is what shipped, and matches "keeping a top-level error string alias" literally rather than contradicting it.
- Extracted the whole classification (exception type -> status/code/retryable) into `Webserver::ClassifyException(std::exception_ptr) -> ErrorInfo`, exposed via `webserver.h`, so it is unit-testable without a live server (`webserver_error_tests.cpp`, 7 tests) - `error_handler` itself is a thin wrapper now.
- **`DosboxError` is one class, not a hierarchy**: `.status`/`.code`/`.message`/`.retryable`/`.route`, all set from the engine's response. A single typed exception with structured fields covers every dispatch need (`e.status == 401`, `e.code == "bridge_timeout"`) without the added indirection of subclasses that behave identically.
- `httpx.TimeoutException` is already a subclass of `httpx.TransportError` (verified against the installed httpx, not assumed) - one `except httpx.TransportError` in both `call` and `_try_connect` covers connect errors, timeouts, and protocol errors uniformly; no separate catch needed.
- **"flip guard and NotConnected to isError" turned out to mean every ad-hoc `except NotConnected/LifecycleError/ToolProtectedKey/ValueError: return _text(str(e))` site**, not just the one inside `guard()`. `bridge.py`'s six unguarded handlers (`_connect`, `_start`, `_stop`, `_logs`, `_setup`, `_swagger`) had the identical bug for their own exception types; fixed all of them with the same `to_error_result()` helper for consistency, since leaving them half-fixed would have kept the exact same "signal inverted" bug for `bridge_start`/`bridge_stop`/`bridge_logs`/`bridge_setup` failures.
- Added `tool` and `route` to the error text (`"[port_write PUT /api/v1/io/port] port must be 0x0000..0xFFFF"`) and a short `hint` for cases where the message alone doesn't make the remedy obvious (retryable -> "may be transient"; `unauthorized` -> "bridge_connect will re-attach"; `not_connected` -> "call bridge_status or bridge_start"). `CallToolResult(isError=True)` is the *only* way to get isError set at all - the installed mcp SDK's `call_tool` wrapper hardcodes `isError=False` for any handler return that isn't already a full `CallToolResult` (verified against the installed SDK source, `mcp.server.lowlevel.server.Server.call_tool`).
- Per-call `timeout=` override added to every `DosboxClient` method and threaded through `Connection`'s existing `**kwargs` passthrough for free. Verified against real httpx internals (`request.extensions["timeout"]`) that omitting it still uses the 30s client default rather than accidentally passing `timeout=None` (which httpx treats as no timeout at all).

Verified end-to-end through the **real MCP SDK dispatch path** (`server.request_handlers[types.CallToolRequest]`, not a mock) against the real engine binary: a `port_write` with an out-of-range port returns `isError: True` with text `"[port_write PUT /api/v1/io/port] port must be 0x0000..0xFFFF"`; a valid `mem_read` returns `isError: False` with the normal JSON payload. 46 new tests total (7 engine-side `ClassifyExceptionTest`, the rest bridge-side across `test_client.py`/`test_connection.py`/`test_bridge_tools.py`).

### 1.6 Bridge hardening: never wedge, pump while paused, report liveness
**Engine. Effort: M. Status: done.**

Three gaps in the trust boundary itself.

- `ProcessRequests` (`bridge.cpp:45-59`) runs `cmd->Execute()` with no try/catch. A `std::bad_alloc` from `SearchMemoryCommand::Execute`'s 16 MiB resize or `LuaStatusCommand`'s unbounded serialization leaves `done` false for that command and every later one in the batch, never clears the queue, never notifies, and escapes into `normal_loop`.
- The queue is unbounded and there is no liveness signal.
- `ProcessRequests` is pumped only from `dosbox.cpp:124` and `debugger.cpp:2956`. Both SDL pause loops (`sdl_gui.cpp:350`, `:2229`) block on `SDL_WaitEvent` and never pump.

Wrap `Execute` in try/catch, recording the message into the command and setting `done` in all cases. Add `std::atomic<uint64_t> last_pump_ms` and a `PumpAgeMs()` accessor; refuse new commands with a distinct `not_pumping` code when the pump is stale and `queue_full` at a depth cap. Call `ProcessRequests()` and `LuaReapStalledWaits()` from both SDL pause loops (two lines each, already on the emulation thread). Extend `GET /api/v1/status` with `{emulation: running|paused|stalled, last_tick_ms_ago, frame}`; make `rendered_frame_count` (`sdl_gui.cpp:1164-1169`) a `std::atomic<uint64_t>` first, because `/status` does not cross the Bridge and reading it today would be an unsynchronized cross-thread read.

**Separate, land first:** `RecordingHandlers::PostPause/PostStop/GetStatus` (`input.cpp:751-818`) call `InputRecording::Stop` directly on the httplib worker, and `Stop` calls `OsdManager::SetIcon`. `OsdManager` has no mutex and no atomics, and its icon array is read by `Render` on the emulation thread. Route Pause and Stop through Commands like `StartRecordingCommand` already does. This is a correctness bug, not a feature; it should be its own commit.

**Why the agent is better off:** every failure mode currently arrives as the same 500. Distinguishing "paused, tell the human to focus the window" from "queue saturated, back off" from "the command failed" is the difference between recovering and looping. Pumping while paused is also what makes background automation reliable at all.

**Risk:** API-driven state changes now happen while the user believes the machine is paused. That is the correct trade for automation, but document it and keep the OSD indicators showing activity. Add `tests/webserver_bridge_tests.cpp` (the first ever): throwing Execute still drains the batch, a timed-out command is erased and never executed afterwards, queue-full is refused without dropping.

**Depends on:** 1.5 for the error codes it returns.

**Shipped, with one deviation from the literal "two lines each" for the SDL pause loops:**

- `ProcessRequests()` now try/catches each command's `Execute()` individually, records `e.what()` into `cmd->error`, and always sets `done = true` - so a throw mid-batch (bad_alloc, whatever) can no longer strand every command queued after it. Also dropped the `queue.empty()` early return so `last_pump_ms` refreshes on every pump, even an empty one - that refresh is the whole basis for `PumpAgeMs()`/`not_pumping`/`/status`, and normal_loop pumps constantly whether or not anything is queued.
- `Bridge::ExecuteCommand` checks `PumpAgeMs() > StalePumpThresholdMs` (1000ms) *before* acquiring the lock or enqueueing, throwing the new `BridgeNotPumping` - a stalled emulator is now diagnosed in microseconds instead of after the command's own timeout (up to 15000ms once 1.8 lands). A `MaxQueueDepth` (64) cap throws `BridgeQueueFull` on overflow; both new exception types get their own `ClassifyException` branches (`not_pumping`/503, `queue_full`/429, both retryable).
- **Deviation:** the plan's "two lines each" for the SDL pause loops (`sdl_gui.cpp`) undersold what "never wedge" requires. Both loops previously called plain `SDL_WaitEvent`, which blocks *indefinitely* with no OS event - inserting a pump call next to it would only pump once per actual event, and a backgrounded/minimized window can go arbitrarily long with none. Switched both to `SDL_WaitEventTimeout(..., PausePumpIntervalMs)` (50ms) so the loop pumps on a real cadence regardless of event traffic, well under the 1000ms staleness threshold so a paused window never reads as `stalled` to the API.
- `GET /api/v1/status` gained `emulation` (`running`/`paused`/`stalled`), `last_tick_ms_ago` (`Bridge::PumpAgeMs()`), and `frame` (`GFX_GetRenderedFrameCount()`). This handler deliberately does not cross the Bridge - a Command here would block or time out in exactly the `stalled` case it exists to report. `rendered_frame_count` (`sdl_gui.cpp`) became `std::atomic<uint64_t>` first, since this is now a genuine unsynchronized cross-thread read otherwise.
- **Landed as its own commit, ahead of the rest:** `RecordingHandlers::PostPause`/`PostStop` called `InputRecording::Pause`/`Stop` directly on the httplib worker thread; `Stop` touches `OsdManager::SetIcon`, which has no mutex and is read by `Render` on the emulation thread - a real data race, not a hypothetical one. Added `PauseRecordingCommand`/`StopRecordingCommand` alongside the existing `StartRecordingCommand`, both routed through the Bridge. `PauseRecordingCommand::Execute()` re-checks `IsRecording()` on the emulation thread rather than trusting the httplib thread's own pre-check, closing a TOCTOU the original code had (recording could stop between a worker-thread check and the actual pause).
- `tests/webserver_bridge_tests.cpp` (new): throwing `Execute()` still drains the batch, a timed-out command is erased and never executed by a later pump, `queue_full` is refused without dropping the commands already queued, and a genuinely stale pump refuses fast (sub-second) rather than waiting out the command's own timeout. `webserver_error_tests.cpp` gained `ClassifyException` coverage for both new exception types.

Verified end-to-end against the real binary: `GET /api/v1/status` reports `emulation: "running"`, a live `frame` count, and `last_tick_ms_ago` near zero; the full record start/pause/resume/stop/stop-again(409)/pause-again(409) lifecycle works correctly through the new Command-routed handlers; normal Bridge-backed routes (`cpu/state`) stay unaffected. Manufacturing a genuine `stalled`/`not_pumping` state against the live binary isn't practical to script safely (it requires the emulation thread to actually stop ticking), so that mechanism is covered by the deterministic unit tests instead; likewise the two SDL pause loops themselves need a real window/OS event source neither the `offscreen` nor `dummy` headless SDL video drivers provide, so their bodies are verified by code review plus the shared pump logic's unit tests rather than a live pause.

### 1.7 Change detection for screen text and frames
**Engine + bridge. Effort: M. Status: done.**

`video/text` returns the full padded grid and nothing else. An 80x25 DOS prompt arrives as one ~2050-character line of escaped `\n` that is 98% trailing spaces, re-paid on every poll. `video/frame` always returns a full image. There is no ETag anywhere in `src/webserver/` (the only conditional-request support is `If-Match` on memory PUT). There is also no cursor position, though `ScreenTextCommand::Execute` already reads the adjacent BIOS fields.

- **Text:** add `cursor_row`/`cursor_col` via `CURSOR_POS_ROW/COL` (`int10.h:310-320`, already included) using the page it already reads. Compute a 64-bit FNV-1a **on the web thread** in `Get`, over `cmd.text_dos` (raw CP437, before UTF-8 conversion). Accept `if_none_match`, guarded with `req.has_param` because `num_param` throws on an absent value; on a match return `{is_text_mode, text_hash, cursor_row, cursor_col, unchanged:true}` and omit `text`. Always return the cursor even in the unchanged response: it moves independently of the character grid and is exactly the "is it at a prompt yet" signal.
- **Frames:** do **not** hash in the handler. `RENDER_UpdateSharedFrame` (`render.cpp:19-25,47-53`) already holds the mutex and already deep-copies the whole framebuffer on every `EndUpdate`; compute the hash there and expose `RENDER_GetSharedFrameHash()` so the web thread reads a `uint64` with no copy. A frame counter alone is not a valid ETag, because the copy happens unconditionally on identical frames. Honour `If-None-Match` on `video/frame`, `video/frame/info` and `video/text` with a 304 and empty body. Make `frame/info` respect `mode=` (it ignores it today, so the geometry of a rendered capture is unknowable).
- **Bridge:** render the text grid as real text, not escaped JSON. rstrip rows, drop trailing blank rows, prefix row numbers, one header line: `80x25 mode 0x03 cursor 12,40 hash 0x9a3f (7 blank rows omitted)`.

**Why the agent is better off:** an unchanged screen poll drops from ~2050 characters to about 30, and the hash gives the agent a one-token handle for "this screen" so it can say "wait until it differs from 0x9a3f" instead of diffing grids in context.

**Be honest:** the Bridge round trip still happens. The saving is payload, not latency. On `mode=rendered` the 304 saves the encode and transfer, not the 2000 ms forced-present wait.

**Feeds:** 1.8's `screen_change` condition.

**Shipped, with real HTTP semantics resolved in favor of the more clearly-stated instruction where the two Text/Frames bullets disagreed:**

- The plan's Text bullet wanted a JSON-bearing "unchanged" response (`{is_text_mode, text_hash, cursor_row, cursor_col, unchanged:true}`) via a query-param `if_none_match`; the Frames bullet wanted a real `304` with an *empty* body via the real `If-None-Match` header, for all three routes. A `304` with a body is a spec violation (RFC 7232) that both httplib (server) and httpx (the bridge's own client) can silently mishandle - shipped as a true, empty-body `304` with the real header, uniformly across `video/frame`, `video/frame/info` and `video/text`. `text_hash`/`frame_hash` still ride in the normal `200` JSON body (`video/text`, `video/frame/info`) so the bridge can read them without needing header access it doesn't have today - `video/frame`'s binary response can only carry it via the `ETag` header.
- **Frames get an extra optimization the plan didn't fully spell out**: the shared-frame hash is checked *before* calling `RENDER_GetSharedFrame()`, so a match skips its mutex-locked deep copy entirely instead of paying for it and discarding the result. `mode=rendered` has no equivalent precomputed hash - it still pays the up-to-2s forced-present wait, matching the plan's own "be honest" note.
- `RENDER_UpdateSharedFrame`'s hash covers `height * abs(pitch)` bytes, matching `encode_raw`'s existing size computation for the same buffer (a flipped image is addressed via a negative pitch from its last row).
- New `src/utils/fnv_hash.h` (FNV-1a 64-bit, verified against the official reference test vectors, not just self-consistency) shared by both the frame and text hashing paths.
- `EtagMatches`/`FormatEtag` extracted as pure, header-exposed functions rather than kept as file-local `httplib::Request`-taking helpers - a first draft of the `If-None-Match` parsing hit a real dangling-`string_view` bug (caught by `-Wdangling-gsl`, not by review) from binding a view directly to a temporary; the pure-function form is both safer and directly unit-testable.
- Bridge: found and fixed a discrepancy in the same pass - `dos_to_utf8`'s row-separating newlines include one after the *last* row, so splitting `text` on `\n` yields one more element than the engine's own `rows` count, which was double-counted as a phantom extra blank row. Bounded to `rows` before counting.

Verified end-to-end against the real binary and through the real MCP tool dispatch path: `video/text` first request returns cursor position and hash with a full grid; a repeat with matching `If-None-Match` returns a true `304`/`Content-Length: 0`; `video/frame` and `video/frame/info` do the same and share the identical hash (same underlying frame); `video/frame/info?mode=rendered` now reports the actual rendered geometry (758x569) distinctly from the raw framebuffer's (720x400), where before it silently reported the wrong one regardless of `mode=`. The bridge's `screen_text` tool renders a real numbered grid instead of an escaped JSON blob, with the blank-row count now exactly matching the visible screen.

### 1.8 Server-side wait
**Engine + bridge. Effort: L. Status: done.**

The single largest token and turn saving available. There is no wait-for-condition anywhere in the API: no long-poll, no SSE, no streaming (verified by grep for `set_chunked_content_provider`). Every "has it finished" is a poll, and a 60-second wait at 1 Hz is 60 model turns. The engine's own `dosbox.wait_for_text` (`lua_api.cpp:513-585`) exists only inside Lua, behind a script upload and a 2-second load rate limiter.

`POST /api/v1/wait` with `{for, timeout_ms, ...}`, long-polled on the **web thread**, never inside a Bridge Command (a 30 s wait inside `Execute` would freeze the emulation thread and every other queued request). Implement a `WaitRegistry` holding validated predicates behind a mutex and condvar; the precedent is `RenderedFrameTap` (`frame_tap.cpp:56-104`), which is already a web-thread condvar wait fed from the render path.

Evaluate from **two** places: the frame hook where `ReplayDispatchFrame`, `ApplyFreezes` and `LuaDispatchFrame` already run in sequence (`sdl_gui.cpp:1233-1235`), and **from `DEBUG_Loop`** (`debugger.cpp:2951-2957`). Without the second, a `stopped` condition can never fire, because no frames are presented while stopped in the debugger. Conversely, `text` and `frames` waits are unsatisfiable while stopped: return a distinguishable `emulator_stopped` reason rather than a bare timeout. Same for the SDL pause loops after 1.6.

Conditions: `text` (via the already-exported `Lua::MatchSubstring` over `Lua::ReadScreenText`), `screen_change` (the hashes from 1.7), `frames`, `replay_done` (the `pending_dispatched`/`pending_total` counters at `input.cpp:176-186`, which exist and have no reader), `memory` (value compare, width 1/2/4), `stopped`, `script_done`, `program`.

Hard requirements:
- **Evaluate `ReadScreenText` once per frame and share the string.** Per-waiter evaluation is ~2000 `mem_readb` calls per waiter per frame at 70 fps; the waiter cap alone does not bound that.
- Validate on the web thread before registering: `for` a strict enum, `timeout_ms` clamped 1..15000 (not 30000, see below), pattern <= 256 bytes, memory range checked with 64-bit math.
- Cap concurrent waiters at 4 and return 429. httplib's pool is `max(8, hw_concurrency-1)`; unbounded waiters starve the server of the threads needed to recover.
- Drain and notify the registry in `WEBSERVER_Destroy`.
- Response carries the observation that satisfied it (the matched screen, the stop context), so no follow-up call is needed. That pattern should govern the whole surface.

Bridge: one `wait_for` tool, and a per-call timeout override, because `DosboxClient` pins `timeout=30.0` for everything (`client.py:20`). Clamping the server side to 15 s keeps a margin under the client timeout.

**Why the agent is better off:** "wait until the installer finishes" becomes one call and roughly 40 tokens instead of 60 polls at 700+ tokens each, and it removes the class of racy sleeps where the agent guesses a delay, screenshots, and guesses again.

**Depends on:** 1.5 (timeout exceptions are unhandled today and would surface as `timed out` with a dead client attached), 1.7 (hashes for `screen_change`), 1.6 (the paused case must return a clean timeout, not a 500).

**Optional generalization:** a `PolledCommand` subclass on the Bridge (`Poll()` evaluated outside the mutex from both existing pump sites) would make deferred execution reusable beyond waits. Not required for 1.8; consider it only if a second deferred consumer appears - skipped, as proposed, since nothing else needs it yet.

**Shipped, as proposed, with a few discoveries and one deliberate extension:**

- New `src/webserver/wait.h`/`wait.cpp`: a `WaitRegistry` singleton mirroring `RenderedFrameTap`'s mutex+condvar shape (not a Bridge Command - the wait itself blocks the web thread on a condvar, never the emulation thread). `WaitFor()` registers a waiter and blocks up to `timeout_ms`; `Tick(frames_flowing)` runs on the emulation thread and is the only thing that ever reads emulator-core state for this feature.
- All 8 conditions implemented: `text`, `screen_change`, `frames`, `replay_done`, `memory`, `stopped`, `script_done`, `program`. `screen_change` takes an explicit `baseline_hash` (reusing the `text_hash`/`frame_hash` a caller already has from 1.7, per `source: "text"|"frame"`) rather than capturing a baseline at registration time, so a caller never needs a throwaway read first. `program` supports both a `pattern` substring match and, when `pattern` is omitted, an edge-triggered "wait until the program name changes" form.
- `Tick()` is called from **three** places, not two: the frame hook (`GFX_EndUpdate`, `frames_flowing=true`), `DEBUG_Loop` (`frames_flowing=false`), and - per the plan's own "same for the SDL pause loops after 1.6" - both SDL pause loops added in 1.6 (`frames_flowing=false`). `ReadScreenText()` is memoized once per `Tick()` call via a lazy local, shared by both `text` and `screen_change(source=text)` regardless of how many waiters need it.
- **Extended** the plan's "text and frames waits are unsatisfiable while stopped" rule to `screen_change` and `replay_done` too - both are exactly as frame-hook-dependent as the two the plan named (replay dispatch only advances from the frame hook; a text/frame hash cannot change if no frame renders). All four resolve as `emulator_stopped` immediately on the first `frames_flowing=false` tick rather than waiting out the timeout; `memory`, `stopped`, `script_done` and `program` still progress normally while stopped or paused, since none of them depend on a frame being presented.
- **Discovery that changed scope:** the plan assumed `DosboxClient` needed a per-call timeout override added (`client.py:20` pins `timeout=30.0`). It's already there - every verb method already accepts an optional `timeout=` kwarg and forwards it, just unused by any existing tool. `wait_for` is the first caller; no bridge client change was needed, only using what was already plumbed through.
- **Addition beyond the plan's text:** `stopped` returns a `501` ("debugger capability not built in this binary") immediately when `C_DEBUGGER` is off, matching the existing convention in `debug.cpp`'s other debugger-gated routes, rather than silently timing out on every call for a condition that can never be satisfied in that build.
- Concurrent waiters capped at `MaxWaiters = 4`, refused with a new `TooManyWaiters` (429, `too_many_waiters`) distinct from `BridgeQueueFull` - this registry never touches the Bridge queue at all. `WaitRegistry::DrainAll()` is called first thing in `WEBSERVER_Destroy()`, ahead of `OSDPORT_Destroy()`/`server.stop()`, so an in-flight wait doesn't block shutdown from joining.
- A real validation bug caught during testing, not review: checking `is_number_unsigned()` on `addr`/`value` rejected values built from a plain C++ integer literal (nlohmann only tags the unsigned representation when JSON *text* had no leading `-`; a literal like `0x1000` is always the signed representation regardless of value). Fixed by validating via the inclusive `is_number_integer()` plus an explicit non-negative check, which is also more robust for real requests than depending on that representation detail.
- Bridge: new `dosbox_mcp/tools/wait.py`, one `wait_for` tool taking the same flat `{for, timeout_ms, ...}` shape as the engine route (extra fields the chosen condition ignores are harmless), registered read-only (so it needs no mode gating) in `server.py`'s tool-registration loop. Passes `args` straight through as the JSON body and sets the httpx per-call timeout to `timeout_ms/1000 + 5.0` so the engine's own deadline always fires first.

40 new engine-side tests (parsing/validation plus `WaitRegistry` mechanics - deliberately scoped to conditions that don't need a booted DOS session, since the unit-test binary never runs one; documented in the test file) and 4 bridge-side tests, all passing. Verified end-to-end against the real binary and through the real MCP tool dispatch path: `text`/`frames`/`replay_done` resolve correctly (including a real off-by-one caught live - `Z:\>` not `C:\>`, since the default drive is `Z:`, not a code bug); `screen_change`/`program` correctly time out when nothing changes; `memory` distinguishes a real timeout (address in range, value never matches) from an out-of-range address; the 5th concurrent waiter gets a real `429` while the other 4 resolve normally; a wait in flight during `/api/v1/control/shutdown` resolves with `reason: "shutting_down"` almost immediately instead of blocking; and the bridge's `wait_for` tool correctly reports `isError: false` for a legitimate timeout but `isError: true` for a bad condition (caught by the tool schema's own enum before the handler even runs) and for the `stopped`-without-debugger case.

---

## Tier 2: high-value additions

### 2A. Capability and debugger

#### 2.1 Capability descriptor with state and limits
**Engine + bridge + protocol. Effort: L. Status: done.**

Feature flags are the wrong granularity in three ways. They are fiction (`webserver.cpp:441-446` hardcodes memory, input, cpu_registers, cpu_control, port_io and freeze to literal `true`; only `debugger` is real), so `guard(feature=...)` can block exactly one group on exactly one build. They cannot say "present but degraded". And every operational limit is invisible: the 10 MiB body cap, the 250 ms Bridge timeout, 32000 events, 4096 chars, 256 KB scripts, the 16 MiB search span, the 128 MiB read cap.

The degraded case is the worst silent failure in the system. `DEBUG_Breakpoint`/`DEBUG_IntBreakpoint` are called only from `core_normal/prefix_none.h:739,752` and `core_full/op.h:347,349`. The dynrec and dyn_x86 cores call only `DEBUG_HeavyIsBreakpoint`, and every one of those is inside `#if C_HEAVY_DEBUGGER`. The existing `build-debugger/` tree has `OPT_DEBUGGER=ON, OPT_HEAVY_DEBUGGER=OFF`, so under the default `core=auto` an execute breakpoint has **no check site at all**: 200 OK from add, 200 OK from continue, and a program that never stops.

Add `capabilities[group] = {state: on|off|degraded, reason, limits:{}}` to `dosbox/info` as a 1.2 addition, keeping `features` as a boolean projection so 1.0/1.1 peers are untouched. Cover every served group, not just the optional ones. Report all three inputs to the breakpoint predicate (built, heavy, effective core) so the reason can be phrased accurately. Populate limits from the same named `constexpr` values the validators use so they cannot drift.

**Thread safety, mandatory:** do not add a plain `CPU_IsDynamicCoreActive()` over `is_dynamic_core_active()` (`cpu.cpp:160-169`). It reads the global `cpudecoder` pointer, which the emulation thread reassigns at runtime, and `dosbox/info` is a plain lambda that does not cross the Bridge. Routing info through the Bridge is worse: it is the call the bridge makes at attach, and a 250 ms timeout there means "cannot connect". Keep a `std::atomic<CoreKind>` written by `ConfigureCpuCore` and the auto-switch path, and read the atomic. The `core` setting is `WhenIdle`, so reading the Property object from the web thread is also a race.

**Scope:** land the descriptor and the limit constants first. The bridge-side "rewrite `inputSchema` maximums at attach and send `listChanged`" half is mechanically sound (mcp 1.29 does re-run the list handler and refill its cache) but needs `ToolsCapability(listChanged=True)` and a session, and the first `list_tools` may already have been served. The descriptor alone lets tool *descriptions* carry real numbers, which is most of the win.

Delete `dosbox_mcp/capabilities.py` (dead: imported only by its own test) or make it the single group-to-capability map `server.py:88-99` consumes.

**Why the agent is better off:** it stops discovering limits by failing, and it stops setting breakpoints that structurally cannot fire.

**Shipped**, as scoped (descriptor and limit constants; the bridge-side `inputSchema`/`listChanged` rewrite explicitly deferred), with one correction to the plan's own claim about dynamic-core breakpoints:

- New `src/webserver/capabilities.{h,cpp}`. `BuildCapabilitiesBlock()` returns `capabilities[group] = {state: on|off|degraded, reason, limits}` for 11 groups: the original 7 (`memory`, `input`, `cpu_registers`, `cpu_control`, `port_io`, `freeze`, `debugger`) plus `script`, `drive`, `capture`, `wait` (the last four were never in `features` before at all - new groups the old hardcoded block simply didn't cover). `FeaturesProjection()` collapses that back to the old boolean shape (`state != off`), so a 1.0/1.1 peer sees exactly what it always saw - `dosbox/info` still returns `features`, unchanged in shape, alongside the new `capabilities` and a sibling `limits` object for the two server-wide caps (`max_request_body_bytes`, `bridge_default_timeout_ms`) that don't belong to any one group.
- **CPU core tracking, new:** `enum class CoreKind { Normal, Simple, Full, Dynamic }` in `cpu.h`, plus `std::atomic<CoreKind> active_core_kind` in `cpu.cpp`, mirroring the `rendered_frame_count` pattern from 1.6 (`cpudecoder` itself is a plain, non-atomic function pointer written only on the emulation thread - unsafe to read from `dosbox/info`'s plain lambda, which must answer without crossing the Bridge). Written at `ConfigureCpuCore` and the runtime auto-switch in `CPU_SET_CRX`, per the plan; **also** written at the reverse switch in `CPU_RestoreRealModeCyclesConfig` (leaving `auto_core` protected mode and reverting to Normal), which the plan's text didn't call out explicitly but is required for the atomic not to go permanently stale after the first `auto`-mode excursion into protected mode. The `386_prefetch`/`486_prefetch` cputype override (which can force `CPU_Core_Prefetch_Run` independent of the `core` setting) is deliberately not tracked separately - Prefetch shares the identical breakpoint check site as Normal, so folding it into `CoreKind::Normal` doesn't change any capability verdict.
- **Correction, found while mapping the actual check sites (not assumed from the plan text):** the plan's motivating paragraph claims "under `core=auto` an execute breakpoint has no check site at all" for the dynamic/JIT cores. That's not quite right. Neither `core_dyn_x86.cpp` nor `core_dynrec.cpp` has a native call to the light `DEBUG_Breakpoint()` check, but they also don't decode `0xCC` (INT3) themselves - an untranslated `0xCC` (which is exactly what an execute breakpoint patches into guest memory) falls through to `BR_Opcode`, which runs that one instruction through the interpreter (`CPU_Core_Normal_Run`), and *that* does hit the check. So execute/interrupt breakpoints still fire on the dynamic core, just indirectly, via a per-instruction fallback rather than a call site native to the JIT. `ComputeDebuggerCapability()` reports this accurately: `degraded` (not `off`) with a reason naming the interpreter-fallback mechanism specifically for `CoreKind::Dynamic`, and naming the concrete core (`normal`/`simple`/`full`) otherwise. Memory (`type="memory"`) breakpoints are a separate, unconditional `off` component of the same `degraded` state whenever `C_HEAVY_DEBUGGER` is 0, regardless of core - that check site is fully compiled out project-wide, confirmed by grep, not just absent from the JIT cores.
- `ComputeDebuggerCapability(built, heavy, core)` is a pure function (built/heavy/core all parameters, not read from macros/atomics internally), so every combination is unit-testable regardless of what this binary actually has compiled in - 6 dedicated tests cover off/on/degraded across all four `CoreKind` values.
- **Limit constants hoisted out of function-local/magic-number scope so the descriptor can cite them without duplicating (and risking drift from) the enforced value:** `MaxRequestBodyBytes` (`webserver.h`, was a bare `10 * 1024 * 1024` literal at the `set_payload_max_length` call site), `MaxInputEvents`/`MaxTypedTextChars` (`input.h`, were function-local `constexpr` in `input.cpp`), `MaxMemoryTransferBytes`/`MaxSearchSpanBytes` (`private/memory.h` - the same `128 * 1024 * 1024` literal was duplicated at both the read-length `num_param` bound and the write-size check; now one named constant backs both), `DefaultBridgeTimeoutMs` (`bridge.h`, was a bare `250` default-argument literal). `MaxEventTimeMs`/`MaxEventFrame`/`MinTypingCps`/`MaxTypingCps` (already namespace-scope `constexpr` in `input.cpp`, but non-`inline` and therefore internal-linkage - moved into `input.h` as `inline constexpr` so `capabilities.cpp` can reference them too). Freeze (`FreezeRegistry::MaxEntries`), DOS allocations (`AllocationRegistry::MaxEntries`), Lua script (`Lua::ScriptValidator::MaxBodySize`, `Lua::LuaEngine::Default{MemoryCapBytes,InstructionLimit,WallClockLimitMs}`) and wait (`wait.h`'s `Min/Max/DefaultWaitTimeoutMs`, `MaxPatternLen`, `MaxWaiters`) constants were already class/namespace-scope and header-exposed - included directly, no changes needed.
- **Found and fixed in passing, unrelated to 2.1's own diff:** `tests/webserver_bridge_tests.cpp`'s `StalePumpIsRefusedFastWithoutQueuing` (from 1.6) relies on `Bridge::Instance()` already existing with an old `last_pump_ms` by the time its sleep completes; every other test in that file calls a `RefreshPump()` helper first to establish that baseline, but this one didn't. Under `ctest`'s default one-process-per-test isolation (`--gtest_filter`), nothing had touched the singleton yet, so it was lazily constructed - with `last_pump_ms` set to "now" - only *after* the sleep, making the test's own timeout window measure staleness against nothing. Reproduced deterministically (100% of runs, both isolated and as part of the full suite) on a clean pre-2.1 checkout, confirming it predates this work; fixed by adding the same `RefreshPump()` call every other test in the file already makes.
- Bridge: `Connection` gains a `capabilities` property, mirroring `features` - populated in `_try_connect` from `info.get("capabilities", {})`, cleared in `detach()`, included in `status()`. `guard()` now checks `connection.capabilities.get(feature)` first when present: `state == "off"` refuses with the engine's own `reason` folded into the message; `degraded` still passes the call through (matching what the boolean `features` projection already implies); an engine that predates 1.2 and sends no `capabilities` block at all falls back to the exact old boolean check, byte-for-byte unchanged. Deleted `dosbox_mcp/capabilities.py` and its test - confirmed dead (imported only by itself) before removing, per the research step, not assumed from the plan's own note.

18 new engine-side tests (`webserver_capabilities_tests.cpp`: `CapabilityStateNameTest`, `ComputeDebuggerCapabilityTest` x6, `BuildCapabilitiesBlockTest` x3, `FeaturesProjectionTest` x2, `BuildServerLimitsTest`) plus the `RefreshPump()` fix, and 9 new bridge-side tests (`test_connection.py`: 4 new `guard()` cases, 2 new `TestStatus` cases, extended `test_detach_clears_state`). Verified end-to-end against the real `build-debugger` binary (`C_DEBUGGER=1`, `C_HEAVY_DEBUGGER=0`): `GET /api/v1/dosbox/info` reports `debugger: {state: "degraded", reason: "...normal core", limits: {built: true, heavy_debugger_built: false, effective_core: "normal"}}`, `features.debugger: true` (backward-compatible - `degraded` still projects `true`), every other group `on` with limits matching the enforced constants exactly, and the new top-level `limits` block. Full engine test suite: 1101 tests, same 20 pre-existing `MountPolicyTest` failures (environment-specific, unrelated), zero new failures after the `RefreshPump()` fix. Full bridge suite: 172 passed, 2 skipped (pre-existing), zero failures.

#### 2.2 Debugger stop record and debug/wait
**Engine + bridge. Effort: M.**

`debug/status` returns `{"debugging": bool}` and nothing else; continue and step return `{status, debugging}`. `CBreakpoint::CheckBreakpoint` has the matching breakpoint in hand at its `return true` (`debugger.cpp:644-673`) and `DEBUG_Breakpoint` discards it. So "run to breakpoint and tell me where I am" is four-plus tool calls and still cannot distinguish a breakpoint hit from a manual pause.

Set a file-static `DebugStopInfo` at the match sites just before `return true` (do **not** change `CheckBreakpoint`'s return type: three call sites, including `debugger.cpp:3981`, for the same information). Populate it also in `DEBUG_Enable`, the pause command, and `DEBUG_SingleStep`. There is exactly one place where `debugging = true` is assigned (`debugger.cpp:3026`), which is the choke point. Contents: `stop_id` (monotonic), reason, cs/eip/linear, register snapshot (reuse `Webserver::Registers::load`), CPU mode and the live core, breakpoint id and descriptor, and 16 raw bytes at CS:EIP.

Add `GET /api/v1/debug/wait?since_stop_id=&timeout_ms=` on the web thread against a condvar in a `Webserver::DebugEvents` singleton, never a Bridge Command. Clamp `timeout_ms` to 1..15000 and cap concurrent waiters explicitly.

**Correction that changes the shape:** `debug/continue` cannot return the resulting stop record. `DEBUG_Resume` is `DEBUG_Run(1, false)`: it executes one instruction, arms breakpoints, restores the normal loop and returns; the next stop happens arbitrarily later. Continue returns the **current** `stop_id` it is resuming from, and the client calls `debug/wait?since_stop_id=<that>`. Step is different: `DEBUG_Run(1, true)` really has executed by the time the response is written, so step can return the new record directly.

Publish side must be allocation-free: copy a POD under a short mutex and notify; build the JSON on the web thread.

**Why the agent is better off:** "continue, then tell me what happened" becomes two calls with no polling, and the answer already contains registers, the breakpoint that fired, and the code at the stop point.

**Note found while verifying:** `cpu.cpp:891` handles a hit INT3 breakpoint by setting `CPU_Cycles=0` and returning without routing to `debugCallback`, so that path never enters the debugger. The stop record will expose the inconsistency.

**Depends on:** 1.5 (client timeout handling for the long poll).

#### 2.3 Watchpoint honesty
**Engine. Effort: S, plus M if watchpoints are actually wanted.**

`POST /debug/breakpoints` with `type: "memory"` returns 200 with the engine's own read-back, and never fires: every memory branch of `CheckBreakpoint` is inside `#if C_HEAVY_DEBUGGER` (`debugger.cpp:674-733`), which the shipped debugger build has off. The engine's own BPM/BPMR/BPPM/BPLM commands are hidden on that build (`debugger.cpp:1998-2044`), so REST exposes a facility the UI does not. `tools/debug.py:59-61` asserts the opposite of the truth.

**Ship first, alone:** a `watchpoints` boolean in the features/capabilities block sourced from `C_HEAVY_DEBUGGER`, a 501 in the existing NotBuilt shape when `type=="memory"` and it is false, and a rewritten tool description. A refusal an agent can read beats a breakpoint that never fires.

**Two real bugs to fix if you extend the type set:**
1. `AddMemBreakpoint` never seeds the current byte (ctor sets `ahValue(0)`), so a watchpoint on any address whose byte is non-zero fires immediately. `BPMR` does call `FlagMemoryAsUnread`; the write variant does not.
2. The memory branch uses `return false` where it means `continue` (`debugger.cpp:684,690,693,707`), so one failed read aborts the scan of the entire breakpoint list and silently skips later breakpoints.

**Be honest about the cost:** `C_HEAVY_DEBUGGER` puts `DEBUG_HeavyIsBreakpoint` on the per-instruction path in every core. Shipping a heavy build is a performance decision. If the answer is no heavy build, do only the flag and the 501 and skip the rest.

#### 2.4 Execution control and stable breakpoint identity
**Engine. Effort: M.**

`DEBUG_SingleStep` is exactly one instruction, so stepping over a DOS call is hundreds of requests. `StepOver()` already exists and works (`debugger.cpp:929-946`) but is static and absent from the header, and the resume half lives in the F10 key handler, so exporting it means exporting both. `AddBreakpoint` takes a `once` flag that `DEBUG_AddExecuteBreakpoint` hardcodes to false. Breakpoint identity is a list position, and all three add functions `push_front`, so every add renumbers everything.

Export `DEBUG_StepOver()` (doing both halves, returning false when the instruction is not call/int/loop/rep so the caller falls back to a plain step), `DEBUG_RunToAddress(seg, off)` as one-shot breakpoint plus resume, and a `once` field on the breakpoint add body. Give `CBreakpoint` a monotonic `uint32 id` assigned at construction, report it alongside `index`, and accept either on DELETE with an unambiguous body shape (`{"id":...}` vs `{"index":...}`).

**Step-N is a footgun as usually specified.** `DEBUG_Run(n, quickexit=true)` never calls `ActivateBreakpoints`, so a multi-instruction step runs with every breakpoint disarmed. Either drive the decoder one instruction at a time checking `CheckBreakpoint` between iterations, or cap at 64 and state plainly in the description that breakpoints do not fire during a multi-step. Also note that N separate HTTP steps each get a `PIC_runIRQs` between them (`DEBUG_Loop`) while a loop inside one `Execute` does not, so a trace advances the CPU without advancing the interrupt timeline. The whole run holds the Bridge mutex, so size the cap against an explicit raised `WaitForCompletion`.

**Depends on:** 2.2 for the stop reason returned after each step.

#### 2.5 Disassembly route
**Engine + bridge. Effort: M.**

`DasmI386` (`debugger_disasm.cpp:1084`) is declared only in the private `debugger_inc.h` and reaches no route. An agent paused at a breakpoint reads base64 and decodes x86 in its head. Worse, on a non-heavy build active execute breakpoints are `0xCC` bytes patched into guest memory, so bytes read through `/api/v1/memory` while running contain traps rather than the program.

The file is genuinely self-contained: it includes only `dosbox.h`, `hardware/memory.h` and four cstd headers, and its sole engine dependency is `mem_readb<MemOpMode::SkipBreakpoints>`, which is instantiated unconditionally. So it can compile outside `C_DEBUGGER`.

**Build mechanics, corrected:** dropping the `#if C_DEBUGGER` is not sufficient. `src/CMakeLists.txt:6-8` only adds the debugger subdirectory when `OPT_DEBUGGER`, and `src/debugger/CMakeLists.txt` unconditionally links imgui. Move the file to its own directory added unconditionally, or split the CMakeLists. Put the declaration in a new public header; `debugger_inc.h` drags in SDL3.

**Validation, mandatory:** `DasmI386(char* buffer, ...)` takes no buffer length and writes via `uprintf` with no cap. Feeding it attacker-chosen bytes at an HTTP-supplied address is exactly the untrusted-input case. Add an explicit output-size parameter or a bounded internal buffer with truncation. `getbyte()` walks forward with no limit, so range-check start and count up front with 64-bit math.

Must run as a Bridge Command: decode state is file-static (`ubufs`, `ubufp`, `getbyte_mac`, `opmap1`) and `mem_readb` goes through the TLB. Clamp count 1..256 via `num_param` and raise that route's timeout above 250 ms. Read the raw-bytes column with `SkipBreakpoints`.

**`AnalyzeInstruction` is not portable.** It depends on `CDebugVar` (private to `debugger.cpp`), a static result buffer, `curSelectorName` and `CALLBACK_GetDescription`. Keep the annotation behind `#if C_DEBUGGER` or reimplement a minimal EA resolver. Do not promise annotation on stock builds.

**Return structured operand and target addresses**, not only a rendered string, so 2.17 can substitute symbols into them.

#### 2.6 Conditional breakpoints, hit counts, ignore counts
**Engine. Effort: M.**

The only condition today is AH/AL matching on interrupt breakpoints. An execute breakpoint inside a hot helper stops on the first hit with no way to say "only when AX==0x4C00" or "skip the first 200".

**Do not evaluate the condition inside `CheckBreakpoint`.** On the shipped build, execute breakpoints are `0xCC` patched into guest memory. When `DEBUG_Breakpoint` returns false at a patched byte, `core_normal/prefix_none.h:736-745` falls through to `CPU_SW_Interrupt_NoIOPLCheck(3, GETIP)`, delivering a **real INT3 to the guest** with EIP past the byte that replaced a real instruction. Today `CheckBreakpoint` effectively never returns false at a patched address; conditions and ignore counts make false the common case, so every skipped hit would corrupt the program.

Correct design: let `CheckBreakpoint` keep returning true so the trap is consumed and `oldData` is restored, then evaluate the condition at the stop point (top of `DEBUG_Loop`, or in the 2.2 publish path) and auto-resume when false. That costs a full stop/resume cycle per non-matching hit. Say so in the tool description, so an agent knows a condition on a hot function is slow rather than free.

`hit_count` can still be incremented inside `CheckBreakpoint` (it does not change the return value). Condition shape is fixed, never an expression parser: `{reg, op, value}` or `{segment, offset, width, op, value}`, validated entirely on the web thread. Memory operands use `mem_readb_checked` and friends (declared in `cpu/paging.h:396`, not `hardware/memory.h`). Reusing `Webserver::RegisterKind` means extending it with 8/16-bit sub-register names, which shares a table with `PUT /cpu/register`: check that the extension does not accidentally widen what that route accepts.

**Depends on:** 2.2 (needs the stop-point hook), 2.4 (stable ids).

#### 2.7 Call stack / backtrace
**Engine. Effort: L. Partly speculative.**

Nothing produces a call stack. Walking SS:BP and disambiguating near from far by decoding backwards to confirm a `call` is the same trick `StepOver` relies on. Report per-frame `confidence` and terminate when saved BP does not strictly increase, leaves SS, is below SP, is zero, or a read faults.

**Be honest:** DOS real-mode code is full of hand-written asm, interrupt handlers and leaf routines with no frame pointer. The common answer will be a 1 to 2 frame walk with low confidence. Render low-confidence frames visibly distinct or an agent will chase a function that never called anything. Hard-cap total memory reads and backwards-decode attempts, and raise the command timeout: 64 frames times 4 length probes is 256 `DasmI386` calls inside the Bridge mutex.

**Depends on:** 2.5. `DEBUG_StepOut` belongs here, not in 2.4, and should only be offered when frame 1 resolves with high confidence.

### 2B. Memory

#### 2.8 Memory tool rework: segmented addressing, readable views, compare-and-swap
**Bridge, plus a small engine echo. Effort: M.**

The two most-used memory tools are strictly weaker than the routes behind them. The engine serves `/memory/:segment/:offset/:len` with live register-name resolution (CS/SS/DS/ES/FS/GS on the emulation thread); the bridge only builds the linear form, so every real-mode read forces `cpu_read_registers` plus `seg*16+off` arithmetic by hand. The engine implements `If-Match` compare-and-swap returning 412 with the conflicting bytes; the bridge cannot reach it because `DosboxClient.put` takes no `headers` parameter. Both directions speak only base64, which a model cannot reliably decode in context. And `mem_read`'s description says "max 65536" while the engine's cap is 128 MiB and the bridge enforces nothing.

Add `segment` (enum of register names, case-insensitive, or an integer 0..0xFFFF, validated client-side before it is interpolated into a URL path). Document the semantic difference: the register form resolves on the emulation thread at `Execute` time, the numeric form is folded on the web thread, so `"ds"` means DS at the instant of the read and `0x1234` means a fixed address.

Add `view`: `hex` (offset-prefixed dump with a CP437 column via Python's built-in `cp437` codec), `bytes`/`words`/`dwords` (little-endian integer arrays), `text`, `base64` (default for bulk). **Cap the rendered views independently**: hex on 64 KiB is ~4000 lines, a bigger bomb than the base64 it replaces. Refuse the rendered views above a few KiB. Default length 256. Pick one client-side length cap and make description, schema `maximum` and enforcement agree, or you have only moved the lie.

Add `include_registers` defaulting false, and state plainly that this is client-side filtering: `ReadMemoryCommand::Execute` loads registers unconditionally.

Add `expected` (base64) as `If-Match`. **The 412 does not reach the tool by adding headers alone:** `client._handle` raises on any status >= 400 and the 412 body has no top-level `error` key, so the conflicting bytes arrive stringified inside a RuntimeError. Special-case 412 into a typed conflict carrying the parsed actual bytes, or CAS is worse than useless.

**Why the agent is better off:** "read 64 bytes at ES:DI" is one call instead of two plus arithmetic, the result is readable, and CAS makes patching a running game safe instead of a blind clobber of whatever the guest wrote a millisecond earlier.

**Protocol:** `If-Match`/412 is shipped engine behavior that `PROTOCOL.md` never described. Add it, with a test for both the 200 and 412 paths (there is none today on either side).

#### 2.9 mem_search: bound it, then make it a discovery loop
**Engine + bridge. Three commits.**

`SearchMemoryCommand::Post` permits a 16 MiB span and returns every match with no cap and no limit, and the bridge re-dumps it at `indent=2`, one integer per line. A width=1 search for a common byte over conventional memory is megabytes of transcript from one call. There is also no differential next-scan, so the one memory-discovery workflow that matters is structurally unavailable.

**(a) Cap, ship alone. Effort: S.** Add `limit` (default 256, max 4096) and return `{matches, total, truncated}`. Do **not** add `offset` to the plain search: without a stored result set, page N re-scans the whole span on the emulation thread. Adding fields to an existing response changes a shape `PROTOCOL.md` makes normative via openapi.json, so add additively and default the limit high enough that existing callers are unaffected, or bump the minor.

**(b) Masked signature scan. Effort: M.** `POST /memory/scan {pattern: "8B 46 ?? 50 E8", start, end, limit}`. Parse and validate on the web thread: hex pairs or `??` only, 1..256 bytes, at least one fixed byte, reject all-wildcard and reject patterns whose fixed-byte count is so low the scan blows the 2000 ms budget. This is what finds a Ghidra function in live memory and is the mechanism behind 2.16's auto-anchor.

**(c) Snapshot and refine. Effort: L.** `POST /memory/snapshot` returning a handle, and `POST /memory/diff {handle, op: changed|unchanged|increased|decreased|equals}`. Cap by **total bytes** (e.g. 32 MiB across all snapshots) with LRU eviction, not by count. Cap a stored address set at 65536, because refine reads each recorded address individually on the emulation thread inside one `Execute` with the Bridge mutex held. Do only the memcpy on the emulation thread; keep the registry and the comparison on the web thread, following `FreezeRegistry`. Re-validate every address against `MEM_TotalPages()*MemPageSize` with 64-bit math before reading. Clear the registry in `WEBSERVER_Destroy`.

**Caveat for (b) and (c):** on a non-heavy debugger build, active execute breakpoints are `0xCC` bytes in guest memory, so a scan or diff taken while running can match traps. Read through `SkipBreakpoints` or document it.

#### 2.10 Allocation tools
**Bridge. Effort: S. Depends on 1.4.**

Once the truncation, the free-by-registry gate and the JSON error bodies land, expose `mem_alloc` / `mem_free` / `mem_allocations`. Free-memory totals (largest conventional block, conventional free, UMB, XMS) belong on the allocate response or the allocations route, **not** in `dos/internals`: `private/dos.h:36-39` states that command's purpose verbatim as pointers only, "not a place to pull random info that can also be read by the client from these addresses directly". Everything else the enrichment proposals wanted (PSP, parent chain, environment, current drive, DOS version) is reachable from the list-of-lists and the SDA the route already hands out.

Two small defects in the same struct while you are there: `list_of_lists`, `dos_swappable_area` and `first_shell` are declared `uint16_t` but assigned `PhysPt` (`uint32_t`), and `WalkMcbChain` truncates silently on a corrupt chain with no flag in the response. Add `truncated`.

### 2C. Machine control

#### 2.11 Drive and mount visibility, then swapping
**Engine + bridge. Effort: M.**

"Automated multi-disk installs" is a headline feature an agent cannot perform. `POST /drive/swap` is fully implemented with mount-policy validation and correct build-before-release ordering, `PROTOCOL.md:126` lists it in the 1.0 contract, and no MCP tool calls it. Neither does anything call `mount/lock`. There is no read side at all. The decisive detail: `MountPolicy::ValidateImagePath` denies every API-origin mount when `allowed_image_roots` is empty, and the default config has no roots. So the out-of-the-box agent experience is a flat 400 "Blocked by mount policy" with no way to discover that an operator must configure roots.

- `GET /api/v1/drive` as a Bridge Command walking `Drives[]`. **Do not use `GetTypeString()`/`GetInfoString()`**: they go through `MSG_Get` and return localized UI strings. Map `DosDriveType` to stable lowercase identifiers yourself and keep `GetInfo()` as a separate free-text field. `GetInfo()` on a local drive is the mounted host path: emitting it is an information disclosure, acceptable behind the token on loopback, but decide it deliberately in a comment rather than leaking it by accident.
- **Surface `DenyReason` as a stable code.** `MountPolicy` already computes DoesNotResolve / NotRegularFile / SymlinkComponent / SystemPath / OutsideWhitelist / NotADiskImage and `drive.cpp:46` throws all of it away for one string. This single change is worth more to an agent than the listing route.
- `GET /api/v1/mount/policy` returning locked state and the configured roots.
- `GET /api/v1/mount/images`, if wanted: non-recursive, regular files only, hard cap per root, canonicalize each entry and re-run `IsUnderAnyRoot` before emitting, skip symlink components, run on the **web thread** (filesystem I/O must never sit inside a Bridge Command).
- Bridge: `drive_list`, `mount_status`, `mount_images` (read-only), `drive_swap` and `mount_lock` in group `media`, with `mount_lock` marked destructive since the latch is one-way by design.

**Note in the tool description:** `drive/swap` has no check that the target drive is mounted, and it constructs a `fatDrive` (real disk I/O) inside the Bridge with the emulation thread blocked under a 5000 ms deadline.

#### 2.12 Replay lifecycle: status and cancel
**Engine + bridge. Effort: M.**

`POST /input/sequence` returns `{status:"ok", events_scheduled:N}` the moment the chain is armed. The engine tracks exactly what an agent needs (`pending_dispatched`/`pending_total`, `frame_pending_*`, plus drift) and only ever logs it. There is no cancel, so a 90-second replay armed by mistake runs to completion and every further POST gets 409.

`GET /api/v1/input/replay/status` returning `{active, engine, total, dispatched, remaining, elapsed_ms, drift_ms, current_frame}`. The PIC path has **no active flag**: completion is implied by the queue being empty while the counters keep their last values, so derive `active` from the queue under `pending_mutex` and report a finished run as `{active:false, dispatched:N, total:N}` rather than as a live replay. `ReplayDispatchFrame` reads `frame_replay_active` outside its mutex; a status route must take the lock.

`DELETE /api/v1/input/replay` drains the queues, calls `PIC_RemoveEvents` (must run on the emulation thread, so a Bridge Command; no deadlock risk, `pic_input_handler` already takes `pending_mutex` on that thread), and clears the titlebar and OSD flags.

Add a stall watchdog so a wedged chain self-heals: with unbounded timing validated away by 1.4, the remaining wedge cases are slow-but-legal ones.

Rewrite the `input_sequence` description to say the call returns immediately and point at the status tool.

**Depends on:** 1.4 (timing validation), 1.6 (recording handlers onto the Bridge, same file).

#### 2.13 Named recording store
**Engine + bridge. Effort: L.**

All four `input/record/*` routes have zero MCP tools, and the shape is wrong for an agent anyway: `PostStop` returns the entire event vector as JSON, `rec_buffer` has no cap, and every host mouse motion appends an event. A two-minute mouse-driven install is tens of thousands of JSON objects the agent must pull into context and post straight back.

**Do this first, before any cap:** coalesce consecutive `mouse_move` events landing in the same rendered frame at record time (`OnMouseMove` currently appends every host motion). Host mice run at 125 to 1000 Hz against a ~70 Hz frame clock; summing `x_rel`/`y_rel` within a frame and keeping the last `x_abs`/`y_abs` cuts volume roughly an order of magnitude and loses nothing the replay engine can act on. That is what turns 30k events into ~3k.

Then an **in-memory** named store: `POST /input/record/stop?name=<slug>` moves the vector into a process-lifetime map, `?include_events=false` suppresses the body, `GET /input/recordings` returns metadata only, `DELETE /input/recordings/:name`. Extend `InputSequenceCommand::Post` to accept `{"recording":"<name>"}`, resolved on the web thread into an already-validated `std::vector<InputEvent>` before the Command is constructed. Zero filesystem surface. Validate `name` with the rule from `Lua::ScriptValidator::ValidateParams` (<=64 chars, `[A-Za-z0-9_-]`).

Cap `rec_buffer` at 32000 (matching the replay cap) **and set a `truncated` flag** surfaced by both `record/status` and the stop response, or an agent replays half an install and cannot tell.

**Optional, separate commit:** JSON persistence under `<config>/recordings/`. If you do it, a file on disk is untrusted input on the way back in: re-parse through the same per-type field whitelist `/input/sequence` uses, never memcpy into `InputEvent`. Canonicalize and reject symlinked directories, same discipline as `MountPolicy`.

#### 2.14 DOS mouse position
**Engine + bridge. Effort: M.**

Mouse-driven installers and GUI-era DOS software are not drivable. `x_abs`/`y_abs` are whitelisted, parsed, recorded from genuine host coordinates and round-tripped through `record/stop`, and `dispatch_input_event` calls only `MOUSE_InjectMoved(x_rel, y_rel)`. The bridge has capitulated: `input.py:66-76` tells the agent there is no absolute positioning and teaches a corner-sweep hack, and `x_abs`/`y_abs` are not even in its schema.

**The obvious fix does not work.** Adding `MOUSE_InjectAbsolute` that calls `interface.NotifyMoved(0,0,x_abs,y_abs)` reaches `MOUSEDOS_NotifyMoved`, which branches on the static `use_relative`. With the mouse captured (the normal case, and the only case for a windowless run) that is true, so `move_cursor_captured` consumes `pending.x_rel/y_rel` and the absolute coordinates are ignored. Absolute coordinates only reach `move_cursor_seamless`, where they are host-resolution-normalized, not guest pixels.

Also note the drift story is not what it looks like: `MOUSE_EventMoved` and `MOUSE_InjectMoved` both scale for the interfaces and hook the *unscaled* values, so record and replay are symmetric. The drift comes from the DOS driver's own mickey accumulation and acceleration.

Build the closed loop instead. `MOUSEDOS_GetPosition(x,y,started)` and `MOUSEDOS_SetPosition(x,y)` over the driver's plain state (`get_pos_x`/`get_pos_y`, `state.SetPosX`/`SetPosY`). `GET /api/v1/input/mouse` returns `{driver_started, x, y, buttons}`. `POST /api/v1/input/mouse {x,y}` is a Bridge Command that clamps to `state.GetMinPos*/GetMaxPos*` (do not skip this: `limit_coordinates()` exists for a reason), sets the position, and triggers the move event so the guest callback fires and the cursor redraws. Do not chase "inject the exact relative delta": `move_cursor_captured` runs deltas through pixels-per-mickey plus a move threshold, so a naive delta will not land.

**Document the limit honestly:** this is the INT 33h driver's position. Guests talking to the PS/2 or serial mouse directly (Windows 3.x, some protected-mode games) have no readable position; report `driver_started:false` rather than lying. For those, relative injection remains the only path.

Cheap follow-on: honour `x_abs` in `dispatch_input_event` when present, tracked by a `has_abs` bool set during parsing (a zero test is wrong, (0,0) is a legal position). Add `x_abs`/`y_abs` to the bridge schema and delete the corner-sweep paragraph in the same change.

#### 2.15 Capture status: path, frames, mode, compression
**Engine + bridge. Effort: S.**

`capture/video/status` returns `{capturing, mode, last_stop_reason}`. An automation run that records a seven-disk install produces an AVI the agent cannot name, size-check, or confirm got frames. The data exists: `video.frames` and `video.written` are live in the capture struct and the resolved path is computed inside `CAPTURE_CreateFile` and only logged.

Retain the path (add an out-param to `CAPTURE_CreateFile` or resolve it in `capture_video.cpp`; do **not** change its signature, it has eight callers across audio, video, midi, image, serial and OPL). Add accessors and extend the status command to `{capturing, mode, path, frames, elapsed_ms, bytes_written, compression_level, last_stop_reason}`. Retain path, frames and reason for one capture past the stop, since stop-then-ask is the normal sequence. `elapsed_ms` needs a start timestamp measured from **file creation**, not the API call: `CAPTURE_StartVideoCapture` only sets state to Pending and the file is not created until the first frame arrives.

Bridge: **one** tool change, not two. Fold `compression` (0..9) and `mode` (raw|rendered) into `video_capture_start` as optional arguments, set then start, atomically from the agent's point of view. Separate compression tools would be near-useless: the level is latched at `deflateInit2` and PUT is refused while recording.

**Why the agent is better off:** `frames` is the highest-value field. It is the only way to detect that a capture was nominally running while the emulator was paused or minimized and nothing was written.

**Disclosure note:** the path is a host filesystem path, same decision as 2.11.

### 2D. Reverse engineering support

#### 2.16 Rebuild the Ghidra mapping
**Bridge. Effort: M.**

`_to_live` computes `offset = ghidra_address - delta` and `linear = base_segment*16 + offset` with no check that the offset lands in 0..0xFFFF or is non-negative, and hands the result straight to `debug_breakpoint_add`, where the engine validates the segment but takes `offset` as a raw `uint32_t` with no range check. `_set_base` validates nothing. One live segment by construction. Lost on every restart. And all four tools register with `feature="debugger"` though the module header states the arithmetic never touches the engine, so on a stock build the agent carries four permanently-refusing tools, and in observe mode the three read-only ones survive to say "No mapping set" forever.

**Ship first, standalone:** validation in both directions (validate the **sum**, not just the parts) and dropping the feature gate. Removing the gate also needs a group that survives observe mode, or `set_base` is dropped again. Add the missing engine-side offset bound too; the bridge is not the only client.

Then: a list of ranges `[{ghidra_start, ghidra_end, live_segment, delta, label}]` resolved by containment, so multi-segment EXEs work and an uncovered address yields "no range covers this" rather than a wrong number.

Then `debug_map_auto`: take a byte signature from Ghidra, find it with 2.9(b), derive segment and delta with no manual pausing. A `.COM` variant can derive the load segment from the MCB chain (`pspSegment` + 0x10), handling "not found in the chain" explicitly since `WalkMcbChain` caps at 1000 blocks and truncates silently.

**Persistence, corrected:** persist the **delta and label only** and re-derive the live segment on attach. `delta` is stable across runs for a `.COM`; `base_segment` depends on what DOS has resident, so persisting it silently produces wrong answers on the next boot, which is the failure mode this item exists to eliminate.

**Depends on:** 2.9(b) for auto-anchoring.

#### 2.17 Symbol annotation, in the bridge
**Bridge. Effort: M.**

Every address in every response is a bare integer. A transcript full of `0x1E4F2` is unreadable and unmemorable, and the agent re-queries Ghidra per address.

**Build it in the bridge, not the engine.** The symbol data originates from Ghidra, which the agent talks to; the Ghidra-to-live mapping already lives in `tools/ghidra.py`; closure-held state is the established pattern there. An engine-side symbol store would mean a 20000-entry, 128-byte-name untrusted payload retained in emulator memory, a new webserver module, address validation and a protocol bump, all for metadata with zero emulator-side semantics. It also works on stock non-debugger builds for free, which was the original motivation for keeping it out of `src/debugger/`.

`debug_symbols_load` takes `list_functions`/`list_globals` output directly and translates through 2.16. Then annotate every address-bearing response the bridge already relays: disassembly lines, stop records, backtrace frames, and the MCB map.

The one thing that needs engine cooperation is 2.5 returning structured operand and target addresses rather than only a rendered string.

**Depends on:** 2.5, 2.16.

#### 2.18 Batch endpoint
**Engine + bridge. Effort: M. Value partly speculative.**

Every route is one request at one instruction boundary, so a struct read spanning two calls tears, a read-modify-write is racy, and a six-register VGA sequence costs six requests during which the CRTC index has moved. The only atomicity primitive is single-address CAS.

`POST /api/v1/batch {ops, on_error}` with a deliberately narrow op set (mem read/write/cas, cpu read/write, port read/write, freeze set/clear), fully parsed and range-checked on the web thread into a POD list, applied in order in one `Execute`. Caps: 64 ops, 1 MiB read, 256 KiB write, no nesting, no input/script/debug/capture ops. Scale the timeout as `250 + 4*ops` capped at 2000.

**This is the largest new untrusted-input parser in the codebase and it sits directly on the trust boundary.** Do not build it before 1.4 establishes the pure-validator pattern and the malformed-input corpus. The correctness argument is sound; whether agents actually hit the tearing case often enough to justify the parser is unproven.

---

## Tier 3: ergonomics and polish

### 3.1 One risk taxonomy driving annotations and the capability mode
**Bridge. Effort: M.**

`add_tool` sets `ToolAnnotations(readOnlyHint=read_only)` and nothing else, and the same boolean also drives `_mode_allows`, which approximates risk at group granularity via `_INTERACT_GROUPS`. That approximation is why `script_run` is available in the mode whose stated purpose is to withhold memory writes, while Lua exposes `mem_write`, key/mouse injection and `mount_lock`.

Replace the boolean with an explicit classification (`read`, `mutate_guest`, `mutate_host`, `destructive`, `lifecycle`) plus a `title`. Derive annotations mechanically and derive the mode gate from the same value, removing `_INTERACT_GROUPS` and forcing per-tool classification. Classify `script_run` as `mutate_guest`. Note the spec default: `destructiveHint` defaults to true when `readOnlyHint` is false, so declaring it explicitly on the *non*-destructive mutators (`mem_write`, `freeze_set`, `input_type`) matters as much as on `dosbox_shutdown`. `idempotentHint` stays a per-tool declaration: `mem_write` is idempotent, `input_type` emphatically is not.

Tighten every schema. mcp 1.29 validates arguments against `inputSchema` before dispatch, so each constraint is enforced for free: `enum:[1,2,4]` on width, `[1,2]` on io width, port and segment `maximum:65535`, `maxLength:4096` on text, `maxItems:32000` on events, `cps` 1..1000 (and clamp engine-side too), `additionalProperties:false` everywhere. On `input_sequence` events, a `oneOf` per type matches the engine's per-type allow-list exactly and catches the `x` vs `x_rel` typo the engine hard-400s on. Source the numbers from 2.1's limits so they cannot drift.

Fix the two false descriptions: `mem_read`'s "max 65536" against a 128 MiB engine cap, and `script_run`'s claim that any capability is reachable through Lua (the Lua surface has no port I/O, no CPU registers, no debugger, no freeze, no search, no drive swap, no framebuffer, and relative-only mouse with no wheel). Do **not** "fix" the claim that there is no absolute mouse positioning until 2.14 lands: it is currently true.

**`outputSchema` is a trap as usually sequenced.** Once a tool declares one, the SDK returns an error result if the handler returns unstructured content only. Every guarded handler returns a plain content list on its error paths, so adding `outputSchema` to `cpu_read_registers` would turn "Cannot reach dosbox at ..." into "Output validation error". Land 1.5 first, then migrate each tool's return to the structured form in the same commit that adds its schema.

**Dependency floor:** `pyproject.toml` pins `mcp>=1.2,<2`. `Tool.title` and `outputSchema` do not exist that far back, and argument validation landed much later than 1.2, so on a minimum-floor install none of this is enforced. Raise the floor.

Add a test that iterates the registry and fails on any tool without a title, a risk class, and consistent annotations.

**Depends on:** 1.5, 2.1.

### 3.2 Bridge output rendering
**Bridge. Effort: S.**

`dos_memory_map` fetches `/dos/internals`, discards list-of-lists, the SDA and first-shell, and dumps up to 1000 raw MCB objects at `indent=2`. `dosbox_status` fires three Bridge round trips for largely duplicate data and reports the program name three times. `mem_search` re-dumps the engine's array at `indent=2`, one integer per line. Summarize by default with a `detail:true` escape hatch, and stop pretty-printing arrays.

### 3.3 screen_capture parameters
**Engine + bridge. Effort: S. Depends on 1.3.**

`screen.py:46-50` hardcodes `format=png` and exposes nothing else, while the engine offers jpeg with quality, raw, and `mode=rendered`, so the agent can never see the post-shader image the human sees, never trade quality for size, and never ask for a thumbnail. `encode_png` pins compression level 1, roughly double the bytes of level 6.

Add `mode`, `format`, `quality`, `scale` (fixed enum divisor, box filter on the RGB888 buffer, on the web thread) and `crop` (validated against the actual frame dimensions after acquisition, rejected not clamped, applied before `convert_to_rgb888`). Raise the PNG default to level 6 but keep it parameterized.

**Do not default to jpeg.** Chroma subsampling smears text-mode glyphs and CGA/EGA dithering, which is exactly what the vision model has to read. PNG at level 6 (lossless, roughly half today's bytes) is the right default; let the agent opt into jpeg for large graphics frames.

### 3.4 Bridge cleanup
**Bridge. Effort: S.**

- Delete `bridge_help`: it re-sends the tool list the client already received, and derives one-liners via `description.split(". ")[0]`, which collapses `debug_breakpoint_add`'s three-kind explanation to "Add a breakpoint".
- Delete `bridge_version`: a strict subset of `bridge_status`.
- Add `debug` and `control` to `KNOWN_ROUTE_PREFIXES`. `/api/v1/control/shutdown` is already a false positive today, and all seven debug routes join it the moment openapi.json is corrected, at which point `bridge_swagger` starts telling the agent that tools the bridge itself exposes are outside the protocol.

**Do not fold 48 tools into action enums.** Measured, the whole registry is ~18.4k chars and the realistic saving is 1.5 to 2k, not a halving. Worse, `_mode_allows` gates registration per tool from `read_only`, and every family named for folding mixes read and write actions, so merging makes each merged tool non-read-only and observe mode loses `freeze_list`, `video_capture_status`, `script_status`, `debug_map_status`, `bridge_status`, `bridge_logs` and `bridge_swagger`. That is a straight regression for the mode whose purpose is read-only observation. If you want folding, fold only within one `read_only` class.

### 3.5 Script ergonomics
**Engine + bridge. Effort: S.**

Split `script_run` into `script_load(script, name, seed, debug, start=True)` and `script_start`, passing the three query params the engine already validates and that MCP can reach none of today. Map 429 to a message quoting `Retry-After`.

Engine: `GET /api/v1/script/log` serving the remembered `mgr.Log().FilePath()`, tail-capped, with **no caller-supplied path component at all**, refusing when no debug script is loaded. Open a separate read handle: the emulation thread holds the FILE* open and writes to it, so accept a possibly-torn last line and never route the read through the Bridge. Fold the path into `script/status`.

Separate commit, standalone bug fix: move the rate-limiter check in `LuaLoadCommand::Post` **below** the content-type, body and param validation. Today a request about to be rejected 415/413/400 still burns the 2-second slot, so a Content-Type mistake costs two seconds per retry.

### 3.6 Instance identity and restart detection
**Engine + bridge. Effort: S.**

`Connection.call` catches a RuntimeError containing "401", detaches, re-attaches, and **replays the request**. That is exactly what an engine restart looks like from outside. After the silent re-attach, the freeze registry, the loaded script, every breakpoint and the entire guest state are gone, and the agent keeps typing into a fresh COMMAND.COM and misreading every screen after that. Replaying a mutating request (an input sequence, a memory PUT) into a fresh DOS session is worse than an error.

Generate a 128-bit `instance_id` at `WEBSERVER_Init` reusing the CSPRNG in `generate_api_token` (same fail-closed posture) and add `{instance_id, pid, started_at_unix, uptime_ms}` to the **authenticated** `dosbox/info` only. Bridge: store it at attach, compare after re-attach, raise a distinct `EngineRestarted` carrying both ids instead of replaying. Report it in `bridge_status`, which currently reports `engine_name` from a field the engine structurally never sends.

**Do not add a `webserver_state_dir` setting.** Parallel instances are already isolatable and the engine's own harness proves it: `conftest.py:76-91` runs concurrent instances with HOME + XDG_CONFIG_HOME + `DOSBOX_API_TOKEN`, and an env-token instance never writes the token file at all. **Do not teach macOS `get_or_create_config_dir()` to honour `XDG_CONFIG_HOME`** either: plenty of macOS users set it globally, and making it authoritative silently relocates the config dir of every existing install. HOME is the isolation lever.

### 3.7 Move handlers off the event loop
**Bridge. Effort: S.**

`async def call_tool` calls the sync handler inline, and every handler makes a blocking httpx call with a flat 30 s timeout. One slow call (a `mode=rendered` frame waits up to 2 s in the engine) stalls the whole server: no concurrency, no cancellation, no progress notifications. Move handlers to `anyio.to_thread.run_sync`.

**Prerequisite for** any background work in the bridge, including resource subscriptions.

### 3.8 Bound the Lua output serializer and get it off the emulation thread
**Engine. Effort: M.**

`LuaTableToJson` caps recursion depth at 10 and nothing else: no node budget, no byte budget, no visited set. Ten tables each holding ten references to the previous one is about a hundred Lua slots and expands to 10^10 emitted nodes, inside a Bridge Command, on the emulation thread, with the Bridge mutex held. Because `ProcessRequests` has no try/catch (until 1.6), the resulting `bad_alloc` takes the process down.

Add a budget struct (max nodes, max bytes, a visited set of `lua_topointer` values) and report `output_truncated`. Split the work: `Execute` copies into a plain owned structure already reduced by the budget; the nlohmann conversion and dump happen on the web thread in `Get`. The intermediate representation must own its strings, since the Lua GC runs between phases.

`dosbox.output` is the only structured channel a script has back to the caller, so agents will use it heavily. Do this before encouraging that.

**Depends on:** 1.1 (nothing uses scripts today), 1.6.

---

## Tier 4: hygiene and infrastructure

### 4.1 Regenerate openapi.json and keep it honest
**Engine. Effort: S. Highest value in this tier.**

The spec is declared normative by `PROTOCOL.md`, is served unauthenticated, and is what `bridge_swagger` reads. It documents 36 paths against 48 registered, omits all seven debug routes, and still says `info.version 0.84-da1` against 0.84-da3. The only test asserts `openapi == "3.1.0"` and that `/api/v1/status` is a key.

Regenerate, and add a test that walks the registered route table and asserts every `(path, method)` appears in the spec. Note the naming divergence while regenerating: the spec table writes `memory/{offset}/{length}`, the engine registers `{len}`.

Also, `config_home` is mounted at `/` ahead of `resource_home`, so a file dropped in the user-writable config dir shadows the shipped resource and is served unauthenticated, including this normative document. Worth a decision.

### 4.2 Close the protocol negotiation loop
**Engine + bridge + protocol. Effort: S.**

The engine emits no `mcp_protocol` and no `name`, and `/api/v1/hello` does not exist, so `effective_version()` always takes the implicit-1.0 branch while both peers ship the entire 1.1 debug surface. `bridge_status` reports protocol 1.0, `engine_name` is permanently null, and `bridge_swagger` will flag the debug routes as outside the protocol.

Add `name` and `mcp_protocol` to `dosbox/info`, sourced from one `constexpr` in `webserver.h`. Register `GET /api/v1/hello` returning exactly `{name, version, mcp_protocol}` and nothing else: it is pre-auth, so no pid, no uptime, no state read, no Bridge crossing. Add a separate `IsPublicApiPath(method, path)` predicate consulted before the bearer check, exact-match on the one string, GET/HEAD only, Host allowlist still applied. **Do not widen `IsPublicDocPath`**: the comment there explains exactly why it is exact-match, and the config_home mount makes any widening a path toward the token file.

Make `KNOWN_ROUTE_PREFIXES` a dict keyed by minor. Raising `BRIDGE_PROTOCOL` to 1.1 is cosmetic until something consumes it.

**Do not gate tool registration on the negotiated version.** `Connection` starts disconnected by design and `bridge_start` must spawn before any attach exists, so at `build_server()` time there is no negotiated version; registration-time gating on connection state would break `bridge_connect` and `bridge_start`. Feature gating stays call-time, inside `guard()`, which is what the spec already says.

### 4.3 Tests
**Both. Effort: L.**

The gaps, in rough priority:
- 29 of ~33 REST-backed MCP tools have never issued a real or mocked HTTP request in any test; they are only asserted to be registered by name. A wrong path, verb or body ships green. The two route tests that exist were both written after production bugs.
- The debugger REST group (7 routes) has zero tests in either repo, and the 501-not-built shape is unasserted.
- `memory/freeze`, `io/port`, `memory/search` and `cpu/register PUT` have no HTTP-level test anywhere. They back 8 MCP tools.
- `If-Match`/412 has no test on either side.
- No C++ test for `Bridge` (the stated trust boundary) or for `input.cpp` (the largest untrusted-input parser).
- `test_api_contract.py:73` hardcodes `features["debugger"] is False`, so the existing debugger build cannot pass the suite. Parametrize over the build (read the expected value from the served block or an env marker) rather than deleting the assertion.
- `tests/run-e2e.py` reaches 5 of 13 suites; mount policy, path traversal, token provisioning, openapi docs, screen text and graphics modes are invisible to the documented runner.
- The bridge has no live-engine lane at all. The engine's harness (`conftest.py`) is good and is not packaged, importable or published, so the bridge cannot reuse it.

### 4.4 CI
**Both. Effort: M.**

The engine's four workflows only deploy the website. The bridge repo has no `.github` at all. Nothing runs gtest, `run-e2e.py`, pytest, ruff or bandit automatically, and nothing runs the two repos against each other.

### 4.5 Delete the stale trees
**Engine. Effort: S.**

`extras/mcp/` is a six-week-old fork of a third of the bridge (no `protocol.py`, `lifecycle.py`, `cli.py`, `tools/bridge.py`, `tools/ghidra.py`, no PROTOCOL.md), is not packaged, is referenced by no build rule, is license-attributed in the shipped `THIRD_PARTY_LICENSES.txt`, and its README tells developers to test against it. Anyone reading the engine repo for the MCP surface reads the wrong code. Delete it or reduce it to a pointer.

`extras/api/` ships a JS client that sends no Authorization header (so it cannot work against a token-enforcing engine), declares response fields the engine deliberately removed, has zero tests, and is the only place `compareAndSwap` and `alloc`/`free` are exercised at all. Decide: fix it or delete it.

### 4.6 Make the default token obtainable
**Engine. Effort: S.**

With `webserver_token_file=false` (the default) and no `DOSBOX_API_TOKEN`, the only output is `"API token: %.8s..."`. The full 64-char token is never printed anywhere. Meanwhile the setting help says "generated at startup and printed to the log output", the token_file help says "instead of printing it to the log", and the OpenAPI security scheme says "the API token printed to the log at startup". The out-of-the-box configuration cannot be authenticated to, and three user-facing texts claim otherwise. Either print it or fix all three texts and change the default.

### 4.7 Token scopes
**Engine + bridge + protocol. Effort: L. Value depends on deployment.**

The engine issues one all-powerful token. The only least-privilege control is the bridge's `mode`, enforced at registration time inside one Python process, invisible to the engine and bypassable by any local process holding the token, or by the agent itself through `script_run`.

A `webserver_token_scopes` setting (`read, write, input, script, media, debug, control`) checked in the pre-routing handler right after the bearer comparison, backed by a pure `RequiredScopeFor(method, path)` table, would let an operator hand out a read+input token honestly and would close the `script_run` escape hatch at the layer that can actually close it.

**Be honest about when this pays:** it matters for unattended runs and for multiple agents sharing a machine. For a single supervised agent it is policy overhead. The table is a security control that must be exhaustive and fail closed, with a test asserting every registered route appears in it, or it will drift exactly as openapi.json did.

---

## Dependency graph

```
1.1 script_run          (independent, do first)
1.2 bridge_start        (independent)
1.3 frame RAII    ------------------------------> 3.3 screen_capture params
1.4 validation sweep ---------------------------> 2.10 alloc tools
                     \-------------------------> 2.12 replay lifecycle
                      \------------------------> 2.18 batch
1.5 error envelope ----> 1.8 wait
                    \--> 2.2 stop record (long poll)
                     \-> 3.1 outputSchema
1.6 bridge hardening --> 1.8 wait (paused case)
                     \-> 2.12 replay (same file, recording handlers)
                      \> 3.8 lua serializer
1.7 change detection --> 1.8 wait (screen_change)
2.1 capabilities ------> 3.1 schema maximums
2.2 stop record -------> 2.4 execution control
                    \--> 2.6 conditional breakpoints
2.4 stable ids --------> 2.6
2.5 disassembly -------> 2.7 backtrace -> step-out
                    \--> 2.17 symbols (structured operands)
2.9b masked scan ------> 2.16 ghidra auto-anchor -> 2.17 symbols
2.13 mouse coalescing -> 2.13 recording store (do coalescing first)
3.7 async handlers ----> any bridge background work
```

Critical path for the biggest win: **1.5 -> 1.7 -> 1.8**. Everything else can proceed in parallel.

---

## Speculative, deferred, or rejected

**Machine checkpoints (RAM + CPU snapshot with restore).** XL, highest risk on the list. Converts brittle automation into search, which is genuinely transformative, but restoring RAM under unrestored device state (VGA CRTC, DMA mid-flight, sound buffers) wedges the guest in ways that look like emulator bugs, and missing the dyn-core cache flush produces silent wrong execution rather than a crash. If attempted: name it `checkpoint` not `savestate`, default it off behind a byte budget, return a `not_restored` list on every restore, refuse across a video-mode change, and refuse while a capture, replay or script is in flight. Do not start it before Tier 1 is done.

**Server-sent event stream.** Largely subsumed by 1.8 and 2.2. MCP has no server-push to the model, so the bridge would consume the stream in a background task and surface it as a drainable buffer, which is real client-side work on top of the engine change. Long-lived connections in a thread-per-connection server need a hard concurrency cap. Revisit only if a consumer appears that waits cannot serve.

**DOS/BIOS interrupt trace ring.** Plausible and cheap when disabled (one relaxed atomic on the hot DOS path), and it would work on every core and on non-debugger builds where breakpoints do not. But the value ("why did the installer fail") is unproven against just reading the screen, and it puts a hook on `DOS_21Handler`. Guest-controlled paths going into a JSON response need the same sanitization `TITLEBAR_NotifyProgramName` does.

**MCP resources and subscriptions.** The ETag half is in 1.7 and pays off unconditionally. The resources half depends on client support that is uneven, and on 3.7 landing first, or the notification channel is silently unreliable, which is worse than not having it. Land 1.7 and measure before building `resources.py`.

**Backtrace (2.7).** Kept in the plan but honestly heuristic in DOS real mode. Expect most stops to resolve one or two frames.

**Folding 48 tools into action enums.** Rejected. Measured saving is small and it regresses observe mode. Keep only the four cleanups in 3.4.

**Engine-side symbol table.** Rejected in favour of 2.17. Pure metadata with zero emulator-side semantics does not belong behind the trust boundary.

**`webserver_state_dir` and macOS XDG support.** Rejected in 3.6. Redundant with existing env-based isolation, and the macOS change silently relocates existing installs.

**Enriching `/dos/internals` with PSP, environment, current drive, DOS version.** Rejected. Contradicts the documented purpose of that command, and all of it is derivable from the pointers it already returns.