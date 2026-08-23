// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_DOS_H
#define DOSBOX_WEBSERVER_DOS_H

#include "webserver/bridge.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hardware/memory.h"
#include "http/http.h"

namespace Webserver {

struct McbBlock {
	uint16_t segment     = 0;
	uint8_t type         = 0;
	uint16_t psp_segment = 0;
	uint16_t size_paras  = 0;
	std::string filename = {};
	bool is_last         = false;
};

using McbReader = std::function<McbBlock(uint16_t segment)>;

std::vector<McbBlock> WalkMcbChain(uint16_t start_segment,
                                   const McbReader& reader, int max_blocks);

// True when the walk stopped before a proper terminating ('Z') block -
// either max_blocks was hit, or an invalid type byte aborted it early.
// Either way, `chain` is not the whole picture: a caller summing or
// searching it (free-memory totals, dos/internals' memoryMap) should
// surface that rather than silently reporting a partial result as
// complete.
bool McbChainTruncated(const std::vector<McbBlock>& chain);

// Get pointers to interesting data structures, this command is just to prevent
// breakages if these ever change and users hard-code these offsets. It's not
// a place to pull random info that can also be read by the client from these
// addresses directly.
class DosInternalsCommand : public Command {
	PhysPt list_of_lists             = {};
	PhysPt dos_swappable_area        = {};
	PhysPt first_shell               = {};
	std::vector<McbBlock> memory_map = {};
	bool memory_map_truncated        = false;

public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);
};

enum class MemoryArea { Conv, Uma, Xms };

enum class AllocStrategy { FirstFit, BestFit, LastFit };

// The name AllocMemoryCommand::Post accepts/reports for each MemoryArea
// value ("CONV"/"UMA"/"XMS") - shared with MemoryAllocationsCommand so
// a listed allocation's area matches what a caller would have sent to
// mem_alloc.
std::string_view AreaName(MemoryArea area);

struct AllocationInfo {
	// Bytes actually reserved (paragraph/page-rounded), not the raw
	// request size - what DOS/the page allocator itself thinks it gave
	// out, matching the LOG_DEBUG lines in AllocDos/AllocXms.
	uint32_t size   = 0;
	MemoryArea area = MemoryArea::Conv;
	// The PSP DOS_AllocateMemory stamped this block's MCB with
	// (dos.psp() at allocation time) - meaningless (left 0) for Xms,
	// which is page-allocator-backed and never MCB/PSP-tracked at all.
	// FreeMemoryCommand re-reads the block's *current* MCB owner before
	// freeing a Conv/Uma entry and refuses if it no longer matches this:
	// DOS_FreeProcessMemory silently reclaims every block a program
	// owns when that program exits, with no notification to this
	// registry, so a still-tracked address can by then legitimately
	// belong to a different, currently-running program - freeing it
	// would corrupt that program's memory, not just fail cleanly.
	uint16_t owner_psp = 0;
};

// Tracks addresses this API has allocated and not yet freed, so
// FreeMemoryCommand can refuse to reach DOS_FreeMemory/MEM_ReleasePages
// with an address the API never minted. Without this gate, an
// unvalidated address from the request body reaches
// MEM_ReleasePages(addr / MEM_PAGE_SIZE), which indexes its internal
// handle vector with no bounds check.
//
// Mutex-guarded even though today's only callers (AllocMemoryCommand
// and FreeMemoryCommand::Execute) are already serialized on the
// emulation thread via the Bridge, matching FreezeRegistry's pattern in
// case that changes.
class AllocationRegistry {
public:
	static constexpr size_t MaxEntries = 4096;

	bool IsFull() const;
	void Add(uint32_t addr, uint32_t size, MemoryArea area,
	         uint16_t owner_psp = 0);

	// nullopt if addr was never allocated through this API, or was
	// already freed - the caller must not proceed to free it. Otherwise
	// the removed entry, so the caller can still use its area/owner_psp
	// after this call (e.g. the current-owner check described on
	// AllocationInfo::owner_psp) without a second lookup of an entry
	// this call has already erased.
	std::optional<AllocationInfo> Remove(uint32_t addr);

	// Every currently-live allocation, address ascending - the order is
	// otherwise unspecified (unordered_map iteration), and a stable
	// order matters for a route a caller might poll repeatedly.
	std::vector<std::pair<uint32_t, AllocationInfo>> List() const;

	static AllocationRegistry& Instance();

private:
	mutable std::mutex mtx                                  = {};
	std::unordered_map<uint32_t, AllocationInfo> live_addrs = {};
};

class AllocMemoryCommand : public Command {
public:
	AllocMemoryCommand(const uint16_t bytes, const MemoryArea area,
	                   const AllocStrategy strategy)
	        : area(area),
	          strategy(strategy),
	          bytes(bytes)

	{}

	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

private:
	MemoryArea area          = {};
	AllocStrategy strategy   = AllocStrategy::BestFit;
	uint32_t addr            = 0;
	uint16_t bytes           = 0;
	uint32_t allocated_bytes = 0;
	// Set instead of addr staying 0 for the ordinary "engine is out of
	// room" case, so Post can report which one actually happened
	// instead of a single generic failure message.
	bool registry_full = false;

	void AllocDos();
	void AllocXms();
};

class FreeMemoryCommand : public Command {
public:
	FreeMemoryCommand(const uint32_t addr) : addr(addr) {}

	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

private:
	uint32_t addr = 0;
	bool success  = false;
	// Set instead of success staying false for the ordinary "never
	// allocated/already freed" case, so Post can report the more
	// specific reason - see AllocationInfo::owner_psp.
	bool owner_changed = false;
};

// Free-memory totals plus a snapshot of what this API has allocated -
// deliberately not part of DosInternalsCommand (dos/internals exists to
// hand out pointers, not to aggregate information already readable from
// the memory it points at).
class MemoryAllocationsCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

	std::vector<std::pair<uint32_t, AllocationInfo>> allocations = {};

	uint32_t conventional_free_bytes          = 0;
	uint32_t conventional_largest_block_bytes = 0;
	bool conventional_truncated               = false;

	uint32_t umb_free_bytes = 0;
	bool umb_truncated      = false;

	uint32_t xms_free_bytes = 0;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DOS_H
