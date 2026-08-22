// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_MEMORY_SNAPSHOT_H
#define DOSBOX_WEBSERVER_MEMORY_SNAPSHOT_H

#include "webserver/bridge.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "http/http.h"

namespace Webserver {

// A live snapshot starts Dense (a plain byte copy of [start, end)) and
// switches to Sparse the first time it's diffed - from then on it
// tracks only the addresses that are still candidates, each with the
// value recorded for it as of the most recent diff. This is what makes
// "refine" cheap: round 2 onward reads and compares only the survivors
// of round 1, not the whole original range again.
enum class SnapshotMode { Dense, Sparse };

// "equals" is a valid request-body value but not a distinct member here:
// ParseDiffOp maps it straight to Unchanged (see its own comment) since
// there is only ever one recorded value per candidate to compare
// against, so the two names can't mean anything different.
enum class DiffOp { Changed, Unchanged, Increased, Decreased };

// Throws std::invalid_argument for anything but the five names the
// protocol defines (changed, unchanged, increased, decreased, equals).
DiffOp ParseDiffOp(std::string_view name);

// What SnapshotRegistry::GetReadPlan says the caller needs to read from
// guest memory before it can call DiffDense/DiffSparse. Dense mode
// wants [start, end); Sparse mode wants exactly `addresses`, each read
// at `width` bytes (the width locked in by whichever diff call first
// moved this handle to Sparse mode).
struct SnapshotReadPlan {
	SnapshotMode mode               = SnapshotMode::Dense;
	uint32_t start                  = 0;
	uint32_t end                    = 0;
	std::vector<uint32_t> addresses = {};
	int width                       = 1;
	// The entry's generation at the moment this plan was read. Pass
	// back to DiffDense/DiffSparse unchanged - they refuse to apply a
	// comparison computed from a plan that is no longer current (e.g.
	// another request diffed this same handle first), rather than
	// silently mixing this caller's freshly-read data with whatever
	// candidate set that other request already narrowed it to.
	uint64_t generation = 0;
};

struct DiffMatch {
	uint32_t addr  = 0;
	uint32_t value = 0;
};

struct DiffResult {
	std::vector<DiffMatch> matches = {};
	// The true number of positions that satisfied `op` this round, which
	// can exceed matches.size() (response-level truncation, capped by
	// the caller's `limit`) and can also exceed `candidates` (registry-
	// level truncation, capped at SnapshotRegistry::MaxCandidates - a
	// handle refined from a round with more true survivors than that
	// will not have all of them available to refine further). When
	// `candidates` is capped this way, the ones kept are the
	// numerically-lowest matching addresses in the scanned range, an
	// arbitrary-but-deterministic choice with no relation to which one
	// the caller is actually looking for - a round this unselective
	// should be treated as "narrow the search and take a new snapshot",
	// not "keep refining this handle and trust the survivors."
	uint64_t total      = 0;
	bool truncated      = false;
	uint64_t candidates = 0;
};

// Tracks live memory snapshots for POST /memory/snapshot and
// POST /memory/diff. Follows FreezeRegistry's shape: a mutex-protected
// singleton holding pure data, with no knowledge of the Bridge or the
// emulation thread - callers read fresh guest memory via a Command
// first (SnapshotRangeReadCommand/SnapshotSparseReadCommand below),
// then hand the result to this registry for the actual comparison.
//
// Capped by total bytes across every live snapshot (LRU-evicted, not
// capped by count) plus a generous backstop entry-count cap purely
// against the degenerate case of many minimal-size snapshots each
// individually under the byte cap but expensive in aggregate as
// unordered_map bookkeeping.
class SnapshotRegistry {
public:
	static constexpr size_t MaxTotalBytes = 32 * 1024 * 1024; // 32 MiB
	static constexpr size_t MaxEntries    = 4096;
	// Cap on the number of addresses a Sparse-mode handle tracks.
	// Refine reads every tracked address individually, in one Bridge
	// Command, with the Bridge mutex held for the whole call - without
	// this cap a single diff on an unbounded candidate set could stall
	// the emulation thread for as long as that takes.
	static constexpr size_t MaxCandidates = 65536;

	// A single unordered_map<uint32_t,uint32_t> node costs far more
	// than the 8 bytes of its logical (address, value) pair once
	// allocator/node-pointer/bucket-array overhead is counted - this is
	// a deliberately conservative per-candidate estimate (not a
	// sizeof()-derived one, which would again undercount) so the byte
	// budget below reflects something closer to the real heap cost of a
	// Sparse-mode entry.
	static constexpr size_t ApproxSparseEntryOverheadBytes = 48;

	static SnapshotRegistry& Instance();

	// Takes ownership of `bytes` (already read from guest memory by the
	// caller). Evicts least-recently-used entries first if needed to
	// make room under MaxTotalBytes/MaxEntries; throws
	// std::invalid_argument if `bytes` alone still wouldn't fit even in
	// an empty registry (unreachable given the per-request span cap is
	// well under MaxTotalBytes, but checked before evicting anything
	// else, not after - a request that can never fit must not first
	// evict every other live snapshot as a side effect of failing).
	uint64_t Create(uint32_t start, uint32_t end, std::vector<uint8_t> bytes);

	// nullopt if `handle` doesn't exist (never existed, or evicted).
	// Counts as a touch: this is always the first step of an actual
	// diff attempt (never exposed as its own standalone lookup), so the
	// Bridge round-trip a caller spends reading fresh memory before
	// calling DiffDense/DiffSparse should not itself make the handle
	// the single most eviction-prone entry in the registry.
	std::optional<SnapshotReadPlan> GetReadPlan(uint64_t handle);

	// `fresh_bytes` must be exactly (end - start) bytes for the range
	// GetReadPlan(handle) most recently returned, and `generation` must
	// be the value that same GetReadPlan call returned. Moves the
	// handle from Dense to Sparse mode. nullopt if `handle` no longer
	// exists (e.g. evicted between this caller's GetReadPlan and this
	// call) or its generation has since moved on (e.g. another request
	// diffed this same handle first) - the comparison would otherwise
	// mix this caller's freshly-read data with a candidate set it never
	// actually observed.
	std::optional<DiffResult> DiffDense(uint64_t handle, uint64_t generation,
	                                    DiffOp op, int width, size_t limit,
	                                    const std::vector<uint8_t>& fresh_bytes);

	// Same generation contract as DiffDense. `addresses` and
	// `fresh_values` must correspond index-for-index to what
	// GetReadPlan(handle) most recently returned; a nullopt entry in
	// `fresh_values` means that address is no longer readable (e.g.
	// memory shrank) and is dropped as a candidate, not compared.
	// Removes the handle entirely once no candidates survive.
	std::optional<DiffResult> DiffSparse(
	        uint64_t handle, uint64_t generation, DiffOp op, size_t limit,
	        const std::vector<uint32_t>& addresses,
	        const std::vector<std::optional<uint32_t>>& fresh_values);

	void Clear();

private:
	struct Entry {
		SnapshotMode mode                = SnapshotMode::Dense;
		uint32_t start                   = 0;
		uint32_t end                     = 0;
		std::vector<uint8_t> dense_bytes = {};

		int width                                            = 1;
		std::unordered_map<uint32_t, uint32_t> sparse_values = {};

		size_t byte_size    = 0;
		uint64_t last_touch = 0;
		// Incremented on every mutation (Create, DiffDense,
		// DiffSparse). DiffDense/DiffSparse refuse to run against a
		// generation that has since moved on - see their own comments.
		uint64_t generation = 0;
	};

	mutable std::mutex mtx                      = {};
	std::unordered_map<uint64_t, Entry> entries = {};
	uint64_t next_handle                        = 1;
	uint64_t next_touch                         = 1;
	size_t total_bytes                          = 0;

	// Assumes mtx already held. Evicts least-recently-touched entries
	// until adding `bytes_needed` more would fit under MaxTotalBytes,
	// or until nothing is left to evict. A full scan per eviction
	// (O(evicted_count * remaining_count), worst case O(MaxEntries^2)
	// when the registry is entirely evicted in one call) rather than a
	// proper LRU structure - bounded by MaxEntries either way (a few
	// million cheap uint64_t comparisons at the extreme, not
	// unbounded), and simple/obviously-correct beats a hand-rolled
	// intrusive LRU list for a cap this size.
	void EvictLocked(size_t bytes_needed);
};

// Reads [start, end) as one contiguous block - used both to create a
// Dense snapshot and to gather fresh bytes for a Dense-mode diff.
// Mirrors SearchMemoryCommand::Execute's bounds-check shape.
class SnapshotRangeReadCommand : public Command {
public:
	SnapshotRangeReadCommand(uint32_t start, uint32_t end)
	        : start(start),
	          end(end)
	{}

	void Execute() override;

	const std::vector<uint8_t>& Bytes() const
	{
		return bytes;
	}

private:
	uint32_t start = 0;
	uint32_t end   = 0;

	std::vector<uint8_t> bytes = {};
};

// Reads `width` bytes at each of `addresses` individually, all inside
// one Execute() call so refining a candidate set costs one Bridge
// round-trip regardless of how many addresses it holds. An address
// that no longer fits in emulated memory (re-validated here, not
// trusted from whenever it was first recorded) yields nullopt at its
// position rather than failing the whole batch - it can no longer be
// tracked as a candidate, which is exactly what "dropped" means to the
// caller.
class SnapshotSparseReadCommand : public Command {
public:
	SnapshotSparseReadCommand(std::vector<uint32_t> addresses, int width)
	        : addresses(std::move(addresses)),
	          width(width)
	{}

	void Execute() override;

	const std::vector<std::optional<uint32_t>>& Values() const
	{
		return values;
	}

private:
	std::vector<uint32_t> addresses = {};
	int width                       = 1;

	std::vector<std::optional<uint32_t>> values = {};
};

struct MemorySnapshotHandlers {
	static void Post(const httplib::Request& req, httplib::Response& res);
};

struct MemoryDiffHandlers {
	static void Post(const httplib::Request& req, httplib::Response& res);
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_MEMORY_SNAPSHOT_H
