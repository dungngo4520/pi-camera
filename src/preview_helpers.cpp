#include "preview_helpers.h"
#include "safe_path.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sys/statvfs.h>

namespace picamera {

std::string makeCaptureFilename(const std::string &dir,
                                const std::string &prefix,
                                OutputFormat fmt) {
    auto now = std::chrono::system_clock::now();
    auto nowT = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm;
    std::tm *tmPtr = nullptr;
#ifdef _WIN32
    if (std::localtime_s(&tm, &nowT) == 0) tmPtr = &tm;
#else
    tmPtr = localtime_r(&nowT, &tm);
#endif
    char buf[64] = {};
    if (!tmPtr || std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm) == 0) {
        // Fallback: use raw epoch time if localtime/strftime fails
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(nowT));
    }
    char ts[80];
    std::snprintf(ts, sizeof(ts), "%s-%03lld", buf,
                  static_cast<long long>(ms.count()));
    return safeCapturePath(dir, prefix, ts, extensionFor(fmt));
}

bool hasDiskSpace(const std::string &dir, uint64_t minBytes) {
    struct statvfs stat;
    if (statvfs(dir.c_str(), &stat) != 0) {
        std::cerr << "Preview: cannot check disk space on " << dir
                  << ": " << errnoString(errno) << "\n";
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
        if (!fs::exists(dir)) return files;
        std::vector<std::pair<fs::file_time_type, std::string>> entries;
        for (const auto &entry : fs::directory_iterator(dir)) {
            // Handle per-entry errors gracefully — a single bad symlink or
            // permission-denied file should not empty the entire listing.
            try {
                // Use symlink_status to avoid following symlinks — a symlink
                // pointing outside the capture dir should not appear as a
                // capturable image in the playback browser.
                if (!fs::is_regular_file(entry.symlink_status())) continue;
                auto ext = entry.path().extension().string();
                // Lowercase compare (cast to unsigned char to avoid UB
                // on negative char values with non-ASCII paths).
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
                    ext == ".dng" || ext == ".ppm" || ext == ".raw") {
                    // Use the error_code overload of last_write_time so a
                    // stale NFS handle or permission issue on one entry
                    // doesn't throw (the outer try/catch would swallow it,
                    // but being explicit avoids the exception overhead and
                    // makes the skip intent clear).
                    std::error_code mtimeEc;
                    auto mtime = fs::last_write_time(entry, mtimeEc);
                    if (mtimeEc) continue;
                    entries.emplace_back(mtime, entry.path().string());
                }
            } catch (const std::exception &e) {
                // Skip this entry, keep iterating
                (void)e;
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (auto &e : entries) files.push_back(std::move(e.second));
    } catch (const std::exception &e) {
        std::cerr << "Preview: failed to list captures: " << e.what() << "\n";
    }
    return files;
}

} // namespace picamera
