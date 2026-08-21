// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/dos.h"
#include "bridge.h"
#include "webserver.h"

#include <limits>
#include <mutex>

#include "http/http.h"
#include "json/json.h"

#include "cpu/paging.h"
#include "cpu/registers.h"
#include "dos/dos.h"
#include "dos/dos_memory.h"
#include "utils/string_utils.h"

using json = nlohmann::json;

namespace Webserver {

constexpr int DosBlockSize = 16;

std::vector<McbBlock> WalkMcbChain(const uint16_t start_segment,
                                   const McbReader& reader, const int max_blocks)
{
	std::vector<McbBlock> chain;
	uint16_t seg = start_segment;

	for (int i = 0; i < max_blocks; ++i) {
		auto block    = reader(seg);
		block.segment = seg;

		if (block.type == 0x5A) {
			block.is_last = true;
			chain.push_back(block);
			break;
		}
		if (block.type != 0x4D) {
			break;
		}

		chain.push_back(block);
		seg = seg + block.size_paras + 1;
	}
	return chain;
}

static McbBlock ReadMcbFromGuest(const uint16_t segment)
{
	DOS_MCB mcb(segment);
	McbBlock block    = {};
	block.segment     = segment;
	block.type        = mcb.GetType();
	block.psp_segment = mcb.GetPSPSeg();
	block.size_paras  = mcb.GetSize();

	char name[9] = {};
	mcb.GetFileName(name);
	block.filename = name;
	return block;
}

void DosInternalsCommand::Execute()
{
	list_of_lists      = RealToPhysical(dos_infoblock.GetPointer());
	dos_swappable_area = PhysicalMake(DOS_SDA_SEG, DOS_SDA_OFS);
	first_shell        = PhysicalMake(DOS_FIRST_SHELL, 0);

	memory_map = WalkMcbChain(dos.firstMCB, ReadMcbFromGuest, 1000);

	LOG_DEBUG("API: DosInternalsCommand()");
}

void DosInternalsCommand::Get(const httplib::Request&, httplib::Response& res)
{
	DosInternalsCommand cmd;
	cmd.WaitForCompletion();

	json j;
	j["listOfLists"]      = cmd.list_of_lists;
	j["dosSwappableArea"] = cmd.dos_swappable_area;
	j["firstShell"]       = cmd.first_shell;

	json map = json::array();
	for (const auto& b : cmd.memory_map) {
		json entry;
		entry["segment"]    = b.segment;
		entry["type"]       = b.type;
		entry["pspSegment"] = b.psp_segment;
		entry["sizeParas"]  = b.size_paras;
		entry["sizeBytes"]  = static_cast<uint32_t>(b.size_paras) * 16;
		entry["filename"]   = b.filename;
		entry["isLast"]     = b.is_last;
		map.push_back(entry);
	}
	j["memoryMap"] = map;

	send_json(res, j);
}

AllocationRegistry& AllocationRegistry::Instance()
{
	static AllocationRegistry instance;
	return instance;
}

bool AllocationRegistry::IsFull() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return live_addrs.size() >= MaxEntries;
}

void AllocationRegistry::Add(const uint32_t addr)
{
	std::lock_guard<std::mutex> lock(mtx);
	live_addrs.insert(addr);
}

bool AllocationRegistry::Remove(const uint32_t addr)
{
	std::lock_guard<std::mutex> lock(mtx);
	return live_addrs.erase(addr) > 0;
}

void AllocMemoryCommand::AllocDos()
{
	uint16_t blocks   = (bytes + DosBlockSize - 1) / DosBlockSize;
	auto old_strategy = DOS_GetMemAllocStrategy();
	uint16_t segment  = 0;

	uint16_t new_strategy = 0;

	switch (area) {
	case MemoryArea::Conv:
		switch (strategy) {
		case AllocStrategy::FirstFit:
			new_strategy = DosMemAllocStrategy::LowMemoryFirstFit;
			break;
		case AllocStrategy::BestFit:
			new_strategy = DosMemAllocStrategy::LowMemoryBestFit;
			break;
		case AllocStrategy::LastFit:
			new_strategy = DosMemAllocStrategy::LowMemoryLastFit;
			break;
		default: assertm(false, "Invalid alloc strategy"); break;
		}
		break;

	case MemoryArea::Uma:
		switch (strategy) {
		case AllocStrategy::FirstFit:
			new_strategy = DosMemAllocStrategy::UmbMemoryFirstFit;
			break;
		case AllocStrategy::BestFit:
			new_strategy = DosMemAllocStrategy::UmbMemoryBestFit;
			break;
		case AllocStrategy::LastFit:
			new_strategy = DosMemAllocStrategy::UmbMemoryLastFit;
			break;
		default: assertm(false, "Invalid alloc strategy"); break;
		}
		break;
	default: assertm(false, "Invalid memory area"); break;
	}

	DOS_SetMemAllocStrategy(new_strategy);

	auto ok = DOS_AllocateMemory(&segment, &blocks);
	addr    = PhysicalMake(segment, 0);

	LOG_DEBUG("API: AllocMemoryCommand(%d): result=%d, %d bytes at %p (DOS allocator)",
	          bytes,
	          ok,
	          blocks * DosBlockSize,
	          addr);

	DOS_SetMemAllocStrategy(old_strategy);

	if (!ok) {
		addr = 0;
	} else if (blocks * DosBlockSize < bytes) {
		DOS_FreeMemory(segment);
		addr = 0;
	}
}

void AllocMemoryCommand::AllocXms()
{
	auto num_pages = (bytes + MEM_PAGE_SIZE - 1) / MEM_PAGE_SIZE;
	auto handle    = MEM_AllocatePages(num_pages, true);

	// Returns 0 on error or out of memory, nullptr is handled as error below.
	addr = handle * MEM_PAGE_SIZE;
	LOG_DEBUG("API: AllocMemoryCommand(%d), handle=%d: %d bytes at %p (XMS/page allocator)",
	          bytes,
	          handle,
	          num_pages * MEM_PAGE_SIZE,
	          addr);
}

void AllocMemoryCommand::Execute()
{
	if (AllocationRegistry::Instance().IsFull()) {
		// Refuse rather than mint an address the registry cannot
		// track: FreeMemoryCommand would never be able to free it.
		addr = 0;
		return;
	}

	if (area < MemoryArea::Xms) {
		AllocDos();
	} else {
		AllocXms();
	}

	if (addr) {
		AllocationRegistry::Instance().Add(addr);
	}
}

void AllocMemoryCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto j        = json::parse(req.body);
	uint32_t size = j.at("size");

	// AllocMemoryCommand's constructor takes a uint16_t byte count, as
	// does every allocator underneath it (DOS memory blocks are
	// paragraph-counted with a uint16_t block count). Without this
	// check, a size above 65535 narrows silently at the constructor
	// call: a request for 65536 truncates to 0 and allocates nothing
	// while still reporting a "valid" address.
	constexpr uint32_t MaxAllocBytes = std::numeric_limits<uint16_t>::max();
	if (size == 0 || size > MaxAllocBytes) {
		throw std::invalid_argument("'size' must be between 1 and " +
		                            std::to_string(MaxAllocBytes) + " bytes");
	}

	const auto area = [&]() {
		using enum MemoryArea;

		if (j.contains("area")) {
			std::string req_area = j["area"];
			upcase(req_area);

			if (req_area == "CONV") {
				return Conv;
			} else if (req_area == "UMA") {
				return Uma;
			} else if (req_area == "XMS") {
				return Xms;
			} else {
				throw std::invalid_argument(
				        "Invalid memory area: " + req_area);
			}
		} else {
			return Conv;
		}
	}();

	const auto strategy = [&]() {
		using enum AllocStrategy;

		if (j.contains("strategy")) {
			std::string req_strategy = j["strategy"];
			upcase(req_strategy);

			if (req_strategy == "FIRST_FIT") {
				return FirstFit;
			} else if (req_strategy == "BEST_FIT") {
				return BestFit;
			} else if (req_strategy == "LAST_FIT") {
				return LastFit;
			} else {
				throw std::invalid_argument(
				        "Invalid alloc strategy: " + req_strategy);
			}
		} else {
			return BestFit;
		}
	}();

	if (area == MemoryArea::Xms && strategy != AllocStrategy::BestFit) {
		throw std::invalid_argument("XMS allocator only supports best_fit");
	}

	AllocMemoryCommand cmd(size, area, strategy);
	cmd.WaitForCompletion();

	if (cmd.addr) {
		json j;
		j["addr"] = cmd.addr;
		send_json(res, j);
	} else {
		res.status = httplib::StatusCode::ServiceUnavailable_503;
	}
}

void FreeMemoryCommand::Execute()
{
	if (!AllocationRegistry::Instance().Remove(addr)) {
		// Never allocated through this API (or already freed): never
		// reach DOS_FreeMemory/MEM_ReleasePages with an address the
		// API did not mint, whether that address is simply wrong,
		// out of range, or a double free.
		success = false;
		return;
	}

	if (addr < XMS_START * MEM_PAGE_SIZE) {
		success = DOS_FreeMemory(addr / DosBlockSize);
		LOG_DEBUG("API: FreeMemoryCommand(%p): success=%d (DOS allocator)",
		          addr,
		          success);
	} else {
		auto free_before = MEM_FreeTotal();
		MEM_ReleasePages(addr / MEM_PAGE_SIZE);

		auto released = static_cast<int64_t>(MEM_FreeTotal()) - free_before;
		success = released > 0;

		LOG_DEBUG("API: FreeMemoryCommand(%p): released=%d (page allocator)",
		          addr,
		          released);
	}
}

void FreeMemoryCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto j        = json::parse(req.body);
	uint32_t addr = j.at("addr");

	FreeMemoryCommand cmd(addr);
	cmd.WaitForCompletion();

	if (!cmd.success) {
		res.status = httplib::StatusCode::BadRequest_400;
	}
}

} // namespace Webserver
