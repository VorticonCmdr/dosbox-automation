// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "video.h"
#include "webserver.h"

#include "webserver/private/frame_tap.h"

#include "gui/render/render_shared.h"
#include "misc/rendered_image.h"
#include "misc/unicode.h"
#include "misc/video.h"

#include "hardware/memory.h"
#include "ints/int10.h"
#include "lua/lua_api.h"
#include "utils/fnv_hash.h"
#include "utils/string_utils.h"

#include "libs/json/json.h"

#include <jpeglib.h>
#include <png.h>

#include <csetjmp>
#include <cstring>
#include <optional>
#include <vector>

using json = nlohmann::json;

namespace Webserver {

// Frees a RenderedImage's deep-copied framebuffer on every exit from the
// scope it guards, including an exception thrown while encoding it.
// RenderedImage itself exposes free() rather than a destructor, since
// it is passed and copied by value elsewhere without always owning a
// deep copy; this file does own one, so it wraps it in RAII locally.
struct FrameGuard {
	RenderedImage& frame;
	~FrameGuard()
	{
		frame.free();
	}
};

static std::vector<uint8_t> convert_to_rgb888(const RenderedImage& image)
{
	const auto w = image.params.width;
	const auto h = image.params.height;
	std::vector<uint8_t> rgb(w * h * 3);

	for (int y = 0; y < h; y++) {
		const auto* src_row = image.image_data + y * image.pitch;
		auto* dst_row       = rgb.data() + y * w * 3;

		switch (image.params.pixel_format) {
		case PixelFormat::Indexed8:
			for (int x = 0; x < w; x++) {
				const auto idx     = src_row[x];
				dst_row[x * 3 + 0] = image.palette[idx].red;
				dst_row[x * 3 + 1] = image.palette[idx].green;
				dst_row[x * 3 + 2] = image.palette[idx].blue;
			}
			break;
		case PixelFormat::BGRX32_ByteArray:
			for (int x = 0; x < w; x++) {
				dst_row[x * 3 + 0] = src_row[x * 4 + 2]; // R
				dst_row[x * 3 + 1] = src_row[x * 4 + 1]; // G
				dst_row[x * 3 + 2] = src_row[x * 4 + 0]; // B
			}
			break;
		case PixelFormat::BGR24_ByteArray:
			for (int x = 0; x < w; x++) {
				dst_row[x * 3 + 0] = src_row[x * 3 + 2]; // R
				dst_row[x * 3 + 1] = src_row[x * 3 + 1]; // G
				dst_row[x * 3 + 2] = src_row[x * 3 + 0]; // B
			}
			break;
		case PixelFormat::RGB555_Packed16:
			for (int x = 0; x < w; x++) {
				const auto px = reinterpret_cast<const uint16_t*>(
				        src_row)[x];
				dst_row[x * 3 + 0] = static_cast<uint8_t>(
				        ((px >> 10) & 0x1F) * 255 / 31);
				dst_row[x * 3 + 1] = static_cast<uint8_t>(
				        ((px >> 5) & 0x1F) * 255 / 31);
				dst_row[x * 3 + 2] = static_cast<uint8_t>(
				        (px & 0x1F) * 255 / 31);
			}
			break;
		case PixelFormat::RGB565_Packed16:
			for (int x = 0; x < w; x++) {
				const auto px = reinterpret_cast<const uint16_t*>(
				        src_row)[x];
				dst_row[x * 3 + 0] = static_cast<uint8_t>(
				        ((px >> 11) & 0x1F) * 255 / 31);
				dst_row[x * 3 + 1] = static_cast<uint8_t>(
				        ((px >> 5) & 0x3F) * 255 / 63);
				dst_row[x * 3 + 2] = static_cast<uint8_t>(
				        (px & 0x1F) * 255 / 31);
			}
			break;
		default: std::memset(dst_row, 0, w * 3); break;
		}
	}
	return rgb;
}

RenderedImage CropView(const RenderedImage& image, const int x, const int y,
                       const int w, const int h)
{
	const auto frame_w = image.params.width;
	const auto frame_h = image.params.height;

	if (w < 1 || h < 1) {
		throw std::invalid_argument("crop_w and crop_h must be at least 1");
	}
	if (x < 0 || y < 0 || x > frame_w - w || y > frame_h - h) {
		throw std::invalid_argument(
		        "crop rectangle (" + std::to_string(x) + "," +
		        std::to_string(y) + "," + std::to_string(w) + "," +
		        std::to_string(h) + ") does not fit inside the " +
		        std::to_string(frame_w) + "x" +
		        std::to_string(frame_h) + " frame");
	}

	// Sharing image_data/pitch/palette rather than deep-copying: every
	// row/column this view can ever address is a strict subset of what
	// the bounds check above already proved lies inside the original
	// allocation, so no new bounds reasoning is needed for the pointer
	// rebase below.
	RenderedImage view         = image;
	const auto bytes_per_pixel = static_cast<int>(
	        (get_bits_per_pixel(image.params.pixel_format) + 7) / 8);
	view.image_data += static_cast<ptrdiff_t>(y) * image.pitch +
	                   static_cast<ptrdiff_t>(x) * bytes_per_pixel;
	view.params.width  = w;
	view.params.height = h;
	return view;
}

// Downscales `image` by an integer divisor with a box filter (each
// divisor x divisor block of source pixels averages into one output
// pixel), applied to the RGB888 buffer rather than the native pixel
// format - one implementation regardless of video mode. divisor <= 1
// is a no-op copy.
Rgb888Buffer prepare_rgb888(const RenderedImage& image, const int divisor)
{
	auto rgb     = convert_to_rgb888(image);
	const auto w = image.params.width;
	const auto h = image.params.height;

	if (divisor <= 1) {
		return Rgb888Buffer{std::move(rgb), w, h};
	}

	const auto out_w = w / divisor;
	const auto out_h = h / divisor;
	if (out_w < 1 || out_h < 1) {
		throw std::invalid_argument("scale is too large for this frame's size");
	}

	Rgb888Buffer out;
	out.width  = out_w;
	out.height = out_h;
	out.pixels.resize(static_cast<size_t>(out_w) * out_h * 3);

	for (int y = 0; y < out_h; y++) {
		auto* dst_row = out.pixels.data() +
		                static_cast<size_t>(y) * out_w * 3;
		for (int x = 0; x < out_w; x++) {
			uint32_t sum[3] = {0, 0, 0};
			for (int dy = 0; dy < divisor; dy++) {
				const auto* src_row = rgb.data() +
				                      static_cast<size_t>(
				                              y * divisor + dy) *
				                              w * 3;
				for (int dx = 0; dx < divisor; dx++) {
					const auto* px = src_row +
					                 static_cast<size_t>(
					                         x * divisor + dx) *
					                         3;
					sum[0] += px[0];
					sum[1] += px[1];
					sum[2] += px[2];
				}
			}
			const auto n = static_cast<uint32_t>(divisor * divisor);
			auto* dst    = dst_row + x * 3;
			dst[0]       = static_cast<uint8_t>(sum[0] / n);
			dst[1]       = static_cast<uint8_t>(sum[1] / n);
			dst[2]       = static_cast<uint8_t>(sum[2] / n);
		}
	}
	return out;
}

namespace {
struct JpegErrorMgr {
	struct jpeg_error_mgr pub  = {};
	std::jmp_buf setjmp_buffer = {};
};
} // namespace

// libjpeg's default error_exit prints a message and calls exit(),
// terminating the whole process on any fatal libjpeg error - reachable
// from a single malformed request. Longjmp back into encode_jpeg
// instead so it becomes an ordinary C++ exception.
static void jpeg_error_exit(j_common_ptr cinfo)
{
	auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
	std::longjmp(err->setjmp_buffer, 1);
}

static std::string encode_jpeg(Rgb888Buffer rgb, int quality = 98)
{
	const auto w = rgb.width;
	const auto h = rgb.height;

	struct jpeg_compress_struct cinfo = {};
	JpegErrorMgr jerr                 = {};
	cinfo.err                         = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit               = jpeg_error_exit;

	// Declared (and zero-initialised) before setjmp so the error branch
	// below can always safely free() them - free(nullptr) is a no-op -
	// regardless of how far encoding got before failing.
	unsigned char* buf     = nullptr;
	unsigned long buf_size = 0;

	// setjmp/longjmp is confined to this function's single scope: rgb
	// (the only local with a non-trivial destructor) is constructed
	// above this point (by the caller, moved in as a parameter) and
	// destroyed by encode_jpeg's normal return or unwind, never skipped
	// by the jump target below.
	if (setjmp(jerr.setjmp_buffer)) {
		jpeg_destroy_compress(&cinfo);
		free(buf);
		throw std::runtime_error("JPEG encoding failed");
	}

	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, &buf, &buf_size);

	cinfo.image_width      = w;
	cinfo.image_height     = h;
	cinfo.input_components = 3;
	cinfo.in_color_space   = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	while (cinfo.next_scanline < cinfo.image_height) {
		auto* row = rgb.pixels.data() + cinfo.next_scanline * w * 3;
		jpeg_write_scanlines(&cinfo, &row, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	std::string result(reinterpret_cast<char*>(buf), buf_size);
	free(buf);
	return result;
}

static void write_png_to_buffer(png_structp png_ptr, png_bytep data, png_size_t length)
{
	auto* out = static_cast<std::string*>(png_get_io_ptr(png_ptr));
	out->append(reinterpret_cast<char*>(data), length);
}

static std::string encode_png(Rgb888Buffer rgb, int level = 6)
{
	const auto w = rgb.width;
	const auto h = rgb.height;

	std::string result;

	auto* png_ptr  = png_create_write_struct(PNG_LIBPNG_VER_STRING,
	                                         nullptr,
	                                         nullptr,
	                                         nullptr);
	auto* info_ptr = png_create_info_struct(png_ptr);

	png_set_write_fn(png_ptr, &result, write_png_to_buffer, nullptr);

	png_set_IHDR(png_ptr,
	             info_ptr,
	             w,
	             h,
	             8,
	             PNG_COLOR_TYPE_RGB,
	             PNG_INTERLACE_NONE,
	             PNG_COMPRESSION_TYPE_DEFAULT,
	             PNG_FILTER_TYPE_DEFAULT);

	png_set_compression_level(png_ptr, level);
	png_write_info(png_ptr, info_ptr);

	for (int y = 0; y < h; y++) {
		auto* row = rgb.pixels.data() + y * w * 3;
		png_write_row(png_ptr, row);
	}

	png_write_end(png_ptr, nullptr);
	png_destroy_write_struct(&png_ptr, &info_ptr);

	return result;
}

std::string EncodeRaw(const RenderedImage& image)
{
	const auto w      = static_cast<uint32_t>(image.params.width);
	const auto h      = static_cast<uint32_t>(image.params.height);
	const auto pf     = static_cast<uint8_t>(image.params.pixel_format);
	const auto is_pal = image.is_paletted();
	const uint16_t pal_count = is_pal ? 256 : 0;

	// Always tightly packed (width * bytes_per_pixel, no source
	// padding), and always the row count actually copied below -
	// distinct from image.pitch, which is the source's own row
	// stride and, for a horizontally cropped view (CropView), wider
	// than what a single row of this output actually needs.
	const auto bytes_per_pixel = static_cast<uint32_t>(
	        (get_bits_per_pixel(image.params.pixel_format) + 7) / 8);
	const auto row_bytes = w * bytes_per_pixel;
	const auto pitch     = static_cast<int32_t>(row_bytes);

	const auto data_size    = static_cast<size_t>(h) * row_bytes;
	const auto header_size  = sizeof(w) + sizeof(h) + sizeof(pitch) +
	                          sizeof(pf) + sizeof(pal_count);
	const auto palette_size = static_cast<size_t>(pal_count * 3);

	std::string result;
	result.resize(header_size + palette_size + data_size);
	auto* p = result.data();

	std::memcpy(p, &w, sizeof(w));
	p += sizeof(w);
	std::memcpy(p, &h, sizeof(h));
	p += sizeof(h);
	std::memcpy(p, &pitch, sizeof(pitch));
	p += sizeof(pitch);
	std::memcpy(p, &pf, sizeof(pf));
	p += sizeof(pf);
	std::memcpy(p, &pal_count, sizeof(pal_count));
	p += sizeof(pal_count);

	if (is_pal) {
		for (int i = 0; i < pal_count; i++) {
			*p++ = image.palette[i].red;
			*p++ = image.palette[i].green;
			*p++ = image.palette[i].blue;
		}
	}

	// Row by row, using the source's own pitch as the stride between
	// rows: a cropped view's pitch is still the original (wider) row
	// stride, so a single contiguous copy would pull in the next
	// row's leading bytes instead of stopping at row_bytes.
	for (uint32_t y = 0; y < h; y++) {
		const auto* src_row = image.image_data +
		                      static_cast<ptrdiff_t>(y) * image.pitch;
		std::memcpy(p, src_row, row_bytes);
		p += row_bytes;
	}

	return result;
}

static const char* pixel_format_name(PixelFormat pf)
{
	switch (pf) {
	case PixelFormat::Indexed8: return "Indexed8";
	case PixelFormat::RGB555_Packed16: return "RGB555_Packed16";
	case PixelFormat::RGB565_Packed16: return "RGB565_Packed16";
	case PixelFormat::BGR24_ByteArray: return "BGR24_ByteArray";
	case PixelFormat::BGRX32_ByteArray: return "BGRX32_ByteArray";
	default: return "Unknown";
	}
}

// --- ETags (video/frame, video/frame/info, video/text) ---

std::string FormatEtag(const uint64_t hash)
{
	return format_str("%016llx", static_cast<unsigned long long>(hash));
}

bool EtagMatches(std::string_view raw_if_none_match, const uint64_t hash)
{
	if (raw_if_none_match.empty()) {
		return false;
	}
	// RFC 7232 requires ETags to be quoted; accepted unquoted too,
	// matching the If-Match precedent already in
	// WriteMemoryCommand::Put (memory.cpp).
	if (raw_if_none_match.size() >= 2 && raw_if_none_match.starts_with('"') &&
	    raw_if_none_match.ends_with('"')) {
		raw_if_none_match.remove_prefix(1);
		raw_if_none_match.remove_suffix(1);
	}
	return raw_if_none_match == FormatEtag(hash);
}

static std::string quote_etag(const uint64_t hash)
{
	return "\"" + FormatEtag(hash) + "\"";
}

static std::string get_if_none_match(const httplib::Request& req)
{
	if (!req.has_header("If-None-Match")) {
		return {};
	}
	return std::string(req.get_header_value("If-None-Match"));
}

struct AcquiredFrame {
	RenderedImage image = {};
	uint64_t hash       = 0;
};

// Acquires the frame matching req's 'mode', or returns nullopt having
// already written the full response itself: a 503 when no frame is
// available, or a 304 when If-None-Match already matches.
//
// For the default (raw/shared) source the hash is a pre-computed atomic
// (RENDER_GetSharedFrameHash(), set by RENDER_UpdateSharedFrame on the
// emulation thread) checked *before* RENDER_GetSharedFrame()'s deep
// copy, so a match skips that copy entirely rather than paying for it
// only to discard the result. mode=rendered has no such precomputed
// hash - the frame has to be captured and hashed before it's known
// whether anything changed, so a 304 there still pays the up-to-2s
// forced-present wait; it only saves the encode and transfer.
static std::optional<AcquiredFrame> AcquireFrame(const httplib::Request& req,
                                                 httplib::Response& res)
{
	const auto source = ParseFrameSource(req.get_param_value("mode"));
	const auto if_none_match = get_if_none_match(req);

	if (source == FrameSource::Rendered) {
		// Long enough for the forced present to come around even at
		// low frame rates; a paused or minimized emulator times out.
		constexpr auto RenderedFrameTimeout = std::chrono::milliseconds(2000);

		auto rendered = GetRenderedFrameTap().RequestAndWait(
		        RenderedFrameTimeout);
		if (!rendered) {
			res.status = 503;
			json err;
			err["error"] = "No rendered frame available (nothing is being presented)";
			send_json(res, err);
			return std::nullopt;
		}

		AcquiredFrame result;
		result.image = *rendered;
		const auto data_size = static_cast<size_t>(
		                               std::abs(result.image.pitch)) *
		                       static_cast<size_t>(result.image.params.height);
		result.hash = Fnv1aHash(result.image.image_data, data_size);

		if (EtagMatches(if_none_match, result.hash)) {
			result.image.free();
			res.status = httplib::StatusCode::NotModified_304;
			res.set_header("ETag", quote_etag(result.hash));
			return std::nullopt;
		}
		return result;
	}

	if (!RENDER_HasSharedFrame()) {
		res.status = 503;
		json err;
		err["error"] = "No frame available yet";
		send_json(res, err);
		return std::nullopt;
	}

	const auto hash = RENDER_GetSharedFrameHash();
	if (EtagMatches(if_none_match, hash)) {
		res.status = httplib::StatusCode::NotModified_304;
		res.set_header("ETag", quote_etag(hash));
		return std::nullopt;
	}

	AcquiredFrame result;
	result.hash  = hash;
	result.image = RENDER_GetSharedFrame();
	return result;
}

void VideoHandlers::GetFrame(const httplib::Request& req, httplib::Response& res)
{
	// Parse every parameter that can throw before acquiring the frame:
	// acquisition below deep-copies the framebuffer, and RenderedImage
	// frees through an explicit free() call, not a destructor, so an
	// exception between acquiring and that call would otherwise leak it
	// (num_param() on a malformed 'quality' being the likely case, and
	// the most likely request to carry a typo given how often this
	// route is polled). Validated regardless of whether the request
	// ends up a 304: a malformed parameter is the caller's mistake
	// either way.
	auto format = req.get_param_value("format");

	if (format.empty()) {
		const auto accept = req.get_header_value("Accept");
		if (accept.find("image/png") != std::string::npos) {
			format = "png";
		} else if (accept.find("application/octet-stream") !=
		           std::string::npos) {
			format = "raw";
		}
	}

	int quality = 98;
	if (format != "png" && format != "raw" && req.has_param("quality")) {
		quality = num_param<int>(req, Source::Param, "quality", 1, 100);
	}

	// A separate knob from 'quality': PNG is lossless regardless of
	// level, this only trades encode time for size, so conflating it
	// with jpeg's visual-fidelity 'quality' would be misleading.
	int png_level = 6;
	if (format == "png" && req.has_param("png_level")) {
		png_level = num_param<int>(req, Source::Param, "png_level", 0, 9);
	}

	int scale = 1;
	if (req.has_param("scale")) {
		scale = num_param<int>(req, Source::Param, "scale", 1, 8);
		if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
			throw std::invalid_argument("scale must be 1, 2, 4, or 8");
		}
		// scale=1 is a no-op regardless of format (prepare_rgb888 never
		// runs for format=raw either way) - only a real scale request
		// is incompatible with raw's native, unconverted pixel data.
		if (scale != 1 && format == "raw") {
			throw std::invalid_argument(
			        "scale is not supported for format=raw");
		}
	}

	const bool has_crop = req.has_param("crop_x") || req.has_param("crop_y") ||
	                      req.has_param("crop_w") || req.has_param("crop_h");
	int crop_x = 0;
	int crop_y = 0;
	int crop_w = 0;
	int crop_h = 0;
	if (has_crop) {
		if (!(req.has_param("crop_x") && req.has_param("crop_y") &&
		      req.has_param("crop_w") && req.has_param("crop_h"))) {
			throw std::invalid_argument(
			        "crop_x, crop_y, crop_w and crop_h must all be "
			        "given together");
		}
		// A generous, format-independent sanity ceiling - the real
		// bound (the acquired frame's own width/height) isn't known
		// until after acquisition below, and is enforced there by
		// CropView.
		crop_x = num_param<int>(req, Source::Param, "crop_x", 0, 65535);
		crop_y = num_param<int>(req, Source::Param, "crop_y", 0, 65535);
		crop_w = num_param<int>(req, Source::Param, "crop_w", 1, 65535);
		crop_h = num_param<int>(req, Source::Param, "crop_h", 1, 65535);
	}

	auto acquired = AcquireFrame(req, res);
	if (!acquired) {
		return;
	}

	FrameGuard guard{acquired->image}; // owns and frees the real allocation
	res.set_header("ETag", quote_etag(acquired->hash));

	// `view` may alias `acquired->image` directly (no crop) or share
	// its allocation through a narrower window (CropView, crop given) -
	// either way it must never itself be free()d; `guard` above owns
	// the real allocation regardless of which view is encoded below.
	const RenderedImage view =
	        has_crop ? CropView(acquired->image, crop_x, crop_y, crop_w, crop_h)
	                 : acquired->image;

	if (format == "png") {
		auto data = encode_png(prepare_rgb888(view, scale), png_level);
		res.set_content(std::move(data), "image/png");
	} else if (format == "raw") {
		auto data = EncodeRaw(view);
		res.set_content(std::move(data), "application/octet-stream");
	} else {
		auto data = encode_jpeg(prepare_rgb888(view, scale), quality);
		res.set_content(std::move(data), "image/jpeg");
	}
}

void VideoHandlers::GetFrameInfo(const httplib::Request& req, httplib::Response& res)
{
	auto acquired = AcquireFrame(req, res);
	if (!acquired) {
		return;
	}

	FrameGuard guard{acquired->image};
	res.set_header("ETag", quote_etag(acquired->hash));
	const auto& frame = acquired->image;

	const auto& vm = frame.params.video_mode;

	json j;
	j["width"]        = frame.params.width;
	j["height"]       = frame.params.height;
	j["pixel_format"] = pixel_format_name(frame.params.pixel_format);
	j["pitch"]        = frame.pitch;
	j["is_paletted"]  = frame.is_paletted();
	j["frame_hash"]   = FormatEtag(acquired->hash);

	j["video_mode"]["width"]             = vm.width;
	j["video_mode"]["height"]            = vm.height;
	j["video_mode"]["is_graphics_mode"]  = vm.is_graphics_mode;
	j["video_mode"]["is_double_scanned"] = vm.is_double_scanned_mode;
	j["video_mode"]["graphics_standard"] = to_string(vm.graphics_standard);
	j["video_mode"]["color_depth"]       = to_string(vm.color_depth);
	j["video_mode"]["bios_mode_number"]  = vm.bios_mode_number;

	j["rendered_double_scan"] = frame.params.rendered_double_scan;
	j["double_width"]         = frame.params.double_width;
	j["double_height"]        = frame.params.double_height;

	send_json(res, j);
}

void ScreenTextCommand::Execute()
{
	bios_mode    = CurMode->mode;
	is_text_mode = INT10_IsTextMode(*CurMode);

	// Cursor position is a BIOS-level concept independent of the current
	// video mode, and moves independently of the character grid - read
	// it unconditionally so it's available even when text_dos otherwise
	// stays empty below.
	page       = real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE);
	cursor_row = CURSOR_POS_ROW(static_cast<uint8_t>(page));
	cursor_col = CURSOR_POS_COL(static_cast<uint8_t>(page));

	if (!is_text_mode) {
		return;
	}

	// Same live BIOS-data-area reads ReadScreenText uses, so the
	// reported geometry matches the buffer it actually walked.
	columns  = real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS);
	rows     = CurMode->theight;
	text_dos = Lua::ReadScreenText();
}

void ScreenTextCommand::Get(const httplib::Request& req, httplib::Response& res)
{
	ScreenTextCommand cmd;
	cmd.WaitForCompletion();

	// Hashed on the web thread over the raw CP437 bytes, before UTF-8
	// conversion: cheap (a screen's worth of text), and keeps the hash
	// stable regardless of how the conversion step evolves.
	const auto hash = Fnv1aHash(cmd.text_dos);
	res.set_header("ETag", quote_etag(hash));

	if (EtagMatches(get_if_none_match(req), hash)) {
		res.status = httplib::StatusCode::NotModified_304;
		return;
	}

	json j;
	j["is_text_mode"] = cmd.is_text_mode;
	j["bios_mode"]    = cmd.bios_mode;
	j["columns"]      = cmd.columns;
	j["rows"]         = cmd.rows;
	j["page"]         = cmd.page;
	j["cursor_row"]   = cmd.cursor_row;
	j["cursor_col"]   = cmd.cursor_col;
	j["text_hash"]    = FormatEtag(hash);

	// CP437 screen codes to UTF-8 so box-drawing and shade glyphs survive
	// the JSON round-trip instead of being dropped as invalid bytes.
	// WithControlCodes keeps the row-separating newlines as line breaks.
	j["text"] = dos_to_utf8(cmd.text_dos, DosStringConvertMode::WithControlCodes);

	send_json(res, j);
}

} // namespace Webserver
