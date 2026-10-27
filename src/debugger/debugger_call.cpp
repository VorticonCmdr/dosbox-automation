// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "debugger_call.h"

#include "debugger.h"

#include "cpu/callback.h"
#include "cpu/cpu.h"
#include "cpu/lazyflags.h"
#include "cpu/paging.h"
#include "cpu/registers.h"
#include "dosbox.h"
#include "fpu/fpu.h"
#include "hardware/memory.h"
#include "hardware/pic.h"
#include "hardware/timer.h"
#include "hardware/video/vga.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>

namespace DebugCall {

bool active = false;

namespace {

constexpr int SliceCycles = 10'000;

constexpr std::array<std::pair<std::string_view, CallKind>, 3> CallKindNames = {
        {
         {"near16", CallKind::Near16},
         {"far16", CallKind::Far16},
         {"near32", CallKind::Near32},
         }
};

constexpr std::array<std::pair<std::string_view, ReturnMode>, 2> ReturnModeNames = {
        {
         {"push", ReturnMode::Push},
         {"replace", ReturnMode::Replace},
         }
};

constexpr std::array<std::pair<std::string_view, Register>, 13> RegisterNames = {
        {
         {"eax", Register::Eax},
         {"ebx", Register::Ebx},
         {"ecx", Register::Ecx},
         {"edx", Register::Edx},
         {"esi", Register::Esi},
         {"edi", Register::Edi},
         {"ebp", Register::Ebp},
         {"esp", Register::Esp},
         {"eflags", Register::Eflags},
         {"ds", Register::Ds},
         {"es", Register::Es},
         {"fs", Register::Fs},
         {"gs", Register::Gs},
         }
};

// DOSBox-implemented handler functions known to only read emulator state or
// write guest RAM (the IVT, MCB chain, BIOS data area), so a RAM restore
// covers them completely. Everything not listed here is reported: the
// current directory, for example, lives in Drives[]->curdir, open files in
// Files[], EMS mappings and mouse state in their C++ drivers. An empty AH
// list means every function of that handler.
struct RestorableHandler {
	std::string_view description;
	std::vector<uint8_t> functions;
};

const std::vector<RestorableHandler>& restorable_handlers()
{
	static const std::vector<RestorableHandler> handlers = {
	        // get drive, set/get vector, get date/time, get DTA, version,
	        // InDOS pointer, memory allocate/free/resize, get PSP, list of
		// lists
	        {      "DOS Int 21",
		 {0x19, 0x25, 0x2a, 0x2c, 0x2f, 0x30, 0x34, 0x35, 0x48, 0x49, 0x4a, 0x51, 0x52, 0x62}},
	        // DPMI host and XMS driver detection
	        {      "DOS Int 2f",                                                     {0x16, 0x43}},
	        {"Int 11 Equipment",                                                               {}},
	        {   "Int 12 Memory",                                                               {}},
	        // tick count, RTC time and date reads
	        {     "Int 1a Time",                                               {0x00, 0x02, 0x04}},
	        // get cursor, get video mode
	        {    "Int 10 video",                                                     {0x03, 0x0f}},
	        // extended memory size, system configuration
	        {     "Int 15 Bios",                                                     {0x88, 0xc0}},
	        // nested-run plumbing of CALLBACK_RunRealFar / CALLBACK_Idle
	        {            "stop",                                                               {}},
	        {            "idle",                                                               {}},
	};
	return handlers;
}

struct Observations {
	std::map<uint16_t, PortUse> ports = {};
	std::map<std::tuple<uint8_t, uint8_t, int>, InterruptUse> interrupts = {};
	std::map<std::pair<std::string, uint8_t>, HandlerUse> handlers = {};
};

Observations observed = {};

// Everything the call restores. CPU_Regs, Segments, CPUBlock, LazyFlags and
// FPU_rec are plain aggregates (no owning pointers), so assignment is a
// complete copy.
struct Snapshot {
	CPU_Regs regs        = {};
	Segments segs        = {};
	CPUBlock cpu_block   = {};
	LazyFlags lazy       = {};
	FPU_rec fpu_state    = {};
	VgaLatch vga_latch   = {};
	int cycles           = 0;
	int cycle_left       = 0;
	CPU_Decoder* decoder = nullptr;
	LoopHandler* loop    = nullptr;

	std::vector<uint8_t> ram     = {};
	std::vector<uint8_t> vram    = {};
	std::vector<uint8_t> fastmem = {};
	size_t ram_size              = 0;
	size_t vram_size             = 0;
	size_t fastmem_size          = 0;
};

// Guest RAM the call mechanics overwrite (return slot, near16 stub): put
// back before diffing, so only the routine's own writes show up.
struct Overwrite {
	uint32_t address           = 0;
	std::vector<uint8_t> bytes = {};
};

struct RunState {
	bool sentinel_hit = false;
	// Stack offsets (masked by cpu.stack.mask): the one the callee must
	// return to, and the one it actually returned with. Compared modulo the
	// stack size, since a 16-bit SP at the top of its segment wraps to 0.
	uint32_t return_sp                             = 0;
	uint32_t sp_at_sentinel                        = 0;
	int64_t cycles                                 = 0;
	int64_t max_cycles                             = 0;
	int64_t timer_ticks                            = 0;
	int decoder_switches                           = 0;
	bool timers                                    = false;
	std::string error                              = {};
	std::vector<Overwrite> overwrites              = {};
	std::chrono::steady_clock::time_point deadline = {};
};

RunState state                = {};
callback_number_t sentinel_cb = 0;

uint32_t ram_bytes()
{
	return MEM_TotalPages() * 4096;
}

uint32_t stack_linear()
{
	return SegPhys(SegNames::ss) + (reg_esp & cpu.stack.mask);
}

Bitu sentinel_handler()
{
	state.sentinel_hit   = true;
	state.sp_at_sentinel = reg_esp & cpu.stack.mask;
	return CBRET_STOP;
}

void ensure_sentinel()
{
	if (sentinel_cb != 0) {
		return;
	}
	sentinel_cb = CALLBACK_Allocate();
	CALLBACK_Setup(sentinel_cb, &sentinel_handler, CB_RETF, "debug call return");
}

void fail(std::string message)
{
	if (state.error.empty()) {
		state.error = std::move(message);
	}
}

// A mode switch inside the call (an extender servicing an interrupt, say) can
// make CPU_SET_CRX install a dynamic core. Its translations of code the call
// modifies would outlive the RAM restore, which bypasses the code-page
// invalidation, so the call always runs on the normal core.
bool force_normal_core()
{
#if C_DYNAMIC_X86
	if (cpudecoder == &CPU_Core_Dyn_X86_Run) {
		cpudecoder = &CPU_Core_Normal_Run;
		return true;
	}
	if (cpudecoder == &CPU_Core_Dyn_X86_Trap_Run) {
		cpudecoder = &CPU_Core_Normal_Trap_Run;
		return true;
	}
#endif
#if C_DYNREC
	if (cpudecoder == &CPU_Core_Dynrec_Run) {
		cpudecoder = &CPU_Core_Normal_Run;
		return true;
	}
	if (cpudecoder == &CPU_Core_Dynrec_Trap_Run) {
		cpudecoder = &CPU_Core_Normal_Trap_Run;
		return true;
	}
#endif
	return false;
}

void note_handler(const callback_number_t cb)
{
	const char* d           = CALLBACK_GetDescription(cb);
	std::string description = d ? d : "callback " + std::to_string(cb);
	auto& use               = observed.handlers[{description, reg_ah}];
	use.description         = std::move(description);
	use.ah                  = reg_ah;
	++use.count;
}

// Same shape as normal_loop() (dosbox.cpp) minus the Bridge pump, frame
// presentation and wall-clock pacing. It is also installed as the main loop
// for the duration of the call, so a DOS or BIOS handler that runs guest
// code through a nested DOSBOX_RunMachine() (CALLBACK_RunRealInt and
// friends) stays inside the call's budget instead of pumping the Bridge
// again - ProcessRequests() holds the Bridge mutex while we run.
Bitu call_loop()
{
	while (true) {
		if (!state.error.empty() || state.sentinel_hit) {
			return 1;
		}
		if (state.timers) {
			if (!PIC_RunQueue()) {
				TIMER_AddTick();
				++state.timer_ticks;
				continue;
			}
		} else {
			CPU_Cycles    = SliceCycles;
			CPU_CycleLeft = 0;
		}
		if (force_normal_core()) {
			++state.decoder_switches;
		}
		const int start_cycles = CPU_Cycles;
		const auto ret         = (*cpudecoder)();
		state.cycles += std::max(0, start_cycles - std::max(0, CPU_Cycles));

		if (state.cycles >= state.max_cycles) {
			fail("did not return within " +
			     std::to_string(state.max_cycles) + " cycles");
			return 1;
		}
		if (std::chrono::steady_clock::now() > state.deadline) {
			fail("request timeout reached before the routine returned");
			return 1;
		}
		if (ret < 0) {
			fail("the CPU core requested an exit");
			return 1;
		}
		if (ret > 0) {
			if (static_cast<Bitu>(ret) == debugCallback) {
				fail("a debugger breakpoint fired inside the call");
				return 1;
			}
			if (ret >= CB_MAX) {
				return 0;
			}
			const auto cb = static_cast<callback_number_t>(ret);
			if (cb != sentinel_cb) {
				note_handler(cb);
			}
			const Bitu result = (*Callback_Handlers[ret])();
			if (result) {
				return result;
			}
		}
	}
}

void take_snapshot(Snapshot& s)
{
	FillFlags();
	s.regs       = cpu_regs;
	s.segs       = Segs;
	s.cpu_block  = cpu;
	s.lazy       = lflags;
	s.fpu_state  = fpu;
	s.vga_latch  = vga.latch;
	s.cycles     = CPU_Cycles;
	s.cycle_left = CPU_CycleLeft;
	s.decoder    = cpudecoder;
	s.loop       = DOSBOX_GetLoop();

	// The buffers live for the whole process and only ever grow: one call
	// request must not pay for allocating and freeing tens of megabytes
	// (in an unoptimised build that alone is the bulk of a request).
	const auto copy_into =
	        [](std::vector<uint8_t>& dst, const uint8_t* src, const size_t n) {
		        if (dst.size() < n) {
			        dst.resize(n);
		        }
		        std::memcpy(dst.data(), src, n);
	        };
	s.ram_size = ram_bytes();
	copy_into(s.ram, MemBase, s.ram_size);
	s.vram_size    = (vga.mem.linear && vga.vmemsize) ? vga.vmemsize : 0;
	s.fastmem_size = (vga.fastmem && vga.vmemsize) ? 2 * vga.vmemsize : 0;
	if (s.vram_size) {
		copy_into(s.vram, vga.mem.linear, s.vram_size);
	}
	if (s.fastmem_size) {
		copy_into(s.fastmem, vga.fastmem, s.fastmem_size);
	}
}

void restore_snapshot(const Snapshot& s)
{
	std::memcpy(MemBase, s.ram.data(), s.ram_size);
	if (s.vram_size) {
		std::memcpy(vga.mem.linear, s.vram.data(), s.vram_size);
	}
	if (s.fastmem_size) {
		std::memcpy(vga.fastmem, s.fastmem.data(), s.fastmem_size);
	}
	vga.latch     = s.vga_latch;
	cpu_regs      = s.regs;
	Segs          = s.segs;
	cpu           = s.cpu_block;
	lflags        = s.lazy;
	fpu           = s.fpu_state;
	CPU_Cycles    = s.cycles;
	CPU_CycleLeft = s.cycle_left;
	cpudecoder    = s.decoder;
	DOSBOX_SetLoop(s.loop);
	// Page tables are guest RAM and were just rewritten underneath the TLB
	PAGING_ClearTLB();
}

bool range_within(const Range& r)
{
	const uint64_t limit = (r.space == Space::Ram) ? ram_bytes() : vga.vmemsize;
	return r.start < r.end && r.end <= limit;
}

const uint8_t* space_base(const Space space)
{
	return space == Space::Ram ? MemBase : vga.mem.linear;
}

bool apply_register(const RegisterWrite& w, std::string& error)
{
	auto set_segment = [&](const SegNames seg) {
		if (cpu.pmode) {
			if (CPU_SetSegGeneral(seg, w.value & 0xffff)) {
				error = "loading the selector into a segment register faulted";
				return false;
			}
		} else {
			SegSet16(seg, static_cast<uint16_t>(w.value));
		}
		return true;
	};
	switch (w.reg) {
	case Register::Eax: reg_eax = w.value; return true;
	case Register::Ebx: reg_ebx = w.value; return true;
	case Register::Ecx: reg_ecx = w.value; return true;
	case Register::Edx: reg_edx = w.value; return true;
	case Register::Esi: reg_esi = w.value; return true;
	case Register::Edi: reg_edi = w.value; return true;
	case Register::Ebp: reg_ebp = w.value; return true;
	case Register::Esp: reg_esp = w.value; return true;
	case Register::Eflags: CPU_SetFlagsd(w.value); return true;
	case Register::Ds: return set_segment(SegNames::ds);
	case Register::Es: return set_segment(SegNames::es);
	case Register::Fs: return set_segment(SegNames::fs);
	case Register::Gs: return set_segment(SegNames::gs);
	}
	return false;
}

bool remember(const uint32_t address, const uint32_t length)
{
	if (static_cast<uint64_t>(address) + length > ram_bytes()) {
		return false;
	}
	state.overwrites.push_back(
	        {address,
		 std::vector<uint8_t>(MemBase + address, MemBase + address + length)});
	return true;
}

// Writes `value` (2 or 4 bytes) into the return slot: pushed below SS:SP, or
// over the return address already at SS:SP. Returns false when the slot is
// not in guest RAM.
bool place_return(const ReturnMode mode, const uint32_t value, const int size)
{
	if (mode == ReturnMode::Push) {
		const auto slot = SegPhys(SegNames::ss) +
		                  ((reg_esp - size) & cpu.stack.mask);
		if (!remember(slot, size)) {
			return false;
		}
		state.return_sp = reg_esp & cpu.stack.mask;
		if (size == 4) {
			CPU_Push32(value);
		} else {
			CPU_Push16(static_cast<uint16_t>(value));
		}
		return true;
	}
	const auto slot = stack_linear();
	if (!remember(slot, size)) {
		return false;
	}
	state.return_sp = (reg_esp + size) & cpu.stack.mask;
	if (size == 4) {
		mem_writed(slot, value);
	} else {
		mem_writew(slot, static_cast<uint16_t>(value));
	}
	return true;
}

// Places the sentinel return address and points CS:EIP at the entry.
bool prepare_call(const Request& req, std::string& error)
{
	const auto slot_error = "the return address slot lies outside guest RAM";
	switch (req.kind) {
	case CallKind::Far16: {
		const auto ret = CALLBACK_RealPointer(sentinel_cb);
		// A far return pops offset then segment: one dword, offset low
		const uint32_t far_ptr = (static_cast<uint32_t>(RealSegment(ret))
		                          << 16) |
		                         RealOffset(ret);
		if (!place_return(req.return_mode, far_ptr, 4)) {
			error = slot_error;
			return false;
		}
		SegSet16(SegNames::cs, req.segment);
		reg_eip = req.offset;
		return true;
	}
	case CallKind::Near16: {
		const uint32_t stub = (static_cast<uint32_t>(req.segment) << 4) +
		                      req.stub_offset;
		if (!remember(stub, 4)) {
			error = "the return stub would lie outside guest RAM";
			return false;
		}
		// The same 4-byte callback opcode CALLBACK_Setup emits (FE 38
		// nn nn)
		MemBase[stub + 0] = 0xfe;
		MemBase[stub + 1] = 0x38;
		MemBase[stub + 2] = static_cast<uint8_t>(sentinel_cb & 0xff);
		MemBase[stub + 3] = static_cast<uint8_t>(sentinel_cb >> 8);
		SegSet16(SegNames::cs, req.segment);
		if (!place_return(req.return_mode, req.stub_offset, 2)) {
			error = slot_error;
			return false;
		}
		reg_eip = req.offset;
		return true;
	}
	case CallKind::Near32: {
		if (req.segment != SegValue(SegNames::cs)) {
			error = "near32: segment must be the current CS selector";
			return false;
		}
		const PhysPt target  = CALLBACK_PhysPointer(sentinel_cb);
		const PhysPt cs_base = SegPhys(SegNames::cs);
		if (target < cs_base) {
			error = "near32: the return callback lies below the CS base";
			return false;
		}
		if (!place_return(req.return_mode, target - cs_base, 4)) {
			error = slot_error;
			return false;
		}
		reg_eip = req.offset;
		return true;
	}
	}
	return false;
}

std::vector<RegisterValue> read_registers()
{
	FillFlags();
	return {
	        {   "eax",                reg_eax},
	        {   "ebx",                reg_ebx},
	        {   "ecx",                reg_ecx},
	        {   "edx",                reg_edx},
	        {   "esi",                reg_esi},
	        {   "edi",                reg_edi},
	        {   "ebp",                reg_ebp},
	        {   "esp",                reg_esp},
	        {   "eip",                reg_eip},
	        {"eflags",              reg_flags},
	        {    "cs", SegValue(SegNames::cs)},
	        {    "ds", SegValue(SegNames::ds)},
	        {    "es", SegValue(SegNames::es)},
	        {    "ss", SegValue(SegNames::ss)},
	        {    "fs", SegValue(SegNames::fs)},
	        {    "gs", SegValue(SegNames::gs)}
        };
}

CaseResult run_case(const Request& req, const Case& c)
{
	CaseResult result      = {};
	observed               = {};
	state.sentinel_hit     = false;
	state.cycles           = 0;
	state.timer_ticks      = 0;
	state.decoder_switches = 0;
	state.overwrites.clear();
	state.error.clear();

	for (const auto& w : c.registers) {
		if (!apply_register(w, result.error)) {
			return result;
		}
	}
	for (const auto& p : c.memory) {
		std::memcpy(MemBase + p.address, p.bytes.data(), p.bytes.size());
	}
	// The diff baseline: the snapshot with this case's inputs applied.
	// Reused across cases and requests for the same reason as the snapshot.
	static std::vector<std::vector<uint8_t>> baselines = {};
	if (baselines.size() < req.ranges.size()) {
		baselines.resize(req.ranges.size());
	}
	for (size_t i = 0; i < req.ranges.size(); ++i) {
		const auto& r      = req.ranges[i];
		const auto* base   = space_base(r.space);
		const size_t bytes = r.end - r.start;
		if (baselines[i].size() < bytes) {
			baselines[i].resize(bytes);
		}
		std::memcpy(baselines[i].data(), base + r.start, bytes);
	}
	if (!prepare_call(req, result.error)) {
		return result;
	}

	cpudecoder = &CPU_Core_Normal_Run;
	DOSBOX_SetLoop(&call_loop);
	active = true;
	while (state.error.empty() && !state.sentinel_hit) {
		if (call_loop() != 0 && !state.sentinel_hit && state.error.empty()) {
			fail("an unexpected callback stop ended the call");
		}
	}
	active = false;

	// The return slot and the stub are ours, not the routine's
	for (const auto& o : state.overwrites) {
		std::memcpy(MemBase + o.address, o.bytes.data(), o.bytes.size());
	}
	result.cycles        = state.cycles;
	result.timer_ticks   = state.timer_ticks;
	const uint32_t depth = (state.sp_at_sentinel - state.return_sp) &
	                       cpu.stack.mask;
	if (state.sentinel_hit && depth > cpu.stack.mask / 2) {
		fail("returned to the sentinel at a deeper stack level than the call");
	}
	result.returned  = state.sentinel_hit && state.error.empty();
	result.error     = state.error;
	result.registers = read_registers();

	for (size_t i = 0; i < req.ranges.size(); ++i) {
		const auto& r = req.ranges[i];
		auto d        = DiffBytes(baselines[i].data(),
		                          space_base(r.space) + r.start,
		                          r.end - r.start,
		                          r.start,
		                          8,
		                          req.max_diff_bytes);
		d.range       = r;
		result.ranges.push_back(std::move(d));
	}
	for (const auto& [port, use] : observed.ports) {
		result.ports.push_back(use);
	}
	for (const auto& [key, use] : observed.interrupts) {
		result.interrupts.push_back(use);
	}
	for (const auto& [key, use] : observed.handlers) {
		result.handlers.push_back(use);
	}
	result.unrestored = DescribeUnrestored(result.ports,
	                                       result.interrupts,
	                                       result.handlers,
	                                       result.timer_ticks);
	if (state.decoder_switches > 0) {
		result.unrestored.emplace_back(
		        "the CPU switched to a dynamic core during the call; it ran on "
		        "the normal core instead");
	}
	return result;
}

} // namespace

std::optional<CallKind> ParseCallKind(const std::string_view name)
{
	for (const auto& [n, k] : CallKindNames) {
		if (n == name) {
			return k;
		}
	}
	return std::nullopt;
}

std::optional<ReturnMode> ParseReturnMode(const std::string_view name)
{
	for (const auto& [n, m] : ReturnModeNames) {
		if (n == name) {
			return m;
		}
	}
	return std::nullopt;
}

std::optional<Register> ParseRegister(const std::string_view name)
{
	for (const auto& [n, r] : RegisterNames) {
		if (n == name) {
			return r;
		}
	}
	return std::nullopt;
}

std::string_view RegisterName(const Register reg)
{
	for (const auto& [n, r] : RegisterNames) {
		if (r == reg) {
			return n;
		}
	}
	return "?";
}

RangeDiff DiffBytes(const uint8_t* before, const uint8_t* after,
                    const uint32_t length, const uint32_t base_address,
                    const uint32_t gap, const int64_t max_bytes)
{
	RangeDiff out    = {};
	int64_t reported = 0;
	uint32_t i       = 0;
	while (i < length) {
		if (before[i] == after[i]) {
			++i;
			continue;
		}
		// Extend the run while the next difference is within `gap` bytes
		uint32_t last = i;
		uint32_t j    = i + 1;
		while (j < length && j - last <= gap) {
			if (before[j] != after[j]) {
				last = j;
			}
			++j;
		}
		for (uint32_t k = i; k <= last; ++k) {
			out.changed_bytes += (before[k] != after[k]) ? 1 : 0;
		}
		const int64_t run_length = static_cast<int64_t>(last - i) + 1;
		if (!out.truncated && reported + run_length <= max_bytes) {
			out.diffs.push_back(
			        {base_address + i,
				 std::vector<uint8_t>(after + i, after + last + 1)});
			reported += run_length;
		} else {
			out.truncated = true;
		}
		i = last + 1;
	}
	return out;
}

bool IsRestorableHandler(const std::string_view description, const uint8_t ah)
{
	for (const auto& h : restorable_handlers()) {
		if (h.description == description) {
			return h.functions.empty() ||
			       std::find(h.functions.begin(), h.functions.end(), ah) !=
			               h.functions.end();
		}
	}
	return false;
}

std::vector<std::string> DescribeUnrestored(const std::vector<PortUse>& ports,
                                            const std::vector<InterruptUse>& interrupts,
                                            const std::vector<HandlerUse>& handlers,
                                            const int64_t timer_ticks)
{
	std::vector<std::string> out = {};
	if (!ports.empty()) {
		out.emplace_back("port I/O: device state is not restored");
	}
	bool irq_handlers = false;
	for (const auto& h : handlers) {
		if (IsRestorableHandler(h.description, h.ah)) {
			continue;
		}
		// Hardware interrupt service is covered by the machine-time entry
		if (h.description.starts_with("IRQ") ||
		    h.description.starts_with("irq")) {
			irq_handlers = true;
			continue;
		}
		std::array<char, 8> ah_hex = {};
		std::snprintf(ah_hex.data(), ah_hex.size(), "%02Xh", h.ah);
		out.push_back("emulator-side handler '" + h.description +
		              "' (AH=" + ah_hex.data() +
		              ") may have changed state outside the snapshot");
	}
	bool exception = false;
	bool hardware  = irq_handlers;
	for (const auto& i : interrupts) {
		if (i.kind == InterruptKind::Exception) {
			exception = true;
		} else if (i.kind == InterruptKind::Hardware) {
			hardware = true;
		}
	}
	if (exception) {
		out.emplace_back("CPU exception raised during the call");
	}
	if (hardware || timer_ticks > 0) {
		out.emplace_back("machine time advanced: PIC/PIT state is not restored");
	}
	return out;
}

void NotePort(const uint16_t port, const bool is_write)
{
	auto& use = observed.ports[port];
	use.port  = port;
	if (is_write) {
		++use.writes;
	} else {
		++use.reads;
	}
}

void NoteInterrupt(const uint8_t number, const uint8_t ah, const InterruptKind kind)
{
	const uint8_t key_ah = kind == InterruptKind::Software ? ah : 0;
	auto& use = observed.interrupts[{number, key_ah, static_cast<int>(kind)}];
	use.number = number;
	use.ah     = key_ah;
	use.kind   = kind;
	++use.count;
}

std::vector<CaseResult> Run(const Request& req, std::string& error)
{
	if (!DEBUG_IsDebugging()) {
		error = "not_paused";
		return {};
	}
	if (paging.enabled) {
		error = "paging is enabled: guest linear and physical addresses differ, "
		        "which calls do not support yet";
		return {};
	}
	const bool real_mode = !cpu.pmode;
	if ((req.kind == CallKind::Near32) == real_mode) {
		error = req.kind == CallKind::Near32
		              ? "near32 needs protected mode"
		              : "near16/far16 need real mode (not protected or V86 mode)";
		return {};
	}
	if (req.kind == CallKind::Near32 && !cpu.code.big) {
		error = "near32 needs a 32-bit code segment";
		return {};
	}
	for (const auto& c : req.cases) {
		for (const auto& p : c.memory) {
			if (static_cast<uint64_t>(p.address) + p.bytes.size() >
			    ram_bytes()) {
				error = "a memory patch lies outside guest RAM";
				return {};
			}
		}
	}
	for (const auto& r : req.ranges) {
		if (!range_within(r)) {
			error = "an output range lies outside its memory space";
			return {};
		}
	}
	ensure_sentinel();

	const auto armed     = DEBUG_DisarmBreakpoints();
	static Snapshot snap = {};
	take_snapshot(snap);

	state            = {};
	state.max_cycles = req.max_cycles;
	state.timers     = req.timers;
	state.deadline   = std::chrono::steady_clock::now() +
	                   std::chrono::milliseconds(req.timeout_ms);

	std::vector<CaseResult> results = {};
	for (const auto& c : req.cases) {
		if (std::chrono::steady_clock::now() > state.deadline) {
			// One result per case, always: a case the timeout kept
			// from running says so instead of silently going missing
			CaseResult skipped = {};
			skipped.error      = "not run: request timeout reached";
			results.push_back(std::move(skipped));
			continue;
		}
		results.push_back(run_case(req, c));
		restore_snapshot(snap);
	}
	DEBUG_RearmBreakpoints(armed);
	return results;
}

} // namespace DebugCall
