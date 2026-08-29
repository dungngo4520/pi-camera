#pragma once

#include <cstdint>
#include <vector>

namespace picamera {

std::vector<uint8_t> nv12ToRgb(const uint8_t *y, const uint8_t *uv,
                                uint32_t w, uint32_t h, uint32_t stride);

// Convert NV12 to RGB565 (big-endian, for SPI displays) with center-crop
// and nearest-neighbor scaling to dispW x dispH.
// Output buffer must be dispW * dispH * 2 bytes.
void nv12ToRgb565Scaled(const uint8_t *y, const uint8_t *uv,
                        uint32_t srcW, uint32_t srcH, uint32_t stride,
                        uint8_t *out, uint32_t dispW, uint32_t dispH);

}
