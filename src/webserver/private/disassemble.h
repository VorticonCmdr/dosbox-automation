// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_DISASSEMBLE_H
#define DOSBOX_WEBSERVER_DISASSEMBLE_H

#include "webserver/bridge.h"

#include <cstdint>
#include <string>
#include <vector>

#include "http/http.h"

namespace Webserver {

// count is capped at this; each instruction can take up to
// debugger_disasm.cpp's own MaxInstructionBytes, so the worst case per
// request is bounded and cheap regardless.
constexpr uint32_t MaxDisassembleCount = 256;

struct DisassembledInstruction {
	uint32_t offset = 0; // physical offset from the request's segment:offset
	uint32_t length  = 0;  // bytes this instruction occupies
	std::string text = {}; // rendered mnemonic and operands
	bool has_target  = false;
	// Absolute physical target address of a relative branch (Jcc, JMP
	// rel, CALL rel, LOOP*, JCXZ), only meaningful when has_target.
	// Indirect/far branches aren't extracted as structured data - text
	// still renders correctly, there's just nothing here to substitute a
	// symbol into (2.17's job, if ever extended to that case).
	uint32_t target       = 0;
	std::string bytes_b64 = {}; // raw instruction bytes, base64
};

// x86 disassembly, unconditionally available - unlike every other route in
// this directory, the underlying decoder (debugger/debugger_disasm.h) does
// not need C_DEBUGGER (2.5). Decodes count instructions starting at
// segment:offset; stops early (and sets truncated) rather than walk past
// the end of emulated memory.
class DisassembleCommand : public Command {
public:
	DisassembleCommand(uint16_t segment, uint32_t offset, uint32_t count)
	        : segment(segment),
	          offset(offset),
	          count(count)
	{}

	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);

	std::vector<DisassembledInstruction> instructions = {};
	// truncated: decoding started fine but ran out of room partway
	// through (instructions are variable-length). Distinct from the
	// inherited Command::error, set instead when segment:offset itself
	// is already outside emulated memory - a real client error.
	bool truncated = false;

private:
	uint16_t segment = 0;
	uint32_t offset  = 0;
	uint32_t count   = 0;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DISASSEMBLE_H
