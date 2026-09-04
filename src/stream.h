#pragma once

#include <cstdint>
#include <vector>

namespace picamera {

// A frame grabbed from the camera stream (NV12 format).
// Owns its pixel data so the caller can use it safely without holding
// the stream's internal mutex — the callback can overwrite the stream's
// internal buffer at any time.
struct StreamFrame {
    std::vector<uint8_t> yData;
    std::vector<uint8_t> uvData;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;

    const uint8_t *y() const { return yData.empty() ? nullptr : yData.data(); }
    const uint8_t *uv() const { return uvData.empty() ? nullptr : uvData.data(); }
};

} // namespace picamera
