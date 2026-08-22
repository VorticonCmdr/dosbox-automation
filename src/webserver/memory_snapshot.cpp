// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/memory_snapshot.h"
#include "webserver.h"

#include "private/memory.h"

#include "debugger/debugger.h"
#include "hardware/memory.h"

#include "json/json.h"

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

DiffOp ParseDiffOp(const std::string_view name)
{
	if (name == "changed") {
		return DiffOp::Changed;
	}
	if (name == "unchanged") {
		return DiffOp::Unchanged;
	}
	if (name == "increased") {
		return DiffOp::Increased;
	}
	if (name == "decreased") {
		return DiffOp::Decreased;
	}
	// A literal synonym for "unchanged", not a distinct comparison - the
	// protocol lists both names (matching vocabulary from other memory-
	// scanning tools) but there is only ever one recorded value per
	// candidate to compare against, so there is nothing for the two to
	// mean differently.
	if (name == "equals") {
		return DiffOp::Unchanged;
	}
	throw std::invalid_argument(
	        "op must be one of changed, unchanged, increased, decreased, equals");
}

namespace {

bool MatchesOp(const DiffOp op, const uint32_t stored, const uint32_t fresh)
{
	switch (op) {
	case DiffOp::Changed: return fresh != stored;
	case DiffOp::Unchanged: return fresh == stored;
	case DiffOp::Increased: return fresh > stored;
	case DiffOp::Decreased: return fresh < stored;
	}
	return false;
}

uint32_t ReadLittleEndian(const std::vector<uint8_t>& buf,
                          const uint32_t offset, const int width)
{
	uint32_t v = 0;
	for (int b = 0; b < width; ++b) {
		v |= static_cast<uint32_t>(buf[offset + b]) << (8 * b);
	}
	return v;
}

} // namespace

// --- SnapshotRegistry ---

SnapshotRegistry& SnapshotRegistry::Instance()
{
	static SnapshotRegistry instance;
	return instance;
}

void SnapshotRegistry::EvictLocked(const size_t bytes_needed)
{
	while (!entries.empty() && (total_bytes + bytes_needed > MaxTotalBytes ||
	                            entries.size() >= MaxEntries)) {
		auto oldest = entries.begin();
		for (auto it = entries.begin(); it != entries.end(); ++it) {
			if (it->second.last_touch < oldest->second.last_touch) {
				oldest = it;
			}
		}
		total_bytes -= oldest->second.byte_size;
		entries.erase(oldest);
	}
}

uint64_t SnapshotRegistry::Create(const uint32_t start, const uint32_t end,
                                  std::vector<uint8_t> bytes)
{
	std::lock_guard<std::mutex> lock(mtx);

	const size_t new_bytes = bytes.size();

	// Checked before evicting anything: a request that could never fit
	// regardless of what's evicted must not first evict every other
	// live snapshot as a side effect of a failure that was already
	// certain.
	if (new_bytes > MaxTotalBytes) {
		throw std::invalid_argument(
		        "snapshot span exceeds the total snapshot budget (" +
		        std::to_string(MaxTotalBytes) + " bytes)");
	}

	EvictLocked(new_bytes);

	const uint64_t handle = next_handle++;

	Entry entry;
	entry.mode        = SnapshotMode::Dense;
	entry.start       = start;
	entry.end         = end;
	entry.dense_bytes = std::move(bytes);
	entry.byte_size   = new_bytes;
	entry.last_touch  = next_touch++;
	entry.generation  = 1;

	entries.emplace(handle, std::move(entry));
	total_bytes += new_bytes;

	return handle;
}

std::optional<SnapshotReadPlan> SnapshotRegistry::GetReadPlan(const uint64_t handle)
{
	std::lock_guard<std::mutex> lock(mtx);

	const auto it = entries.find(handle);
	if (it == entries.end()) {
		return std::nullopt;
	}

	// See the declaration's comment: a peek that's always the first
	// step of an actual diff attempt counts as real use.
	it->second.last_touch = next_touch++;

	SnapshotReadPlan plan;
	plan.mode       = it->second.mode;
	plan.generation = it->second.generation;
	if (plan.mode == SnapshotMode::Dense) {
		plan.start = it->second.start;
		plan.end   = it->second.end;
	} else {
		plan.width = it->second.width;
		plan.addresses.reserve(it->second.sparse_values.size());
		for (const auto& [addr, value] : it->second.sparse_values) {
			plan.addresses.push_back(addr);
		}
	}
	return plan;
}

std::optional<DiffResult> SnapshotRegistry::DiffDense(
        const uint64_t handle, const uint64_t generation, const DiffOp op,
        const int width, const size_t limit, const std::vector<uint8_t>& fresh_bytes)
{
	std::lock_guard<std::mutex> lock(mtx);

	const auto it = entries.find(handle);
	if (it == entries.end() || it->second.mode != SnapshotMode::Dense ||
	    it->second.generation != generation) {
		return std::nullopt;
	}
	Entry& entry = it->second;

	DiffResult result;
	std::unordered_map<uint32_t, uint32_t> candidates;

	const auto& stored = entry.dense_bytes;
	for (uint32_t i = 0; i + width <= stored.size(); ++i) {
		const uint32_t stored_v = ReadLittleEndian(stored, i, width);
		const uint32_t fresh_v = ReadLittleEndian(fresh_bytes, i, width);
		if (!MatchesOp(op, stored_v, fresh_v)) {
			continue;
		}
		++result.total;
		const uint32_t addr = entry.start + i;
		if (result.matches.size() < limit) {
			result.matches.push_back({addr, fresh_v});
		}
		if (candidates.size() < MaxCandidates) {
			candidates[addr] = fresh_v;
		}
	}

	result.truncated  = result.total > result.matches.size();
	result.candidates = candidates.size();

	total_bytes -= entry.byte_size;
	entry.mode = SnapshotMode::Sparse;
	entry.dense_bytes.clear();
	entry.dense_bytes.shrink_to_fit();
	entry.width         = width;
	entry.sparse_values = std::move(candidates);
	entry.byte_size = entry.sparse_values.size() * ApproxSparseEntryOverheadBytes;
	entry.last_touch = next_touch++;
	entry.generation += 1;
	total_bytes += entry.byte_size;

	if (entry.sparse_values.empty()) {
		entries.erase(it);
	}

	return result;
}

std::optional<DiffResult> SnapshotRegistry::DiffSparse(
        const uint64_t handle, const uint64_t generation, const DiffOp op,
        const size_t limit, const std::vector<uint32_t>& addresses,
        const std::vector<std::optional<uint32_t>>& fresh_values)
{
	std::lock_guard<std::mutex> lock(mtx);

	const auto it = entries.find(handle);
	if (it == entries.end() || it->second.mode != SnapshotMode::Sparse ||
	    it->second.generation != generation) {
		return std::nullopt;
	}
	Entry& entry = it->second;

	DiffResult result;
	std::unordered_map<uint32_t, uint32_t> candidates;

	for (size_t i = 0; i < addresses.size(); ++i) {
		if (!fresh_values[i]) {
			continue;
		}
		const uint32_t addr  = addresses[i];
		const auto stored_it = entry.sparse_values.find(addr);
		if (stored_it == entry.sparse_values.end()) {
			continue;
		}
		const uint32_t fresh_v = *fresh_values[i];
		if (!MatchesOp(op, stored_it->second, fresh_v)) {
			continue;
		}
		++result.total;
		if (result.matches.size() < limit) {
			result.matches.push_back({addr, fresh_v});
		}
		if (candidates.size() < MaxCandidates) {
			candidates[addr] = fresh_v;
		}
	}

	result.truncated  = result.total > result.matches.size();
	result.candidates = candidates.size();

	total_bytes -= entry.byte_size;
	entry.sparse_values = std::move(candidates);
	entry.byte_size = entry.sparse_values.size() * ApproxSparseEntryOverheadBytes;
	entry.last_touch = next_touch++;
	entry.generation += 1;
	total_bytes += entry.byte_size;

	if (entry.sparse_values.empty()) {
		entries.erase(it);
	}

	return result;
}

void SnapshotRegistry::Clear()
{
	std::lock_guard<std::mutex> lock(mtx);
	entries.clear();
	total_bytes = 0;
}

// --- Bridge Commands ---

void SnapshotRangeReadCommand::Execute()
{
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;
	if (end > mem_total || start >= end) {
		error = "Invalid snapshot range";
		return;
	}
	bytes.resize(end - start);

	// Active execute breakpoints on a non-heavy-debugger build patch a
	// 0xCC trap byte into guest memory (CBreakpoint::Activate); read
	// through them so a snapshot/diff sees the real instruction
	// underneath, not the trap - same mechanism ScanMemoryCommand::Execute
	// uses (memory.cpp). One range query up front, bounded by the
	// number of registered breakpoints rather than the span, instead
	// of a per-byte lookup.
	const auto breakpoint_bytes = DEBUG_GetOriginalBytesInRange(
	        static_cast<PhysPt>(start), static_cast<PhysPt>(end));

	for (uint32_t i = 0; i < bytes.size(); ++i) {
		const auto addr = static_cast<PhysPt>(start + i);
		if (auto it = breakpoint_bytes.find(addr);
		    it != breakpoint_bytes.end()) {
			bytes[i] = it->second;
		} else {
			bytes[i] = mem_readb<MemOpMode::SkipBreakpoints>(addr);
		}
	}
}

void SnapshotSparseReadCommand::Execute()
{
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;

	// Candidate addresses are not contiguous, so there is no single
	// tight [start, end) to query - but DEBUG_GetOriginalBytesInRange's
	// cost is one pass over the breakpoint list regardless of how wide
	// a range it's asked about, so querying the whole address space
	// once costs the same as a tight range would and avoids computing
	// address bounds here.
	const auto breakpoint_bytes =
	        DEBUG_GetOriginalBytesInRange(0, static_cast<PhysPt>(mem_total));

	values.resize(addresses.size());
	for (size_t i = 0; i < addresses.size(); ++i) {
		const uint64_t addr64 = addresses[i];
		if (addr64 + static_cast<uint64_t>(width) > mem_total) {
			values[i] = std::nullopt;
			continue;
		}
		uint32_t v = 0;
		for (int b = 0; b < width; ++b) {
			const auto byte_addr = static_cast<PhysPt>(addr64 + b);
			uint8_t byte_val     = 0;
			if (auto it = breakpoint_bytes.find(byte_addr);
			    it != breakpoint_bytes.end()) {
				byte_val = it->second;
			} else {
				byte_val = mem_readb<MemOpMode::SkipBreakpoints>(
				        byte_addr);
			}
			v |= static_cast<uint32_t>(byte_val) << (8 * b);
		}
		values[i] = v;
	}
}

// --- HTTP handlers ---

namespace {

json MatchesToJson(const std::vector<DiffMatch>& matches)
{
	json arr = json::array();
	for (const auto& m : matches) {
		json entry;
		entry["addr"]  = m.addr;
		entry["value"] = m.value;
		arr.push_back(entry);
	}
	return arr;
}

void SendDiffResult(Response& res, const DiffResult& result)
{
	json j;
	j["matches"]    = MatchesToJson(result.matches);
	j["total"]      = result.total;
	j["truncated"]  = result.truncated;
	j["candidates"] = result.candidates;
	send_json(res, j);
}

// A client-supplied handle no longer referencing a live snapshot is a
// 404, matching this codebase's established convention for the same
// class of "id/handle doesn't currently exist" case (FreezeHandlers::
// Delete, the breakpoint-delete handler in debug.cpp) rather than a
// generic 400 - but still with the structured error_code/retryable
// body every other error in this module gets via the exception-driven
// path, since httplib's exception handler only maps to 400/429/503/504.
void SendHandleNotFound(Response& res, const uint64_t handle)
{
	res.status = httplib::StatusCode::NotFound_404;
	json err;
	err["error"] = "no snapshot with handle " + std::to_string(handle) +
	               " (never existed, evicted, or already diffed by "
	               "another request - re-read it before retrying)";
	err["error_code"] = "not_found";
	err["retryable"]  = false;
	send_json(res, err);
}

} // namespace

void MemorySnapshotHandlers::Post(const Request& req, Response& res)
{
	auto j = json::parse(req.body);

	const uint32_t start = j.at("start").get<uint32_t>();
	const uint32_t end   = j.at("end").get<uint32_t>();

	if (end <= start || end - start > MaxSearchSpanBytes) {
		throw std::invalid_argument("snapshot span must be 1.." +
		                            std::to_string(MaxSearchSpanBytes) +
		                            " bytes");
	}

	SnapshotRangeReadCommand cmd(start, end);
	cmd.WaitForCompletion(2000);
	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	const uint64_t handle = SnapshotRegistry::Instance().Create(start,
	                                                            end,
	                                                            cmd.Bytes());

	json result;
	result["handle"] = handle;
	result["start"]  = start;
	result["end"]    = end;
	result["bytes"]  = end - start;
	send_json(res, result);
}

void MemoryDiffHandlers::Post(const Request& req, Response& res)
{
	auto j = json::parse(req.body);

	const uint64_t handle = j.at("handle").get<uint64_t>();
	const DiffOp op       = ParseDiffOp(j.at("op").get<std::string>());
	const uint32_t limit  = j.value("limit", DefaultSearchLimit);

	if (limit < 1 || limit > MaxSearchLimit) {
		throw std::invalid_argument("limit must be 1.." +
		                            std::to_string(MaxSearchLimit));
	}

	const auto plan = SnapshotRegistry::Instance().GetReadPlan(handle);
	if (!plan) {
		SendHandleNotFound(res, handle);
		return;
	}

	if (plan->mode == SnapshotMode::Dense) {
		const int width = j.value("width", 1);
		if (width != 1 && width != 2 && width != 4) {
			throw std::invalid_argument("width must be 1, 2, or 4");
		}

		SnapshotRangeReadCommand cmd(plan->start, plan->end);
		cmd.WaitForCompletion(2000);
		if (!cmd.error.empty()) {
			throw std::out_of_range(cmd.error);
		}

		const auto result = SnapshotRegistry::Instance().DiffDense(
		        handle, plan->generation, op, width, limit, cmd.Bytes());
		if (!result) {
			SendHandleNotFound(res, handle);
			return;
		}
		SendDiffResult(res, *result);
		return;
	}

	if (j.contains("width") && j.at("width").get<int>() != plan->width) {
		throw std::invalid_argument(
		        "width is locked to " + std::to_string(plan->width) +
		        " for this handle once refinement has started");
	}

	SnapshotSparseReadCommand cmd(plan->addresses, plan->width);
	cmd.WaitForCompletion(2000);
	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	const auto result = SnapshotRegistry::Instance().DiffSparse(
	        handle, plan->generation, op, limit, plan->addresses, cmd.Values());
	if (!result) {
		SendHandleNotFound(res, handle);
		return;
	}
	SendDiffResult(res, *result);
}

} // namespace Webserver
