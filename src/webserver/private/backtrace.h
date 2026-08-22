// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_BACKTRACE_H
#define DOSBOX_WEBSERVER_BACKTRACE_H

#include "webserver/bridge.h"

#include <cstdint>
#include <string>
#include <vector>

#include "http/http.h"

namespace Webserver {

// max_frames is capped here; each frame transition can cost up to
// debugger/debugger_backtrace.cpp's own 4 backward-decode probes, so the
// worst case per request (max_frames * 4 DasmI386 calls) stays bounded.
constexpr uint32_t MaxBacktraceFrames = 64;
// Used when the caller doesn't specify max_frames - deep chains are rare
// enough in practice that a full 64-frame walk on every call would be
// wasted work most of the time.
constexpr uint32_t DefaultBacktraceFrames = 16;

struct BacktraceFrame {
	uint32_t bp          = 0;
	uint16_t segment     = 0;
	uint32_t offset      = 0;
	bool high_confidence = false;
};

// Call-stack walking, unconditionally available - same reasoning as
// DisassembleCommand (2.5): the underlying walk (debugger/
// debugger_backtrace.h) doesn't need C_DEBUGGER either. See that header
// for the algorithm and its honesty caveats - this is a best-effort,
// heuristic feature, not a guaranteed-accurate stack unwinder.
class BacktraceCommand : public Command {
public:
	explicit BacktraceCommand(uint32_t max_frames) : max_frames(max_frames)
	{}

	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response& res);

	std::vector<BacktraceFrame> frames = {};
	std::string stopped_reason         = {};

private:
	uint32_t max_frames = 0;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_BACKTRACE_H
