#pragma once

// Pure-logic image processing helpers for HDR merge, long-exposure noise
// reduction (dark-frame subtraction), film grain overlay, and tall-image
// rotation. These are unit-testable on x86 (no camera/display dependency).

#include <cstdint>
#include <string>
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

// Extract a Y-plane (luminance) from an RGB24 buffer using the BT.601
// formula: Y = (77*R + 150*G + 29*B) / 256. The output Y-plane has the
// given stride (>= width). Returns empty on invalid input.
std::vector<uint8_t> rgb24ToY(const uint8_t *rgb, uint32_t width,
                              uint32_t height, uint32_t stride);

// Build an RGB24 buffer from a Y-plane and a UV (NV12 chroma) plane.
// This is used by HDR merge to reconstruct RGB from a merged Y-plane and
// the chroma from one of the original frames. Returns empty on failure.
std::vector<uint8_t> yuvToRgb24(const uint8_t *y, const uint8_t *uv,
                                uint32_t width, uint32_t height,
                                uint32_t stride);

// Copyright text entry character set: A-Z, 0-9, space, '-', '.', and
// special control markers '<' (backspace) and '>' (done). Returns the
// character at the given index (wrapping). Pure logic for unit testing.
char copyrightCharAt(int index);

// Number of characters in the copyright entry character set.
int copyrightCharCount();

// Cycle the character at the given position in the buffer. If direction
// is +1, advance to the next character; if -1, go to the previous.
// The buffer character at `pos` is replaced. Pure logic for unit testing.
void copyrightCycleChar(std::string &buf, int pos, int direction);

} // namespace picamera
