// SPDX-FileCopyrightText: 2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_RENDER_SHARED_H
#define DOSBOX_RENDER_SHARED_H

#include <cstdint>

#include "misc/rendered_image.h"

void RENDER_UpdateSharedFrame(const RenderedImage& image);
RenderedImage RENDER_GetSharedFrame();
bool RENDER_HasSharedFrame();

// FNV-1a of the shared frame's pixel data, computed once in
// RENDER_UpdateSharedFrame (where the mutex and the source buffer are
// already held) rather than by every caller that only wants to know if
// the frame changed. An ETag for video/frame and video/frame/info.
uint64_t RENDER_GetSharedFrameHash();

#endif // DOSBOX_RENDER_SHARED_H
