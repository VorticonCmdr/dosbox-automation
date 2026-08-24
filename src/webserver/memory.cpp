// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/memory.h"
#include "bridge.h"
#include "webserver.h"

#include "base64/base64.h"
#include "http/http.h"
#include "utils/string_utils.h"
#include "json/json.h"

#include "cpu/registers.h"
#include "debugger/debugger.h"
#include "dos/dos_memory.h"
#include "hardware/memory.h"

#include <limits>

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

Segment StrToBaseSegment(const std::string_view str)
{
	auto val = upcase(str);
	static const std::unordered_map<std::string_view, Segment> lookup = {
	        {"CS", Segment::CS},
	        {"SS", Segment::SS},
	        {"DS", Segment::DS},
	        {"ES", Segment::ES},
	        {"FS", Segment::FS},
	        {"GS", Segment::GS},
	};

	if (auto it = lookup.find(val); it != lookup.end()) {
		return it->second;
	} else {
		return Segment::None;
	}
}

uint32_t BaseSegmentToOffset(const Segment segment)
{
	switch (segment) {
	case Segment::CS: return SegPhys(SegNames::cs);
	case Segment::SS: return SegPhys(SegNames::ss);
	case Segment::DS: return SegPhys(SegNames::ds);
	case Segment::ES: return SegPhys(SegNames::es);
	case Segment::FS: return SegPhys(SegNames::fs);
	case Segment::GS: return SegPhys(SegNames::gs);
	case Segment::None: return 0;
	default: return 0;
	}
}

static void parse_mem_addr(const httplib::Request& req, Segment& segment,
                           uint32_t& offset)
{
	offset  = num_param<uint32_t>(req, Source::Path, "offset");
	segment = Segment::None;

	if (req.path_params.find("segment") != req.path_params.end()) {
		auto& segment_param = req.path_params.at("segment");
		segment             = StrToBaseSegment(segment_param);

		// Segment can either be a register to resolve later or an
		// address which we can already resolve here.
		if (segment == Segment::None) {
			const auto seg_addr = PhysicalMake(
			        num_param<uint16_t>(req, Source::Path, "segment"), 0);

			// 64-bit arithmetic deliberately: offset is untrusted
			// HTTP input, and a uint32_t addition can overflow and
			// wrap into a small, in-range-looking value instead of
			// correctly failing the emulation-thread bounds check
			// later.
			const uint64_t resolved = static_cast<uint64_t>(offset) +
			                          static_cast<uint64_t>(seg_addr);
			if (resolved > std::numeric_limits<uint32_t>::max()) {
				throw std::invalid_argument(
				        "segment:offset exceeds the addressable range");
			}
			offset = static_cast<uint32_t>(resolved);
		}
	}
}

void ReadMemoryCommand::Execute()
{
	regs.load();

	// 64-bit arithmetic deliberately: BaseSegmentToOffset()'s live
	// segment base plus offset (untrusted HTTP input for the linear/
	// numeric-segment forms) can overflow uint32_t and silently wrap
	// into a small, in-range-looking address instead of correctly
	// failing the bounds check below.
	const uint64_t addr64 = static_cast<uint64_t>(BaseSegmentToOffset(base)) +
	                        static_cast<uint64_t>(offset);

	LOG_DEBUG("API: ReadMemoryCommand(0x%06x, %d)",
	          static_cast<uint32_t>(addr64),
	          len);

	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;

	const uint64_t end_addr = addr64 + len;

	if (end_addr > mem_total) {
		error = "Address range 0x" + std::to_string(addr64) + " + " +
		        std::to_string(len) + " exceeds emulated memory size (" +
		        std::to_string(mem_total) + " bytes)";
		return;
	}

	effective_addr = static_cast<uint32_t>(addr64);

	memory.resize(len);
	MEM_BlockRead(effective_addr, memory.data(), len);
}

void ReadMemoryCommand::Get(const Request& req, Response& res)
{
	// MaxMemoryTransferBytes per request ought to be enough for everyone.
	// This limit just prevents bad things when accidentally requesting an
	// unreasonably large size.
	auto num_bytes = num_param<uint32_t>(req,
	                                     Source::Path,
	                                     "len",
	                                     1,
	                                     static_cast<uint32_t>(
	                                             MaxMemoryTransferBytes));

	Segment segment;
	uint32_t offset;
	parse_mem_addr(req, segment, offset);

	ReadMemoryCommand cmd(segment, offset, num_bytes);
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	if (req.get_header_value("accept").starts_with(TypeJson)) {
		json j;
		j["registers"]      = cmd.regs;
		j["memory"]["addr"] = cmd.effective_addr;
		j["memory"]["data"] = base64::to_base64(cmd.memory);

		send_json(res, j);

	} else {
		// Only do base64 if explicitly requested, binary/download by
		// default.
		res.set_header("Content-Disposition",
		               "attachment; filename =\"memory.bin\"");
		res.set_content(cmd.memory, TypeBinary);
	}
}

void WriteMemoryCommand::Execute()
{
	// 64-bit arithmetic deliberately - see ReadMemoryCommand::Execute's
	// comment: BaseSegmentToOffset() plus offset can overflow
	// uint32_t and silently wrap into a small, in-range-looking address.
	const uint64_t addr64 = static_cast<uint64_t>(BaseSegmentToOffset(base)) +
	                        static_cast<uint64_t>(offset);

	LOG_DEBUG("API: WriteMemoryCommand(0x%06x, %d)",
	          static_cast<uint32_t>(addr64),
	          data.size());

	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;

	const uint64_t end_addr = addr64 + data.size();

	if (end_addr > mem_total) {
		error = "Address range 0x" + std::to_string(addr64) + " + " +
		        std::to_string(data.size()) +
		        " exceeds emulated memory size (" +
		        std::to_string(mem_total) + " bytes)";
		return;
	}

	effective_addr = static_cast<uint32_t>(addr64);

	if (!expected_data.empty()) {
		// Separate bound: the CAS comparison reads expected_data.size()
		// bytes, a different length than the write payload just
		// checked above - without this, a client could send a short
		// 'data' (passing the check above) paired with a longer
		// If-Match value that reads past emulated memory instead of
		// failing with the same clear error.
		const uint64_t expected_end_addr = addr64 + expected_data.size();
		if (expected_end_addr > mem_total) {
			error = "If-Match range 0x" + std::to_string(addr64) +
			        " + " + std::to_string(expected_data.size()) +
			        " exceeds emulated memory size (" +
			        std::to_string(mem_total) + " bytes)";
			return;
		}

		conflict_data.resize(expected_data.size());

		MEM_BlockRead(effective_addr,
		              conflict_data.data(),
		              conflict_data.size());

		if (expected_data != conflict_data) {
			return;
		}

		conflict_data.clear();
	}

	MEM_BlockWrite(effective_addr, data.data(), data.size());
}

void WriteMemoryCommand::Put(const httplib::Request& req, httplib::Response& res)
{
	Segment segment;
	uint32_t offset;
	parse_mem_addr(req, segment, offset);

	std::string data;

	if (req.get_header_value("Content-Type") == TypeJson) {
		auto j = json::parse(req.body);
		data   = base64::from_base64(j.at("data").get<std::string>());

	} else if (req.get_header_value("Content-Type") == TypeBinary) {
		data = req.body;

	} else {
		throw std::invalid_argument("Content-Type must be either " +
		                            std::string(TypeJson) + " or " +
		                            std::string(TypeBinary));
	}

	if (data.size() > MaxMemoryTransferBytes) {
		throw std::invalid_argument(
		        "Write data exceeds maximum size of 128 MiB");
	}

	std::string expected_data;

	if (req.has_header("If-Match")) {
		// The standard requires ETags in this and ETags are quoted
		// but we accept unquoted because no one is gonna bother
		std::string etag_hdr  = req.get_header_value("If-Match");
		std::string_view etag = etag_hdr;
		if (etag.starts_with('"') && etag.ends_with('"')) {
			etag.remove_prefix(1);
			etag.remove_suffix(1);
		}
		expected_data = base64::from_base64(etag);
	}

	WriteMemoryCommand cmd(segment, offset, std::move(data), std::move(expected_data));
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	json j;
	j["memory"]["addr"] = cmd.effective_addr;
	if (!cmd.conflict_data.empty()) {
		res.status = httplib::StatusCode::PreconditionFailed_412;
		j["memory"]["data"] = base64::to_base64(cmd.conflict_data);
	}

	send_json(res, j);
}

// --- Memory value scan ---

std::vector<uint32_t> ScanBufferForValue(const std::vector<uint8_t>& buf,
                                         const uint32_t value, const int width,
                                         const size_t limit, size_t* total_out)
{
	if (width != 1 && width != 2 && width != 4) {
		throw std::invalid_argument("width must be 1, 2, or 4");
	}
	std::vector<uint32_t> hits;
	size_t total = 0;
	if (buf.size() >= static_cast<size_t>(width)) {
		for (uint32_t i = 0; i + width <= buf.size(); ++i) {
			uint32_t v = 0;
			for (int b = 0; b < width; ++b) {
				v |= static_cast<uint32_t>(buf[i + b]) << (8 * b);
			}
			if (v == value) {
				++total;
				if (hits.size() < limit) {
					hits.push_back(i);
				}
			}
		}
	}
	if (total_out) {
		*total_out = total;
	}
	return hits;
}

void SearchMemoryCommand::Execute()
{
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;
	if (end > mem_total || start >= end) {
		error = "Invalid search range";
		return;
	}
	const auto len = end - start;
	std::vector<uint8_t> buf(len);
	MEM_BlockRead(start, buf.data(), len);

	size_t total_count = 0;
	matches = ScanBufferForValue(buf, value, width, limit, &total_count);
	total   = total_count;

	for (auto& m : matches) {
		m += start;
	}
}

void SearchMemoryCommand::Post(const Request& req, Response& res)
{
	auto j = json::parse(req.body);

	const uint32_t value = j.at("value").get<uint32_t>();
	const int width      = j.value("width", 1);
	const uint32_t start = j.at("start").get<uint32_t>();
	const uint32_t end   = j.at("end").get<uint32_t>();
	const uint32_t limit = j.value("limit", DefaultSearchLimit);

	if (width != 1 && width != 2 && width != 4) {
		throw std::invalid_argument("width must be 1, 2, or 4");
	}

	if (end <= start || end - start > MaxSearchSpanBytes) {
		throw std::invalid_argument("search span must be 1.." +
		                            std::to_string(MaxSearchSpanBytes) +
		                            " bytes");
	}

	if (limit < 1 || limit > MaxSearchLimit) {
		throw std::invalid_argument("limit must be 1.." +
		                            std::to_string(MaxSearchLimit));
	}

	SearchMemoryCommand cmd(start, end, value, width, limit);
	cmd.WaitForCompletion(2000);
	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	json result;
	result["matches"]   = cmd.Matches();
	result["total"]     = cmd.Total();
	result["truncated"] = cmd.Total() > cmd.Matches().size();
	send_json(res, result);
}

// --- Memory masked signature scan ---

ScanPattern ParseScanPattern(const std::string& text)
{
	const auto tokens = split(text);

	if (tokens.size() < MinScanPatternBytes ||
	    tokens.size() > MaxScanPatternBytes) {
		throw std::invalid_argument(
		        "pattern must have " + std::to_string(MinScanPatternBytes) +
		        ".." + std::to_string(MaxScanPatternBytes) +
		        " space-separated byte tokens");
	}

	ScanPattern pattern;
	pattern.reserve(tokens.size());

	for (const auto& token : tokens) {
		if (token == "??") {
			pattern.emplace_back(std::nullopt);
			continue;
		}

		const auto value = token.size() == 2 && is_hex_digits(token)
		                         ? parse_int(token, 16)
		                         : std::nullopt;

		if (!value || *value < 0 || *value > 0xFF) {
			// Echoing the offending token back is helpful, but the
			// token is untrusted and unbounded in length (a single
			// token can't overrun MaxScanPatternBytes, which counts
			// tokens, not characters) - cap what gets reflected
			// into the error body.
			constexpr size_t MaxEchoedTokenChars = 32;
			const std::string shown =
			        token.size() > MaxEchoedTokenChars
			                ? token.substr(0, MaxEchoedTokenChars) + "..."
			                : token;
			throw std::invalid_argument("pattern token '" + shown +
			                            "' must be two hex digits or '?\?'");
		}

		pattern.push_back(static_cast<uint8_t>(*value));
	}

	return pattern;
}

std::vector<uint32_t> ScanBufferForPattern(const std::vector<uint8_t>& buf,
                                           const ScanPattern& pattern,
                                           const size_t limit, size_t* total_out)
{
	std::vector<uint32_t> hits;
	size_t total         = 0;
	const size_t pat_len = pattern.size();

	// Only the fixed bytes ever need comparing - a wildcard matches
	// unconditionally. Looping the raw pattern including wildcards per
	// candidate position would still cost one iteration per wildcard
	// even though nothing gets compared, making the true worst case
	// per position O(pat_len) rather than O(fixed_count) - and the
	// caller's CPU budget check (ScanMemoryCommand::Post) assumes
	// exactly the latter. Precomputing just the fixed (offset, value)
	// pairs once keeps that assumption honest.
	std::vector<std::pair<size_t, uint8_t>> fixed_bytes;
	fixed_bytes.reserve(pat_len);
	for (size_t j = 0; j < pat_len; ++j) {
		if (pattern[j]) {
			fixed_bytes.emplace_back(j, *pattern[j]);
		}
	}

	if (pat_len > 0 && buf.size() >= pat_len) {
		for (uint32_t i = 0; i + pat_len <= buf.size(); ++i) {
			bool matched = true;
			for (const auto& [offset, value] : fixed_bytes) {
				if (buf[i + offset] != value) {
					matched = false;
					break;
				}
			}
			if (matched) {
				++total;
				if (hits.size() < limit) {
					hits.push_back(i);
				}
			}
		}
	}

	if (total_out) {
		*total_out = total;
	}
	return hits;
}

bool PatternSelectiveEnoughForSpan(const size_t count_fixed, const uint64_t span_len)
{
	uint64_t capacity       = 1;
	const size_t iterations = std::min<size_t>(count_fixed, 4);
	for (size_t i = 0; i < iterations; ++i) {
		capacity *= 256;
	}
	return capacity >= span_len;
}

bool PatternWithinScanCpuBudget(const size_t count_fixed, const uint64_t span_len)
{
	// Valid only because ScanBufferForPattern's inner loop is now
	// genuinely O(count_fixed) per candidate position (see the fixed_bytes
	// precomputation above) - span_len and count_fixed are both bounded
	// (MaxSearchSpanBytes, MaxScanPatternBytes) well below what could
	// overflow this multiplication in uint64_t.
	return span_len * static_cast<uint64_t>(count_fixed) <= MaxScanWorstCaseOps;
}

void ScanMemoryCommand::Execute()
{
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;
	if (end > mem_total || start >= end) {
		error = "Invalid scan range";
		return;
	}

	const auto len = end - start;
	std::vector<uint8_t> buf(len);

	// Active execute breakpoints on a non-heavy-debugger build patch a
	// 0xCC trap byte into guest memory (CBreakpoint::Activate); read
	// through them so a scan matches the real instruction underneath,
	// not the trap. SkipBreakpoints only suppresses this read's own
	// memory-read-watchpoint side effect (a heavy-debugger-only
	// concept) - it does not see through the 0xCC patch, hence the
	// separate substitution below. One range lookup up front, bounded
	// by the number of registered breakpoints rather than the span -
	// querying per byte would cost span_len * breakpoint_count and
	// blow straight past the CPU budget below regardless of the
	// pattern.
	const auto breakpoint_bytes = DEBUG_GetOriginalBytesInRange(
	        static_cast<PhysPt>(start), static_cast<PhysPt>(end));

	for (uint32_t i = 0; i < len; ++i) {
		const auto addr = static_cast<PhysPt>(start + i);
		if (auto it = breakpoint_bytes.find(addr);
		    it != breakpoint_bytes.end()) {
			buf[i] = it->second;
		} else {
			buf[i] = mem_readb<MemOpMode::SkipBreakpoints>(addr);
		}
	}

	size_t total_count = 0;
	matches = ScanBufferForPattern(buf, pattern, limit, &total_count);
	total   = total_count;

	for (auto& m : matches) {
		m += start;
	}
}

void ScanMemoryCommand::Post(const Request& req, Response& res)
{
	auto j = json::parse(req.body);

	const std::string pattern_text = j.at("pattern").get<std::string>();
	const uint32_t start           = j.at("start").get<uint32_t>();
	const uint32_t end             = j.at("end").get<uint32_t>();
	const uint32_t limit           = j.value("limit", DefaultSearchLimit);

	auto pattern = ParseScanPattern(pattern_text);

	size_t fixed_count = 0;
	for (const auto& b : pattern) {
		if (b.has_value()) {
			++fixed_count;
		}
	}

	if (fixed_count == 0) {
		throw std::invalid_argument(
		        "pattern must contain at least one fixed byte, not "
		        "just wildcards");
	}

	if (end <= start || end - start > MaxSearchSpanBytes) {
		throw std::invalid_argument("scan span must be 1.." +
		                            std::to_string(MaxSearchSpanBytes) +
		                            " bytes");
	}

	const uint64_t span_len = static_cast<uint64_t>(end) - start;

	if (pattern.size() > span_len) {
		throw std::invalid_argument(
		        "pattern (" + std::to_string(pattern.size()) +
		        " bytes) does not fit in a " +
		        std::to_string(span_len) + "-byte span");
	}

	if (!PatternSelectiveEnoughForSpan(fixed_count, span_len)) {
		throw std::invalid_argument(
		        "pattern's " + std::to_string(fixed_count) +
		        " fixed byte(s) is not selective enough for a " +
		        std::to_string(span_len) +
		        "-byte span; add more fixed bytes or narrow the range");
	}

	if (!PatternWithinScanCpuBudget(fixed_count, span_len)) {
		throw std::invalid_argument(
		        "pattern's fixed-byte count is too high for a span "
		        "this large and would risk exceeding the scan time "
		        "budget; narrow the range or reduce fixed bytes");
	}

	if (limit < 1 || limit > MaxSearchLimit) {
		throw std::invalid_argument("limit must be 1.." +
		                            std::to_string(MaxSearchLimit));
	}

	ScanMemoryCommand cmd(start, end, std::move(pattern), limit);
	cmd.WaitForCompletion(2000);
	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	json result;
	result["matches"]   = cmd.Matches();
	result["total"]     = cmd.Total();
	result["truncated"] = cmd.Total() > cmd.Matches().size();
	send_json(res, result);
}

} // namespace Webserver
