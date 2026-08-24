// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_MEMORY_H
#define DOSBOX_WEBSERVER_MEMORY_H

#include "cpu.h"
#include "webserver/bridge.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "http/http.h"

namespace Webserver {

enum class Segment { None, CS, SS, DS, ES, FS, GS };

// "CS"/"DS"/etc (case-insensitive) to the matching Segment, or
// Segment::None for anything else - including a numeric paragraph
// value, which the caller resolves separately via
// BaseSegmentToOffset()/PhysicalMake(). Exposed so BatchCommand
// (batch.cpp) resolves a JSON-body 'segment' field the exact same way
// ReadMemoryCommand/WriteMemoryCommand resolve their path-param one.
Segment StrToBaseSegment(std::string_view str);

// The live physical base address of a register-named Segment (0 for
// Segment::None, since a numeric segment is folded into the caller's
// offset instead - see StrToBaseSegment's comment).
uint32_t BaseSegmentToOffset(Segment segment);

// Shared by ReadMemoryCommand::Get's 'len' bound and
// WriteMemoryCommand::Put's body-size check, so the two directions can't
// silently drift apart, and by the capability descriptor
// (capabilities.cpp) so it reports the same number it enforces.
constexpr size_t MaxMemoryTransferBytes = 128 * 1024 * 1024; // 128 MiB

// SearchMemoryCommand::Post's [start, end) span bound.
constexpr uint32_t MaxSearchSpanBytes = 16 * 1024 * 1024; // 16 MiB

// SearchMemoryCommand::Post's 'limit' bound and default. A width=1 scan
// for a common byte over the full span can match millions of times;
// without a cap the response is megabytes of transcript from one call.
// 'total' (the real match count) is still reported uncapped, so a
// caller can tell a capped result from a complete one.
constexpr uint32_t DefaultSearchLimit = 256;
constexpr uint32_t MaxSearchLimit     = 4096;

// ScanMemoryCommand::Post's pattern token-count bound. A token is one
// hex-pair byte or one '??' wildcard.
constexpr size_t MinScanPatternBytes = 1;
constexpr size_t MaxScanPatternBytes = 256;

// ScanMemoryCommand::Post's worst-case CPU bound. The masked-compare
// scan is early-exit: cost per candidate start position is bounded by
// the pattern's fixed-byte count, so (span * fixed_count) bounds total
// work even for guest memory crafted to defeat the early exit. 1e9
// assumes a conservative 500M byte-compares/sec for the scalar loop,
// keeping worst case comfortably under the 2000ms Bridge timeout.
constexpr uint64_t MaxScanWorstCaseOps = 1'000'000'000;

class ReadMemoryCommand : public Command {
public:
	ReadMemoryCommand(const Segment base, const uint32_t offset, const uint32_t len)
	        : base(base),
	          offset(offset),
	          len(len)
	{}

	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

private:
	// Request
	Segment base    = {};
	uint32_t offset = {};
	uint32_t len    = {};

	// Response
	// Memory is std::string to avoid ugly casts for httplib
	std::string memory      = {};
	uint32_t effective_addr = {};
	Registers regs          = {};
};

class WriteMemoryCommand : public Command {
public:
	WriteMemoryCommand(const Segment base, const uint32_t offset,
	                   std::string data, std::string expected_data)
	        : base(base),
	          offset(offset),
	          data(std::move(data)),
	          expected_data(std::move(expected_data))
	{}

	void Execute() override;
	static void Put(const httplib::Request& req, httplib::Response& res);

private:
	// Request
	Segment base     = {};
	uint32_t offset  = {};
	std::string data = {};
	// Only write the data if the current data at the address exactly
	// matches this. Usable as an atomic CAS to implement a mutex.
	std::string expected_data = {};

	// Response
	uint32_t effective_addr = {};
	// Only filled if expected_data was set and didn't match.
	std::string conflict_data = {};
};

// Scan a byte buffer for little-endian matches of `value` at `width`
// (1, 2, or 4). Returns the offsets where a match starts, capped at
// `limit` entries; when `total_out` is non-null, it receives the true
// number of matches found (which may exceed `limit`/the returned
// vector's size). Defaults keep every pre-existing call site (and its
// exact return-value assertions) unchanged: unlimited, no total.
std::vector<uint32_t> ScanBufferForValue(
        const std::vector<uint8_t>& buf, uint32_t value, int width,
        size_t limit      = std::numeric_limits<size_t>::max(),
        size_t* total_out = nullptr);

class SearchMemoryCommand : public Command {
public:
	SearchMemoryCommand(uint32_t start, uint32_t end, uint32_t value,
	                    int width, uint32_t limit)
	        : start(start),
	          end(end),
	          value(value),
	          width(width),
	          limit(limit)
	{}

	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

	const std::vector<uint32_t>& Matches() const
	{
		return matches;
	}

	// The true number of matches in [start, end), even when it exceeds
	// Matches().size() because `limit` capped what was returned.
	uint64_t Total() const
	{
		return total;
	}

private:
	uint32_t start = 0;
	uint32_t end   = 0;
	uint32_t value = 0;
	int width      = 1;
	uint32_t limit = DefaultSearchLimit;

	std::vector<uint32_t> matches = {};
	uint64_t total                = 0;
};

// One pattern byte for a masked signature scan: a fixed value to match,
// or nullopt for a '??' wildcard that matches anything.
using ScanPattern = std::vector<std::optional<uint8_t>>;

// Parse a space-separated signature string like "8B 46 ?? 50 E8" into a
// ScanPattern. Throws std::invalid_argument for a malformed token or a
// token count outside MinScanPatternBytes..MaxScanPatternBytes. Does not
// check for an all-wildcard pattern - that policy check belongs to the
// caller (ScanMemoryCommand::Post), since it doesn't depend on the text
// alone.
ScanPattern ParseScanPattern(const std::string& text);

// Scan a byte buffer for positions where every fixed (non-wildcard)
// pattern byte matches; wildcard positions match unconditionally. Cost
// per candidate position is O(fixed byte count), not O(pattern
// length) - wildcards are precomputed out of the inner loop rather
// than walked and skipped. Mirrors ScanBufferForValue's limit/total_out
// contract.
std::vector<uint32_t> ScanBufferForPattern(
        const std::vector<uint8_t>& buf, const ScanPattern& pattern,
        size_t limit      = std::numeric_limits<size_t>::max(),
        size_t* total_out = nullptr);

// Is `count_fixed` fixed bytes enough to keep a pattern's expected
// number of matches over a `span_len`-byte range near one, assuming
// uniformly distributed byte values? 256^4 already exceeds the largest
// possible span (span fits in uint32_t), so capping at 4 iterations
// always resolves the comparison - no risk of overflowing capacity even
// for the maximum pattern length (256 fixed bytes).
bool PatternSelectiveEnoughForSpan(size_t count_fixed, uint64_t span_len);

// Does a `count_fixed`-fixed-byte pattern stay within the scan's CPU
// budget over a `span_len`-byte range? Relies on ScanBufferForPattern's
// per-position cost genuinely being O(count_fixed).
bool PatternWithinScanCpuBudget(size_t count_fixed, uint64_t span_len);

class ScanMemoryCommand : public Command {
public:
	ScanMemoryCommand(uint32_t start, uint32_t end, ScanPattern pattern,
	                  uint32_t limit)
	        : start(start),
	          end(end),
	          pattern(std::move(pattern)),
	          limit(limit)
	{}

	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

	const std::vector<uint32_t>& Matches() const
	{
		return matches;
	}

	// The true number of matches in [start, end), even when it exceeds
	// Matches().size() because `limit` capped what was returned.
	uint64_t Total() const
	{
		return total;
	}

private:
	uint32_t start      = 0;
	uint32_t end        = 0;
	ScanPattern pattern = {};
	uint32_t limit      = DefaultSearchLimit;

	std::vector<uint32_t> matches = {};
	uint64_t total                = 0;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_MEMORY_H
