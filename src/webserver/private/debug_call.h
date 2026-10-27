// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_DEBUG_CALL_H
#define DOSBOX_WEBSERVER_DEBUG_CALL_H

#include "webserver/bridge.h"

#include <cstdint>
#include <string>
#include <vector>

#include "debugger/debugger_call.h"
#include "http/http.h"
#include "json/json.h"

namespace Webserver {

// Request caps. A call request makes the emulation thread run guest code
// while it holds the Bridge, so both the work and the response size have
// hard upper bounds.
constexpr size_t MaxCallCases              = 64;
constexpr size_t MaxCallPatchesPerCase     = 256;
constexpr size_t MaxCallPatchBytes         = 1 << 20; // whole request
constexpr size_t MaxCallRanges             = 16;
constexpr int64_t MaxCallRangeBytes        = 16 << 20; // one range
constexpr int64_t MaxCallResponseDiffBytes = 64 << 20; // cases x ranges x
                                                       // max_diff_bytes
constexpr int64_t MaxCallCycles            = 4'000'000'000;
constexpr int MaxCallTimeoutMs             = 60'000;

// Parses and validates POST /api/v1/debug/call. Throws
// std::invalid_argument on any malformed, out-of-range or oversized field.
// Addresses are only checked against the emulated machine's actual memory
// sizes on the emulation thread (DebugCall::Run).
DebugCall::Request ParseDebugCallRequest(const nlohmann::json& j);

nlohmann::json DebugCallResultToJson(const DebugCall::CaseResult& r);

class DebugCallCommand : public Command {
public:
	explicit DebugCallCommand(DebugCall::Request request)
	        : request(std::move(request))
	{}

	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

	std::vector<DebugCall::CaseResult> results = {};
	bool not_paused                            = false;

private:
	DebugCall::Request request = {};
};

} // namespace Webserver

#endif
