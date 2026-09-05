#include "image_effects.h"

#include <algorithm>
#include <cstring>

namespace picamera {

std::vector<uint8_t> hdrMergeY(const std::vector<const uint8_t *> &frames,
                               uint32_t width, uint32_t height,
                               uint32_t stride) {
  if (frames.empty() || width == 0 || height == 0)
    return {};
  size_t planeSize = static_cast<size_t>(stride) * height;
  std::vector<uint8_t> result(planeSize);
  for (size_t i = 0; i < planeSize; ++i) {
    uint32_t sum = 0;
    for (const auto *frame : frames) {
      if (!frame)
        return {};
      sum += frame[i];
    }
    result[i] = static_cast<uint8_t>(sum / frames.size());
  }
  return result;
}

std::vector<uint8_t> darkFrameSubtract(const uint8_t *image,
                                       const uint8_t *dark, uint32_t width,
                                       uint32_t height, uint32_t stride) {
  if (!image || !dark || width == 0 || height == 0)
    return {};
  size_t planeSize = static_cast<size_t>(stride) * height;
  std::vector<uint8_t> result(planeSize);
  for (size_t i = 0; i < planeSize; ++i) {
    int val = static_cast<int>(image[i]) - static_cast<int>(dark[i]);
    result[i] = static_cast<uint8_t>(std::clamp(val, 0, 255));
  }
  return result;
}

void applyGrainEffect(uint8_t *yPlane, uint32_t width, uint32_t height,
                      uint32_t stride, int strength, uint32_t seed) {
  if (!yPlane || width == 0 || height == 0 || strength <= 0)
    return;
  // Deterministic LCG pseudo-random generator seeded by pixel position + seed.
  // This avoids needing <random> and produces reproducible grain patterns.
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      size_t idx = static_cast<size_t>(y) * stride + x;
      uint32_t state = seed ^ (x * 2654435761u + y * 40503u);
      state = state * 1103515245u + 12345u;
      int noise = static_cast<int>((state >> 16) & 0xFF) - 128;
      noise = noise * strength / 128;
      int val = static_cast<int>(yPlane[idx]) + noise;
      yPlane[idx] = static_cast<uint8_t>(std::clamp(val, 0, 255));
    }
  }
}

bool isPortrait(uint32_t width, uint32_t height) {
  return height > width;
}

std::vector<uint8_t> rotateRgb565Cw(const uint8_t *src, uint32_t srcW,
                                    uint32_t srcH) {
  if (!src || srcW == 0 || srcH == 0)
    return {};
  // Rotated dimensions: dstW = srcH, dstH = srcW
  std::vector<uint8_t> dst(static_cast<size_t>(srcW) * srcH * 2);
  for (uint32_t y = 0; y < srcH; ++y) {
    for (uint32_t x = 0; x < srcW; ++x) {
      // Source pixel (x, y) → destination pixel (srcH-1-y, x)
      size_t srcIdx = (static_cast<size_t>(y) * srcW + x) * 2;
      size_t dstIdx = (static_cast<size_t>(x) * srcH + (srcH - 1 - y)) * 2;
      dst[dstIdx] = src[srcIdx];
      dst[dstIdx + 1] = src[srcIdx + 1];
    }
  }
  return dst;
}

void applyNightBoost(uint8_t *yPlane, uint32_t width, uint32_t height,
                     uint32_t stride, float factor) {
  if (!yPlane || width == 0 || height == 0 || factor <= 1.0f)
    return;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      size_t idx = static_cast<size_t>(y) * stride + x;
      int val = static_cast<int>(yPlane[idx] * factor);
      yPlane[idx] = static_cast<uint8_t>(std::clamp(val, 0, 255));
    }
  }
}

} // namespace picamera
