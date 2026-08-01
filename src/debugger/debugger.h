// SPDX-FileCopyrightText:  2002-2025 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_DEBUG_H
#define DOSBOX_DEBUG_H

#include "config/config.h"
#include "config/setup.h"
#include "dosbox.h"
#include "hardware/memory.h"

#include <cstdint>
#include <vector>

#if C_DEBUGGER

void DEBUG_AddConfigSection(const ConfigPtr& conf);
void DEBUG_Init();
void DEBUG_Destroy();

void DEBUG_DrawScreen();
bool DEBUG_Breakpoint();
bool DEBUG_IntBreakpoint(uint8_t intNum);
void DEBUG_Enable(bool pressed);
void DEBUG_CheckExecuteBreakpoint(uint16_t seg, uint32_t off);
bool DEBUG_ExitLoop(void);
bool DEBUG_IsDebugging();
bool DEBUG_Resume(void);
bool DEBUG_SingleStep(void);
void DEBUG_RefreshPage(int scroll);
Bitu DEBUG_EnableDebugger();

// Breakpoint API for callers other than the interactive debugger UI (e.g.
// the automation API). AH/AL values equal to DEBUG_BPINT_ANY are wildcards,
// matching any interrupt AH/AL respectively; mirrors the BPINT_ALL sentinel
// used by the BPINT command.
constexpr uint16_t DEBUG_BPINT_ANY = 0x100;

enum class DebugBreakpointType { Execute, Interrupt, Memory };

struct DebugBreakpointInfo {
	uint16_t index      = 0;
	DebugBreakpointType type = DebugBreakpointType::Execute;
	uint16_t segment    = 0;
	uint32_t offset     = 0;
	uint8_t int_num     = 0;
	uint16_t ah         = 0;
	uint16_t al         = 0;
	bool once           = false;
	bool active         = false;
};

void DEBUG_AddExecuteBreakpoint(uint16_t seg, uint32_t off);
void DEBUG_AddIntBreakpoint(uint8_t int_num, uint16_t ah, uint16_t al);
void DEBUG_AddMemBreakpoint(uint16_t seg, uint32_t off);
// Index is the breakpoint's position in DEBUG_ListBreakpoints(), which
// shifts whenever a breakpoint is added or removed -- it is not a stable
// identifier across calls that mutate the breakpoint list.
bool DEBUG_DeleteBreakpointByIndex(uint16_t index);
void DEBUG_DeleteAllBreakpoints(void);
std::vector<DebugBreakpointInfo> DEBUG_ListBreakpoints(void);

void LOG_StartUp();
void LOG_Init();
void LOG_Destroy();

extern Bitu cycle_count;
extern Bitu debugCallback;

#endif // C_DEBUGGER

#if C_DEBUGGER && C_HEAVY_DEBUGGER
bool DEBUG_HeavyIsBreakpoint();
void DEBUG_HeavyWriteLogInstruction();

template <typename T>
void DEBUG_UpdateMemoryReadBreakpoints(const PhysPt addr);

#else

template <typename T>
constexpr void DEBUG_UpdateMemoryReadBreakpoints(const PhysPt)
{
	// no-op
}
#endif // C_DEBUGGER && C_HEAVY_DEBUGGER

#endif // DOSBOX_DEBUG_H
