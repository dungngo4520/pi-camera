#pragma once

// Pure-logic helpers extracted from preview.cpp so they can be unit-tested
// on x86 (preview.cpp is hardware code not in picamera_core). These functions
// do filename building, disk-space checks (statvfs), and directory listing
// (std::filesystem) — no libcamera/libgpiod dependency.

#include "camera_config.h"
#include "camera_mode.h"

#include <string>
#include <vector>

namespace picamera {

// Generate a timestamped filename: prefix_YYYYMMDD-HHMMSS-mmm.ext
// Millisecond precision prevents filename collisions (and thus O_EXCL
// EEXIST failures) when two captures happen within the same second —
// normal for burst/bracket shooting. The millisecond suffix is zero-padded
// to 3 digits so filenames sort lexicographically by time.
// Uses safeCapturePath to prevent path traversal via malicious
// --capture-dir/prefix.
std::string makeCaptureFilename(const std::string &dir,
                                const std::string &prefix, OutputFormat fmt);

// Generate a sequentially-numbered filename: prefix_IMGXXXX.ext
// Finds the highest existing IMG number in the directory and increments.
std::string makeSequentialFilename(const std::string &dir,
                                   const std::string &prefix, OutputFormat fmt);

// Create a date-based subfolder (YYYY-MM-DD) under dir if useDateSubfolders
// is true. Returns the (possibly new) directory path. If false, returns dir
// unchanged. Creates the subfolder if it doesn't exist.
std::string ensureDateSubfolder(const std::string &dir, bool useDateSubfolders);

// Check that there's enough disk space for a capture.
// Returns true if at least `minBytes` is available. Fails closed (returns
// false) if the filesystem can't be queried — safer than silently allowing
// a write to a potentially full/corrupt filesystem.
// Default 50 MB covers full-res JPEG (~5-10 MB) with generous headroom;
// callers writing DNG/RAW (~15-20 MB) should pass a larger threshold.
bool hasDiskSpace(const std::string &dir,
                  uint64_t minBytes = 50ull * 1024 * 1024);

// List captured image files in the capture directory, sorted by modification
// time (newest first). Used by the playback browser.
std::vector<std::string> listCaptures(const std::string &dir);

// --- File protection (playback) ---
// Protected files are tracked in a `.protected` marker file in the capture
// directory, listing protected basenames (one per line). This avoids needing
// root (chattr +i) and is portable across filesystems.

// Returns true if the file (by basename) is in the .protected list.
bool isFileProtected(const std::string &dir, const std::string &filename);

// Toggle protection for a file. Returns true if the file is protected after
// the toggle, false if unprotected (or on error).
bool toggleFileProtection(const std::string &dir, const std::string &filename);

// Returns the set of protected basenames in the capture directory.
std::vector<std::string> listProtectedFiles(const std::string &dir);

// --- File rating (playback) ---
// Ratings are stored in .rating sidecar files (one per image, containing
// a single integer 0-5). This follows the existing .protected sidecar
// pattern.

// Read the rating for a file. Returns 0 if no .rating sidecar exists.
int readFileRating(const std::string &dir, const std::string &filename);

// Write a rating (0-5) for a file to a .rating sidecar. Returns true on
// success. A rating of 0 deletes the sidecar file.
bool writeFileRating(const std::string &dir, const std::string &filename,
                     int rating);

} // namespace picamera
