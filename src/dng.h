#pragma once

#include <cstdint>
#include <string>

namespace picamera {

// DNG metadata extracted from libcamera properties + capture controls.
struct DngMetadata {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerPixel = 12;       // 10, 12, 14, or 16
    uint32_t blackLevel = 0;          // sensor black level (per-channel)
    uint32_t whiteLevel = 0;          // sensor white level (saturation)
    char bayerPattern[4] = {'R','G','G','B'};  // CFA pattern (RGGB, GRBG, etc.)
    uint32_t activeTop = 0;           // active area crop (top, left, bottom, right)
    uint32_t activeLeft = 0;
    uint32_t activeBottom = 0;
    uint32_t activeRight = 0;
    // EXIF metadata
    uint64_t exposureTimeUs = 0;      // exposure time in microseconds
    float analogueGain = 0;           // ISO = gain * 100
    float digitalGain = 0;
    uint32_t isoSpeed = 0;            // computed ISO = analogueGain * 100
    uint32_t timestampSec = 0;        // Unix timestamp for DateTime tag
};

// Write a minimal DNG (Digital Negative) file from raw Bayer data.
// The raw data is expected to be unpacked 16-bit samples (one per pixel),
// in the Bayer pattern specified by metadata.bayerPattern.
// Returns true on success.
bool writeDng(const char *path, const uint8_t *rawData, size_t rawSize,
              const DngMetadata &meta);

} // namespace picamera
