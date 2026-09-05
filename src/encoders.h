#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace picamera {

// EXIF metadata for JPEG/PNG output. Mirrors the subset of DngMetadata
// that can be embedded in JPEG APP1/EXIF or PNG tEXt chunks.
struct ExifMetadata {
  uint64_t exposureTimeUs = 0; // exposure time in microseconds
  float analogueGain = 0;      // ISO = gain * 100
  uint32_t timestampSec = 0;   // Unix timestamp for DateTime tag
  uint32_t width = 0;          // image width (optional, for reference)
  uint32_t height = 0;         // image height (optional, for reference)
  int colorSpace = 0;          // 0 = sRGB, 1 = AdobeRGB (EXIF ColorSpace tag)
  std::string copyright;       // EXIF Copyright tag (empty = omitted)
};

// Build a complete EXIF APP1 data buffer (starting with "Exif\0\0" +
// TIFF header + IFD0 + ExifIFD) from the given metadata. Pure logic —
// no libjpeg dependency, unit-testable on x86. Returns the byte buffer
// suitable for passing to jpeg_write_marker() as the data payload
// (the marker code 0xE1 and 2-byte length are handled by libjpeg).
std::vector<uint8_t> buildExifData(const ExifMetadata &meta);

// All writers open the file with O_CREAT|O_EXCL|O_NOFOLLOW. If the requested
// path already exists (collision), they retry with _2, _3, ... suffixes via
// the atomic safeFileOpen/safeFileOpenFd helpers (no lstat probe, so no
// TOCTOU race). On success, if `actualPath` is non-null it is set to the
// path actually written to (which may differ from `path` when a suffix was
// needed). On failure, `actualPath` is left unmodified and no partial file
// is left behind (the file is unlinked on write failure).
[[nodiscard]] bool writePng(const std::string &path, const uint8_t *rgb,
                            uint32_t w, uint32_t h, int compressionLevel = 6,
                            std::string *actualPath = nullptr,
                            const ExifMetadata *meta = nullptr);
[[nodiscard]] bool writePpm(const uint8_t *rgb, size_t size, uint32_t w,
                            uint32_t h, const std::string &path,
                            std::string *actualPath = nullptr);
[[nodiscard]] bool writeRaw(const uint8_t *y, size_t ySize, const uint8_t *uv,
                            size_t uvSize, const std::string &path,
                            std::string *actualPath = nullptr);
// Write a hardware-encoded JPEG (MJPEG) bitstream directly to disk.
// EXIF metadata cannot be injected without re-encoding the bitstream —
// the ISP produces a complete JPEG with its own headers. To embed EXIF
// in JPEGs, use writeJpegRgb() (software encode) instead.
[[nodiscard]] bool writeJpeg(const uint8_t *data, size_t size,
                             const std::string &path,
                             std::string *actualPath = nullptr);

// Software JPEG encode from RGB data via libjpeg-turbo.
// quality: 1-100 (default 90). Returns true on success.
// If `meta` is non-null, EXIF metadata is embedded via an APP1 marker.
[[nodiscard]] bool writeJpegRgb(const uint8_t *rgb, uint32_t w, uint32_t h,
                                const std::string &path, int quality = 90,
                                std::string *actualPath = nullptr,
                                const ExifMetadata *meta = nullptr);

} // namespace picamera
