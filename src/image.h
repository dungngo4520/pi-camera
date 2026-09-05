#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace picamera {

// Convert NV12 to RGB24. ySize/uvSize are the sizes of the Y and UV planes
// (in bytes) and are validated against the expected plane sizes to prevent
// out-of-bounds reads. Returns empty vector on validation failure.
std::vector<uint8_t> nv12ToRgb(const uint8_t *y, const uint8_t *uv, uint32_t w,
                               uint32_t h, uint32_t stride, size_t ySize,
                               size_t uvSize);

// Convert NV12 to RGB565 (big-endian, for SPI displays) with center-crop
// and nearest-neighbor scaling to dispW x dispH.
// ySize/uvSize are the sizes of the Y and UV planes (in bytes) and are
// validated to prevent out-of-bounds reads.
// outSize must be >= dispW * dispH * 2; returns false if any validation fails.
bool nv12ToRgb565Scaled(const uint8_t *y, const uint8_t *uv, uint32_t srcW,
                        uint32_t srcH, uint32_t stride, size_t ySize,
                        size_t uvSize, uint8_t *out, uint32_t dispW,
                        uint32_t dispH, size_t outSize);

// NV12 -> RGB565 with an explicit source crop region (srcX, srcY, cropW,
// cropH) and nearest-neighbor scaling to dispW x dispH. Used by the focus
// magnifier to zoom into the center of the viewfinder. cropX/cropY must be
// even (NV12 chroma is 2x2 subsampled). Returns false on validation failure.
bool nv12ToRgb565CroppedScaled(const uint8_t *y, const uint8_t *uv,
                               uint32_t srcW, uint32_t srcH, uint32_t stride,
                               size_t ySize, size_t uvSize, uint32_t cropX,
                               uint32_t cropY, uint32_t cropW, uint32_t cropH,
                               uint8_t *out, uint32_t dispW, uint32_t dispH,
                               size_t outSize);

// Downscale an NV12 frame by an integer factor (2 or 4) using 2x2 / 4x4
// block averaging for the Y plane and the same for the UV plane.
// Returns the downscaled frame as packed NV12 (Y then UV, contiguous,
// stride == outW). Returns empty on validation failure.
// outW = srcW / factor (rounded down to even), outH = srcH / factor.
std::vector<uint8_t> downscaleNv12(const uint8_t *y, const uint8_t *uv,
                                   uint32_t srcW, uint32_t srcH,
                                   uint32_t stride, size_t ySize, size_t uvSize,
                                   int factor);

// Crop an NV12 frame to the given region (cropX, cropY, cropW, cropH).
// cropX/cropY must be even (NV12 chroma 2x2 subsampling). Returns the cropped
// frame as packed NV12 (Y then UV, stride == cropW). Returns empty on failure.
std::vector<uint8_t> cropNv12(const uint8_t *y, const uint8_t *uv,
                              uint32_t srcW, uint32_t srcH, uint32_t stride,
                              size_t ySize, size_t uvSize, uint32_t cropX,
                              uint32_t cropY, uint32_t cropW, uint32_t cropH);

} // namespace picamera
