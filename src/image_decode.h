#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace picamera {

// Decode a JPEG or PNG file to RGB565 (big-endian, for SPI displays),
// scaled to fit dispW x dispH with center-crop + nearest-neighbor.
// Returns an empty vector on failure (file not found, corrupt, unsupported).
// The output is dispW * dispH * 2 bytes, ready to blit to the display.
//
// Path safety: the file is opened with O_NOFOLLOW and non-regular files are
// rejected, but this function does NOT verify containment in a trusted root.
// Callers MUST pass a path produced by safeCapturePath() (or otherwise
// validated) to prevent path traversal. All current callers do this.
std::vector<uint8_t> decodeImageToRgb565(const std::string &path,
                                         uint32_t dispW, uint32_t dispH);

// Decode a JPEG file to RGB24 at native resolution. Returns an empty vector
// on failure. The output is w * h * 3 bytes. Used by HDR merge to decode
// bracket frames for Y-plane extraction.
std::vector<uint8_t> decodeJpegFileToRgb24(const std::string &path,
                                           uint32_t &w, uint32_t &h);

} // namespace picamera
