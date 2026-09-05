#pragma once

// Pure-logic image processing helpers for HDR merge, long-exposure noise
// reduction (dark-frame subtraction), film grain overlay, and tall-image
// rotation. These are unit-testable on x86 (no camera/display dependency).

#include <cstdint>
#include <vector>

namespace picamera {

// HDR merge: average corresponding Y-plane pixels from multiple frames.
// Simple exposure fusion — all frames must have the same dimensions.
// Returns the merged Y-plane. Returns empty on dimension mismatch.
std::vector<uint8_t> hdrMergeY(const std::vector<const uint8_t *> &frames,
                               uint32_t width, uint32_t height,
                               uint32_t stride);

// Long-exposure noise reduction: subtract a dark frame from the image.
// Both must have the same dimensions. The result is clamped to [0, 255].
// Returns the corrected Y-plane. Returns empty on dimension mismatch.
std::vector<uint8_t> darkFrameSubtract(const uint8_t *image,
                                       const uint8_t *dark, uint32_t width,
                                       uint32_t height, uint32_t stride);

// Apply film grain noise to a Y-plane. Uses a simple deterministic pseudo-
// random pattern seeded by pixel position + seed. Strength controls the
// maximum noise amplitude (0-50 typical). Modifies the buffer in place.
void applyGrainEffect(uint8_t *yPlane, uint32_t width, uint32_t height,
                      uint32_t stride, int strength, uint32_t seed);

// Check if an image is portrait (taller than wide).
bool isPortrait(uint32_t width, uint32_t height);

// Rotate an RGB565 buffer 90 degrees clockwise. srcW/srcH are the source
// dimensions. Returns the rotated buffer. Used for tall-image playback.
std::vector<uint8_t> rotateRgb565Cw(const uint8_t *src, uint32_t srcW,
                                    uint32_t srcH);

// Apply night-mode brightness boost to a Y-plane. Multiplies Y values by
// the given factor (e.g., 2.0 for 2x boost), clamping to 255. Modifies
// the buffer in place.
void applyNightBoost(uint8_t *yPlane, uint32_t width, uint32_t height,
                     uint32_t stride, float factor);

} // namespace picamera
