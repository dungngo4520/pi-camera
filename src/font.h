#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace picamera {

// Monospace font rendering for RGB565 display overlays.
//
// Uses DejaVu Sans Mono (or Liberation Mono / Noto Sans Mono as fallback)
// rendered at 8px pixel size with anti-aliasing. Glyphs are cached on first
// use. Supports full ASCII.
//
// FreeType is a required build dependency (linked via CMake
// pkg_check_modules for freetype2).

// Draw a single character at (x, y) on an RGB565 framebuffer.
// fg = foreground color (RGB565), bg = background color (RGB565, or pass
// 0xFFFF with transparent=true for no background fill).
// fbW/fbH = framebuffer dimensions.
void drawChar(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
              char ch, uint16_t fg, uint16_t bg, bool transparent = false);

// Draw a single character at (x, y) with integer scale factor.
// Each font pixel becomes a `scale`x`scale` block. Used for large
// countdown numbers and other prominent text.
void drawCharScaled(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                    char ch, int scale, uint16_t fg, uint16_t bg,
                    bool transparent = false);

// Draw a string at (x, y). Returns the width in pixels consumed.
int drawText(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
             std::string_view text, uint16_t fg, uint16_t bg,
             bool transparent = false);

// Draw a battery icon at (x, y) with the given fill percentage (0-100).
// iconW x iconH = 18x9 pixels (battery body + terminal nub).
// fill color: green >50%, yellow 20-50%, red <20%.
void drawBatteryIcon(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                     int percent);

// RGB565 color helpers
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                               (b >> 3));
}

constexpr uint16_t kColorWhite = 0xFFFF;
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorGreen = rgb565(0, 200, 0);
constexpr uint16_t kColorYellow = rgb565(220, 200, 0);
constexpr uint16_t kColorRed = rgb565(220, 0, 0);
constexpr uint16_t kColorGray = rgb565(120, 120, 120);

} // namespace picamera
