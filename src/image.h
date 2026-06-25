#pragma once

#include <cstdint>
#include <vector>

namespace picamera {

std::vector<uint8_t> nv12ToRgb(const uint8_t *y, const uint8_t *uv,
                                uint32_t w, uint32_t h, uint32_t stride);

}
