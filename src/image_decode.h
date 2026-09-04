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

} // namespace picamera
