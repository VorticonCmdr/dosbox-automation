// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_BRIDGE_H
#define DOSBOX_WEBSERVER_BRIDGE_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace Webserver {

// Thrown by Bridge::ExecuteCommand when the emulation thread does not
// pump the queue within the deadline. This is the *routine* failure -
// a paused or minimized emulator never calls ProcessRequests() - not a
// crash, so error_handler (webserver.cpp) maps it to a distinct,
// retryable response instead of a generic 500.
class BridgeTimeout : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class Command {
public:
	virtual ~Command() {}
	virtual void Execute() = 0;

	void WaitForCompletion(const uint32_t timeout_ms = 250);

	// Set by Execute() to report errors without throwing on the
	// emulation thread. Handlers should check this after
	// WaitForCompletion() and throw on the webserver thread.
	std::string error = {};

private:
	friend class Bridge;

	bool done = false;
};

class Bridge {
public:
	static Bridge& Instance();

	// Called by the web server thread
	void ExecuteCommand(Command& cmd, const uint32_t timeout_ms);

	// Called by the main thread running the CPU emulation
	void ProcessRequests();

private:
	std::mutex mtx              = {};
	std::condition_variable cv  = {};
	std::vector<Command*> queue = {};

	Bridge(const Bridge&)            = delete;
	Bridge& operator=(const Bridge&) = delete;
	Bridge()                         = default;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_BRIDGE_H
