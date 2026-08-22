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

// Runs `count` instructions (default 1) with breakpoints disarmed for the
// burst - matches the interactive debugger's own numeric-run keys (1/5/6
// etc.), not a new mechanism. Only breakpoints already active (patched
// into memory by a prior continue/run) are honoured mid-burst; one added
// since is invisible until the next continue. Capped by the caller at
// DEBUG_MaxStepCount, since the whole call runs inside one Bridge Command
// with the Bridge mutex held.
bool DEBUG_SingleStep(int32_t count = 1);
constexpr int32_t DEBUG_MaxStepCount = 64;

// Runs to the next call/int/loop/rep by planting a one-shot breakpoint
// past it and resuming (matches the F10 key handler); returns false when
// the current instruction isn't one of those, so the caller can fall back
// to DEBUG_SingleStep(). Like DEBUG_Resume, the actual stop happens
// arbitrarily later, published by whatever runs next - callers poll
// debug/wait for it.
bool DEBUG_StepOver();

// Plants a one-shot breakpoint at seg:off and resumes - the run-to-cursor
// pattern, with an explicit target instead of StepOver's disassembly-based
// detection. Same "actual stop happens later" caveat as DEBUG_StepOver.
bool DEBUG_RunToAddress(uint16_t seg, uint32_t off);

void DEBUG_RefreshPage(int scroll);
Bitu DEBUG_EnableDebugger();

// Breakpoint API for callers other than the interactive debugger UI (e.g.
// the automation API). AH/AL values equal to DEBUG_BPINT_ANY are wildcards,
// matching any interrupt AH/AL respectively; mirrors the BPINT_ALL sentinel
// used by the BPINT command.
constexpr uint16_t DEBUG_BPINT_ANY = 0x100;

enum class DebugBreakpointType { Execute, Interrupt, Memory };

struct DebugBreakpointInfo {
	// Monotonic, assigned once at construction, never reused or
	// renumbered - the stable identifier index deliberately isn't. Use
	// this, not index, to refer to a breakpoint across calls that add or
	// remove others.
	uint64_t id               = 0;
	uint16_t index            = 0;
	DebugBreakpointType type  = DebugBreakpointType::Execute;
	uint16_t segment          = 0;
	uint32_t offset           = 0;
	uint8_t int_num           = 0;
	uint16_t ah               = 0;
	uint16_t al               = 0;
	bool once                 = false;
	bool active               = false;
};

void DEBUG_AddExecuteBreakpoint(uint16_t seg, uint32_t off, bool once = false);
void DEBUG_AddIntBreakpoint(uint8_t int_num, uint16_t ah, uint16_t al,
                            bool once = false);
void DEBUG_AddMemBreakpoint(uint16_t seg, uint32_t off, bool once = false);
// Index is the breakpoint's position in DEBUG_ListBreakpoints(), which
// shifts whenever a breakpoint is added or removed -- it is not a stable
// identifier across calls that mutate the breakpoint list. Prefer
// DEBUG_DeleteBreakpointById where the caller has an id.
bool DEBUG_DeleteBreakpointByIndex(uint16_t index);
bool DEBUG_DeleteBreakpointById(uint64_t id);
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
