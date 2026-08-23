// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_CAPTURE_H
#define DOSBOX_WEBSERVER_CAPTURE_H

#include "libs/http/http.h"
#include "webserver/bridge.h"

#include "capture/capture.h"

namespace Webserver {

class CaptureStartCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

	VideoCaptureMode mode = VideoCaptureMode::Raw;

	// -1 = leave the configured level for `mode` unchanged. Setting it
	// and starting are folded into one Command so a caller gets both
	// atomically in a single round trip, rather than a separate PUT
	// /capture/video/compression race against this POST.
	int compression_level = -1;

	// Set by Execute() instead of erroring when compression_level was
	// requested but a capture is already running (Post() maps this to
	// 409, same as CaptureCompressionSetCommand does for the standalone
	// route) - silently dropping an explicit request would be worse
	// than refusing it.
	bool rejected_recording_active = false;
};

class CaptureStopCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);
};

class CaptureStatusCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

	bool capturing                   = false;
	VideoCaptureMode mode            = VideoCaptureMode::Raw;
	VideoCaptureEndReason end_reason = VideoCaptureEndReason::NotEnded;

	// path/frames/elapsed_ms/bytes_written all describe the current (or,
	// after a stop, the most recently finished) recording - see
	// CAPTURE_GetVideoPath's doc comment for the retained-past-stop
	// semantics. path is empty and the rest are 0 if no video capture
	// has run yet this session.
	std_fs::path path      = {};
	uint32_t frames        = 0;
	int64_t elapsed_ms     = 0;
	uint32_t bytes_written = 0;

	// The zlib level configured for `mode` at query time. Accurate for a
	// running capture (the level is latched at start and PUT
	// /capture/video/compression is refused while recording), but only
	// a best-effort figure for a finished one if the configured level
	// was changed after it stopped.
	int compression_level = 0;
};

class CaptureCompressionGetCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

	int raw_level      = 0;
	int rendered_level = 0;
};

class CaptureCompressionSetCommand : public Command {
public:
	void Execute() override;
	static void Put(const httplib::Request& req, httplib::Response& res);

	// -1 = leave unchanged; filled with the applied values on success
	int raw_level                  = -1;
	int rendered_level             = -1;
	bool rejected_recording_active = false;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_CAPTURE_H
