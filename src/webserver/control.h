// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_CONTROL_H
#define DOSBOX_WEBSERVER_CONTROL_H

#include "private/shutdown.h"

#include "http/http.h"

namespace Webserver {

struct ControlHandlers {
	static void GetProgramState(const httplib::Request& req,
	                            httplib::Response& res);
	static void GetStatus(const httplib::Request& req, httplib::Response& res);

	// Pre-auth protocol handshake (GET /api/v1/hello, see
	// IsPublicApiPath): a bridge can check version/protocol
	// compatibility before it has a token. Deliberately not a Command -
	// nothing here reads emulator state or crosses the Bridge, so
	// there's nothing to synchronize with the emulation thread over.
	static void GetHello(const httplib::Request& req, httplib::Response& res);
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_CONTROL_H
