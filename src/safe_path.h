#pragma once

// Safe path construction utilities to prevent path traversal and symlink
// attacks.
//
// All capture file paths are built through these helpers so that a malicious
// or malformed --capture-dir / --capture-prefix / --capture-file cannot escape
// the intended capture directory or overwrite arbitrary files.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace picamera {

// Thread-safe errno-to-string conversion. strerror() is not thread-safe
// (static buffer); std::system_category().message() is thread-safe and
// returns a std::string. Used in all error logging paths.
inline std::string errnoString(int err) {
  return std::system_category().message(err);
}

// Reject a path component if it contains "..", absolute paths, or control
// characters. Returns true if the component is safe to use as a filename
// or subdirectory within a known root.
bool isSafePathComponent(std::string_view component);

// Build a capture filename safely: rootDir/prefix_timestamp.ext
// Rejects if rootDir is empty, prefix is unsafe, or the resulting path
// would be unreasonably long (> 4096 chars). Returns empty string on failure.
std::string safeCapturePath(std::string_view rootDir, std::string_view prefix,
                            std::string_view timestamp, std::string_view ext);

// Check that a resolved path is inside the given root directory.
// Both should be absolute or relative; uses simple lexical containment
// (no symlink resolution needed if both are constructed from safe components).
//
// Note: production callers should generally prefer isCanonicalPathInside()
// or isFilePathInsideDir(), which canonicalize before comparing and therefore
// resist symlink-based escape. This helper is retained for callers that
// already hold two canonical paths and want a cheap lexical containment check.
bool isPathInside(std::string_view path, std::string_view root);

// Validate a device path like /dev/spidev0.0 or /dev/i2c-1.
// Must start with /dev/ and not contain "..".
bool isSafeDevicePath(std::string_view path);

// Validate a relative file path (may contain subdirectory separators).
// Rejects absolute paths, ".." traversal, and control characters.
// Each path component is validated via isSafePathComponent.
bool isSafeFilePath(std::string_view path);

// Canonicalize a directory path by resolving symlinks and ".." components.
// Uses weakly_canonical so the directory need not exist yet. Returns empty
// string on failure (e.g. a path component that should exist doesn't, or
// the canonical form escapes the filesystem root). The returned path has
// no trailing slash and no ".." components.
std::string canonicalizeDir(const std::string &dir);

// Check that a resolved (canonical) path is inside the given canonical root.
// Both paths should be absolute and canonical (no symlinks, no "..").
// Uses lexical comparison after ensuring both are absolute.
bool isCanonicalPathInside(const std::string &path, const std::string &root);

// Verify that a file path (dir + relative filename) will resolve to a
// location inside the canonical capture directory. Canonicalizes the
// parent directory of the file (resolving symlinks on existing components)
// and checks that it is inside rootDir. This prevents symlinked
// subdirectories from escaping the capture directory.
// Both rootDir and filePath should be canonical (from canonicalizeDir).
// Returns true if the file path is safely contained within rootDir.
bool isFilePathInsideDir(const std::string &filePath,
                         const std::string &rootDir);

// Split a path into stem and extension for suffix-retry logic. The
// extension is the last dot-suffix that appears after the final slash, so
// "/a/b.c/d" has no extension (stem="/a/b.c/d", ext="") while
// "/a/b/img.jpg" → stem="/a/b/img", ext=".jpg". Shared by safeFileOpenFd
// and the encoder safe-open helpers to avoid duplicating the dot/slash split.
struct PathStemExt {
  std::string stem;
  std::string ext;
};
PathStemExt splitPathStemExt(const std::string &path);

// Build the i-th suffix-retry candidate: i == 1 returns the original
// `path`; i >= 2 returns stem + "_" + i + ext. Used by the atomic
// open(O_EXCL) retry loops in safeFileOpenFd and the encoder helpers.
std::string suffixedCandidate(const PathStemExt &se, const std::string &path,
                              int i);

// Atomically open a file for writing with O_CREAT|O_EXCL|O_NOFOLLOW, retrying
// with _2, _3, ... suffixes on EEXIST (no lstat probe, so no TOCTOU race).
// On success, returns a non-negative fd and sets `outPath` to the path
// actually opened (may differ from `path` when a suffix was needed). On
// failure, returns -1 and `outPath` is left unmodified. The caller owns the
// fd and must close() it (and unlink(outPath) on write failure).
int safeFileOpenFd(const std::string &path, std::string &outPath);

// Checked multiplication for size_t — returns false on overflow.
inline bool checkedMul(size_t a, size_t b, size_t &result) {
  if (a == 0 || b == 0) {
    result = 0;
    return true;
  }
  if (a > SIZE_MAX / b)
    return false;
  result = a * b;
  return true;
}

// Checked addition for size_t — returns false on overflow.
inline bool checkedAdd(size_t a, size_t b, size_t &result) {
  if (a > SIZE_MAX - b)
    return false;
  result = a + b;
  return true;
}

// Checked multiplication for uint64_t — returns false on overflow.
// Only needed on 32-bit where size_t != uint64_t; on 64-bit the size_t
// overload above already handles uint64_t arguments.
#if SIZE_MAX != UINT64_MAX
inline bool checkedMul(uint64_t a, uint64_t b, uint64_t &result) {
  if (a == 0 || b == 0) {
    result = 0;
    return true;
  }
  if (a > UINT64_MAX / b)
    return false;
  result = a * b;
  return true;
}

// Checked addition for uint64_t — returns false on overflow.
inline bool checkedAdd(uint64_t a, uint64_t b, uint64_t &result) {
  if (a > UINT64_MAX - b)
    return false;
  result = a + b;
  return true;
}
#endif

} // namespace picamera
