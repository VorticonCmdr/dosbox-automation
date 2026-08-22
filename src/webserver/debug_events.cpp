// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "debug_events.h"

#include "wait.h"

#include "cpu/cpu.h"
#include "hardware/memory.h"

#include <chrono>

namespace Webserver {

namespace {

std::string CoreKindName(const CoreKind core)
{
	switch (core) {
	case CoreKind::Normal: return "normal";
	case CoreKind::Simple: return "simple";
	case CoreKind::Full: return "full";
	case CoreKind::Dynamic: return "dynamic";
	}
	return "normal";
}

} // namespace

uint64_t PublishDebugStop(const std::string& reason, const bool debugging,
                          std::optional<DebugStopBreakpoint> breakpoint)
{
	DebugStopInfo info;
	info.reason    = reason;
	info.debugging = debugging;
	info.registers.load();
	info.linear_eip     = SegPhys(SegNames::cs) + info.registers.eip;
	info.protected_mode = cpu.pmode;
	info.core           = CoreKindName(CPU_GetActiveCoreKind());
	info.breakpoint     = std::move(breakpoint);

	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;
	for (size_t i = 0; i < info.code_bytes.size(); ++i) {
		const uint64_t addr = static_cast<uint64_t>(info.linear_eip) + i;
		info.code_bytes[i] = addr < mem_total
		                            ? mem_readb<MemOpMode::SkipBreakpoints>(
		                                      static_cast<PhysPt>(addr))
		                            : 0;
	}

	return DebugEvents::Instance().Publish(std::move(info));
}

DebugEvents& DebugEvents::Instance()
{
	static DebugEvents instance;
	return instance;
}

uint64_t DebugEvents::Publish(DebugStopInfo info)
{
	std::lock_guard<std::mutex> lock(mtx);
	info.stop_id = ++next_stop_id;
	latest       = std::move(info);
	cv.notify_all();
	return latest.stop_id;
}

DebugEvents::WaitResult DebugEvents::WaitFor(const uint64_t since_stop_id,
                                             const uint32_t timeout_ms)
{
	std::unique_lock<std::mutex> lock(mtx);

	if (waiter_count >= MaxWaiters) {
		throw TooManyWaiters(
		        "Too many concurrent /api/v1/debug/wait requests (max " +
		        std::to_string(MaxWaiters) + ")");
	}
	++waiter_count;

	const bool woke = cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
		return draining || latest.stop_id > since_stop_id;
	});

	--waiter_count;

	return WaitResult{woke && !draining, latest};
}

DebugStopInfo DebugEvents::Current()
{
	std::lock_guard<std::mutex> lock(mtx);
	return latest;
}

void DebugEvents::DrainAll()
{
	std::lock_guard<std::mutex> lock(mtx);
	draining = true;
	cv.notify_all();
}

} // namespace Webserver
