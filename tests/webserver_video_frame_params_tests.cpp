// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/video.h"

#include <gtest/gtest.h>

#include <cstring>

using Webserver::CropView;
using Webserver::EncodeRaw;
using Webserver::prepare_rgb888;

namespace {

// 4x4 BGR24 image, one distinct byte value per row (row 0 = 0x00, row 1
// = 0x11, ...) so which rows/columns a view reads is directly observable.
RenderedImage make_test_image()
{
	RenderedImage image = {};

	image.params.width        = 4;
	image.params.height       = 4;
	image.params.pixel_format = PixelFormat::BGR24_ByteArray;
	image.pitch               = 4 * 3;

	image.image_data = new uint8_t[4 * 4 * 3];
	for (int row = 0; row < 4; row++) {
		std::memset(image.image_data + row * image.pitch,
		            row * 0x11,
		            static_cast<size_t>(image.pitch));
	}
	return image;
}

// 4x4 BGRX32 image (4 bytes/pixel) - the pixel format the real default
// (mode=raw) capture path actually uses, unlike make_test_image()'s
// BGR24 - so CropView's bytes_per_pixel generalization is exercised
// against the format that matters most in practice.
RenderedImage make_bgrx32_image()
{
	RenderedImage image = {};

	image.params.width        = 4;
	image.params.height       = 4;
	image.params.pixel_format = PixelFormat::BGRX32_ByteArray;
	image.pitch               = 4 * 4;

	image.image_data = new uint8_t[4 * 4 * 4];
	for (int row = 0; row < 4; row++) {
		std::memset(image.image_data + row * image.pitch,
		            row * 0x11,
		            static_cast<size_t>(image.pitch));
	}
	return image;
}

// Same 4x4 layout as make_test_image(), but with pitch genuinely
// negative: row 0 (as reported by width/height) is stored LAST in
// memory, walked via pitch < 0 from a pointer to row 0's own start.
// Exercises CropView/EncodeRaw against the negative-pitch
// representation render.cpp documents, even though it isn't reachable
// from GetFrame's current call graph.
RenderedImage make_negative_pitch_image()
{
	RenderedImage image = {};

	image.params.width        = 4;
	image.params.height       = 4;
	image.params.pixel_format = PixelFormat::BGR24_ByteArray;

	const int row_bytes = 4 * 3;
	image.pitch         = -row_bytes;

	auto* buffer = new uint8_t[4 * row_bytes];
	// Buffer stores rows bottom-to-top: buffer[0] is display row 3,
	// buffer[3*row_bytes] is display row 0. image_data points at
	// display row 0's start, i.e. the buffer's last row.
	for (int stored_row = 0; stored_row < 4; stored_row++) {
		const int display_row = 3 - stored_row;
		std::memset(buffer + stored_row * row_bytes,
		            display_row * 0x11,
		            static_cast<size_t>(row_bytes));
	}
	image.image_data = buffer + 3 * row_bytes;
	return image;
}

// RenderedImage::free() would delete[] the wrong pointer for
// make_negative_pitch_image() (image_data doesn't point at the true
// new[] base) - free the real buffer this test itself allocated.
void free_negative_pitch_image(RenderedImage& image)
{
	delete[] (image.image_data + 3 * image.pitch);
	image.image_data = nullptr;
}

TEST(CropView, IdentityCropReturnsTheWholeImage)
{
	auto image = make_test_image();

	auto view = CropView(image, 0, 0, 4, 4);

	EXPECT_EQ(view.params.width, 4);
	EXPECT_EQ(view.params.height, 4);
	EXPECT_EQ(view.image_data, image.image_data);

	image.free();
}

TEST(CropView, NarrowsWidthAndHeight)
{
	auto image = make_test_image();

	auto view = CropView(image, 1, 1, 2, 2);

	EXPECT_EQ(view.params.width, 2);
	EXPECT_EQ(view.params.height, 2);

	image.free();
}

TEST(CropView, RebasesToTheRequestedTopLeftPixel)
{
	auto image = make_test_image();

	// Crop starting at row 2, column 1: row byte value 0x22, offset by
	// one BGR24 pixel (3 bytes) into that row.
	auto view = CropView(image, 1, 2, 2, 1);

	EXPECT_EQ(view.image_data[0], 0x22);
	EXPECT_EQ(view.image_data, image.image_data + 2 * image.pitch + 1 * 3);

	image.free();
}

TEST(CropView, PitchIsUnchangedFromTheSource)
{
	// The view still shares the source's row stride - only the pixel
	// window (width/height, base pointer) narrows, so a row-by-row
	// consumer can still step between rows correctly.
	auto image = make_test_image();

	auto view = CropView(image, 0, 0, 2, 2);

	EXPECT_EQ(view.pitch, image.pitch);

	image.free();
}

TEST(CropView, RejectsAZeroOrNegativeSize)
{
	auto image = make_test_image();

	EXPECT_THROW(CropView(image, 0, 0, 0, 2), std::invalid_argument);
	EXPECT_THROW(CropView(image, 0, 0, 2, 0), std::invalid_argument);
	EXPECT_THROW(CropView(image, 0, 0, -1, 2), std::invalid_argument);

	image.free();
}

TEST(CropView, RejectsANegativeOrigin)
{
	auto image = make_test_image();

	EXPECT_THROW(CropView(image, -1, 0, 2, 2), std::invalid_argument);
	EXPECT_THROW(CropView(image, 0, -1, 2, 2), std::invalid_argument);

	image.free();
}

TEST(CropView, RejectsARectangleThatExtendsPastTheRightEdge)
{
	auto image = make_test_image();

	EXPECT_THROW(CropView(image, 3, 0, 2, 1), std::invalid_argument);

	image.free();
}

TEST(CropView, RejectsARectangleThatExtendsPastTheBottomEdge)
{
	auto image = make_test_image();

	EXPECT_THROW(CropView(image, 0, 3, 1, 2), std::invalid_argument);

	image.free();
}

TEST(CropView, RejectsAWidthLargerThanTheWholeFrame)
{
	auto image = make_test_image();

	EXPECT_THROW(CropView(image, 0, 0, 5, 1), std::invalid_argument);

	image.free();
}

TEST(CropView, AcceptsARectangleExactlyAtTheBottomRightCorner)
{
	auto image = make_test_image();

	auto view = CropView(image, 2, 2, 2, 2);

	EXPECT_EQ(view.image_data, image.image_data + 2 * image.pitch + 2 * 3);
	EXPECT_EQ(view.image_data[0], 0x22);

	image.free();
}

TEST(CropView, RebasesCorrectlyForA4BytePerPixelFormat)
{
	// The real default (mode=raw) capture path returns BGRX32_ByteArray
	// (4 bytes/pixel), not the 3-byte BGR24 every other CropView test
	// above uses - this is the format that matters most in practice.
	auto image = make_bgrx32_image();

	auto view = CropView(image, 1, 2, 2, 1);

	EXPECT_EQ(view.image_data, image.image_data + 2 * image.pitch + 1 * 4);
	EXPECT_EQ(view.image_data[0], 0x22);
	EXPECT_EQ(view.params.width, 2);
	EXPECT_EQ(view.params.height, 1);

	image.free();
}

TEST(CropView, RebasesCorrectlyForANegativePitchSource)
{
	auto image = make_negative_pitch_image();

	// Row 2 (display order) starts at image_data + 2*pitch, which -
	// pitch being negative - is a LOWER address than image_data itself.
	auto view = CropView(image, 1, 2, 2, 1);

	EXPECT_EQ(view.image_data, image.image_data + 2 * image.pitch + 1 * 3);
	EXPECT_EQ(view.image_data[0], 0x22);
	EXPECT_EQ(view.pitch, image.pitch);

	free_negative_pitch_image(image);
}

TEST(PrepareRgb888, DivisorOneIsANoOpCopy)
{
	auto image = make_test_image();

	auto rgb = prepare_rgb888(image, 1);

	EXPECT_EQ(rgb.width, 4);
	EXPECT_EQ(rgb.height, 4);
	ASSERT_EQ(rgb.pixels.size(), 4u * 4 * 3);
	// Row 2's BGR24 source bytes (0x22) become R=G=B=0x22 in RGB888.
	EXPECT_EQ(rgb.pixels[2 * 4 * 3], 0x22);

	image.free();
}

TEST(PrepareRgb888, DivisorTwoAveragesEachTwoByTwoBlock)
{
	// A 4x4 BGR24 image where rows 0-1 are uniformly 0x10 and rows 2-3
	// are uniformly 0x30 - each 2x2 block is internally uniform, so its
	// average is exactly that block's own value, not a rounded blend of
	// two different row values.
	RenderedImage image       = {};
	image.params.width        = 4;
	image.params.height       = 4;
	image.params.pixel_format = PixelFormat::BGR24_ByteArray;
	image.pitch               = 4 * 3;
	image.image_data          = new uint8_t[4 * image.pitch];
	std::memset(image.image_data, 0x10, static_cast<size_t>(2 * image.pitch));
	std::memset(image.image_data + 2 * image.pitch,
	            0x30,
	            static_cast<size_t>(2 * image.pitch));

	auto rgb = prepare_rgb888(image, 2);

	EXPECT_EQ(rgb.width, 2);
	EXPECT_EQ(rgb.height, 2);
	ASSERT_EQ(rgb.pixels.size(), 2u * 2 * 3);
	EXPECT_EQ(rgb.pixels[0], 0x10); // top-left output pixel (rows 0-1)
	EXPECT_EQ(rgb.pixels[1 * 2 * 3], 0x30); // bottom-left output pixel
	                                        // (rows 2-3)

	image.free();
}

TEST(PrepareRgb888, RejectsADivisorLargerThanTheImage)
{
	auto image = make_test_image();

	EXPECT_THROW(prepare_rgb888(image, 8), std::invalid_argument);

	image.free();
}

TEST(PrepareRgb888, WorksOnACroppedView)
{
	// prepare_rgb888 reads image.params.width/height, which CropView
	// narrows - confirms scale composes with crop rather than silently
	// operating on the original, uncropped dimensions.
	auto image = make_test_image();
	auto view  = CropView(image, 0, 0, 2, 2);

	auto rgb = prepare_rgb888(view, 2);

	EXPECT_EQ(rgb.width, 1);
	EXPECT_EQ(rgb.height, 1);

	image.free();
}

TEST(EncodeRaw, HeaderReportsTightlyPackedDimensionsNotSourcePitch)
{
	// A padded source (pitch wider than width * bytes_per_pixel) -
	// EncodeRaw's header must report the tightly packed pitch
	// (width * bpp), not the source's own (wider) stride.
	RenderedImage image       = {};
	image.params.width        = 2;
	image.params.height       = 2;
	image.params.pixel_format = PixelFormat::BGR24_ByteArray;
	image.pitch               = 3 * 3; // one padding pixel per row
	image.image_data          = new uint8_t[2 * image.pitch];
	std::memset(image.image_data, 0, static_cast<size_t>(2 * image.pitch));

	auto raw = EncodeRaw(image);

	uint32_t width  = 0;
	uint32_t height = 0;
	int32_t pitch   = 0;
	std::memcpy(&width, raw.data(), sizeof(width));
	std::memcpy(&height, raw.data() + 4, sizeof(height));
	std::memcpy(&pitch, raw.data() + 8, sizeof(pitch));

	EXPECT_EQ(width, 2u);
	EXPECT_EQ(height, 2u);
	EXPECT_EQ(pitch, 2 * 3); // width * bytes_per_pixel, not image.pitch (9)

	image.free();
}

TEST(EncodeRaw, CopiesOnlyRowBytesPerRowNotTheSourcesWiderStride)
{
	// Same padded source as above, but with a distinguishable byte
	// pattern: row 0 = 0xAA repeated, row 1 = 0xBB repeated, including
	// the padding pixel a naive contiguous copy would also pull in.
	// The output must contain exactly 2*2*3 = 12 data bytes, none of
	// them the padding byte 0xBB bleeding into row 0's tail.
	RenderedImage image       = {};
	image.params.width        = 2;
	image.params.height       = 2;
	image.params.pixel_format = PixelFormat::BGR24_ByteArray;
	image.pitch               = 3 * 3;
	image.image_data          = new uint8_t[2 * image.pitch];
	std::memset(image.image_data, 0xAA, 3 * 3);
	std::memset(image.image_data + image.pitch, 0xBB, 3 * 3);

	auto raw = EncodeRaw(image);

	// header (15 bytes: w,h,pitch,pf,pal_count) + 2*2*3 data bytes
	ASSERT_EQ(raw.size(), 15u + 12u);
	const auto* data = reinterpret_cast<const uint8_t*>(raw.data()) + 15;
	for (int i = 0; i < 6; i++) {
		EXPECT_EQ(data[i], 0xAA) << "row 0, byte " << i;
	}
	for (int i = 0; i < 6; i++) {
		EXPECT_EQ(data[6 + i], 0xBB) << "row 1, byte " << i;
	}

	image.free();
}

TEST(EncodeRaw, WorksOnAHorizontallyCroppedView)
{
	// A horizontally narrowed CropView is exactly the case that would
	// break under the old single-contiguous-memcpy implementation:
	// image.pitch is still the ORIGINAL (wider) row stride here.
	auto image = make_test_image();
	auto view  = CropView(image, 1, 0, 2, 4);

	auto raw = EncodeRaw(view);

	uint32_t width = 0;
	std::memcpy(&width, raw.data(), sizeof(width));
	EXPECT_EQ(width, 2u);
	ASSERT_EQ(raw.size(), 15u + 2u * 4 * 3);

	image.free();
}

} // namespace
