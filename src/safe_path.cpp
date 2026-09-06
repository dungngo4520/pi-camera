#include "safe_path.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace picamera {

bool isSafePathComponent(std::string_view component) {
  if (component.empty())
    return false;
  // Reject absolute paths and any path containing a separator
  // (a component should be a single name, not a path).
  if (component[0] == '/')
    return false;
  if (component.find('/') != std::string_view::npos)
    return false;
  // Reject path traversal — exact match only, not substring (so
  // legitimate filenames like "photo..jpg" or "a..b" are allowed).
  if (component == ".." || component == ".")
    return false;
  // Reject control characters (0x00-0x1F, 0x7F)
  for (char c : component) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
      return false;
  }
  // Reject leading dash (could be mistaken for a CLI flag in some contexts)
  if (component[0] == '-')
    return false;
  return true;
}

std::string safeCapturePath(std::string_view rootDir, std::string_view prefix,
                            std::string_view timestamp, std::string_view ext) {
  if (rootDir.empty())
    return {};
  // Component-based ".." and control-char check (allows "photos..archive").
  {
    size_t start = 0;
    while (start <= rootDir.size()) {
      size_t end = rootDir.find('/', start);
      std::string_view comp = (end == std::string_view::npos)
                                  ? rootDir.substr(start)
                                  : rootDir.substr(start, end - start);
      if (comp == ".." || comp == ".")
        return {};
      if (end == std::string_view::npos)
        break;
      start = end + 1;
    }
  }
  for (char c : rootDir) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
      return {};
  }
  if (!isSafePathComponent(prefix))
    return {};
  if (timestamp.empty())
    return {};
  // Timestamp should be safe (generated internally), but validate anyway
  for (char c : timestamp) {
    if (c == '/' || c == '.' || static_cast<unsigned char>(c) < 0x20)
      return {};
  }
  // Extension should not contain path separators
  for (char c : ext) {
    if (c == '/' || static_cast<unsigned char>(c) < 0x20)
      return {};
  }

  std::string path;
  path.reserve(rootDir.size() + 1 + prefix.size() + 1 + timestamp.size() + 1 +
               ext.size());
  path.append(rootDir);
  if (path.back() != '/')
    path.push_back('/');
  path.append(prefix);
  path.push_back('_');
  path.append(timestamp);
  path.push_back('.');
  path.append(ext);

  if (path.size() >= 4096)
    return {}; // PATH_MAX includes NUL terminator
  return path;
}

bool isPathInside(std::string_view path, std::string_view root) {
  if (root.empty())
    return false;
  // Normalize: ensure root ends with '/' for prefix matching
  if (path.size() <= root.size())
    return false;
  if (path.substr(0, root.size()) != root)
    return false;
  // If root doesn't end with '/', the next char in path must be '/'
  if (root.back() != '/' && path[root.size()] != '/')
    return false;
  // Reject ".." as a path component after the root prefix.
  // Check components rather than substring to avoid false positives
  // on legitimate filenames like "foo..bar.jpg".
  std::string_view rest = path.substr(root.size());
  size_t start = 0;
  while (start <= rest.size()) {
    size_t end = rest.find('/', start);
    std::string_view comp = (end == std::string_view::npos)
                                ? rest.substr(start)
                                : rest.substr(start, end - start);
    if (comp == "..")
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return true;
}

bool isSafeDevicePath(std::string_view path) {
  if (path.substr(0, 5) != "/dev/")
    return false;
  // Reject ".." as a path component (not as a substring — filenames
  // like /dev/foo..bar are legitimate, if rare).
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    std::string_view comp = (end == std::string_view::npos)
                                ? path.substr(start)
                                : path.substr(start, end - start);
    if (comp == ".." || comp == ".")
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  // No control characters or DEL (0x7F).
  for (char c : path) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7F)
      return false;
  }
  // Strict allowlist: only permit specific device node patterns that
  // the application actually uses (SPI, I2C, GPIO). This prevents
  // arbitrary device access (e.g. /dev/mem, /dev/sda) even if a CLI
  // flag is tricked into pointing elsewhere.
  // Allowed: /dev/spidevN.M, /dev/i2c-N, /dev/gpiochipN
  // /dev/spidev<digits>.<digits>
  if (path.substr(0, 11) == "/dev/spidev") {
    auto rest = path.substr(11);
    auto dot = rest.find('.');
    if (dot == std::string_view::npos)
      return false;
    auto bus = rest.substr(0, dot);
    auto dev = rest.substr(dot + 1);
    if (bus.empty() || dev.empty())
      return false;
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    if (!std::all_of(bus.begin(), bus.end(), isDigit))
      return false;
    if (!std::all_of(dev.begin(), dev.end(), isDigit))
      return false;
    return true;
  }
  // /dev/i2c-<digits>
  if (path.substr(0, 9) == "/dev/i2c-") {
    auto rest = path.substr(9);
    if (rest.empty())
      return false;
    return std::all_of(rest.begin(), rest.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
  }
  // /dev/gpiochip<digits>
  if (path.substr(0, 13) == "/dev/gpiochip") {
    auto rest = path.substr(13);
    if (rest.empty())
      return false;
    return std::all_of(rest.begin(), rest.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
  }
  return false;
}

bool isSafeFilePath(std::string_view path) {
  if (path.empty())
    return false;
  // Reject absolute paths
  if (path[0] == '/')
    return false;
  // Reject control characters
  for (char c : path) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
      return false;
  }
  // Validate each component between slashes — isSafePathComponent
  // rejects exact ".." and "." components, but allows filenames
  // containing ".." as a substring (e.g. "photo..jpg"). Empty
  // components (from "//" sequences) are also rejected.
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    std::string_view comp = (end == std::string_view::npos)
                                ? path.substr(start)
                                : path.substr(start, end - start);
    if (comp.empty()) {
      // Reject empty components from "//" in the middle.
      // A trailing slash produces an empty final component, which is
      // invalid for file paths (would resolve to a directory).
      return false;
    }
    if (!isSafePathComponent(comp)) {
      return false;
    }
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return true;
}

PathStemExt splitPathStemExt(const std::string &path) {
  // Only the last dot, and only if it's after the last slash — so
  // "/a/b.c/d" has no extension.
  auto dot = path.rfind('.');
  auto slash = path.rfind('/');
  PathStemExt se;
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    se.stem = path.substr(0, dot);
    se.ext = path.substr(dot);
  } else {
    se.stem = path;
    se.ext = "";
  }
  return se;
}

std::string suffixedCandidate(const PathStemExt &se, const std::string &path,
                              int i) {
  if (i == 1)
    return path;
  std::string p = se.stem;
  p += "_";
  p += std::to_string(i);
  p += se.ext;
  return p;
}

int safeFileOpenFd(const std::string &path, std::string &outPath) {
  const PathStemExt se = splitPathStemExt(path);
  for (int i = 1; i <= 999; ++i) {
    std::string p = suffixedCandidate(se, path, i);
    int fd =
        open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
             S_IRUSR | S_IWUSR | S_IRGRP);
    if (fd >= 0) {
      outPath = p;
      return fd;
    }
    // EEXIST: collision, retry with suffix. ELOOP: symlink (O_NOFOLLOW),
    // retry with suffix so a symlink at one name doesn't block saves.
    if (errno != EEXIST && errno != ELOOP)
      return -1;
  }
  return -1;
}

std::string canonicalizeDir(const std::string &dir) {
  if (dir.empty())
    return {};
  // Reject control characters
  for (char c : dir) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
      return {};
  }
  try {
    namespace fs = std::filesystem;
    // Symlink/traversal handling strategy:
    //   1. fs::absolute() makes the path absolute so weakly_canonical
    //      never returns a relative result for non-existent dirs.
    //   2. fs::weakly_canonical() resolves symlinks for components that
    //      already exist on disk and collapses ".." lexically — this
    //      catches symlinked directories that point outside the capture
    //      root. For non-existent trailing components it performs only
    //      lexical normalization (no symlink to resolve yet).
    //   3. A final component-based ".." rejection (below) catches any
    //      non-existent traversal attempt that weakly_canonical's lexical
    //      phase might not fully collapse, so ".." can never survive in
    //      the returned canonical path.
    fs::path abs = fs::absolute(dir);
    fs::path canon = fs::weakly_canonical(abs);
    std::string result = canon.string();
    // Remove trailing slash if present (but keep "/" itself)
    if (result.size() > 1 && result.back() == '/')
      result.pop_back();
    // Final safety check: no ".." should remain as a path component
    // after canonicalization. Use component-based checking (not
    // substring) so legitimate names like "..hidden" are not rejected.
    size_t start = 0;
    while (start <= result.size()) {
      size_t end = result.find('/', start);
      std::string_view comp =
          (end == std::string::npos)
              ? std::string_view(result).substr(start)
              : std::string_view(result).substr(start, end - start);
      if (comp == "..")
        return {};
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
    return result;
  } catch (const std::exception &) {
    return {};
  }
}

bool isCanonicalPathInside(const std::string &path, const std::string &root) {
  if (path.empty() || root.empty())
    return false;
  // Both should be absolute and canonical. Use lexical comparison.
  if (path[0] != '/' || root[0] != '/')
    return false;
  if (path == root)
    return false; // must be strictly inside, not the root itself
  // Special case: root == "/" — every absolute path is inside it.
  // The general prefix check below would fail because path[1] is the
  // first char of the subdirectory, not '/'.
  if (root == "/") {
    // Reject ".." components in the path (defense in depth).
    std::string_view rest(path.data() + 1, path.size() - 1);
    size_t start = 0;
    while (start <= rest.size()) {
      size_t end = rest.find('/', start);
      std::string_view comp = (end == std::string_view::npos)
                                  ? rest.substr(start)
                                  : rest.substr(start, end - start);
      if (comp == "..")
        return false;
      if (end == std::string_view::npos)
        break;
      start = end + 1;
    }
    return true;
  }
  // Check that path starts with root + "/"
  if (path.size() <= root.size())
    return false;
  if (path.substr(0, root.size()) != root)
    return false;
  if (path[root.size()] != '/')
    return false;
  // Reject ".." as a path component (not as a substring, to allow
  // legitimate filenames like "foo..bar.jpg").
  std::string_view rest(path.data() + root.size() + 1,
                        path.size() - root.size() - 1);
  size_t start = 0;
  while (start <= rest.size()) {
    size_t end = rest.find('/', start);
    std::string_view comp = (end == std::string_view::npos)
                                ? rest.substr(start)
                                : rest.substr(start, end - start);
    if (comp == "..")
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return true;
}

// Check that filePath's parent directory is inside rootDir after symlink
// resolution. The final filename component is NOT canonicalized — callers
// must enforce O_NOFOLLOW (for creation) or symlink_status (for listing)
// to reject symlinked files that would resolve outside rootDir.
bool isFilePathInsideDir(const std::string &filePath,
                         const std::string &rootDir) {
  if (filePath.empty() || rootDir.empty())
    return false;
  if (rootDir[0] != '/')
    return false; // rootDir must be canonical (absolute)
  try {
    namespace fs = std::filesystem;
    fs::path fp(filePath);
    fs::path parent = fp.parent_path();
    if (parent.empty())
      return false;
    // Canonicalize the parent directory — this resolves symlinks on
    // existing path components, preventing symlinked subdirectories
    // from escaping the capture directory.
    fs::path canonParent = fs::weakly_canonical(parent);
    std::string canonParentStr = canonParent.string();
    if (canonParentStr.size() > 1 && canonParentStr.back() == '/')
      canonParentStr.pop_back();
    // Check that the canonical parent is inside (or equal to) rootDir.
    // The parent can be the rootDir itself (file is directly in rootDir).
    if (canonParentStr == rootDir)
      return true;
    return isCanonicalPathInside(canonParentStr, rootDir);
  } catch (const std::exception &) {
    return false;
  }
}

} // namespace picamera
