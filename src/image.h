#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace picamera {

// Convert NV12 to RGB24. ySize/uvSize are the sizes of the Y and UV planes
// (in bytes) and are validated against the expected plane sizes to prevent
// out-of-bounds reads. Returns empty vector on validation failure.
std::vector<uint8_t> nv12ToRgb(const uint8_t *y, const uint8_t *uv,
                                uint32_t w, uint32_t h, uint32_t stride,
                                size_t ySize, size_t uvSize);

// Convert NV12 to RGB565 (big-endian, for SPI displays) with center-crop
// and nearest-neighbor scaling to dispW x dispH.
// ySize/uvSize are the sizes of the Y and UV planes (in bytes) and are
// validated to prevent out-of-bounds reads.
// outSize must be >= dispW * dispH * 2; returns false if any validation fails.
bool nv12ToRgb565Scaled(const uint8_t *y, const uint8_t *uv,
                        uint32_t srcW, uint32_t srcH, uint32_t stride,
                        size_t ySize, size_t uvSize,
                        uint8_t *out, uint32_t dispW, uint32_t dispH,
                        size_t outSize);

}
