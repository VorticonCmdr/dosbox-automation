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

static std::string encode_jpeg(const RenderedImage& image, int quality = 98)
{
	auto rgb     = convert_to_rgb888(image);
	const auto w = image.params.width;
	const auto h = image.params.height;

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
	// above this point and destroyed by encode_jpeg's normal return or
	// unwind, never skipped by the jump target below.
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
		auto* row = rgb.data() + cinfo.next_scanline * w * 3;
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

static std::string encode_png(const RenderedImage& image)
{
	auto rgb     = convert_to_rgb888(image);
	const auto w = image.params.width;
	const auto h = image.params.height;

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

	png_set_compression_level(png_ptr, 1);
	png_write_info(png_ptr, info_ptr);

	for (int y = 0; y < h; y++) {
		auto* row = rgb.data() + y * w * 3;
		png_write_row(png_ptr, row);
	}

	png_write_end(png_ptr, nullptr);
	png_destroy_write_struct(&png_ptr, &info_ptr);

	return result;
}

static std::string encode_raw(const RenderedImage& image)
{
	const auto w      = static_cast<uint32_t>(image.params.width);
	const auto h      = static_cast<uint32_t>(image.params.height);
	const auto pitch  = static_cast<int32_t>(image.pitch);
	const auto pf     = static_cast<uint8_t>(image.params.pixel_format);
	const auto is_pal = image.is_paletted();
	const uint16_t pal_count = is_pal ? 256 : 0;

	const auto data_size    = static_cast<size_t>(h * std::abs(pitch));
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

	std::memcpy(p, image.image_data, data_size);

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
	// ends up a 304: a malformed quality is the caller's mistake either
	// way.
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

	auto acquired = AcquireFrame(req, res);
	if (!acquired) {
		return;
	}

	FrameGuard guard{acquired->image};
	res.set_header("ETag", quote_etag(acquired->hash));
	const auto& frame = acquired->image;

	if (format == "png") {
		auto data = encode_png(frame);
		res.set_content(std::move(data), "image/png");
	} else if (format == "raw") {
		auto data = encode_raw(frame);
		res.set_content(std::move(data), "application/octet-stream");
	} else {
		auto data = encode_jpeg(frame, quality);
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
