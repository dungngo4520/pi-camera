#pragma once

// Pure-logic helpers extracted from preview.cpp so they can be unit-tested
// on x86 (preview.cpp is hardware code not in picamera_core). These functions
// do filename building, disk-space checks (statvfs), and directory listing
// (std::filesystem) — no libcamera/libgpiod dependency.

#include "camera_config.h"

#include <string>
#include <vector>

namespace picamera {

// Generate a timestamped filename: prefix_YYYYMMDD-HHMMSS-mmm.ext
// Millisecond precision prevents filename collisions (and thus O_EXCL
// EEXIST failures) when two captures happen within the same second —
// normal for burst/bracket shooting. The millisecond suffix is zero-padded
// to 3 digits so filenames sort lexicographically by time.
// Uses safeCapturePath to prevent path traversal via malicious --capture-dir/prefix.
std::string makeCaptureFilename(const std::string &dir,
                                const std::string &prefix,
                                OutputFormat fmt);

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

} // namespace picamera
