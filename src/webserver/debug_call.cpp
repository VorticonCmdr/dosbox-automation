// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/debug_call.h"

#include "webserver.h"

#include "base64/base64.h"
#include "dosbox.h"

#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

#if C_DEBUGGER

namespace {

void reject_unknown_keys(const json& j, const std::set<std::string_view>& allowed,
                         const std::string& where)
{
	for (const auto& [key, value] : j.items()) {
		if (!allowed.contains(key)) {
			throw std::invalid_argument(where + ": unknown field '" +
			                            key + "'");
		}
	}
}

int64_t require_int(const json& j, const char* key, const int64_t min_value,
                    const int64_t max_value)
{
	if (!j.contains(key) || !j[key].is_number_integer()) {
		throw std::invalid_argument(std::string(key) + " must be an integer");
	}
	const auto v = j[key].get<int64_t>();
	if (v < min_value || v > max_value) {
		throw std::invalid_argument(std::string(key) + " must be " +
		                            std::to_string(min_value) + ".." +
		                            std::to_string(max_value));
	}
	return v;
}

int64_t optional_int(const json& j, const char* key, const int64_t fallback,
                     const int64_t min_value, const int64_t max_value)
{
	return j.contains(key) ? require_int(j, key, min_value, max_value) : fallback;
}

DebugCall::Case parse_case(const json& c, const std::string& where, size_t& patch_bytes)
{
	if (!c.is_object()) {
		throw std::invalid_argument(where + " must be an object");
	}
	reject_unknown_keys(c, {"registers", "memory"}, where);
	DebugCall::Case out = {};
	if (c.contains("registers")) {
		const auto& regs = c["registers"];
		if (!regs.is_object()) {
			throw std::invalid_argument(
			        where + ".registers must be an object");
		}
		for (const auto& [name, value] : regs.items()) {
			const auto reg = DebugCall::ParseRegister(name);
			if (!reg) {
				throw std::invalid_argument(
				        where + ".registers: unknown register '" + name +
				        "' (eax..esp, eflags, ds, es, fs, gs)");
			}
			if (!value.is_number_integer() || value.get<int64_t>() < 0 ||
			    value.get<int64_t>() >
			            std::numeric_limits<uint32_t>::max()) {
				throw std::invalid_argument(
				        where + ".registers." + name +
				        " must be 0..0xFFFFFFFF");
			}
			out.registers.push_back(
			        {*reg, static_cast<uint32_t>(value.get<int64_t>())});
		}
	}
	if (c.contains("memory")) {
		const auto& mem = c["memory"];
		if (!mem.is_array() || mem.size() > MaxCallPatchesPerCase) {
			throw std::invalid_argument(
			        where + ".memory must be an array of at most " +
			        std::to_string(MaxCallPatchesPerCase) + " patches");
		}
		for (const auto& p : mem) {
			if (!p.is_object()) {
				throw std::invalid_argument(
				        where + ".memory entries must be objects");
			}
			reject_unknown_keys(p, {"address", "data"}, where + ".memory[]");
			DebugCall::MemoryPatch patch = {};
			patch.address = static_cast<uint32_t>(require_int(
			        p, "address", 0, std::numeric_limits<uint32_t>::max()));
			if (!p.contains("data") || !p["data"].is_string()) {
				throw std::invalid_argument(
				        where + ".memory[].data must be base64");
			}
			const auto bytes = base64::from_base64(
			        p["data"].get<std::string>());
			if (bytes.empty()) {
				throw std::invalid_argument(
				        where + ".memory[].data must not be empty");
			}
			patch_bytes += bytes.size();
			if (patch_bytes > MaxCallPatchBytes) {
				throw std::invalid_argument(
				        "memory patches exceed " +
				        std::to_string(MaxCallPatchBytes) +
				        " bytes in total");
			}
			patch.bytes.assign(bytes.begin(), bytes.end());
			out.memory.push_back(std::move(patch));
		}
	}
	return out;
}

DebugCall::Range parse_range(const json& r)
{
	if (!r.is_object()) {
		throw std::invalid_argument("ranges entries must be objects");
	}
	reject_unknown_keys(r, {"space", "start", "end"}, "ranges[]");
	DebugCall::Range out = {};
	const auto space     = r.value("space", std::string("ram"));
	if (space == "ram") {
		out.space = DebugCall::Space::Ram;
	} else if (space == "vram") {
		out.space = DebugCall::Space::Vram;
	} else {
		throw std::invalid_argument("ranges[].space must be 'ram' or 'vram'");
	}
	const auto start =
	        require_int(r, "start", 0, std::numeric_limits<uint32_t>::max());
	const auto end = require_int(r, "end", 1, std::numeric_limits<uint32_t>::max());
	if (end <= start || end - start > MaxCallRangeBytes) {
		throw std::invalid_argument(
		        "ranges[]: end must be above start and the range at most " +
		        std::to_string(MaxCallRangeBytes) + " bytes");
	}
	out.start = static_cast<uint32_t>(start);
	out.end   = static_cast<uint32_t>(end);
	return out;
}

std::string_view kind_name(const DebugCall::InterruptKind k)
{
	switch (k) {
	case DebugCall::InterruptKind::Software: return "software";
	case DebugCall::InterruptKind::Exception: return "exception";
	case DebugCall::InterruptKind::Hardware: return "hardware";
	}
	return "?";
}

} // namespace

DebugCall::Request ParseDebugCallRequest(const json& j)
{
	if (!j.is_object()) {
		throw std::invalid_argument("request body must be a JSON object");
	}
	reject_unknown_keys(j,
	                    {"kind",
	                     "segment",
	                     "offset",
	                     "return",
	                     "stub_offset",
	                     "cases",
	                     "registers",
	                     "memory",
	                     "ranges",
	                     "max_cycles",
	                     "max_diff_bytes",
	                     "timeout_ms",
	                     "timers"},
	                    "request");
	DebugCall::Request req = {};

	if (!j.contains("kind") || !j["kind"].is_string()) {
		throw std::invalid_argument(
		        "kind must be 'near16', 'far16' or 'near32'");
	}
	const auto kind = DebugCall::ParseCallKind(j["kind"].get<std::string>());
	if (!kind) {
		throw std::invalid_argument(
		        "kind must be 'near16', 'far16' or 'near32'");
	}
	req.kind = *kind;
	req.segment = static_cast<uint16_t>(require_int(j, "segment", 0, 0xffff));
	const int64_t max_offset = req.kind == DebugCall::CallKind::Near32
	                                 ? std::numeric_limits<uint32_t>::max()
	                                 : 0xffff;
	req.offset = static_cast<uint32_t>(require_int(j, "offset", 0, max_offset));
	if (j.contains("return")) {
		const auto mode = j["return"].is_string()
		                        ? DebugCall::ParseReturnMode(
		                                  j["return"].get<std::string>())
		                        : std::nullopt;
		if (!mode) {
			throw std::invalid_argument(
			        "return must be 'push' or 'replace'");
		}
		req.return_mode = *mode;
	}
	if (j.contains("stub_offset")) {
		if (req.kind != DebugCall::CallKind::Near16) {
			throw std::invalid_argument(
			        "stub_offset only applies to kind 'near16'");
		}
		req.stub_offset = static_cast<uint16_t>(
		        require_int(j, "stub_offset", 0, 0xfffc));
	}

	size_t patch_bytes = 0;
	if (j.contains("cases")) {
		if (j.contains("registers") || j.contains("memory")) {
			throw std::invalid_argument(
			        "give either 'cases' or top-level "
			        "'registers'/'memory', not both");
		}
		const auto& cases = j["cases"];
		if (!cases.is_array() || cases.empty() || cases.size() > MaxCallCases) {
			throw std::invalid_argument(
			        "cases must be an array of 1.." +
			        std::to_string(MaxCallCases) + " cases");
		}
		for (size_t i = 0; i < cases.size(); ++i) {
			req.cases.push_back(
			        parse_case(cases[i],
				           "cases[" + std::to_string(i) + "]",
				           patch_bytes));
		}
	} else {
		json single = json::object();
		if (j.contains("registers")) {
			single["registers"] = j["registers"];
		}
		if (j.contains("memory")) {
			single["memory"] = j["memory"];
		}
		req.cases.push_back(parse_case(single, "request", patch_bytes));
	}

	if (j.contains("ranges")) {
		const auto& ranges = j["ranges"];
		if (!ranges.is_array() || ranges.size() > MaxCallRanges) {
			throw std::invalid_argument(
			        "ranges must be an array of at most " +
			        std::to_string(MaxCallRanges) + " ranges");
		}
		for (const auto& r : ranges) {
			req.ranges.push_back(parse_range(r));
		}
	}

	req.max_cycles = optional_int(j, "max_cycles", req.max_cycles, 1, MaxCallCycles);
	req.max_diff_bytes = optional_int(
	        j, "max_diff_bytes", req.max_diff_bytes, 0, MaxCallRangeBytes);
	req.timeout_ms = static_cast<int>(
	        optional_int(j, "timeout_ms", req.timeout_ms, 1, MaxCallTimeoutMs));
	if (j.contains("timers")) {
		if (!j["timers"].is_boolean()) {
			throw std::invalid_argument("timers must be a boolean");
		}
		req.timers = j["timers"].get<bool>();
	}

	const int64_t response_bound = static_cast<int64_t>(req.cases.size()) *
	                               static_cast<int64_t>(req.ranges.size()) *
	                               req.max_diff_bytes;
	if (response_bound > MaxCallResponseDiffBytes) {
		throw std::invalid_argument(
		        "cases x ranges x max_diff_bytes exceeds " +
		        std::to_string(MaxCallResponseDiffBytes) +
		        " bytes; lower max_diff_bytes or split the request");
	}
	return req;
}

json DebugCallResultToJson(const DebugCall::CaseResult& r)
{
	json out;
	out["returned"]    = r.returned;
	out["error"]       = r.error.empty() ? json(nullptr) : json(r.error);
	out["cycles"]      = r.cycles;
	out["timer_ticks"] = r.timer_ticks;
	json regs          = json::object();
	for (const auto& v : r.registers) {
		regs[std::string(v.name)] = v.value;
	}
	out["registers"] = regs;
	json ranges      = json::array();
	for (const auto& d : r.ranges) {
		json jr;
		jr["space"] = d.range.space == DebugCall::Space::Ram ? "ram" : "vram";
		jr["start"]         = d.range.start;
		jr["end"]           = d.range.end;
		jr["changed_bytes"] = d.changed_bytes;
		jr["truncated"]     = d.truncated;
		json diffs          = json::array();
		for (const auto& diff : d.diffs) {
			diffs.push_back({
			        {"address", diff.address},
			        {"data",
				 base64::to_base64(std::string(diff.bytes.begin(),
				 diff.bytes.end()))}
                        });
		}
		jr["diffs"] = diffs;
		ranges.push_back(jr);
	}
	out["ranges"] = ranges;
	json ports    = json::array();
	for (const auto& p : r.ports) {
		ports.push_back({
		        {  "port",   p.port},
                        { "reads",  p.reads},
                        {"writes", p.writes}
                });
	}
	out["ports"]    = ports;
	json interrupts = json::array();
	for (const auto& i : r.interrupts) {
		interrupts.push_back({
		        {"number",                       i.number},
		        {    "ah",                           i.ah},
		        {  "kind", std::string(kind_name(i.kind))},
		        { "count",                        i.count}
                });
	}
	out["interrupts"] = interrupts;
	json handlers     = json::array();
	for (const auto& h : r.handlers) {
		handlers.push_back({
		        {"handler", h.description},
                        {     "ah",          h.ah},
                        {  "count",       h.count}
                });
	}
	out["handlers"]   = handlers;
	out["unrestored"] = r.unrestored;
	return out;
}

void DebugCallCommand::Execute()
{
	std::string err = {};
	results         = DebugCall::Run(request, err);
	if (err == "not_paused") {
		not_paused = true;
	} else if (!err.empty()) {
		error = err;
	}
}

void DebugCallCommand::Post(const Request& req, Response& res)
{
	DebugCallCommand cmd(ParseDebugCallRequest(json::parse(req.body)));
	// The routine runs inside the command, bounded by the request's own
	// timeout_ms; the extra second covers snapshot and restore.
	cmd.WaitForCompletion(static_cast<uint32_t>(cmd.request.timeout_ms) + 1000);
	if (!cmd.error.empty()) {
		throw std::invalid_argument(cmd.error);
	}
	json j;
	if (cmd.not_paused) {
		j["status"] = "not_paused";
		send_json(res, j);
		return;
	}
	j["status"]  = "ok";
	json results = json::array();
	for (const auto& r : cmd.results) {
		results.push_back(DebugCallResultToJson(r));
	}
	j["results"] = results;
	send_json(res, j);
}

#else // !C_DEBUGGER

void DebugCallCommand::Execute() {}
void DebugCallCommand::Post(const Request&, Response& res)
{
	res.status = httplib::StatusCode::NotImplemented_501;
	json j;
	j["error"] = "debug_call: debugger capability not built in this binary";
	send_json(res, j);
}

#endif

} // namespace Webserver
