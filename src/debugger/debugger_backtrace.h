// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_DEBUGGER_BACKTRACE_H
#define DOSBOX_DEBUGGER_BACKTRACE_H

#include <cstdint>
#include <vector>

// Call-stack walking, available unconditionally (not gated behind
// C_DEBUGGER - same reasoning as debugger_disasm.h: walking SS:BP and
// decoding bytes to confirm a call site has nothing to do with breakpoints
// or single-stepping). Real-mode-style segment:offset throughout
// (seg << 4 + offset), matching every other debug/* route this session
// added (execute/interrupt/memory breakpoints, disassemble) - no
// protected-mode GDT resolution. In genuine protected mode (not vm86) a
// segment value isn't paragraph-relative at all, so both the addresses
// this walks and its own bounds checking would be wrong; treat results
// as unreliable there, same as every other route in this area.
//
// This is inherently a heuristic, best-effort feature (see mcp-plan.md
// 2.7): DOS real-mode code is full of hand-written asm, interrupt
// handlers and leaf routines with no BP-based frame at all. A frame is
// only ever reported as high_confidence when its return address was
// independently confirmed - everything else is a plausible-looking guess
// and should be treated as such.
enum class DebugBacktraceStopReason : uint8_t {
	MaxFrames, // hit the caller's requested/max frame count - the chain may
	           // continue further
	BpZero,    // saved BP was 0
	BpNotIncreasing,     // saved BP didn't move to a strictly higher offset
	                     // (cycle/corruption guard)
	BpBelowStackPointer, // saved BP was below the live stack pointer
	BpOutOfRange,   // offset wraps past the stack segment's own range, or
	                // outside emulated memory
	StackReadFault, // reading the saved BP/return IP faulted
};

struct DebugStackFrame {
	// This frame's own BP (its saved-BP value read from the callee's
	// frame, or the live register for frame 0). Purely informational -
	// callers who want the frame's contents can read it back via the
	// memory API.
	uint32_t bp      = 0;
	uint16_t segment = 0;
	uint32_t offset  = 0;
	// True only for frame 0 (the live PC - not a guess at all) or a
	// frame whose return address was confirmed by backward-decoding a
	// matching *near* CALL instruction ending exactly there. False means
	// the BP chain looked structurally sound but nothing confirmed it's
	// a real frame - the common case for hand-written asm, leaf
	// routines, and far calls (deliberately not chased - see
	// debugger_backtrace.cpp). A false frame's segment is a same-segment
	// guess, propagated from its caller, and may be wrong.
	bool high_confidence = false;
};

struct DebugBacktrace {
	std::vector<DebugStackFrame> frames = {};
	DebugBacktraceStopReason stop_reason = DebugBacktraceStopReason::MaxFrames;
};

// Walks SS:BP starting at the live CS:EIP/EBP, up to max_frames deep
// (frame 0 is always the live position; max_frames == 0 returns no
// frames at all). Runs entirely within the caller's own Bridge Command -
// reads live CPU/memory state directly, does not queue one itself.
DebugBacktrace DEBUG_Backtrace(uint32_t max_frames);

#endif // DOSBOX_DEBUGGER_BACKTRACE_H
