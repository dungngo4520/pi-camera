#pragma once

#include <cstdint>
#include <string>

namespace picamera {

// Minimal 5x7 monospace bitmap font for RGB565 display overlays.
// Supports digits 0-9, '%', 'V', '.', '-', '+', space, and 'A'-'Z' (uppercase).
// Each glyph is 5 columns wide, 7 rows tall. Columns are packed into bytes
// (one byte per column, LSB = top row).

// Draw a single character at (x, y) on an RGB565 framebuffer.
// fg = foreground color (RGB565), bg = background color (RGB565, or pass
// 0xFFFF with transparent=true for no background fill).
// fbW/fbH = framebuffer dimensions.
void drawChar(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
              int x, int y, char ch, uint16_t fg, uint16_t bg,
              bool transparent = false);

// Draw a string at (x, y). Returns the width in pixels consumed.
int drawText(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
             int x, int y, const std::string &text, uint16_t fg, uint16_t bg,
             bool transparent = false);

// Draw a battery icon at (x, y) with the given fill percentage (0-100).
// iconW x iconH = 18x9 pixels (battery body + terminal nub).
// fill color: green >50%, yellow 20-50%, red <20%.
void drawBatteryIcon(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                     int x, int y, int percent);

// RGB565 color helpers
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) |
                                 ((g & 0xFC) << 3) |
                                 (b >> 3));
}

constexpr uint16_t COLOR_WHITE  = 0xFFFF;
constexpr uint16_t COLOR_BLACK  = 0x0000;
constexpr uint16_t COLOR_GREEN  = rgb565(0, 200, 0);
constexpr uint16_t COLOR_YELLOW = rgb565(220, 200, 0);
constexpr uint16_t COLOR_RED    = rgb565(220, 0, 0);
constexpr uint16_t COLOR_GRAY   = rgb565(120, 120, 120);

} // namespace picamera
