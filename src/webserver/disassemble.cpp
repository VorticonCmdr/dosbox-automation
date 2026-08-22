// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/disassemble.h"
#include "webserver.h"

#include "debugger/debugger_disasm.h"

#include "cpu/cpu.h"
#include "hardware/memory.h"

#include "base64/base64.h"
#include "json/json.h"

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

void DisassembleCommand::Execute()
{
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;
	// Real-mode-style seg:off, matching every other debug/* route this
	// session added (execute/interrupt/memory breakpoint addresses,
	// debug/run_to) - no protected-mode GDT resolution. Real-mode DOS
	// code (this tool's overwhelming common case) has no per-segment
	// operand-size bit to get wrong either way; disassembling protected-
	// mode code at a segment other than the live CS may pick the wrong
	// 16-/32-bit default (see the bit32 comment below).
	//
	// 64-bit arithmetic deliberately: segment and offset are untrusted
	// HTTP input, and a uint32_t (segment<<4)+offset can overflow and
	// wrap into a small, in-range-looking address instead of correctly
	// failing the bounds check below (e.g. segment=0xFFFF,
	// offset=0xFFFFFFFF wraps to a valid low address in 32-bit math).
	const uint64_t start = (static_cast<uint64_t>(segment) << 4) +
	                       static_cast<uint64_t>(offset);

	if (start >= mem_total) {
		error = "segment:offset is outside emulated memory (" +
		        std::to_string(mem_total) + " bytes)";
		return;
	}

	instructions.reserve(count);

	uint64_t physical = start;
	for (uint32_t i = 0; i < count; ++i) {
		if (physical >= mem_total) {
			truncated = true;
			break;
		}

		char text[256];
		// bit32 comes from the live CS's current operand-size default,
		// same as every interactive-debugger call site - there's no
		// per-instruction way to know the intended operand size other
		// than decoding it, which is exactly what this is doing.
		const Bitu length = DasmI386(text,
		                             sizeof(text),
		                             static_cast<PhysPt>(physical),
		                             static_cast<PhysPt>(physical),
		                             cpu.code.big);

		// DasmI386's own byte fetcher has no notion of mem_total - only
		// a fixed per-instruction cap (debugger_disasm.cpp's
		// MaxInstructionBytes) - so an instruction starting just inside
		// bounds but longer than the remaining room decodes some of
		// itself from IllegalPageHandler's fabricated fill bytes rather
		// than real guest memory. Drop it instead of reporting a
		// fabricated instruction as real; this is what the "stops early
		// rather than walk past the end of emulated memory" contract
		// (disassemble.h) actually means for a boundary-straddling
		// instruction, not just for the next instruction's start.
		if (physical + static_cast<uint64_t>(length) > mem_total) {
			truncated = true;
			break;
		}

		DisassembledInstruction inst;
		inst.offset     = static_cast<uint32_t>(physical);
		inst.length     = static_cast<uint32_t>(length);
		inst.text       = text;
		inst.has_target = DasmHasRelativeTarget();
		inst.target = inst.has_target
		                    ? static_cast<uint32_t>(DasmLastRelativeTarget())
		                    : 0;

		// Every byte of this instruction is now guaranteed < mem_total
		// (checked above), so no separate per-byte bound is needed here.
		std::string raw_bytes;
		raw_bytes.resize(length);
		for (Bitu b = 0; b < length; ++b) {
			raw_bytes[b] = static_cast<char>(mem_readb<MemOpMode::SkipBreakpoints>(
			        static_cast<PhysPt>(physical + b)));
		}
		inst.bytes_b64 = base64::to_base64(raw_bytes);

		instructions.push_back(std::move(inst));
		physical += length;
	}
}

void DisassembleCommand::Get(const Request& req, Response& res)
{
	const auto segment = num_param<uint16_t>(req, Source::Path, "segment");
	const auto offset  = num_param<uint32_t>(req, Source::Path, "offset");
	const auto count   = num_param<uint32_t>(
	        req, Source::Path, "count", 1, MaxDisassembleCount);

	DisassembleCommand cmd(segment, offset, count);
	// Worst case: MaxDisassembleCount instructions, each up to
	// debugger_disasm.cpp's own per-instruction byte cap - cheap
	// individually, but matches SearchMemoryCommand/DebugStepCommand's
	// own raised-deadline precedent for a bounded, non-trivial worst
	// case under the held Bridge mutex.
	cmd.WaitForCompletion(2000);

	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	json list = json::array();
	for (const auto& inst : cmd.instructions) {
		json ji;
		ji["offset"] = inst.offset;
		ji["length"] = inst.length;
		ji["text"]   = inst.text;
		ji["target"] = inst.has_target ? json(inst.target) : json(nullptr);
		ji["bytes"] = inst.bytes_b64;
		list.push_back(ji);
	}

	json j;
	j["segment"]      = segment;
	j["offset"]       = offset;
	j["truncated"]    = cmd.truncated;
	j["instructions"] = list;
	send_json(res, j);
}

} // namespace Webserver
