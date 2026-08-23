// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/dos.h"
#include "bridge.h"
#include "webserver.h"

#include <algorithm>
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

bool McbChainTruncated(const std::vector<McbBlock>& chain)
{
	return chain.empty() || !chain.back().is_last;
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
	memory_map_truncated = McbChainTruncated(memory_map);

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
	j["memoryMap"]          = map;
	j["memoryMapTruncated"] = cmd.memory_map_truncated;

	send_json(res, j);
}

std::string_view AreaName(const MemoryArea area)
{
	switch (area) {
	case MemoryArea::Conv: return "CONV";
	case MemoryArea::Uma: return "UMA";
	case MemoryArea::Xms: return "XMS";
	}
	return "UNKNOWN";
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

void AllocationRegistry::Add(const uint32_t addr, const uint32_t size,
                             const MemoryArea area, const uint16_t owner_psp)
{
	std::lock_guard<std::mutex> lock(mtx);
	live_addrs[addr] = AllocationInfo{size, area, owner_psp};
}

std::optional<AllocationInfo> AllocationRegistry::Remove(const uint32_t addr)
{
	std::lock_guard<std::mutex> lock(mtx);
	auto it = live_addrs.find(addr);
	if (it == live_addrs.end()) {
		return std::nullopt;
	}
	auto info = it->second;
	live_addrs.erase(it);
	return info;
}

std::vector<std::pair<uint32_t, AllocationInfo>> AllocationRegistry::List() const
{
	std::lock_guard<std::mutex> lock(mtx);
	std::vector<std::pair<uint32_t, AllocationInfo>> result(live_addrs.begin(),
	                                                        live_addrs.end());
	std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});
	return result;
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
	} else {
		allocated_bytes = static_cast<uint32_t>(blocks) * DosBlockSize;
	}
}

void AllocMemoryCommand::AllocXms()
{
	auto num_pages = (bytes + MEM_PAGE_SIZE - 1) / MEM_PAGE_SIZE;
	auto handle    = MEM_AllocatePages(num_pages, true);

	// Returns 0 on error or out of memory, nullptr is handled as error below.
	addr            = handle * MEM_PAGE_SIZE;
	allocated_bytes = static_cast<uint32_t>(num_pages) * MEM_PAGE_SIZE;
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
		addr          = 0;
		registry_full = true;
		return;
	}

	if (area < MemoryArea::Xms) {
		AllocDos();
	} else {
		AllocXms();
	}

	if (addr) {
		// dos.psp() is stable to read here: AllocDos/AllocXms never
		// change it, they only read it (AllocDos indirectly, inside
		// DOS_AllocateMemory). Meaningless for Xms - MEM_AllocatePages
		// has no concept of a PSP owner - so left at its default (0)
		// there; FreeMemoryCommand only consults it for Conv/Uma.
		const uint16_t owner_psp = (area == MemoryArea::Xms) ? 0
		                                                     : dos.psp();
		AllocationRegistry::Instance().Add(addr, allocated_bytes, area, owner_psp);
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
		json err;
		// Structured error_code/retryable alongside the message,
		// matching the shape the centralized exception handler gives
		// every other route (webserver.cpp's send_error) - this path
		// can't reuse that helper directly (it's file-local there), but
		// dosbox-mcp's DosboxClient._handle() depends on both fields
		// being present to tell this failure's two causes apart
		// programmatically, not just in the free-text message. Neither
		// cause clears on its own, so retryable stays false for both -
		// a caller must free something (registry_full) or free up guest
		// memory (insufficient_memory) before retrying, not just wait.
		if (cmd.registry_full) {
			err["error"] = "allocation registry is full (" +
			               std::to_string(AllocationRegistry::MaxEntries) +
			               " live entries) - free some allocations first";
			err["error_code"] = "registry_full";
		} else {
			err["error"] = "insufficient free memory for this allocation";
			err["error_code"] = "insufficient_memory";
		}
		err["retryable"] = false;
		send_json(res, err);
	}
}

void FreeMemoryCommand::Execute()
{
	const auto removed = AllocationRegistry::Instance().Remove(addr);
	if (!removed) {
		// Never allocated through this API (or already freed): never
		// reach DOS_FreeMemory/MEM_ReleasePages with an address the
		// API did not mint, whether that address is simply wrong,
		// out of range, or a double free.
		success = false;
		return;
	}

	if (removed->area != MemoryArea::Xms) {
		// DOS_FreeMemory's own segment-to-MCB relationship
		// (dos_memory.cpp): the MCB header sits one paragraph before
		// the data segment it returned. Re-read who currently owns it
		// - see AllocationInfo::owner_psp for why this can have
		// changed since allocation without this registry ever
		// hearing about it.
		const DOS_MCB mcb(static_cast<uint16_t>(addr / DosBlockSize) - 1);
		if (mcb.GetPSPSeg() != removed->owner_psp) {
			owner_changed = true;
			success       = false;
			return;
		}
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
		json err;
		// See AllocMemoryCommand::Post's identical comment on why
		// error_code/retryable are added by hand here rather than via
		// the (file-local) send_error helper.
		if (cmd.owner_changed) {
			err["error"] =
			        "addr's owner has changed since it was allocated - "
			        "the program it belonged to has likely exited and "
			        "DOS has since reused this memory for a different, "
			        "currently-running program; freeing it now would "
			        "corrupt that program's memory, so this has been "
			        "refused";
			err["error_code"] = "owner_changed";
		} else {
			err["error"] =
			        "addr was not allocated through this API, was "
			        "already freed, or the engine could not free it";
			err["error_code"] = "not_allocated";
		}
		err["retryable"] = false;
		send_json(res, err);
	}
}

void MemoryAllocationsCommand::Execute()
{
	allocations = AllocationRegistry::Instance().List();

	const auto conv_chain = WalkMcbChain(dos.firstMCB, ReadMcbFromGuest, 1000);
	conventional_truncated = McbChainTruncated(conv_chain);
	for (const auto& block : conv_chain) {
		if (block.psp_segment == MCB_FREE) {
			const auto free_bytes = static_cast<uint32_t>(block.size_paras) *
			                        DosBlockSize;
			conventional_free_bytes += free_bytes;
			conventional_largest_block_bytes = std::max(
			        conventional_largest_block_bytes, free_bytes);
		}
	}

	// GetStartOfUMBChain() reads UmbStartSegment when a UMB chain is
	// linked, or 0xffff when it isn't - nothing in the engine ever
	// writes a third value there, so require exact equality rather than
	// just "not 0xffff", matching DOS_AllocateMemory/
	// DOS_FreeProcessMemory/DOS_LinkUMBsToMemChain's own identical
	// check (dos_memory.cpp). A guest-corrupted or client-mem_write'd
	// value would otherwise pass WalkMcbChain a segment that is not a
	// real chain at all - WalkMcbChain can't tell a coincidentally
	// type-byte-matching garbage read from a genuine MCB, so it would
	// silently fold fabricated bytes into umb_free_bytes rather than
	// failing safe.
	if (const uint16_t umb_start = dos_infoblock.GetStartOfUMBChain();
	    umb_start == UmbStartSegment) {
		const auto umb_chain = WalkMcbChain(umb_start, ReadMcbFromGuest, 1000);
		umb_truncated = McbChainTruncated(umb_chain);
		for (const auto& block : umb_chain) {
			if (block.psp_segment == MCB_FREE) {
				const auto free_bytes = static_cast<uint32_t>(
				                                block.size_paras) *
				                        DosBlockSize;
				umb_free_bytes += free_bytes;
			}
		}
	}

	xms_free_bytes = MEM_FreeTotal() * MEM_PAGE_SIZE;

	LOG_DEBUG("API: MemoryAllocationsCommand(): %zu live allocations",
	          allocations.size());
}

void MemoryAllocationsCommand::Get(const httplib::Request&, httplib::Response& res)
{
	MemoryAllocationsCommand cmd;
	cmd.WaitForCompletion();

	json list = json::array();
	for (const auto& [addr, info] : cmd.allocations) {
		json entry;
		entry["addr"] = addr;
		entry["size"] = info.size;
		entry["area"] = std::string(AreaName(info.area));
		list.push_back(entry);
	}

	json j;
	j["allocations"]           = list;
	j["conventionalFreeBytes"] = cmd.conventional_free_bytes;
	j["conventionalLargestBlockBytes"] = cmd.conventional_largest_block_bytes;
	j["conventionalTruncated"] = cmd.conventional_truncated;
	j["umbFreeBytes"]          = cmd.umb_free_bytes;
	j["umbTruncated"]          = cmd.umb_truncated;
	j["xmsFreeBytes"]          = cmd.xms_free_bytes;

	send_json(res, j);
}

} // namespace Webserver
