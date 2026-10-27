# The interactive debugger

This covers the in-emulator, keyboard-driven debugger you get with `Alt+Pause`:
a text console with a disassembly view, register panel and command line. It's
a separate thing from the headless debugger REST API and the web debugger at
`/debugger.html`, both of which expose a subset of the same breakpoint, step
and memory machinery over HTTP for scripted use (see the README's "Web
debugger" section, or `resources/webserver/openapi.json`, for those). This
document is only about the commands you type into the debugger's own input
line.

## Building and enabling it

The debugger is compiled out of a normal build. Configure with
`-DOPT_DEBUGGER=ON` to get it, or `-DOPT_HEAVY_DEBUGGER=ON` for the extra
commands marked "heavy debugger only" below (this also turns `OPT_DEBUGGER`
on). Once it's compiled in, `[debug] debugger_window` (on by default)
controls whether it actually opens a window; set it to `false` to keep
breakpoints, stepping, and memory/register access working purely over the
REST API, without ever drawing anything.

## Opening it

- `Alt+Pause` (`Option+Pause` on macOS) toggles the debugger window at any
  time.
- From the DOS prompt, running `DEBUG <program> [args]` (or the identical
  `DBXDEBUG <program> [args]`) loads `<program>`, arms a breakpoint at its
  first instruction, and drops you into the debugger the moment it starts
  executing.
- `DEBUG /NOMOUSE` skips loading a program and instead zeroes the mouse
  driver's interrupt vector (INT 33h).

## Entering addresses and values

Anywhere a command takes a hex value, you can write a register name instead:
`EAX`/`EBX`/`ECX`/`EDX`/`ESI`/`EDI`/`EBP`/`ESP`/`EIP` (32-bit),
`AX`/`BX`/`CX`/`DX`/`SI`/`DI`/`BP`/`SP`/`IP` (16-bit), or
`CS`/`DS`/`ES`/`FS`/`GS`/`SS` (segment). A value can be followed by `+` or
`-` and another value of either kind, so `CS+SI`, `EAX+10` and `100+40` all
work wherever a `[segment]:[offset]` pair is expected.

## Commands

Type these into the debugger's input line. The debugger uppercases whatever
you type, so case doesn't matter. `HELP` (or `?`) prints this same list from
inside the debugger.

### Breakpoints

| Command | Description |
|---|---|
| `BP [seg]:[off]` | Set an execute breakpoint. |
| `BPINT [int] *` | Break on interrupt `int`, any AH/AL. |
| `BPINT [int] [ah] *` | Break on interrupt `int` with a given AH, any AL. |
| `BPINT [int] [ah] [al]` | Break on interrupt `int` with a given AH and AL. |
| `BPM [seg]:[off]` *(heavy debugger only)* | Break when that memory address changes. |
| `BPMR [seg]:[off]` *(heavy debugger only)* | Break when that memory address is read. |
| `BPPM [selector]:[off]` *(heavy debugger only)* | Break when that protected-mode memory address changes. |
| `BPLM [linear]` *(heavy debugger only)* | Break when that linear address changes. |
| `BPLIST` | List every breakpoint. |
| `BPDEL [n]` | Delete breakpoint number `n`. |
| `BPDEL *` | Delete every breakpoint. |

Leaving off the trailing AH/AL value entirely (`BPINT 21` with nothing else)
works the same as the explicit `*` form.

### Code and data view

| Command | Description |
|---|---|
| `C [seg]:[off]` | Point the disassembly view here. |
| `D [seg]:[off]` | Point the data (hex dump) view here. |

### Registers and memory

| Command | Description |
|---|---|
| `SR [reg] [value]` | Set a register (see the list above) or a single flag bit: `AF`/`CF`/`DF`/`IF`/`OF`/`ZF`/`PF`/`SF`, value `0` or `1`. |
| `SM [seg]:[off] [v1] [v2] ...` | Write a run of byte values starting at that address. |
| `MEMDUMP [seg]:[off] [len]` | Write `len` bytes as a hex listing to `MEMDUMP.TXT` in the working directory. |
| `MEMDUMPBIN [seg]:[off] [len]` | Write `len` bytes raw to `MEMDUMP.BIN`. |

### Named variables

| Command | Description |
|---|---|
| `IV [seg]:[off] [name]` | Name an address; the name replaces the raw address in the disassembly and data views from then on. This is the same list the REST API's `debug_watch_add`/`debug_watch_list`/`debug_watch_delete` operate on. |
| `SV [file]` | Save the current variable list to `file`. |
| `LV [file]` | Load a variable list from `file`. |

### Interrupts

| Command | Description |
|---|---|
| `INT [nr]` | Run interrupt `nr` and stop right after it returns. |
| `INTT [nr]` | Trace into interrupt `nr` one instruction at a time. |
| `INTHAND [nr]` | Point the disassembly view at interrupt `nr`'s handler. |
| `INTVEC [file]` | Write the full 256-entry interrupt vector table to `file`. |

### System and protected-mode info

These write to the emulator's own log output (stderr, or a log file if
configured), not the debugger's on-screen message pane.

| Command | Description |
|---|---|
| `CPU` | CR0/CR2/CR3, CPL and related CPU state. |
| `GDT` | List the Global Descriptor Table. |
| `LDT` | List the Local Descriptor Table. |
| `IDT` | List the Interrupt Descriptor Table. |
| `PAGING [page]` | Show a page table entry; omit `[page]` or use `*` for every present page. Only prints anything with paging enabled. |
| `DOS MCBS` | Walk the DOS Memory Control Block chain. |
| `SELINFO [seg]` | Describe a segment: a register name (`CS`/`DS`/`ES`/`FS`/`GS`/`SS`) or a raw selector value. |

### Logging *(heavy debugger only)*

| Command | Description |
|---|---|
| `LOG [count]` | Write a normal-detail CPU execution log to `LOGCPU.TXT`, `count` instructions. |
| `LOGS [count]` | Same, short format. |
| `LOGL [count]` | Same, long format. |
| `LOGC [count]` | Same, coverage-only format. |
| `HEAVYLOG` | Toggle an automatic CPU log written out on exit. |
| `ZEROPROTECT` | Toggle stopping execution at address 0 (catches jumps through a null pointer). |

### Misc

| Command | Description |
|---|---|
| `ADDLOG [message]` | Write `message` to the debugger's message pane. |
| `EXTEND` | Toggle extra detail in the disassembly view. |
| `TIMERIRQ` | Fire the system timer interrupt once. |
| `HELP` / `?` | Print the built-in command and key list. |

## Keyboard shortcuts

| Key | Action |
|---|---|
| `1`-`5` | With an empty input line, run 5/500/1000/5000/10000 instructions and stop. |
| `F3` / `F6` | Previous command from history. |
| `F4` / `F7` | Next command from history. |
| `F5` | Run (leave the debugger, resume normal execution). |
| `F8` | Toggle printable-character display. |
| `F9` | Set or remove a breakpoint at the cursor's position in the disassembly view. |
| `F10` | Step over the instruction under the cursor. |
| `F11` | Trace into the instruction under the cursor. |
| `Alt+D` / `Alt+E` / `Alt+S` / `Alt+X` / `Alt+B` | Point the data view at `DS:SI` / `ES:DI` / `SS:SP` / `DS:DX` / `ES:BX`. |
| `Escape` | Clear the input line. |
| `Up` / `Down` | Move the disassembly cursor. |
| `Page Up` / `Page Down` | Scroll the data view. |
| `Home` / `End` | Scroll the message pane. |

## What's REST/Lua only

`POST /api/v1/debug/call` runs one guest routine like its callers do, from
the state the debugger is paused in, and puts the machine back afterwards.
It's meant for differential testing of reimplementations: send inputs (any
number of cases per request), get back the registers and memory diffs the
routine produced. Real DOS, BIOS and DPMI answer the routine's interrupts.

- Kinds: `far16` and `near16` in real mode (near16 writes a 4-byte return
  stub at `CS:stub_offset`, 0xFFFC by default, and keeps it out of the
  diff), `near32` in protected mode with a 32-bit code segment. Paging must
  be off.
- `return: push` (default) pushes the sentinel return address like a call
  instruction. When the debugger is paused at the routine's first
  instruction, the caller's return address is already at SS:SP: use
  `return: replace` there, or every stack argument shifts by one slot.
- Restored after every case: guest RAM, VGA memory and latches, CPU and FPU
  state, the cycle counters, the active core and the main loop. Breakpoints
  are disarmed for the call and re-armed as they were.
- Not restored: device registers (anything reached through port I/O),
  state DOSBox keeps in its own structures (current directory, open files,
  EMS mappings, mouse driver) and machine time. The response counts every
  port access, interrupt and DOSBox-side handler call. Handler calls are
  checked against a short allow-list of functions known to touch only
  guest RAM (get time, get vector, memory allocation and the like); the
  rest, plus port I/O, exceptions and elapsed time, are listed under
  `unrestored`, so a client can refuse such results.
- Hardware interrupts stay off unless `timers` is set, so the same inputs
  always give the same result. With `timers` the PIC runs without wall-clock
  pacing; machine time then advances and isn't rewound.
- The routine runs on the emulation thread while it holds the Bridge, so
  other requests wait for it. `max_cycles` and `timeout_ms` bound it.
- The routine runs on the normal core whatever `core` is set to.

Limits are in `capabilities.debug_call` of `GET /api/v1/dosbox/info`.

Stepping out of the current function and running to a specific address exist
through the debugger REST API (`/api/v1/debug/step_out`,
`/api/v1/debug/run_to`) and the MCP debugger tools; there's no `STEPOUT` or
`RUNTO` command in the console. The console and automation surfaces overlap,
but commands such as descriptor-table inspection and CPU execution logging
remain console features. See `/api.html` or
`resources/webserver/openapi.json` for the full route list.
