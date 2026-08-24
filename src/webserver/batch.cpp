// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/batch.h"
#include "private/freeze.h"
#include "private/io_port.h"
#include "webserver.h"

#include "base64/base64.h"
#include "json/json.h"

#include "cpu/cpu.h"
#include "cpu/registers.h"
#include "hardware/memory.h"
#include "hardware/port.h"

#include <algorithm>
#include <limits>

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

uint32_t BatchTimeoutMs(const size_t num_ops)
{
	const uint64_t scaled = static_cast<uint64_t>(BatchBaseTimeoutMs) +
	                        static_cast<uint64_t>(BatchPerOpTimeoutMs) *
	                                static_cast<uint64_t>(num_ops);
	return static_cast<uint32_t>(std::min<uint64_t>(scaled, BatchMaxTimeoutMs));
}

bool ParseOnError(const json& body)
{
	if (!body.contains("on_error")) {
		return true; // "abort", the default
	}
	const auto value = body.at("on_error").get<std::string>();
	if (value == "abort") {
		return true;
	}
	if (value == "continue") {
		return false;
	}
	throw std::invalid_argument("'on_error' must be 'abort' or 'continue'");
}

// Resolves a batch op's 'segment'/'offset' fields the same way
// ReadMemoryCommand/WriteMemoryCommand resolve their path-param
// equivalents (StrToBaseSegment/BaseSegmentToOffset), adapted to JSON's
// native string-vs-number typing instead of a single ambiguous
// string: a register name is a JSON string, a fixed paragraph value is
// a JSON number - no "is this string secretly a number" fallback
// needed, unlike the URL-path form both single-op routes parse.
static void ResolveBatchMemAddr(const json& op_json, Segment& segment, uint32_t& offset)
{
	offset  = op_json.at("offset").get<uint32_t>();
	segment = Segment::None;

	if (!op_json.contains("segment")) {
		return;
	}

	const auto& seg_field = op_json.at("segment");

	if (seg_field.is_string()) {
		segment = StrToBaseSegment(seg_field.get<std::string>());
		if (segment != Segment::None) {
			return; // resolved live at Execute() time
		}
		throw std::invalid_argument(
		        "'segment' string must be a register name "
		        "(cs/ds/es/fs/gs/ss); use a JSON number for a fixed "
		        "paragraph value");
	}
	// A non-negative integer literal parses as number_unsigned from
	// JSON text but as the signed number_integer when a C++ caller
	// (tests) constructs one directly - accept both, then reject
	// negative and out-of-range values by the actual value, not the
	// json library's internal type tag.
	if (!seg_field.is_number_integer() && !seg_field.is_number_unsigned()) {
		throw std::invalid_argument("'segment' must be a string or an integer");
	}
	const auto seg_signed = seg_field.get<int64_t>();
	if (seg_signed < 0 || seg_signed > 0xFFFF) {
		throw std::invalid_argument("numeric segment must be 0x0000..0xFFFF");
	}
	const auto seg_value = static_cast<uint64_t>(seg_signed);

	const auto seg_addr = PhysicalMake(static_cast<uint16_t>(seg_value), 0);
	// 64-bit arithmetic deliberately - see ReadMemoryCommand::Execute's
	// identical comment: offset plus a resolved paragraph address can
	// overflow uint32_t and silently wrap into a small, in-range-
	// looking address instead of correctly failing here.
	const uint64_t resolved = static_cast<uint64_t>(offset) +
	                          static_cast<uint64_t>(seg_addr);
	if (resolved > std::numeric_limits<uint32_t>::max()) {
		throw std::invalid_argument(
		        "segment:offset exceeds the addressable range");
	}
	offset = static_cast<uint32_t>(resolved);
}

std::vector<BatchOp> ParseBatchOps(const json& body)
{
	if (!body.contains("ops") || !body.at("ops").is_array()) {
		throw std::invalid_argument("'ops' must be an array");
	}
	const auto& ops_json = body.at("ops");

	if (ops_json.empty()) {
		throw std::invalid_argument("'ops' must contain at least one operation");
	}
	if (ops_json.size() > MaxBatchOps) {
		throw std::invalid_argument("'ops' must contain at most " +
		                            std::to_string(MaxBatchOps) +
		                            " operations");
	}

	std::vector<BatchOp> ops;
	ops.reserve(ops_json.size());

	size_t total_read_bytes  = 0;
	size_t total_write_bytes = 0;
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;

	for (size_t i = 0; i < ops_json.size(); ++i) {
		const auto& op_json = ops_json.at(i);
		std::string op_name;

		try {
			if (!op_json.is_object() || !op_json.contains("op") ||
			    !op_json.at("op").is_string()) {
				throw std::invalid_argument(
				        "missing or invalid 'op' field");
			}
			op_name = op_json.at("op").get<std::string>();

			BatchOp op;

			if (op_name == "mem_read") {
				op.type = BatchOpType::MemRead;
				ResolveBatchMemAddr(op_json, op.segment, op.offset);
				op.length = op_json.at("len").get<uint32_t>();
				if (op.length < 1 ||
				    op.length > MaxMemoryTransferBytes) {
					throw std::invalid_argument(
					        "len must be 1.." +
					        std::to_string(MaxMemoryTransferBytes));
				}
				total_read_bytes += op.length;
				if (total_read_bytes > MaxBatchReadBytes) {
					throw std::invalid_argument(
					        "total mem_read bytes across this "
					        "batch exceed " +
					        std::to_string(MaxBatchReadBytes));
				}
			} else if (op_name == "mem_write" || op_name == "mem_cas") {
				op.type = (op_name == "mem_write")
				                ? BatchOpType::MemWrite
				                : BatchOpType::MemCas;
				ResolveBatchMemAddr(op_json, op.segment, op.offset);
				op.data = base64::from_base64(
				        op_json.at("data").get<std::string>());

				const bool has_expected = op_json.contains("expected");
				if (op.type == BatchOpType::MemCas && !has_expected) {
					throw std::invalid_argument(
					        "mem_cas requires 'expected'");
				}
				if (op.type == BatchOpType::MemWrite && has_expected) {
					throw std::invalid_argument(
					        "mem_write does not take 'expected' "
					        "- use mem_cas");
				}
				if (has_expected) {
					op.expected = base64::from_base64(
					        op_json.at("expected").get<std::string>());
				}

				if (op.data.size() > MaxMemoryTransferBytes ||
				    op.expected.size() > MaxMemoryTransferBytes) {
					throw std::invalid_argument(
					        "data/expected exceed the maximum "
					        "transfer size");
				}
				total_write_bytes += op.data.size() +
				                     op.expected.size();
				if (total_write_bytes > MaxBatchWriteBytes) {
					throw std::invalid_argument(
					        "total mem_write/mem_cas bytes "
					        "across this batch exceed " +
					        std::to_string(MaxBatchWriteBytes));
				}
			} else if (op_name == "cpu_read") {
				op.type = BatchOpType::CpuRead;
			} else if (op_name == "cpu_write") {
				op.type = BatchOpType::CpuWrite;
				op.register_name =
				        op_json.at("register").get<std::string>();
				op.register_ref = RegisterKind(op.register_name);
				if (op.register_ref.reg_class == RegClass::Unknown) {
					throw std::invalid_argument(
					        "unknown register: " +
					        op.register_name);
				}
				op.cpu_value = op_json.at("value").get<uint32_t>();
				if (op.register_ref.reg_class == RegClass::Segment &&
				    op.cpu_value > 0xFFFF) {
					throw std::invalid_argument(
					        "segment register value must be "
					        "0..0xFFFF");
				}
			} else if (op_name == "port_read" || op_name == "port_write") {
				op.type = (op_name == "port_read")
				                ? BatchOpType::PortRead
				                : BatchOpType::PortWrite;
				op.port = op_json.at("port").get<uint32_t>();
				op.port_width = op_json.value("width", 1);
				ValidatePortRequest(op.port, op.port_width);
				if (op.type == BatchOpType::PortWrite) {
					op.port_value = op_json.at("value").get<uint32_t>();
				}
			} else if (op_name == "freeze_set") {
				op.type = BatchOpType::FreezeSet;
				op.freeze_address = op_json.at("address").get<uint32_t>();
				op.freeze_value = op_json.at("value").get<uint32_t>();
				op.freeze_width = op_json.value("width", 1);
				if (op.freeze_width != 1 && op.freeze_width != 2 &&
				    op.freeze_width != 4) {
					throw std::invalid_argument(
					        "width must be 1, 2, or 4");
				}
				if (!ValidateFreezeRange(op.freeze_address,
				                         op.freeze_width,
				                         mem_total)) {
					throw std::invalid_argument(
					        "address out of range");
				}
			} else if (op_name == "freeze_clear") {
				op.type = BatchOpType::FreezeClear;
				op.freeze_address = op_json.at("address").get<uint32_t>();
			} else {
				throw std::invalid_argument("unknown op: '" +
				                            op_name + "'");
			}

			ops.push_back(std::move(op));
		} catch (const std::exception& e) {
			// Catches json::exception
			// (malformed/missing/wrong-typed field),
			// std::invalid_argument (every validator this function
			// calls: ValidatePortRequest, ValidateFreezeRange callers,
			// RegisterKind rejections, ...), AND anything else a
			// future op branch's parsing might throw - e.g.
			// base64::from_base64 (libs/base64/base64.h) throws a
			// plain std::runtime_error on malformed input, a
			// sibling of invalid_argument rather than a subclass of
			// it, which a narrower two-clause catch here previously
			// let escape uncaught into a generic 500 instead of a
			// clean 400. Every exception type reaching this point
			// started inside pure, engine-free op parsing, so
			// re-throwing all of them as invalid_argument (-> 400
			// via ClassifyException) is always correct, not just
			// convenient.
			throw std::invalid_argument(
			        "ops[" + std::to_string(i) +
			        (op_name.empty() ? "" : " (" + op_name + ")") +
			        "]: " + std::string(e.what()));
		}
	}

	return ops;
}

static bool IsSoftFailure(const BatchOpStatus status)
{
	return status == BatchOpStatus::Conflict ||
	       status == BatchOpStatus::NotFound ||
	       status == BatchOpStatus::RegistryFull ||
	       status == BatchOpStatus::OutOfRange;
}

void BatchCommand::Execute()
{
	const uint64_t mem_total = static_cast<uint64_t>(MEM_TotalPages()) *
	                           MemPageSize;
	results.reserve(ops.size());

	for (const auto& op : ops) {
		BatchOpResult result;
		result.type = op.type;

		switch (op.type) {
		case BatchOpType::MemRead: {
			const uint64_t addr64 = static_cast<uint64_t>(BaseSegmentToOffset(
			                                op.segment)) +
			                        static_cast<uint64_t>(op.offset);
			if (addr64 + op.length > mem_total) {
				result.status = BatchOpStatus::OutOfRange;
				result.message =
				        "address range exceeds emulated "
				        "memory size";
				break;
			}
			result.addr = static_cast<uint32_t>(addr64);
			result.data.resize(op.length);
			MEM_BlockRead(result.addr, result.data.data(), op.length);
			result.status = BatchOpStatus::Ok;
			break;
		}
		case BatchOpType::MemWrite: {
			const uint64_t addr64 = static_cast<uint64_t>(BaseSegmentToOffset(
			                                op.segment)) +
			                        static_cast<uint64_t>(op.offset);
			if (addr64 + op.data.size() > mem_total) {
				result.status = BatchOpStatus::OutOfRange;
				result.message =
				        "address range exceeds emulated "
				        "memory size";
				break;
			}
			result.addr = static_cast<uint32_t>(addr64);
			MEM_BlockWrite(result.addr, op.data.data(), op.data.size());
			result.status = BatchOpStatus::Ok;
			break;
		}
		case BatchOpType::MemCas: {
			const uint64_t addr64 = static_cast<uint64_t>(BaseSegmentToOffset(
			                                op.segment)) +
			                        static_cast<uint64_t>(op.offset);
			// Two separate bounds - matching WriteMemoryCommand::
			// Execute()'s identical reasoning: 'expected' (the
			// compare read) and 'data' (the eventual write) can be
			// different lengths, and each must independently fit.
			if (addr64 + op.expected.size() > mem_total) {
				result.status = BatchOpStatus::OutOfRange;
				result.message =
				        "'expected' range exceeds emulated "
				        "memory size";
				break;
			}
			if (addr64 + op.data.size() > mem_total) {
				result.status = BatchOpStatus::OutOfRange;
				result.message =
				        "'data' range exceeds emulated "
				        "memory size";
				break;
			}
			result.addr = static_cast<uint32_t>(addr64);
			std::string current(op.expected.size(), '\0');
			MEM_BlockRead(result.addr, current.data(), current.size());
			if (current != op.expected) {
				result.status        = BatchOpStatus::Conflict;
				result.conflict_data = std::move(current);
				break;
			}
			MEM_BlockWrite(result.addr, op.data.data(), op.data.size());
			result.status = BatchOpStatus::Ok;
			break;
		}
		case BatchOpType::CpuRead: {
			result.registers.load();
			result.status = BatchOpStatus::Ok;
			break;
		}
		case BatchOpType::CpuWrite: {
			WriteRegisterValue(op.register_ref, op.cpu_value);
			result.value  = op.cpu_value;
			result.status = BatchOpStatus::Ok;
			break;
		}
		case BatchOpType::PortRead: {
			result.value = (op.port_width == 2)
			                     ? IO_ReadW(static_cast<io_port_t>(op.port))
			                     : IO_ReadB(static_cast<io_port_t>(
			                               op.port));
			result.status = BatchOpStatus::Ok;
			break;
		}
		case BatchOpType::PortWrite: {
			if (op.port_width == 2) {
				IO_WriteW(static_cast<io_port_t>(op.port),
				          static_cast<io_val_t>(op.port_value));
			} else {
				IO_WriteB(static_cast<io_port_t>(op.port),
				          static_cast<io_val_t>(op.port_value));
			}
			result.value  = op.port_value;
			result.status = BatchOpStatus::Ok;
			break;
		}
		case BatchOpType::FreezeSet: {
			result.freeze_address = op.freeze_address;
			result.freeze_width   = op.freeze_width;
			result.value          = op.freeze_value;
			// Re-checked here, not just at parse time: mem_read/
			// mem_write/mem_cas above all recompute mem_total and
			// re-validate inside Execute() rather than trusting
			// ParseBatchOps's web-thread snapshot - freeze_set
			// follows the same principle, closing the
			// time-of-check-to-time- of-use gap a Bridge round trip
			// sits between the two.
			if (!ValidateFreezeRange(op.freeze_address,
			                         op.freeze_width,
			                         mem_total)) {
				result.status  = BatchOpStatus::OutOfRange;
				result.message = "address out of range";
				break;
			}
			result.status = FreezeRegistry::Instance().Add(op.freeze_address,
			                                               op.freeze_value,
			                                               op.freeze_width)
			                      ? BatchOpStatus::Ok
			                      : BatchOpStatus::RegistryFull;
			break;
		}
		case BatchOpType::FreezeClear: {
			result.freeze_address = op.freeze_address;
			result.status = FreezeRegistry::Instance().Remove(op.freeze_address)
			                      ? BatchOpStatus::Ok
			                      : BatchOpStatus::NotFound;
			break;
		}
		}

		results.push_back(result);

		if (IsSoftFailure(result.status) && abort_on_error) {
			aborted = true;
			break;
		}
	}

	// Any op never attempted (aborted early) stays Skipped - keeps
	// 'results' exactly ops.size() long, index-correlated with the
	// request even when the batch stopped short.
	while (results.size() < ops.size()) {
		const auto& skipped_op = ops[results.size()];
		BatchOpResult skipped;
		skipped.type   = skipped_op.type;
		skipped.status = BatchOpStatus::Skipped;
		// freeze_set/freeze_clear echo 'address' unconditionally in
		// ResultToJson (unlike every other op type, which gates its
		// address/value fields behind an Ok/Conflict status) - without
		// this, a skipped freeze op would report the fabricated address
		// 0 instead of what the caller actually asked for, and 0 is
		// itself a valid address in this API.
		if (skipped_op.type == BatchOpType::FreezeSet ||
		    skipped_op.type == BatchOpType::FreezeClear) {
			skipped.freeze_address = skipped_op.freeze_address;
		}
		results.push_back(skipped);
	}
}

static const char* OpTypeName(const BatchOpType type)
{
	switch (type) {
	case BatchOpType::MemRead: return "mem_read";
	case BatchOpType::MemWrite: return "mem_write";
	case BatchOpType::MemCas: return "mem_cas";
	case BatchOpType::CpuRead: return "cpu_read";
	case BatchOpType::CpuWrite: return "cpu_write";
	case BatchOpType::PortRead: return "port_read";
	case BatchOpType::PortWrite: return "port_write";
	case BatchOpType::FreezeSet: return "freeze_set";
	case BatchOpType::FreezeClear: return "freeze_clear";
	}
	return "unknown";
}

static const char* OpStatusName(const BatchOpStatus status)
{
	switch (status) {
	case BatchOpStatus::Ok: return "ok";
	case BatchOpStatus::Conflict: return "conflict";
	case BatchOpStatus::NotFound: return "not_found";
	case BatchOpStatus::RegistryFull: return "registry_full";
	case BatchOpStatus::OutOfRange: return "out_of_range";
	case BatchOpStatus::Skipped: return "skipped";
	}
	return "unknown";
}

static json ResultToJson(const BatchOpResult& r)
{
	json j;
	j["op"]     = OpTypeName(r.type);
	j["status"] = OpStatusName(r.status);

	switch (r.type) {
	case BatchOpType::MemRead:
		if (r.status == BatchOpStatus::Ok) {
			j["addr"] = r.addr;
			j["data"] = base64::to_base64(r.data);
		}
		break;
	case BatchOpType::MemWrite:
		if (r.status == BatchOpStatus::Ok) {
			j["addr"] = r.addr;
		}
		break;
	case BatchOpType::MemCas:
		if (r.status == BatchOpStatus::Ok ||
		    r.status == BatchOpStatus::Conflict) {
			j["addr"] = r.addr;
		}
		if (r.status == BatchOpStatus::Conflict) {
			j["data"] = base64::to_base64(r.conflict_data);
		}
		break;
	case BatchOpType::CpuRead:
		if (r.status == BatchOpStatus::Ok) {
			j["registers"] = r.registers;
		}
		break;
	case BatchOpType::CpuWrite:
	case BatchOpType::PortRead:
	case BatchOpType::PortWrite:
		if (r.status == BatchOpStatus::Ok) {
			j["value"] = r.value;
		}
		break;
	case BatchOpType::FreezeSet:
		j["address"] = r.freeze_address;
		if (r.status == BatchOpStatus::Ok) {
			j["value"] = r.value;
			j["width"] = r.freeze_width;
		}
		break;
	case BatchOpType::FreezeClear: j["address"] = r.freeze_address; break;
	}

	if (!r.message.empty()) {
		j["message"] = r.message;
	}

	return j;
}

void BatchCommand::Post(const Request& req, Response& res)
{
	const auto body = json::parse(req.body);

	auto ops            = ParseBatchOps(body);
	const bool on_abort = ParseOnError(body);
	const auto num_ops  = ops.size();

	BatchCommand cmd(std::move(ops), on_abort);
	cmd.WaitForCompletion(BatchTimeoutMs(num_ops));

	if (!cmd.error.empty()) {
		throw std::runtime_error(cmd.error);
	}

	json results_json = json::array();
	for (const auto& r : cmd.Results()) {
		results_json.push_back(ResultToJson(r));
	}

	json j;
	j["results"] = results_json;
	j["aborted"] = cmd.Aborted();
	send_json(res, j);
}

} // namespace Webserver
