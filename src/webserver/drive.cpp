// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "drive.h"
#include "bridge.h"
#include "webserver.h"

#include "config/setup.h"
#include "dos/dos.h"
#include "dos/drives.h"
#include "dos/programs/mount_policy.h"
#include "ints/bios_disk.h"
#include "misc/cross.h"

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

DriveMountCommand::DriveMountCommand(char drive_letter, std::string host_path,
                                     bool readonly, std::string label)
        : drive_letter(drive_letter),
          host_path(std::move(host_path)),
          readonly(readonly),
          label(std::move(label))
{}

void DriveMountCommand::Execute()
{
	// Same authoritative, emulation-thread check as DriveSwapCommand.
	if (MountPolicy::IsLocked()) {
		locked = true;
		error  = "mount is locked";
		LOG_WARNING("DRIVE-MOUNT: Blocked - locked");
		return;
	}

	// WhitelistEnforced unconditionally: this code path is only reachable
	// with the webserver running, so GetCurrentDirPolicy()'s own check for
	// that would always resolve the same way. No conf file is behind an
	// API-driven mount, so conf_anchor is empty - ValidateDirectoryMount
	// already treats that as "no conf-relative root", not as "allow
	// everything".
	const auto verdict = MountPolicy::ValidateDirectoryMount(
	        std::filesystem::path(host_path),
	        /*conf_anchor=*/{},
	        MountPolicy::AllowedBases(),
	        DirMountPolicy::WhitelistEnforced);
	if (!verdict.allowed) {
		deny_reason = verdict.reason;
		error       = "Blocked by mount policy";
		LOG_WARNING("DRIVE-MOUNT: Blocked - policy violation (%s)",
		            std::string(DriveDenyReasonCode(deny_reason)).c_str());
		return;
	}

	// Use the canonical path from validation, not the raw request string.
	const auto& resolved = verdict.resolved;

	// ValidateDirectoryMount validates the path against policy but never
	// checks it is actually a directory - MOUNT.COM does that itself
	// before ever calling it. This is that check.
	std::error_code ec = {};
	if (!std::filesystem::is_directory(resolved, ec) || ec) {
		not_a_directory = true;
		error           = "Not a directory";
		return;
	}

	const auto drv_idx = static_cast<uint8_t>(
	        std::toupper(static_cast<unsigned char>(drive_letter)) - 'A');
	if (drv_idx >= DOS_DRIVES) {
		invalid_drive = true;
		error         = "Invalid drive letter";
		return;
	}

	auto final_path = resolved.string();
	if (!final_path.empty() && final_path.back() != CROSS_FILESPLIT) {
		final_path += CROSS_FILESPLIT;
	}

	// Geometry left at the engine's own defaults (0,0,0,0 = autodetect
	// from the host filesystem), same as a bare `MOUNT <drive> <path>`
	// with no `-size` override. Read-only files under `allow_write_
	// protected_files` mirror MOUNT.COM's own handling of that setting.
	const auto section = get_section("dosbox");
	auto new_drive     = std::make_shared<localDrive>(
	        final_path.c_str(),
	        0,
	        0,
	        0,
	        0,
	        MediaId::HardDisk,
	        readonly,
	        section->GetBool("allow_write_protected_files"));

	// RegisterFilesystemImage replaces this slot's swap-disk list outright
	// (drive_infos.at(idx).disks = {new_drive}), so a directory mount over
	// a letter that a prior drive_swap left in managed, multi-disk state
	// cannot leave stale swap-slot bookkeeping behind. Overwriting an
	// already-mounted letter without requiring an unmount first mirrors
	// MOUNT.COM's own directory-mount behavior exactly - it has never
	// required `-u` before remounting either.
	DriveManager::RegisterFilesystemImage(drv_idx, new_drive);
	Drives.at(drv_idx) = new_drive;

	mem_writeb(RealToPhysical(dos.tables.mediaid) + drv_idx * 9,
	           new_drive->GetMediaByte());

	// Matches MountLocal's own default when no label is given, and its
	// choice to lock the label at mount time (allowupdate=false) rather
	// than let a later on-disk label file silently override it.
	const auto effective_label =
	        label.empty() ? std::string(1,
	                                    static_cast<char>(std::toupper(
	                                            static_cast<unsigned char>(
	                                                    drive_letter)))) +
	                                "_DRIVE"
	                      : label;
	new_drive->dirCache.SetLabel(effective_label.c_str(), false, false);
}

void DriveMountCommand::Post(const Request& req, Response& res)
{
	auto body = json::parse(req.body);

	if (!body.contains("drive") || !body.contains("path")) {
		res.status = httplib::StatusCode::BadRequest_400;
		json err;
		err["error"]      = "Missing 'drive' or 'path' field";
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

	// Early, clean check before touching the Bridge, same as
	// DriveSwapCommand::Post. The authoritative check still happens in
	// Execute() on the emulation thread, since the latch can flip
	// between this check and that one.
	if (MountPolicy::IsLocked()) {
		res.status = httplib::StatusCode::Forbidden_403;
		json err;
		err["error"]      = "mount is locked";
		err["error_code"] = "mount_locked";
		err["retryable"]  = false;
		send_json(res, err);
		return;
	}

	const auto readonly = body.value("readonly", false);
	const auto label    = body.value("label", std::string{});

	DriveMountCommand cmd(drive_str[0],
	                      body["path"].get<std::string>(),
	                      readonly,
	                      label);
	cmd.WaitForCompletion(5000);

	if (!cmd.error.empty()) {
		json err;
		err["error"]     = cmd.error;
		err["retryable"] = false;
		// Structured error_code/retryable alongside the message, same
		// shape as every other route (webserver.cpp's send_error).
		if (cmd.locked) {
			res.status        = httplib::StatusCode::Forbidden_403;
			err["error_code"] = "mount_locked";
		} else if (cmd.deny_reason != DenyReason::None) {
			res.status        = httplib::StatusCode::BadRequest_400;
			err["error_code"] = std::string(
			        DriveDenyReasonCode(cmd.deny_reason));
		} else if (cmd.not_a_directory) {
			res.status        = httplib::StatusCode::BadRequest_400;
			err["error_code"] = "not_a_directory";
		} else if (cmd.invalid_drive) {
			res.status        = httplib::StatusCode::BadRequest_400;
			err["error_code"] = "invalid_drive_letter";
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

DirListing ScanDirectory(const std::filesystem::path& path, size_t cap,
                         size_t max_entries_scanned)
{
	DirListing result;

	std::error_code ec;
	auto it = std::filesystem::directory_iterator(path, ec);
	if (ec) {
		return result;
	}

	if (max_entries_scanned == 0) {
		max_entries_scanned = cap * DefaultMaxScannedEntriesMultiplier;
	}

	// A directory entry's own leaf name can never be ".", "..", or
	// contain a path separator (the OS hands directory_iterator plain
	// filenames), so path/name staying under path needs no re-
	// canonicalization the way ScanImageRoot's cross-root scan does -
	// the only thing left to rule out per entry is a symlink.
	const auto consider = [&](const std::filesystem::directory_entry& entry) {
		std::error_code symlink_ec;
		if (entry.is_symlink(symlink_ec) || symlink_ec) {
			return;
		}

		std::error_code dir_ec;
		if (!entry.is_directory(dir_ec) || dir_ec) {
			return;
		}

		// Checked only once an entry is a confirmed subdirectory, not
		// before - otherwise a directory with exactly `cap` real
		// subdirectories and nothing else would be reported truncated
		// when nothing was actually left out.
		if (result.entries.size() >= cap) {
			result.truncated = true;
			return;
		}

		DirEntryInfo info;
		info.name = entry.path().filename().string();
		info.path = entry.path().string();
		result.entries.push_back(std::move(info));
	};

	// Manual walk with the error_code-returning increment(), not a
	// range-based for - same reasoning as ScanImageRoot: a directory
	// that goes unreadable mid-scan degrades to "here's what was found
	// before that happened" rather than an uncaught exception.
	// entries_scanned bounds raw entries examined regardless of
	// whether they qualify, so a directory full of files (which never
	// pass is_directory) can't be walked without limit.
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

void MountHandlers::GetDirectories(const Request& req, Response& res)
{
	if (!req.has_param("path") || req.get_param_value("path").empty()) {
		// No path: the top-level view is the configured allowed_bases
		// themselves, not an arbitrary filesystem root - there is
		// nothing to canonicalize or validate here, they are already
		// trusted, read-only-after-startup config.
		json entries = json::array();
		for (const auto& base : MountPolicy::AllowedBases()) {
			json e;
			auto name = base.filename().string();
			e["name"] = name.empty() ? base.string() : name;
			e["path"] = base.string();
			entries.push_back(e);
		}

		json j;
		j["path"]      = nullptr;
		j["parent"]    = nullptr;
		j["entries"]   = entries;
		j["truncated"] = false;
		send_json(res, j);
		return;
	}

	const auto canonical = MountPolicy::CanonicalizeExisting(
	        req.get_param_value("path"));
	if (!canonical) {
		res.status = httplib::StatusCode::BadRequest_400;
		json err;
		err["error"] = "path does not resolve to an existing directory";
		err["error_code"] = "does_not_resolve";
		err["retryable"]  = false;
		send_json(res, err);
		return;
	}

	std::error_code dir_ec;
	if (!std::filesystem::is_directory(*canonical, dir_ec) || dir_ec) {
		res.status = httplib::StatusCode::BadRequest_400;
		json err;
		err["error"]      = "path is not a directory";
		err["error_code"] = "not_a_directory";
		err["retryable"]  = false;
		send_json(res, err);
		return;
	}

	if (!MountPolicy::IsUnderAnyRoot(*canonical, MountPolicy::AllowedBases())) {
		res.status = httplib::StatusCode::Forbidden_403;
		json err;
		err["error"]      = "path is outside every allowed mount base";
		err["error_code"] = "outside_whitelist";
		err["retryable"]  = false;
		send_json(res, err);
		return;
	}

	// "" (as opposed to null) tells the caller there's a level above
	// this one, but it's the synthetic top-level roots view rather
	// than a real filesystem path - reached once `parent_path()` walks
	// out of every allowed base (including when `canonical` already
	// *is* one of them).
	std::string parent;
	const auto parent_path = canonical->parent_path();
	if (MountPolicy::IsUnderAnyRoot(parent_path, MountPolicy::AllowedBases())) {
		parent = parent_path.string();
	}

	const auto listing = ScanDirectory(*canonical, MaxDirEntriesPerListing);

	json entries = json::array();
	for (const auto& entry : listing.entries) {
		json e;
		e["name"] = entry.name;
		e["path"] = entry.path;
		entries.push_back(e);
	}

	json j;
	j["path"]      = canonical->string();
	j["parent"]    = parent;
	j["entries"]   = entries;
	j["truncated"] = listing.truncated;
	send_json(res, j);
}

} // namespace Webserver
