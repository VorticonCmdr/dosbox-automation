// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_DEBUGGER_CALL_H
#define DOSBOX_DEBUGGER_CALL_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Run one guest routine like its callers do, from the state the debugger is
// paused in, and put the machine back exactly where it was afterwards.
//
// This is the in-emulator half of differential testing: a client (e.g. a
// reimplementation's test harness) sends inputs, the routine runs on the
// real emulated machine - real DOS, DPMI host, BIOS and VGA answer its
// calls - and the response carries the registers and memory differences
// it produced. Many cases can run in one request, each from the same
// snapshot.
//
// What is restored: guest RAM, VGA memory (linear, fastmem, latches), CPU
// registers, segment state, the CPU block (mode, descriptor tables,
// privilege), lazy flags, FPU, the cycle counters, the active decoder and
// the main loop. What is not: other device state (PIC/PIT, DMA, sound,
// VGA registers, keyboard, mouse) and DOSBox's own host-side structures
// (open DOS file handles, EMS mappings). The call never hides touching
// those: every port access and interrupt is counted, and anything that may
// have changed state outside the snapshot is listed in `unrestored`.
//
// Hardware interrupts are off by default: the PIC queue does not run, so
// machine time stands still and the same inputs always give the same
// result. `timers` runs the PIC (deterministically, without wall-clock
// pacing) for code that waits on the timer; machine time then advances and
// is not rewound.
namespace DebugCall {

enum class CallKind {
	Near16, // real mode, near return: needs a 4-byte stub inside CS
	Far16,  // real mode, far return to a DOSBox callback
	Near32, // protected mode, 32-bit code segment, near return
};

std::optional<CallKind> ParseCallKind(std::string_view name);

// Where the sentinel return address goes.
enum class ReturnMode {
	// Push it, as a call instruction would: right when the paused state is
	// a caller about to call the routine.
	Push,
	// Overwrite the return address already at SS:SP: right when the
	// debugger is paused at the routine's first instruction (a capture
	// point), so stack arguments stay where the routine expects them.
	Replace,
};

std::optional<ReturnMode> ParseReturnMode(std::string_view name);

enum class Register {
	Eax,
	Ebx,
	Ecx,
	Edx,
	Esi,
	Edi,
	Ebp,
	Esp,
	Eflags,
	Ds,
	Es,
	Fs,
	Gs
};

std::optional<Register> ParseRegister(std::string_view name);
std::string_view RegisterName(Register reg);

struct RegisterWrite {
	Register reg   = Register::Eax;
	uint32_t value = 0;
};

struct MemoryPatch {
	uint32_t address           = 0; // physical address in guest RAM
	std::vector<uint8_t> bytes = {};
};

enum class Space { Ram, Vram };

struct Range {
	Space space    = Space::Ram;
	uint32_t start = 0; // inclusive
	uint32_t end   = 0; // exclusive
};

struct Case {
	std::vector<RegisterWrite> registers = {};
	std::vector<MemoryPatch> memory      = {};
};

struct Request {
	CallKind kind    = CallKind::Far16;
	uint16_t segment = 0;
	uint32_t offset  = 0;

	ReturnMode return_mode = ReturnMode::Push;

	// Near16 only: where the return stub is written inside CS. The stub
	// is part of the snapshot, so it disappears on restore.
	uint16_t stub_offset = 0xfffc;

	std::vector<Case> cases   = {};
	std::vector<Range> ranges = {};

	int64_t max_cycles     = 50'000'000;
	int64_t max_diff_bytes = 1 << 20; // per range and case
	int timeout_ms         = 10'000;  // whole request
	bool timers            = false;
};

struct Diff {
	uint32_t address           = 0;
	std::vector<uint8_t> bytes = {};
};

struct RangeDiff {
	Range range             = {};
	std::vector<Diff> diffs = {};
	int64_t changed_bytes   = 0;
	bool truncated          = false;
};

struct PortUse {
	uint16_t port  = 0;
	int64_t reads  = 0;
	int64_t writes = 0;
};

enum class InterruptKind { Software, Exception, Hardware };

struct InterruptUse {
	uint8_t number     = 0;
	uint8_t ah         = 0; // AH at the time of a software interrupt
	InterruptKind kind = InterruptKind::Software;
	int64_t count      = 0;
};

// A handler DOSBox implements itself (DOS, BIOS, EMS, mouse...) that ran
// during the call. These are the code paths that can change emulator-side
// state the snapshot does not cover.
struct HandlerUse {
	std::string description = {};
	uint8_t ah              = 0;
	int64_t count           = 0;
};

struct RegisterValue {
	std::string_view name = {};
	uint32_t value        = 0;
};

struct CaseResult {
	bool returned                        = false;
	std::string error                    = {};
	int64_t cycles                       = 0;
	int64_t timer_ticks                  = 0;
	std::vector<RegisterValue> registers = {};
	std::vector<RangeDiff> ranges        = {};
	std::vector<PortUse> ports           = {};
	std::vector<InterruptUse> interrupts = {};
	std::vector<HandlerUse> handlers     = {};
	std::vector<std::string> unrestored  = {};
};

// Byte differences between `before` and `after` as runs; runs closer than
// `gap` bytes are merged (including the equal bytes between them). Stops
// collecting once `max_bytes` differing bytes are reported and sets
// `truncated`; `changed_bytes` always counts every differing byte.
RangeDiff DiffBytes(const uint8_t* before, const uint8_t* after, uint32_t length,
                    uint32_t base_address, uint32_t gap, int64_t max_bytes);

// Whether a DOSBox-implemented handler call is known to only read state or
// write guest RAM, and so is fully covered by the restore.
bool IsRestorableHandler(std::string_view description, uint8_t ah);

// What the observed ports, interrupts and handler calls imply about state
// outside the snapshot. Pure, so the policy is unit-testable.
std::vector<std::string> DescribeUnrestored(const std::vector<PortUse>& ports,
                                            const std::vector<InterruptUse>& interrupts,
                                            const std::vector<HandlerUse>& handlers,
                                            int64_t timer_ticks);

// Runs every case of `request`. Emulation thread only, with the debugger
// paused. On a request-level failure (bad state, bad address), returns an
// empty vector and sets `error`; per-case failures are in CaseResult::error.
std::vector<CaseResult> Run(const Request& request, std::string& error);

// Hot-path observers, called from the port and interrupt layers only while
// a call is running.
extern bool active;
void NotePort(uint16_t port, bool is_write);
void NoteInterrupt(uint8_t number, uint8_t ah, InterruptKind kind);

} // namespace DebugCall

#endif
