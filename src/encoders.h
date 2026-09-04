#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace picamera {

// All writers open the file with O_CREAT|O_EXCL|O_NOFOLLOW. If the requested
// path already exists (collision), they retry with _2, _3, ... suffixes via
// the atomic safeFileOpen/safeFileOpenFd helpers (no lstat probe, so no
// TOCTOU race). On success, if `actualPath` is non-null it is set to the
// path actually written to (which may differ from `path` when a suffix was
// needed). On failure, `actualPath` is left unmodified and no partial file
// is left behind (the file is unlinked on write failure).
[[nodiscard]] bool writePng(const std::string &path, const uint8_t *rgb, uint32_t w, uint32_t h,
              int compressionLevel = 6, std::string *actualPath = nullptr);
[[nodiscard]] bool writePpm(const uint8_t *rgb, size_t size, uint32_t w, uint32_t h,
              const std::string &path, std::string *actualPath = nullptr);
[[nodiscard]] bool writeRaw(const uint8_t *y, size_t ySize, const uint8_t *uv, size_t uvSize,
              const std::string &path, std::string *actualPath = nullptr);
[[nodiscard]] bool writeJpeg(const uint8_t *data, size_t size, const std::string &path,
               std::string *actualPath = nullptr);

// Software JPEG encode from RGB data via libjpeg-turbo.
// quality: 1-100 (default 90). Returns true on success.
[[nodiscard]] bool writeJpegRgb(const uint8_t *rgb, uint32_t w, uint32_t h,
                  const std::string &path, int quality = 90,
                  std::string *actualPath = nullptr);

}
