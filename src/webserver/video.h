// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_VIDEO_H
#define DOSBOX_WEBSERVER_VIDEO_H

#include "libs/http/http.h"

#include "misc/rendered_image.h"
#include "webserver/bridge.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Webserver {

// ETag helpers shared by video/frame, video/frame/info and video/text.
// Exposed for testing.
std::string FormatEtag(uint64_t hash);
bool EtagMatches(std::string_view raw_if_none_match, uint64_t hash);

// Returns a read-only view of `image` cropped to the sub-rectangle
// (x, y, w, h), given in the image's own pixel coordinates. Shares the
// same allocation, pitch and pixel format as `image` - only image_data
// (rebased) and params.width/height (narrowed) differ; no pixel data
// is copied. Throws std::invalid_argument if the rectangle does not
// fit inside the image: crop is rejected, not clamped, so a caller's
// off-by-one is loud rather than silently reinterpreted.
//
// The returned RenderedImage must never itself be free()d - it does
// not own the allocation it points into; only the original does.
// Exposed for testing.
RenderedImage CropView(const RenderedImage& image, int x, int y, int w, int h);

// A downscaled (or, at divisor <= 1, untouched) copy of `image`'s pixels
// in tightly-packed RGB888, width/height pixels each. Exposed for
// testing.
struct Rgb888Buffer {
	std::vector<uint8_t> pixels;
	int width  = 0;
	int height = 0;
};

// Downscales `image` by an integer divisor with a box filter (each
// divisor x divisor block of source pixels averages into one output
// pixel), applied to the RGB888 buffer rather than the native pixel
// format - one implementation regardless of video mode. divisor <= 1
// is a no-op copy. Throws std::invalid_argument if divisor doesn't
// divide into at least one output pixel. Exposed for testing.
Rgb888Buffer prepare_rgb888(const RenderedImage& image, int divisor);

// Serializes `image` to this route's raw wire format: a small fixed
// header (width, height, pitch, pixel_format, palette entry count),
// the palette if paletted, then pixel data copied row by row using
// image.pitch as the stride between source rows - safe against a
// CropView() result, whose pitch is still the original (wider) row
// stride. The reported 'pitch' in the header is always tightly packed
// (width * bytes-per-pixel), which may differ from image.pitch itself.
// Exposed for testing.
std::string EncodeRaw(const RenderedImage& image);

struct VideoHandlers {
	static void GetFrame(const httplib::Request& req, httplib::Response& res);
	static void GetFrameInfo(const httplib::Request& req, httplib::Response& res);
};

// Text-mode character buffer read (emulation thread via Bridge).
class ScreenTextCommand : public Command {
	bool is_text_mode    = false;
	int columns          = 0;
	int rows             = 0;
	int page             = 0;
	int bios_mode        = 0;
	int cursor_row       = 0;
	int cursor_col       = 0;
	std::string text_dos = {};

public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_VIDEO_H
