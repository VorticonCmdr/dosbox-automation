// SPDX-FileCopyrightText:  2025-2025 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_XMS_H
#define DOSBOX_XMS_H

#include <cstdint>
#include <vector>

#include "config/setup.h"

void XMS_Init(SectionProp& section);
void XMS_Destroy();

struct XmsStatus {
	bool enabled             = false;
	uint32_t total_kb        = 0;
	uint32_t largest_free_kb = 0;

	bool a20_enabled           = false;
	uint32_t a20_times_enabled = 0;

	bool hma_available       = false;
	bool hma_dos_has_control = false;
	bool hma_app_has_control = false;

	bool umb_available = false;
};

struct XmsHandleInfo {
	uint16_t handle    = 0;
	uint32_t size_kb   = 0;
	uint8_t lock_count = 0;
};

// Read-only introspection for the REST API (src/webserver/dos.cpp) -
// never called from XMS's own INT 2Fh/callback handling, which reads
// the file-local a20/hma/umb/xms state directly.
XmsStatus XMS_GetStatus();
std::vector<XmsHandleInfo> XMS_GetHandles();

#endif // DOSBOX_XMS_H
