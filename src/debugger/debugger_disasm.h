// SPDX-FileCopyrightText:  2002-2025 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_DEBUGGER_DISASM_H
#define DOSBOX_DEBUGGER_DISASM_H

#include "hardware/memory.h"

#include <cstddef>

// x86 disassembler, available unconditionally (not gated behind C_DEBUGGER -
// the interactive debugger UI is a separate concern from decoding
// instruction bytes into text, and the automation API wants the latter
// without requiring the former).

// Decodes one instruction starting at the physical address pc, writing its
// rendered mnemonic and operands into buffer (always null-terminated,
// truncated - never overflowed - if buffer_size is too small for the full
// text). cur_ip is the instruction pointer value used to compute displayed
// relative-branch target addresses; bit32 selects 16- vs 32-bit operand/
// address size. Returns the number of bytes the instruction occupies (at
// least 1, even for an unrecognized opcode, which decodes as `db xx`).
Bitu DasmI386(char* buffer, size_t buffer_size, PhysPt pc, Bitu cur_ip, bool bit32);

// The operand size (16 or 32) DasmI386 used for its most recent call.
int DasmLastOperandSize();

// Whether DasmI386's most recent call decoded a relative branch (Jcc, JMP
// rel, CALL rel, LOOP*, JCXZ) with a computable absolute target address -
// if so, DasmLastRelativeTarget() returns it. False for every other
// instruction shape, including indirect/far branches (ModRM/SIB-addressed
// targets are not extracted as structured data - the rendered text in
// DasmI386's buffer is still correct, just not separately machine-readable
// here).
bool DasmHasRelativeTarget();
Bitu DasmLastRelativeTarget();

#endif // DOSBOX_DEBUGGER_DISASM_H
