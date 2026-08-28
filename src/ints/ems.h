// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_EMS_H
#define DOSBOX_EMS_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "config/setup.h"

constexpr uint16_t EmsPageSize = 16 * 1024;

extern const std::string EmsDeviceName;

void EMS_Init(SectionProp& section);
void EMS_Destroy();

struct EmsStatus {
	bool enabled         = false;
	uint16_t total_pages = 0;
	uint16_t free_pages  = 0;
};

struct EmsPageMapping {
	// Which of the EMM_MAX_PHYS (4) physical page-frame slots this is.
	uint8_t physical_page = 0;
	// The handle's own logical page currently mapped into that slot.
	uint16_t logical_page = 0;
};

struct EmsHandleInfo {
	uint16_t handle = 0;
	// The handle's raw 8-byte DOS name - not NUL-terminated, may hold
	// arbitrary bytes. Sanitizing this for display is a REST-response
	// formatting concern, not EMS domain logic - see dos.cpp's
	// SanitizeDosHandleName().
	std::array<char, 8> name                  = {};
	uint16_t pages                            = 0;
	std::vector<EmsPageMapping> page_mappings = {};
};

// Read-only introspection for the REST API (src/webserver/dos.cpp) -
// never called from EMS's own INT 67h handling, which reads
// emm_handles/emm_mappings directly.
EmsStatus EMS_GetStatus();
std::vector<EmsHandleInfo> EMS_GetHandles();

#endif // DOSBOX_EMS_H
