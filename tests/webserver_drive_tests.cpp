// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/drive.h"

#include <algorithm>
#include <fstream>
#include <random>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using namespace Webserver;

TEST(DriveDenyReasonCodeTest, MapsEveryReasonToAStableLowercaseCode)
{
	EXPECT_EQ(DriveDenyReasonCode(DenyReason::None), "none");
	EXPECT_EQ(DriveDenyReasonCode(DenyReason::DoesNotResolve), "does_not_resolve");
	EXPECT_EQ(DriveDenyReasonCode(DenyReason::NotRegularFile), "not_regular_file");
	EXPECT_EQ(DriveDenyReasonCode(DenyReason::SymlinkComponent),
	          "symlink_component");
	EXPECT_EQ(DriveDenyReasonCode(DenyReason::SystemPath), "system_path");
	EXPECT_EQ(DriveDenyReasonCode(DenyReason::OutsideWhitelist),
	          "outside_whitelist");
	EXPECT_EQ(DriveDenyReasonCode(DenyReason::NotADiskImage), "not_a_disk_image");
}

// -- ScanImageRoot --

class ScanImageRootTest : public testing::Test {
protected:
	fs::path tmp_dir = {};

	static fs::path MakeTempDir()
	{
		std::random_device rd = {};
		auto dist = std::uniform_int_distribution<uint64_t>();
		for (int attempt = 0; attempt < 16; ++attempt) {
			const auto name      = "scan_image_root_" +
			                       std::to_string(dist(rd));
			const auto candidate = fs::temp_directory_path() / name;
			std::error_code ec   = {};
			if (fs::create_directory(candidate, ec) && !ec) {
				fs::permissions(candidate, fs::perms::owner_all, ec);
				return candidate;
			}
		}
		return {};
	}

	void SetUp() override
	{
		// Canonicalized up front: ScanImageRoot's real caller
		// (MountHandlers::GetImages) only ever passes roots already
		// canonicalized once at config-parse time
		// (MountPolicy::ParsePathList) - a raw, uncanonicalized
		// temp_directory_path() can itself contain a symlink
		// component on some platforms (e.g. macOS's /tmp ->
		// /private/tmp), which would trip ScanImageRoot's own
		// symlink defenses for a reason having nothing to do with
		// the test.
		auto raw = MakeTempDir();
		ASSERT_FALSE(raw.empty());
		std::error_code ec;
		tmp_dir = fs::canonical(raw, ec);
		ASSERT_FALSE(ec);
	}

	void TearDown() override
	{
		if (!tmp_dir.empty() && fs::exists(tmp_dir)) {
			fs::remove_all(tmp_dir);
		}
	}

	void WriteFile(const std::string& name, size_t bytes)
	{
		std::ofstream f(tmp_dir / name, std::ios::binary);
		f << std::string(bytes, 'x');
	}
};

TEST_F(ScanImageRootTest, EmptyRootReturnsNoImages)
{
	const auto result = ScanImageRoot(tmp_dir, 100);
	EXPECT_TRUE(result.images.empty());
	EXPECT_FALSE(result.truncated);
}

TEST_F(ScanImageRootTest, NonexistentRootReturnsNoImagesNotAnError)
{
	const auto result = ScanImageRoot(tmp_dir / "does_not_exist", 100);
	EXPECT_TRUE(result.images.empty());
	EXPECT_FALSE(result.truncated);
}

TEST_F(ScanImageRootTest, ListsRegularFilesWithTheirSize)
{
	WriteFile("a.img", 1024);
	WriteFile("b.iso", 2048);

	const auto result = ScanImageRoot(tmp_dir, 100);

	ASSERT_EQ(result.images.size(), 2u);
	EXPECT_FALSE(result.truncated);

	auto sizes = std::vector<int64_t>();
	for (const auto& img : result.images) {
		sizes.push_back(img.size_bytes);
		EXPECT_EQ(fs::path(img.path).parent_path(), tmp_dir);
	}
	std::sort(sizes.begin(), sizes.end());
	EXPECT_EQ(sizes, (std::vector<int64_t>{1024, 2048}));
}

TEST_F(ScanImageRootTest, DoesNotRecurseIntoSubdirectories)
{
	std::error_code ec;
	fs::create_directory(tmp_dir / "subdir", ec);
	ASSERT_FALSE(ec);
	std::ofstream(tmp_dir / "subdir" / "nested.img") << "x";
	WriteFile("top.img", 4);

	const auto result = ScanImageRoot(tmp_dir, 100);

	ASSERT_EQ(result.images.size(), 1u);
	EXPECT_EQ(fs::path(result.images[0].path).filename(), "top.img");
}

TEST_F(ScanImageRootTest, SkipsSymlinksEvenWhenTheyResolveToARegularFile)
{
	WriteFile("real.img", 8);
	std::error_code ec;
	fs::create_symlink(tmp_dir / "real.img", tmp_dir / "link.img", ec);
	if (ec) {
		GTEST_SKIP() << "symlink creation not permitted in this environment";
	}

	const auto result = ScanImageRoot(tmp_dir, 100);

	ASSERT_EQ(result.images.size(), 1u);
	EXPECT_EQ(fs::path(result.images[0].path).filename(), "real.img");
}

TEST_F(ScanImageRootTest, ExactlyAtCapIsNotReportedTruncated)
{
	WriteFile("a.img", 1);
	WriteFile("b.img", 1);

	const auto result = ScanImageRoot(tmp_dir, 2);

	EXPECT_EQ(result.images.size(), 2u);
	EXPECT_FALSE(result.truncated);
}

TEST_F(ScanImageRootTest, OverCapStopsAtTheCapAndReportsTruncated)
{
	WriteFile("a.img", 1);
	WriteFile("b.img", 1);
	WriteFile("c.img", 1);

	const auto result = ScanImageRoot(tmp_dir, 2);

	EXPECT_EQ(result.images.size(), 2u);
	EXPECT_TRUE(result.truncated);
}

TEST_F(ScanImageRootTest, BoundsRawEntriesWalkedNotJustAcceptedImages)
{
	// Subdirectories never qualify as images (non-recursive, not a
	// regular file) - without a separate entries-walked bound, none
	// of them would ever touch the image cap, so a root full of them
	// would be scanned without limit.
	for (int i = 0; i < 5; ++i) {
		std::error_code ec;
		fs::create_directory(tmp_dir / ("subdir_" + std::to_string(i)), ec);
		ASSERT_FALSE(ec);
	}

	const auto result = ScanImageRoot(tmp_dir, 100, /*max_entries_scanned=*/3);

	EXPECT_TRUE(result.images.empty());
	EXPECT_TRUE(result.truncated);
}

TEST_F(ScanImageRootTest, ExactlyAtMaxEntriesScannedIsNotReportedTruncated)
{
	for (int i = 0; i < 3; ++i) {
		std::error_code ec;
		fs::create_directory(tmp_dir / ("subdir_" + std::to_string(i)), ec);
		ASSERT_FALSE(ec);
	}

	const auto result = ScanImageRoot(tmp_dir, 100, /*max_entries_scanned=*/3);

	EXPECT_TRUE(result.images.empty());
	EXPECT_FALSE(result.truncated);
}

TEST_F(ScanImageRootTest, DefaultEntriesScannedBoundDoesNotFalselyTruncateAnOrdinaryRoot)
{
	WriteFile("a.img", 1);

	// max_entries_scanned omitted - exercises the real default
	// (cap * DefaultMaxScannedEntriesMultiplier), confirming an
	// ordinary small root isn't truncated by it.
	const auto result = ScanImageRoot(tmp_dir, 1);

	EXPECT_EQ(result.images.size(), 1u);
	EXPECT_FALSE(result.truncated);
}

} // namespace
