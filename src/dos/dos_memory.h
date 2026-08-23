// SPDX-FileCopyrightText:  2024-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_DOS_MEMORY_H
#define DOSBOX_DOS_MEMORY_H

#include <cstdint>

constexpr auto ConventionalMemorySizeKb = 640;
constexpr auto PcjrVideoMemorySizeKb    = 32;
constexpr auto PcjrStandardMemorySizeKb = 128;

// The fixed segment DOS_LinkUMBsToMemChain links the UMB MCB chain at.
// dos_infoblock.GetStartOfUMBChain() reads exactly this value when a
// chain is linked, or 0xffff when it isn't - nothing in this engine
// ever writes a third value there, so any other value means the
// guest-memory word backing it has been corrupted (by guest software,
// or a client's own mem_write) and the chain must not be trusted.
constexpr uint16_t UmbStartSegment = 0x9fff;

#endif // DOSBOX_DOS_MEMORY_H
