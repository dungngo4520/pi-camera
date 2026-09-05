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

uint8_t estimateBlackLevel(const uint8_t *y, uint32_t width, uint32_t height,
                           uint32_t stride, uint32_t borderWidth) {
  if (!y || width == 0 || height == 0 || stride < width)
    return 0;
  if (borderWidth == 0 || borderWidth * 2 >= width || borderWidth * 2 >= height)
    return 0;
  uint64_t sum = 0;
  uint64_t count = 0;
  // Top and bottom border rows.
  for (uint32_t row = 0; row < borderWidth; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      sum += y[static_cast<size_t>(row) * stride + col];
      ++count;
    }
  }
  for (uint32_t row = height - borderWidth; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      sum += y[static_cast<size_t>(row) * stride + col];
      ++count;
    }
  }
  // Left and right border columns (excluding corners already counted).
  for (uint32_t row = borderWidth; row < height - borderWidth; ++row) {
    for (uint32_t col = 0; col < borderWidth; ++col) {
      sum += y[static_cast<size_t>(row) * stride + col];
      ++count;
    }
    for (uint32_t col = width - borderWidth; col < width; ++col) {
      sum += y[static_cast<size_t>(row) * stride + col];
      ++count;
    }
  }
  if (count == 0)
    return 0;
  return static_cast<uint8_t>(sum / count);
}

std::vector<uint8_t> subtractBlackLevel(const uint8_t *y, uint32_t width,
                                        uint32_t height, uint32_t stride,
                                        uint8_t blackLevel) {
  if (!y || width == 0 || height == 0 || stride < width)
    return {};
  size_t planeSize = static_cast<size_t>(stride) * height;
  std::vector<uint8_t> result(planeSize);
  for (size_t i = 0; i < planeSize; ++i) {
    int val = static_cast<int>(y[i]) - static_cast<int>(blackLevel);
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

std::vector<uint8_t> rgb24ToUv(const uint8_t *rgb, uint32_t width,
                               uint32_t height) {
  if (!rgb || width == 0 || height == 0 || (width & 1) || (height & 1))
    return {};
  uint32_t halfW = width / 2;
  uint32_t halfH = height / 2;
  std::vector<uint8_t> uv(static_cast<size_t>(halfW) * halfH * 2);
  for (uint32_t cy = 0; cy < halfH; ++cy) {
    for (uint32_t cx = 0; cx < halfW; ++cx) {
      // Average the 2x2 block of RGB pixels.
      int rSum = 0;
      int gSum = 0;
      int bSum = 0;
      for (uint32_t dy = 0; dy < 2; ++dy) {
        for (uint32_t dx = 0; dx < 2; ++dx) {
          size_t idx = (static_cast<size_t>(cy * 2 + dy) * width +
                        (cx * 2 + dx)) *
                       3;
          rSum += rgb[idx];
          gSum += rgb[idx + 1];
          bSum += rgb[idx + 2];
        }
      }
      int r = rSum / 4;
      int g = gSum / 4;
      int b = bSum / 4;
      int u = (-38 * r - 74 * g + 112 * b) / 256 + 128;
      int v = (112 * r - 94 * g - 18 * b) / 256 + 128;
      size_t uvIdx = (static_cast<size_t>(cy) * halfW + cx) * 2;
      uv[uvIdx] = static_cast<uint8_t>(std::clamp(u, 0, 255));
      uv[uvIdx + 1] = static_cast<uint8_t>(std::clamp(v, 0, 255));
    }
  }
  return uv;
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

std::vector<uint8_t> scaleRgb24Bilinear(const uint8_t *src, uint32_t srcW,
                                        uint32_t srcH, uint32_t outW,
                                        uint32_t outH) {
  if (!src || srcW == 0 || srcH == 0 || outW == 0 || outH == 0)
    return {};
  std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 3);
  // Identity-size copy (fast path).
  if (outW == srcW && outH == srcH) {
    std::memcpy(out.data(), src, out.size());
    return out;
  }
  for (uint32_t y = 0; y < outH; ++y) {
    // Map output row to source coordinate in [0, srcH-1].
    float sy = (outH == 1) ? 0.0f
                           : static_cast<float>(y) * (srcH - 1) / (outH - 1);
    uint32_t y0 = static_cast<uint32_t>(sy);
    uint32_t y1 = std::min(y0 + 1, srcH - 1);
    float fy = sy - static_cast<float>(y0);
    for (uint32_t x = 0; x < outW; ++x) {
      float sx = (outW == 1) ? 0.0f
                             : static_cast<float>(x) * (srcW - 1) / (outW - 1);
      uint32_t x0 = static_cast<uint32_t>(sx);
      uint32_t x1 = std::min(x0 + 1, srcW - 1);
      float fx = sx - static_cast<float>(x0);
      for (int c = 0; c < 3; ++c) {
        size_t i00 = (static_cast<size_t>(y0) * srcW + x0) * 3 + c;
        size_t i10 = (static_cast<size_t>(y0) * srcW + x1) * 3 + c;
        size_t i01 = (static_cast<size_t>(y1) * srcW + x0) * 3 + c;
        size_t i11 = (static_cast<size_t>(y1) * srcW + x1) * 3 + c;
        float top = src[i00] * (1.0f - fx) + src[i10] * fx;
        float bot = src[i01] * (1.0f - fx) + src[i11] * fx;
        float val = top * (1.0f - fy) + bot * fy;
        out[(static_cast<size_t>(y) * outW + x) * 3 + c] =
            static_cast<uint8_t>(std::clamp(val + 0.5f, 0.0f, 255.0f));
      }
    }
  }
  return out;
}

} // namespace picamera
