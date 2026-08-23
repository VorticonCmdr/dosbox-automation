// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "drive.h"
#include "bridge.h"
#include "webserver.h"

#include "dos/dos.h"
#include "dos/drives.h"
#include "dos/programs/mount_policy.h"
#include "ints/bios_disk.h"

#include "libs/json/json.h"

#include <cctype>
#include <filesystem>
#include <sys/stat.h>
#include <system_error>

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

std::string_view DriveDenyReasonCode(DenyReason reason)
{
	switch (reason) {
	case DenyReason::None: return "none";
	case DenyReason::DoesNotResolve: return "does_not_resolve";
	case DenyReason::NotRegularFile: return "not_regular_file";
	case DenyReason::SymlinkComponent: return "symlink_component";
	case DenyReason::SystemPath: return "system_path";
	case DenyReason::OutsideWhitelist: return "outside_whitelist";
	case DenyReason::NotADiskImage: return "not_a_disk_image";
	}
	return "unknown";
}

DriveSwapCommand::DriveSwapCommand(char drive_letter, std::string image_path)
        : drive_letter(drive_letter),
          image_path(std::move(image_path))
{}

void DriveSwapCommand::Execute()
{
	// Once mounts are locked, the configuration is frozen for everyone,
	// the guest commands (BOOT, IMGMOUNT, MOUNT) and the API alike. This
	// is the authoritative check, on the emulation thread where the swap
	// actually happens. The Post handler also checks early for a clean
	// error, but the latch can flip between that check and this one.
	if (MountPolicy::IsLocked()) {
		locked = true;
		error  = "mount is locked";
		LOG_WARNING("DRIVE-SWAP: Blocked - locked");
		return;
	}

	const auto verdict =
	        MountPolicy::ValidateImagePath(std::filesystem::path(image_path),
	                                       MountOrigin::Api,
	                                       MountPolicy::AllowedImageRoots());
	if (!verdict.allowed) {
		deny_reason = verdict.reason;
		error       = "Blocked by mount policy";
		LOG_WARNING("DRIVE-SWAP: Blocked - policy violation (%s)",
		            std::string(DriveDenyReasonCode(deny_reason)).c_str());
		return;
	}

	// Use the canonical path from validation, not the raw request string.
	// Mounting the validated object, not re-resolving an untrusted path.
	const auto& resolved = verdict.resolved.string();

	struct stat st = {};
	if (stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		file_not_found = true;
		error          = "File not found";
		return;
	}

	const auto file_size_kb = static_cast<uint32_t>(st.st_size / 1024);
	bool is_floppy          = false;
	for (const auto& geom : BIOS_GetDiskGeometryList()) {
		if (geom.ksize == file_size_kb) {
			is_floppy = true;
			break;
		}
	}

	const auto drv_idx = static_cast<uint8_t>(
	        std::toupper(static_cast<unsigned char>(drive_letter)) - 'A');

	if (drv_idx >= DOS_DRIVES) {
		invalid_drive = true;
		error         = "Invalid drive letter";
		return;
	}

	// Build the new drive before releasing the old one so a
	// construction failure does not leave the slot empty.
	auto new_drive = std::make_shared<fatDrive>(
	        resolved.c_str(), 512, 0, 0, 0, is_floppy ? 0xF0 : 0xF8, true);

	if (!new_drive->created_successfully) {
		mount_failed = true;
		error        = "Failed to mount image";
		return;
	}

	Drives[drv_idx].reset();

	Drives[drv_idx] = new_drive;

	if (drv_idx < MAX_DISK_IMAGES) {
		imageDiskList[drv_idx] = new_drive->loadedDisk;
	}
}

void DriveSwapCommand::Post(const Request& req, Response& res)
{
	auto body = json::parse(req.body);

	if (!body.contains("drive") || !body.contains("image")) {
		res.status = httplib::StatusCode::BadRequest_400;
		json err;
		err["error"]      = "Missing 'drive' or 'image' field";
		err["error_code"] = "missing_field";
		err["retryable"]  = false;
		send_json(res, err);
		return;
	}

	const auto drive_str = body["drive"].get<std::string>();
	if (drive_str.empty() ||
	    !std::isalpha(static_cast<unsigned char>(drive_str[0]))) {
		res.status = httplib::StatusCode::BadRequest_400;
		json err;
		err["error"]      = "Invalid drive letter";
		err["error_code"] = "invalid_drive_letter";
		err["retryable"]  = false;
		send_json(res, err);
		return;
	}

	if (MountPolicy::IsLocked()) {
		res.status = httplib::StatusCode::Forbidden_403;
		json err;
		err["error"]      = "mount is locked";
		err["error_code"] = "mount_locked";
		err["retryable"]  = false;
		send_json(res, err);
		return;
	}

	DriveSwapCommand cmd(drive_str[0], body["image"].get<std::string>());
	cmd.WaitForCompletion(5000);

	if (!cmd.error.empty()) {
		json err;
		err["error"]     = cmd.error;
		err["retryable"] = false;
		// Structured error_code/retryable alongside the message,
		// matching the shape the centralized exception handler gives
		// every other route (webserver.cpp's send_error) - this path
		// can't reuse that helper directly (it's file-local there).
		if (cmd.locked) {
			res.status        = httplib::StatusCode::Forbidden_403;
			err["error_code"] = "mount_locked";
		} else if (cmd.deny_reason != DenyReason::None) {
			res.status        = httplib::StatusCode::BadRequest_400;
			err["error_code"] = std::string(
			        DriveDenyReasonCode(cmd.deny_reason));
		} else if (cmd.file_not_found) {
			res.status        = httplib::StatusCode::NotFound_404;
			err["error_code"] = "file_not_found";
		} else if (cmd.invalid_drive) {
			res.status        = httplib::StatusCode::BadRequest_400;
			err["error_code"] = "invalid_drive_letter";
		} else if (cmd.mount_failed) {
			res.status = httplib::StatusCode::InternalServerError_500;
			err["error_code"] = "mount_failed";
		} else {
			res.status        = httplib::StatusCode::BadRequest_400;
			err["error_code"] = "unknown";
		}
		send_json(res, err);
		return;
	}

	json result;
	result["status"] = "ok";
	result["drive"]  = std::string(1,
	                               static_cast<char>(std::toupper(
	                                       static_cast<unsigned char>(
	                                               drive_str[0]))));
	send_json(res, result);
}

namespace {

std::string_view DriveTypeCode(DosDriveType type)
{
	switch (type) {
	case DosDriveType::Local: return "local";
	case DosDriveType::Cdrom: return "cdrom";
	case DosDriveType::Fat: return "fat";
	case DosDriveType::Iso: return "iso";
	case DosDriveType::Virtual: return "virtual";
	case DosDriveType::Unknown: return "unknown";
	}
	return "unknown";
}

} // namespace

void DriveListCommand::Execute()
{
	drives.clear();
	drives.reserve(DOS_DRIVES);
	for (uint8_t i = 0; i < DOS_DRIVES; ++i) {
		DriveEntry entry;
		entry.letter = static_cast<char>('A' + i);

		const auto& drive = Drives[i];
		if (!drive) {
			drives.push_back(entry);
			continue;
		}

		entry.mounted = true;
		entry.type    = std::string(DriveTypeCode(drive->GetType()));
		// GetInfo() on a local/FAT/ISO/CD-ROM drive is the mounted
		// host path - deliberate disclosure, not an accidental leak:
		// the operator configured this mount themselves, and this
		// whole surface already sits behind the bearer token on
		// loopback (same call the plan text for this route makes).
		entry.info      = drive->GetInfo();
		entry.read_only = drive->IsReadOnly();
		entry.removable = drive->IsRemovable();
		drives.push_back(entry);
	}
}

void DriveListCommand::Get(const Request&, Response& res)
{
	DriveListCommand cmd;
	cmd.WaitForCompletion();

	json list = json::array();
	for (const auto& d : cmd.drives) {
		json j;
		j["letter"]  = std::string(1, d.letter);
		j["mounted"] = d.mounted;
		if (d.mounted) {
			j["type"]      = d.type;
			j["info"]      = d.info;
			j["read_only"] = d.read_only;
			j["removable"] = d.removable;
		}
		list.push_back(j);
	}

	json j;
	j["drives"] = list;
	send_json(res, j);
}

ImageRootScan ScanImageRoot(const std::filesystem::path& root, size_t cap,
                            size_t max_entries_scanned)
{
	ImageRootScan result;

	std::error_code ec;
	auto it = std::filesystem::directory_iterator(root, ec);
	if (ec) {
		return result;
	}

	if (max_entries_scanned == 0) {
		max_entries_scanned = cap * DefaultMaxScannedEntriesMultiplier;
	}

	// Per-entry qualification only - never advances the iterator, never
	// touches max_entries_scanned. Kept separate from the walk below so
	// each of its early-outs reads as a plain "this entry doesn't
	// count" rather than needing to double as loop control.
	const auto consider = [&](const std::filesystem::directory_entry& entry) {
		// Skip symlinks outright rather than let is_regular_file()
		// follow them - same "any symlink component is hostile"
		// posture MountPolicy applies at mount time, just checked
		// here against the listing instead of the mount attempt.
		std::error_code symlink_ec;
		if (entry.is_symlink(symlink_ec) || symlink_ec) {
			return;
		}

		std::error_code regular_ec;
		if (!entry.is_regular_file(regular_ec) || regular_ec) {
			return;
		}

		// Re-validate independently of the directory walk itself,
		// with the same primitives ValidateImagePath uses at mount
		// time - this listing must never claim "available" for
		// something a subsequent drive/swap would then refuse.
		const auto canonical = MountPolicy::CanonicalizeExisting(
		        entry.path());
		if (!canonical || !MountPolicy::IsUnderAnyRoot(*canonical, {root})) {
			return;
		}

		std::error_code size_ec;
		const auto size = std::filesystem::file_size(*canonical, size_ec);
		if (size_ec) {
			return;
		}

		// Checked only once an entry is a confirmed, countable image,
		// not before - otherwise a root with exactly `cap` real
		// images and nothing else would be reported truncated when
		// nothing was actually left out.
		if (result.images.size() >= cap) {
			result.truncated = true;
			return;
		}

		ImageEntry image;
		image.path       = canonical->string();
		image.size_bytes = static_cast<int64_t>(size);
		result.images.push_back(std::move(image));
	};

	// A manual walk, not a range-based for: this needs the
	// error_code-returning increment() overload, not the throwing
	// operator++() a range-for desugars to - every other filesystem
	// call in this function already takes an error_code out-param for
	// exactly this reason (a root that goes unreadable mid-scan,
	// unmounted removable/network media, the directory deleted, a
	// permission change, degrades to "here's what was found before
	// that happened" rather than an uncaught exception surfacing as an
	// opaque 500). entries_scanned bounds raw entries examined
	// regardless of whether they qualify - a root full of
	// subdirectories or broken symlinks would otherwise be walked
	// without limit, since none of those ever reach `consider`'s own
	// `cap` check.
	const auto end         = std::filesystem::directory_iterator();
	size_t entries_scanned = 0;
	while (it != end) {
		if (entries_scanned >= max_entries_scanned) {
			result.truncated = true;
			break;
		}
		++entries_scanned;

		consider(*it);

		std::error_code inc_ec;
		it.increment(inc_ec);
		if (inc_ec) {
			result.truncated = true;
			break;
		}
	}

	return result;
}

void MountHandlers::GetLock(const Request&, Response& res)
{
	json j;
	j["locked"] = MountPolicy::IsLocked();
	send_json(res, j);
}

void MountHandlers::PostLock(const Request&, Response& res)
{
	MountPolicy::Lock();
	json j;
	j["status"] = "locked";
	send_json(res, j);
}

void MountHandlers::GetPolicy(const Request&, Response& res)
{
	json j;
	j["locked"] = MountPolicy::IsLocked();

	json bases = json::array();
	for (const auto& p : MountPolicy::AllowedBases()) {
		bases.push_back(p.string());
	}
	j["allowed_bases"] = bases;

	json image_roots = json::array();
	for (const auto& p : MountPolicy::AllowedImageRoots()) {
		image_roots.push_back(p.string());
	}
	j["allowed_image_roots"] = image_roots;

	send_json(res, j);
}

void MountHandlers::GetImages(const Request&, Response& res)
{
	json roots = json::array();
	for (const auto& root : MountPolicy::AllowedImageRoots()) {
		const auto scan = ScanImageRoot(root, MaxImagesPerRoot);

		json images = json::array();
		for (const auto& img : scan.images) {
			json j;
			j["path"]       = img.path;
			j["size_bytes"] = img.size_bytes;
			images.push_back(j);
		}

		json r;
		r["root"]      = root.string();
		r["images"]    = images;
		r["truncated"] = scan.truncated;
		roots.push_back(r);
	}

	json j;
	j["roots"] = roots;
	send_json(res, j);
}

} // namespace Webserver
