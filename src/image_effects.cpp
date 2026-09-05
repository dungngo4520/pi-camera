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

std::vector<uint8_t> rgb24ToY(const uint8_t *rgb, uint32_t width,
                              uint32_t height, uint32_t stride) {
  if (!rgb || width == 0 || height == 0 || stride < width)
    return {};
  std::vector<uint8_t> y(static_cast<size_t>(stride) * height);
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      size_t rgbIdx = (static_cast<size_t>(row) * width + col) * 3;
      int r = rgb[rgbIdx];
      int g = rgb[rgbIdx + 1];
      int b = rgb[rgbIdx + 2];
      y[static_cast<size_t>(row) * stride + col] =
          static_cast<uint8_t>((77 * r + 150 * g + 29 * b) / 256);
    }
  }
  return y;
}

std::vector<uint8_t> yuvToRgb24(const uint8_t *y, const uint8_t *uv,
                                uint32_t width, uint32_t height,
                                uint32_t stride) {
  if (!y || !uv || width == 0 || height == 0 || stride < width)
    return {};
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      size_t yIdx = static_cast<size_t>(row) * stride + col;
      int yVal = y[yIdx];
      size_t uvIdx = (static_cast<size_t>(row / 2) * (width / 2) + col / 2) * 2;
      int uVal = uv[uvIdx] - 128;
      int vVal = uv[uvIdx + 1] - 128;
      int r = yVal + static_cast<int>(1.402f * vVal);
      int g = yVal - static_cast<int>(0.344f * uVal + 0.714f * vVal);
      int b = yVal + static_cast<int>(1.772f * uVal);
      size_t rgbIdx = (static_cast<size_t>(row) * width + col) * 3;
      rgb[rgbIdx] = static_cast<uint8_t>(std::clamp(r, 0, 255));
      rgb[rgbIdx + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
      rgb[rgbIdx + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
    }
  }
  return rgb;
}

// Copyright text entry character set.
// Index 0-25: A-Z, 26-35: 0-9, 36: space, 37: '-', 38: '.'
static constexpr char kCopyrightChars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.";

char copyrightCharAt(int index) {
  constexpr int n = sizeof(kCopyrightChars) - 1; // exclude null terminator
  if (n == 0)
    return ' ';
  index = ((index % n) + n) % n; // wrap to [0, n)
  return kCopyrightChars[index];
}

int copyrightCharCount() {
  return static_cast<int>(sizeof(kCopyrightChars) - 1);
}

void copyrightCycleChar(std::string &buf, int pos, int direction) {
  if (pos < 0 || pos >= static_cast<int>(buf.size()))
    return;
  // Find current character index in the set. Default to 0 if not found.
  char current = buf[pos];
  int idx = 0;
  bool found = false;
  for (int i = 0; i < copyrightCharCount(); ++i) {
    if (kCopyrightChars[i] == current) {
      idx = i;
      found = true;
      break;
    }
  }
  if (!found)
    idx = 0;
  idx += direction;
  buf[pos] = copyrightCharAt(idx);
}

} // namespace picamera
