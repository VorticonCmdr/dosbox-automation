// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_WEBSERVER_DRIVE_H
#define DOSBOX_WEBSERVER_DRIVE_H

#include "bridge.h"
#include "libs/http/http.h"

#include "dos/programs/mount_policy.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Webserver {

// Stable, wire-format identifier for a MountPolicy::DenyReason - never
// the raw enum, never a localized MSG_Get() string. DenyReason::None
// means "allowed" and never reaches a caller as an error; mapped to
// "none" anyway so the switch stays exhaustive.
std::string_view DriveDenyReasonCode(DenyReason reason);

class DriveSwapCommand : public Command {
public:
	DriveSwapCommand(char drive_letter, std::string image_path);
	void Execute() override;
	static void Post(const httplib::Request& req, httplib::Response& res);

private:
	char drive_letter      = 'A';
	std::string image_path = {};

	// Set by Execute() on failure, alongside Command::error's
	// human-readable message, so Post() can report a stable
	// error_code and the right HTTP status instead of the single
	// generic 400 every failure used to collapse into.
	bool locked            = false;
	DenyReason deny_reason = DenyReason::None;
	bool file_not_found    = false;
	bool invalid_drive     = false;
	bool mount_failed      = false;
};

// Non-recursive per-root cap for GET /mount/images - a backstop
// against a misconfigured root pointing at a directory with tens of
// thousands of files, not a realistic expectation for a game/image
// collection. Surfaced to callers via capabilities.cpp's
// drive.limits.max_images_per_root, the same number this enforces.
constexpr size_t MaxImagesPerRoot = 500;

// ScanImageRoot's default multiplier from `cap` to the raw
// directory-entry scan bound (see its own doc comment): only entries
// that already qualify as a countable image count against `cap`
// itself, so this second, larger bound exists to stop a root full of
// non-qualifying entries (subdirectories, broken symlinks) from being
// walked without limit just to discover most of them don't count.
constexpr size_t DefaultMaxScannedEntriesMultiplier = 20;

struct DriveEntry {
	char letter      = 'A';
	bool mounted     = false;
	std::string type = {};
	std::string info = {};
	bool read_only   = false;
	bool removable   = false;
};

// GET /api/v1/drive - walks the live Drives[] array, which is
// emulation-thread-owned state, so this needs the Bridge like
// everything else that reads it.
class DriveListCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

	std::vector<DriveEntry> drives = {};
};

struct ImageEntry {
	std::string path   = {};
	int64_t size_bytes = 0;
};

struct ImageRootScan {
	std::vector<ImageEntry> images = {};
	bool truncated                 = false;
};

// One already-canonical root's worth of GET /mount/images: real
// filesystem I/O, so only ever called from the web thread (GetImages),
// never from a Bridge Command. `cap` bounds accepted images, same as
// before; `max_entries_scanned` (0 means cap *
// DefaultMaxScannedEntriesMultiplier) separately bounds the raw
// directory entries examined regardless of whether they qualify, so a
// root stuffed with subdirectories or broken symlinks can't be walked
// without limit before `cap` itself would ever engage. Exposed
// (including the second parameter) for testing.
ImageRootScan ScanImageRoot(const std::filesystem::path& root, size_t cap,
                            size_t max_entries_scanned = 0);

// GET/POST /mount/lock, GET /mount/policy, GET /mount/images. None of
// these touch emulation-thread state - MountPolicy's own config is
// read-only after startup and Lock()/IsLocked() are a plain atomic -
// so none of them cross the Bridge; GetImages does real filesystem
// I/O, which must never sit inside a Bridge Command regardless of
// whether the state it reads happens to be Bridge-free too.
struct MountHandlers {
	static void GetLock(const httplib::Request& req, httplib::Response& res);
	static void PostLock(const httplib::Request& req, httplib::Response& res);
	static void GetPolicy(const httplib::Request& req, httplib::Response& res);
	static void GetImages(const httplib::Request& req, httplib::Response& res);
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DRIVE_H
