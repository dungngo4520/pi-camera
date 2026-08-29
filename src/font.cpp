#include "font.h"

#include <cstring>
#include <algorithm>

namespace picamera {

// --- 5x7 bitmap font ---
// Each glyph: 5 bytes, one per column. Bit 0 = top row, bit 6 = bottom row.
// Bit 7 is unused. Rows are top-to-bottom.
//
// This is a compact subset of the classic 5x7 LCD font, sufficient for
// battery percentage display: digits, %, V, ., -, +, space, A-Z.

// Glyph entry: character + 5 column bytes
struct Glyph {
    char ch;
    uint8_t cols[5];
};

static const Glyph glyphTable[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x0E, 0x04, 0x00}},
    {'-', {0x00, 0x00, 0x0E, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x06}},
    {'%', {0x0C, 0x12, 0x04, 0x09, 0x12}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x08, 0x04, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x08, 0x1E}},
    {'4', {0x08, 0x0C, 0x14, 0x1F, 0x04}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x1E}},
    {'6', {0x06, 0x08, 0x1E, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08}},
    {'8', {0x0E, 0x11, 0x0E, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x1F, 0x02, 0x04}},
    {'V', {0x1C, 0x04, 0x04, 0x04, 0x1C}},
    {'A', {0x0E, 0x11, 0x1F, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x1E, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x1E, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x1E, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x0E}},
    {'H', {0x11, 0x11, 0x1F, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x01, 0x01, 0x01, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x1C, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x1E, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x1E, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x0E, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0E, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x06, 0x08, 0x1F}},
};

static const int glyphTableLen = sizeof(glyphTable) / sizeof(glyphTable[0]);

namespace {

const uint8_t *findGlyph(char ch) {
    // Convert lowercase to uppercase
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    for (int i = 0; i < glyphTableLen; ++i) {
        if (glyphTable[i].ch == ch) return glyphTable[i].cols;
    }
    // Unknown char: return space
    return glyphTable[0].cols;
}

// Write a single RGB565 pixel (big-endian, for SPI display) at (x, y)
inline void setPixel(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                     int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= fbW ||
        static_cast<uint32_t>(y) >= fbH) return;
    size_t idx = (static_cast<size_t>(y) * fbW + x) * 2;
    rgb565[idx]     = static_cast<uint8_t>(color >> 8);   // MSB
    rgb565[idx + 1] = static_cast<uint8_t>(color & 0xFF); // LSB
}

} // namespace

void drawChar(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
              int x, int y, char ch, uint16_t fg, uint16_t bg,
              bool transparent) {
    const uint8_t *glyph = findGlyph(ch);
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            bool on = (bits >> row) & 1;
            uint16_t color = on ? fg : bg;
            if (on || !transparent) {
                setPixel(rgb565, fbW, fbH, x + col, y + row, color);
            }
        }
    }
}

int drawText(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
             int x, int y, const std::string &text, uint16_t fg, uint16_t bg,
             bool transparent) {
    int cx = x;
    for (char ch : text) {
        drawChar(rgb565, fbW, fbH, cx, y, ch, fg, bg, transparent);
        cx += 6; // 5px glyph + 1px spacing
    }
    return cx - x;
}

void drawBatteryIcon(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                     int x, int y, int percent) {
    // Battery icon: 16px wide body + 2px terminal nub = 18px total, 9px tall
    // Body: x to x+15, y to y+8
    // Terminal: x+16 to x+17, y+2 to y+6

    // Clamp percent
    percent = std::max(0, std::min(100, percent));

    // Fill color based on charge level
    uint16_t fillColor;
    if (percent > 50) fillColor = COLOR_GREEN;
    else if (percent > 20) fillColor = COLOR_YELLOW;
    else fillColor = COLOR_RED;

    // Draw outline (1px border)
    for (int dx = 0; dx < 16; ++dx) {
        setPixel(rgb565, fbW, fbH, x + dx, y, COLOR_WHITE);
        setPixel(rgb565, fbW, fbH, x + dx, y + 8, COLOR_WHITE);
    }
    for (int dy = 0; dy < 9; ++dy) {
        setPixel(rgb565, fbW, fbH, x, y + dy, COLOR_WHITE);
        setPixel(rgb565, fbW, fbH, x + 15, y + dy, COLOR_WHITE);
    }

    // Draw terminal nub
    for (int dy = 2; dy <= 6; ++dy) {
        setPixel(rgb565, fbW, fbH, x + 16, y + dy, COLOR_WHITE);
        setPixel(rgb565, fbW, fbH, x + 17, y + dy, COLOR_WHITE);
    }

    // Draw fill bar (inside the outline: x+1 to x+14, y+1 to y+7)
    // Fill width proportional to percent
    int fillW = (13 * percent) / 100; // max 13px interior width
    for (int dx = 0; dx < fillW; ++dx) {
        for (int dy = 1; dy <= 7; ++dy) {
            setPixel(rgb565, fbW, fbH, x + 1 + dx, y + dy, fillColor);
        }
    }

    // Clear unfilled portion to black
    for (int dx = fillW; dx < 13; ++dx) {
        for (int dy = 1; dy <= 7; ++dy) {
            setPixel(rgb565, fbW, fbH, x + 1 + dx, y + dy, COLOR_BLACK);
        }
    }
}

} // namespace picamera
