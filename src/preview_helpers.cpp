#include "preview_helpers.h"
#include "safe_path.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sys/statvfs.h>

namespace picamera {

namespace {
std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}
} // namespace

std::string makeCaptureFilename(const std::string &dir,
                                const std::string &prefix, OutputFormat fmt) {
  auto now = std::chrono::system_clock::now();
  auto nowT = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
  std::tm tm;
  std::tm *tmPtr = nullptr;
#ifdef _WIN32
  if (std::localtime_s(&tm, &nowT) == 0)
    tmPtr = &tm;
#else
  tmPtr = localtime_r(&nowT, &tm);
#endif
  char buf[64] = {};
  if (!tmPtr || std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm) == 0) {
    // Fallback: use raw epoch time if localtime/strftime fails
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(nowT));
  }
  char ts[80];
  std::snprintf(ts, sizeof(ts), "%s-%03lld", buf,
                static_cast<long long>(ms.count()));
  return safeCapturePath(dir, prefix, ts, extensionFor(fmt));
}

bool hasDiskSpace(const std::string &dir, uint64_t minBytes) {
  struct statvfs stat;
  if (statvfs(dir.c_str(), &stat) != 0) {
    std::cerr << "Preview: cannot check disk space on " << dir << ": "
              << errnoString(errno) << "\n";
    return false;
  }
  // Use checked multiplication to prevent theoretical overflow on
  // filesystems with very large block sizes or block counts.
  // f_frsize is the POSIX-correct block size for free-space calculations
  // (f_bsize is an FS-optimal I/O size that may differ on some filesystems).
  uint64_t avail = 0;
  if (!checkedMul(static_cast<uint64_t>(stat.f_bavail),
                  static_cast<uint64_t>(stat.f_frsize), avail)) {
    // Overflow — can't safely determine free space, fail closed.
    std::cerr << "Preview: disk space check overflowed — denying capture\n";
    return false;
  }
  return avail >= minBytes;
}

std::vector<std::string> listCaptures(const std::string &dir) {
  std::vector<std::string> files;
  try {
    namespace fs = std::filesystem;
    if (!fs::exists(dir))
      return files;
    std::vector<std::pair<fs::file_time_type, std::string>> entries;
    for (const auto &entry : fs::directory_iterator(dir)) {
      // Handle per-entry errors gracefully — a single bad symlink or
      // permission-denied file should not empty the entire listing.
      try {
        // Use symlink_status to avoid following symlinks — a symlink
        // pointing outside the capture dir should not appear as a
        // capturable image in the playback browser.
        if (!fs::is_regular_file(entry.symlink_status()))
          continue;
        std::string ext = toLower(entry.path().extension().string());
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".dng" ||
            ext == ".ppm" || ext == ".raw") {
          // Use the error_code overload of last_write_time so a
          // stale NFS handle or permission issue on one entry
          // doesn't throw (the outer try/catch would swallow it,
          // but being explicit avoids the exception overhead and
          // makes the skip intent clear).
          std::error_code mtimeEc;
          auto mtime = fs::last_write_time(entry, mtimeEc);
          if (mtimeEc)
            continue;
          entries.emplace_back(mtime, entry.path().string());
        }
      } catch (const std::exception &e) {
        // Skip this entry, keep iterating
        (void)e;
      }
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    for (auto &e : entries)
      files.push_back(std::move(e.second));
  } catch (const std::exception &e) {
    std::cerr << "Preview: failed to list captures: " << e.what() << "\n";
  }
  return files;
}

std::string makeSequentialFilename(const std::string &dir,
                                   const std::string &prefix,
                                   OutputFormat fmt) {
  namespace fs = std::filesystem;
  int maxNum = 0;
  std::string ext = std::string(extensionFor(fmt));
  std::string extLower = toLower(ext);
  try {
    if (fs::exists(dir)) {
      for (const auto &entry : fs::directory_iterator(dir)) {
        try {
          if (!fs::is_regular_file(entry.symlink_status()))
            continue;
          std::string name = entry.path().filename().string();
          // Match prefix_IMGXXXX.ext pattern
          std::string needle = prefix + "_IMG";
          if (name.size() <= needle.size() + ext.size() + 1)
            continue;
          if (name.compare(0, needle.size(), needle) != 0)
            continue;
          if (toLower(entry.path().extension().string()) != "." + extLower)
            continue;
          size_t numStart = needle.size();
          size_t dotPos = name.rfind('.');
          if (dotPos == std::string::npos || dotPos <= numStart)
            continue;
          std::string numStr = name.substr(numStart, dotPos - numStart);
          if (numStr.empty())
            continue;
          bool allDigits = true;
          for (char c : numStr) {
            if (c < '0' || c > '9') {
              allDigits = false;
              break;
            }
          }
          if (!allDigits)
            continue;
          int num = 0;
          try {
            num = std::stoi(numStr);
          } catch (...) {
            continue;
          }
          maxNum = std::max(num, maxNum);
        } catch (const std::exception &) {
          continue;
        }
      }
    }
  } catch (const std::exception &) { // directory access failure — start at 1
    maxNum = 0;
  }
  int nextNum = maxNum + 1;
  char numBuf[16];
  std::snprintf(numBuf, sizeof(numBuf), "IMG%04d", nextNum);
  return safeCapturePath(dir, prefix, numBuf, ext);
}

std::string ensureDateSubfolder(const std::string &dir,
                                bool useDateSubfolders) {
  if (!useDateSubfolders)
    return dir;
  auto now = std::chrono::system_clock::now();
  auto nowT = std::chrono::system_clock::to_time_t(now);
  std::tm tm;
  std::tm *tmPtr = nullptr;
#ifdef _WIN32
  if (std::localtime_s(&tm, &nowT) == 0)
    tmPtr = &tm;
#else
  tmPtr = localtime_r(&nowT, &tm);
#endif
  if (!tmPtr)
    return dir;
  char dateBuf[16] = {};
  if (std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", tmPtr) == 0)
    return dir;
  namespace fs = std::filesystem;
  fs::path sub = fs::path(dir) / dateBuf;
  std::error_code ec;
  if (!fs::exists(sub, ec)) {
    fs::create_directories(sub, ec);
  }
  return sub.string();
}

// --- File protection ---

namespace {

std::string protectedListPath(const std::string &dir) {
  namespace fs = std::filesystem;
  return (fs::path(dir) / ".protected").string();
}

std::string basenameOf(const std::string &path) {
  namespace fs = std::filesystem;
  return fs::path(path).filename().string();
}

} // namespace

std::vector<std::string> listProtectedFiles(const std::string &dir) {
  std::vector<std::string> result;
  std::ifstream f(protectedListPath(dir));
  if (!f.is_open())
    return result;
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (!line.empty())
      result.push_back(line);
  }
  return result;
}

bool isFileProtected(const std::string &dir, const std::string &filename) {
  std::string base = basenameOf(filename);
  if (base.empty())
    return false;
  auto protectedList = listProtectedFiles(dir);
  return std::find(protectedList.begin(), protectedList.end(), base) !=
         protectedList.end();
}

bool toggleFileProtection(const std::string &dir, const std::string &filename) {
  std::string base = basenameOf(filename);
  if (base.empty())
    return false;

  auto current = listProtectedFiles(dir);
  std::set<std::string> protectedSet(current.begin(), current.end());

  bool nowProtected;
  if (protectedSet.contains(base)) {
    protectedSet.erase(base);
    nowProtected = false;
  } else {
    protectedSet.insert(base);
    nowProtected = true;
  }

  // Write the updated list back. Use a temp file + rename for atomicity.
  std::string listPath = protectedListPath(dir);
  std::string tmpPath = listPath + ".tmp";
  {
    std::ofstream f(tmpPath);
    if (!f.is_open())
      return false;
    for (const auto &name : protectedSet) {
      f << name << "\n";
    }
    if (!f.good())
      return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmpPath, listPath, ec);
  if (ec) {
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return nowProtected;
}

// --- File rating ---

// Build the .rating sidecar path for a file: dir/.<basename>.rating
static std::string ratingSidecarPath(const std::string &dir,
                                     const std::string &filename) {
  namespace fs = std::filesystem;
  fs::path p(filename);
  std::string base = p.filename().string();
  if (base.empty())
    return {};
  return dir + "/." + base + ".rating";
}

int readFileRating(const std::string &dir, const std::string &filename) {
  std::string path = ratingSidecarPath(dir, filename);
  if (path.empty())
    return 0;
  std::ifstream f(path);
  if (!f.is_open())
    return 0;
  int rating = 0;
  f >> rating;
  if (f.fail() || f.bad())
    return 0;
  return std::clamp(rating, 0, 5);
}

bool writeFileRating(const std::string &dir, const std::string &filename,
                     int rating) {
  rating = std::clamp(rating, 0, 5);
  std::string path = ratingSidecarPath(dir, filename);
  if (path.empty())
    return false;
  if (rating == 0) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true; // removing a non-existent file is not an error
  }
  std::ofstream f(path);
  if (!f.is_open())
    return false;
  f << rating << "\n";
  return f.good();
}

} // namespace picamera
