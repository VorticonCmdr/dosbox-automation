// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "debugger_backtrace.h"

#include "debugger_disasm.h"

#include "cpu/cpu.h"
#include "cpu/paging.h"
#include "hardware/memory.h"

#include <cstring>

namespace {

// Candidate backward-decode lengths, in bytes - the shortest common near-
// CALL encodings: FF /2 register-indirect with no displacement (2, e.g.
// "call eax"), E8 rel16 (3, the ordinary 16-bit near call), E8 rel32 or
// 9A ptr16:16 (5 - ambiguous by length alone, resolved below by rejecting
// the far opcode/keyword), FF /2 with a SIB byte and a disp32 (7, a jump-
// table-style indirect call). Deliberately a small, bounded heuristic set
// (this is where 2.7's "max_frames * 4 DasmI386 calls" budget comes
// from), not an attempt to enumerate every legal CALL encoding.
constexpr uint32_t ProbeLengths[] = {2, 3, 5, 7};

uint64_t MemTotalBytes()
{
	return static_cast<uint64_t>(MEM_TotalPages()) * MemPageSize;
}

// Real-mode-style resolution (seg << 4 + offset) - see debugger_backtrace.h
// for why this doesn't attempt protected-mode GDT resolution.
uint64_t RealModeAddress(uint16_t segment, uint32_t offset)
{
	return (static_cast<uint64_t>(segment) << 4) + static_cast<uint64_t>(offset);
}

// Backward-decodes candidate call sites ending exactly at return_offset
// within `segment`, confirming a *near* call only - far calls (the CS
// pushed alongside IP) aren't chased: disambiguating them would mean
// treating an arbitrary stack slot as a plausible segment selector and
// decoding whatever happens to be there, which trades a small
// false-negative rate (far-called frames stay low_confidence) for
// avoiding a real false-positive risk. Confirms a call the same way
// DEBUG_StepOver already trusts DasmI386's rendered text to recognise
// one (debugger.cpp) - matched against "call " with the trailing space,
// not a bare substring search, since the decoder also renders an
// unrelated "callback %Iw" mnemonic (DOSBox's own DOS/BIOS callback
// trampoline opcode) that a plain search for "call" would also match.
//
// Far calls are rejected from the rendered text alone, not by peeking at
// the raw byte(s) at candidate_offset: a direct far call (opcode 0x9A)
// preceded by prefix bytes (a segment override, say) has its opcode byte
// somewhere *inside* the candidate window, not at the start of it, so a
// fixed-position raw-byte check can miss it entirely while the decode
// still lands exactly on return_offset and renders a plausible-looking
// "call ..." mnemonic - a false near-call confirmation for what's
// actually a far call. FF /3 far-indirect always renders its "far "
// keyword regardless of any prefix (checked below); the direct 0x9A form
// never does, but its %Ap operand format always renders as two colon-
// joined hex groups ("call 5678:1234") - checked here instead, which
// stays correct no matter how many prefix bytes precede the opcode. This
// also rejects the rarer near form of a segment-overridden memory
// operand (e.g. "call near ds:[1234]"), which also contains a colon -
// an accepted, conservative false negative (stays low_confidence rather
// than risk a wrong high-confidence far-call match).
bool ConfirmNearCallSite(uint16_t segment, uint32_t return_offset, bool bit32,
                         uint64_t mem_total)
{
	for (const uint32_t len : ProbeLengths) {
		if (return_offset < len) {
			continue;
		}
		const uint32_t candidate_offset = return_offset - len;
		const uint64_t candidate_phys = RealModeAddress(segment,
		                                                candidate_offset);
		if (candidate_phys >= mem_total) {
			continue;
		}

		char text[256];
		const Bitu length = DasmI386(text,
		                             sizeof(text),
		                             static_cast<PhysPt>(candidate_phys),
		                             candidate_offset,
		                             bit32);
		if (candidate_phys + static_cast<uint64_t>(length) > mem_total) {
			// Same boundary-straddling concern the disassemble
			// route guards against: don't trust a decode that ran
			// past emulated memory into IllegalPageHandler's
			// fabricated fill bytes.
			continue;
		}
		if (length != len) {
			// Didn't land exactly on return_offset.
			continue;
		}
		if (std::strncmp(text, "call ", 5) != 0) {
			continue;
		}
		if (std::strstr(text, "far") != nullptr) {
			// FF /3 far indirect - out of scope, see above.
			continue;
		}
		if (std::strchr(text, ':') != nullptr) {
			// Direct far call (CALL ptr16:16/32) - out of scope,
			// see above.
			continue;
		}
		return true;
	}
	return false;
}

} // namespace

DebugBacktrace DEBUG_Backtrace(uint32_t max_frames)
{
	DebugBacktrace result;
	if (max_frames == 0) {
		return result;
	}

	const uint64_t mem_total = MemTotalBytes();
	const bool bit32         = cpu.code.big;
	// Width is governed by CS's operand-size attribute (what actually
	// determines how many bytes `push bp`/a near call's return address
	// transfer), not SS's address-size attribute (cpu.stack.big, used
	// below only for the separate "leaves SS" bound). The two normally
	// move together but aren't the same thing and can diverge - e.g. the
	// "big real mode stack" trick (load SS from a 32-bit descriptor in
	// protected mode, then drop CR0.PE without reloading SS) leaves
	// cpu.pmode false with cpu.stack.big true but cpu.code.big false.
	const uint32_t width       = cpu.code.big ? 4 : 2;
	const uint16_t ss_selector = SegValue(ss);
	// cpu.stack.mask is the same 0xffff/0xffffffff bound the CPU core
	// itself uses to wrap SP-relative addressing (cpu.cpp's push/pop) -
	// reused here as the "leaves SS" bound: BP is addressed exactly like
	// SP for this purpose.
	const uint64_t stack_mask = cpu.stack.mask;

	DebugStackFrame frame0;
	frame0.bp              = reg_ebp & cpu.stack.mask;
	frame0.segment         = SegValue(cs);
	frame0.offset          = reg_eip;
	frame0.high_confidence = true;
	result.frames.push_back(frame0);

	uint32_t bp            = frame0.bp;
	const uint32_t live_sp = reg_esp & cpu.stack.mask;
	uint16_t frame_segment = frame0.segment;

	while (result.frames.size() < max_frames) {
		if (bp == 0) {
			result.stop_reason = DebugBacktraceStopReason::BpZero;
			break;
		}
		if (bp < live_sp) {
			result.stop_reason = DebugBacktraceStopReason::BpBelowStackPointer;
			break;
		}
		// The highest byte this frame touches is the last byte of the
		// return-IP word/dword, at bp + width + width - 1.
		if (static_cast<uint64_t>(bp) + (2ull * width) - 1 > stack_mask) {
			result.stop_reason = DebugBacktraceStopReason::BpOutOfRange;
			break;
		}

		const uint64_t saved_bp_phys = RealModeAddress(ss_selector, bp);
		const uint64_t return_ip_phys = RealModeAddress(ss_selector,
		                                                bp + width);
		if (saved_bp_phys + width > mem_total ||
		    return_ip_phys + width > mem_total) {
			result.stop_reason = DebugBacktraceStopReason::BpOutOfRange;
			break;
		}

		uint32_t saved_bp  = 0;
		uint32_t return_ip = 0;
		bool fault         = false;
		if (width == 4) {
			uint32_t v = 0;
			fault |= mem_readd_checked(static_cast<PhysPt>(saved_bp_phys),
			                           &v);
			saved_bp = v;
			fault |= mem_readd_checked(static_cast<PhysPt>(return_ip_phys),
			                           &v);
			return_ip = v;
		} else {
			uint16_t v = 0;
			fault |= mem_readw_checked(static_cast<PhysPt>(saved_bp_phys),
			                           &v);
			saved_bp = v;
			fault |= mem_readw_checked(static_cast<PhysPt>(return_ip_phys),
			                           &v);
			return_ip = v;
		}
		if (fault) {
			result.stop_reason = DebugBacktraceStopReason::StackReadFault;
			break;
		}

		if (saved_bp <= bp) {
			result.stop_reason = DebugBacktraceStopReason::BpNotIncreasing;
			break;
		}

		DebugStackFrame frame;
		frame.bp      = saved_bp;
		frame.segment = frame_segment; // assumed near - see header
		frame.offset  = return_ip;
		frame.high_confidence = ConfirmNearCallSite(frame_segment,
		                                            return_ip,
		                                            bit32,
		                                            mem_total);
		result.frames.push_back(frame);

		bp = saved_bp;
		// frame_segment is left unchanged: the near-call assumption
		// propagates to the next transition regardless of whether
		// this one was confirmed, so a single unconfirmed far call
		// doesn't also poison every frame above it with a segment
		// that's obviously wrong.
	}

	return result;
}
