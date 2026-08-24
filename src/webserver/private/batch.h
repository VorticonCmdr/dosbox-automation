// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_BATCH_H
#define DOSBOX_WEBSERVER_BATCH_H

#include "cpu.h"
#include "memory.h"
#include "webserver/bridge.h"

#include <cstdint>
#include <string>
#include <vector>

#include "http/http.h"
#include "json/json.h"

namespace Webserver {

// Op count and per-request byte budgets. Deliberately tight compared to
// the single-op routes' own limits (MaxMemoryTransferBytes is 128 MiB):
// a batch runs entirely inside one Bridge round trip, on a timeout
// budget scaled by op count (see BatchTimeoutMs) rather than the
// generous per-call deadlines a single large read/write can afford.
constexpr size_t MaxBatchOps        = 64;
constexpr size_t MaxBatchReadBytes  = 1 * 1024 * 1024; // 1 MiB
constexpr size_t MaxBatchWriteBytes = 256 * 1024;      // 256 KiB

// BatchTimeoutMs(n) == min(BatchBaseTimeoutMs + BatchPerOpTimeoutMs*n,
// BatchMaxTimeoutMs) - enough headroom for MaxBatchOps (64) worth of
// memory/port/freeze work without giving a malformed or pathologically
// slow batch an unbounded hold on the emulation thread. The three
// constants are exposed (not just the function) so the capability
// descriptor (capabilities.cpp) can report the same formula it uses,
// not just one derived data point.
constexpr uint32_t BatchBaseTimeoutMs  = 250;
constexpr uint32_t BatchPerOpTimeoutMs = 4;
constexpr uint32_t BatchMaxTimeoutMs   = 2000;

uint32_t BatchTimeoutMs(size_t num_ops);

enum class BatchOpType {
	MemRead,
	MemWrite,
	MemCas,
	CpuRead,
	CpuWrite,
	PortRead,
	PortWrite,
	FreezeSet,
	FreezeClear,
};

// One already-fully-validated operation, parsed and range-checked on
// the web thread before any Bridge round trip - Execute() below never
// re-validates, it only applies. Every op shares one flat struct
// (rather than a tagged union or per-type subclass) matching this
// codebase's existing convention for a Command's request fields
// (e.g. WriteMemoryCommand carries both 'data' and 'expected_data'
// regardless of whether a CAS was requested); only the fields the
// op's own type actually reads are meaningful.
struct BatchOp {
	BatchOpType type = BatchOpType::MemRead;

	// mem_read / mem_write / mem_cas: already-resolved base segment
	// (live, resolved at Execute() time via BaseSegmentToOffset) plus
	// offset (already folded with a numeric segment, if one was given -
	// see ParseBatchOps). mem_write/mem_cas: 'data' is the write
	// payload; mem_cas additionally requires 'expected'. mem_read:
	// 'length' is the byte count to read.
	Segment segment      = Segment::None;
	uint32_t offset      = 0;
	uint32_t length      = 0;
	std::string data     = {};
	std::string expected = {};

	// cpu_write: pre-resolved via RegisterKind (RegClass::Unknown is
	// never stored here - ParseBatchOps rejects that op instead).
	std::string register_name = {};
	RegisterRef register_ref  = {};
	uint32_t cpu_value        = 0;

	// port_read / port_write
	uint32_t port       = 0;
	int port_width      = 1;
	uint32_t port_value = 0;

	// freeze_set / freeze_clear
	uint32_t freeze_address = 0;
	uint32_t freeze_value   = 0;
	int freeze_width        = 1;
};

// Per-op outcome. Every op that BatchCommand::Execute() attempts gets
// exactly one of these, index-correlated with the request's 'ops'
// array; an op skipped because an earlier op aborted the batch
// (on_error == "abort") gets status Skipped without ever running.
enum class BatchOpStatus {
	Ok,
	Conflict,     // mem_cas only: 'expected' didn't match
	NotFound,     // freeze_clear only: no freeze at that address
	RegistryFull, // freeze_set only: FreezeRegistry::MaxEntries reached
	// mem_read/mem_write/mem_cas/freeze_set: the address range exceeds
	// the emulated machine's real memory size. ParseBatchOps validates
	// this too, at parse time on the web thread using a mem_total
	// snapshot taken then - but that snapshot can be stale by the time
	// Execute() actually runs (a Bridge round trip sits between the
	// two), and for a register-relative mem_* segment the real address
	// isn't even knowable until Execute() reads the live register value
	// on the emulation thread. Execute() therefore always re-validates
	// with a freshly recomputed mem_total rather than trusting the
	// parse-time snapshot, for every one of these four op types
	// uniformly. A per-op status rather than aborting the whole Command
	// via Command::error, so it behaves like every other per-op failure
	// under on_error.
	OutOfRange,
	Skipped, // never attempted - an earlier op aborted the batch
};

struct BatchOpResult {
	BatchOpType type     = BatchOpType::MemRead;
	BatchOpStatus status = BatchOpStatus::Skipped;
	// Populated only for OutOfRange - every other non-Ok status is
	// already fully described by its own fields (conflict_data,
	// freeze_address, ...) with no free-text needed.
	std::string message = {};

	// mem_read: the bytes read. mem_write/mem_cas (Ok only): the
	// address written. mem_cas (Conflict only): the actual current
	// bytes, matching WriteMemoryCommand's own conflict_data field.
	uint32_t addr             = 0;
	std::string data          = {};
	std::string conflict_data = {};

	// cpu_read
	Registers registers = {};

	// cpu_write / port_read / port_write / freeze_set: the resulting
	// or read-back value, one shared field since exactly one of these
	// op kinds is ever active per result.
	uint32_t value = 0;

	// freeze_set / freeze_clear
	uint32_t freeze_address = 0;
	int freeze_width        = 1;
};

// Parses and fully validates req's body into a POD op list - throws
// std::invalid_argument (mapped to 400 by ClassifyException) on any
// malformed op, unknown op type, unrecognised register/segment,
// out-of-range port/width/address, or a request exceeding
// MaxBatchOps/MaxBatchReadBytes/MaxBatchWriteBytes. Nothing internal to
// this parse ever reaches the Bridge - 'batch' itself is not a
// recognised op type, so nesting is structurally impossible rather
// than separately checked for. Exposed for testing.
std::vector<BatchOp> ParseBatchOps(const nlohmann::json& body);

// true = stop applying further ops once one fails (Conflict/NotFound/
// RegistryFull); false = apply every op regardless of earlier outcomes.
// Exposed for testing (ParseOnError).
bool ParseOnError(const nlohmann::json& body);

class BatchCommand : public Command {
public:
	explicit BatchCommand(std::vector<BatchOp> ops, const bool abort_on_error)
	        : ops(std::move(ops)),
	          abort_on_error(abort_on_error)
	{}

	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

	const std::vector<BatchOpResult>& Results() const
	{
		return results;
	}

	bool Aborted() const
	{
		return aborted;
	}

private:
	std::vector<BatchOp> ops = {};
	bool abort_on_error      = true;

	std::vector<BatchOpResult> results = {};
	bool aborted                       = false;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_BATCH_H
