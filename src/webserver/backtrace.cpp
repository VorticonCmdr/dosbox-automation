// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "private/backtrace.h"
#include "webserver.h"

#include "debugger/debugger_backtrace.h"

#include "json/json.h"

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

namespace {

std::string StopReasonToString(DebugBacktraceStopReason reason)
{
	switch (reason) {
	case DebugBacktraceStopReason::BpZero: return "bp_zero";
	case DebugBacktraceStopReason::BpNotIncreasing:
		return "bp_not_increasing";
	case DebugBacktraceStopReason::BpBelowStackPointer:
		return "bp_below_stack_pointer";
	case DebugBacktraceStopReason::BpOutOfRange: return "bp_out_of_range";
	case DebugBacktraceStopReason::StackReadFault:
		return "stack_read_fault";
	case DebugBacktraceStopReason::MaxFrames:
	default: return "max_frames";
	}
}

} // namespace

void BacktraceCommand::Execute()
{
	const auto backtrace = DEBUG_Backtrace(max_frames);
	stopped_reason       = StopReasonToString(backtrace.stop_reason);
	frames.reserve(backtrace.frames.size());
	for (const auto& f : backtrace.frames) {
		BacktraceFrame bf;
		bf.bp              = f.bp;
		bf.segment         = f.segment;
		bf.offset          = f.offset;
		bf.high_confidence = f.high_confidence;
		frames.push_back(bf);
	}
}

void BacktraceCommand::Get(const Request& req, Response& res)
{
	uint32_t max_frames = DefaultBacktraceFrames;
	if (req.has_param("max_frames")) {
		max_frames = num_param<uint32_t>(
		        req, Source::Param, "max_frames", 1, MaxBacktraceFrames);
	}

	BacktraceCommand cmd(max_frames);
	// Worst case matches DisassembleCommand's own bounded worst case
	// (up to MaxBacktraceFrames * 4 DasmI386 calls, the same order of
	// magnitude as MaxDisassembleCount single-call decodes) - same
	// raised deadline.
	cmd.WaitForCompletion(2000);

	json list = json::array();
	for (const auto& f : cmd.frames) {
		json jf;
		jf["bp"]         = f.bp;
		jf["segment"]    = f.segment;
		jf["offset"]     = f.offset;
		jf["confidence"] = f.high_confidence ? "high" : "low";
		list.push_back(jf);
	}

	json j;
	j["frames"]         = list;
	j["stopped_reason"] = cmd.stopped_reason;
	send_json(res, j);
}

} // namespace Webserver
