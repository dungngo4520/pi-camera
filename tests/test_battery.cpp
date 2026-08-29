#include "test_runner.h"
#include "battery.h"
#include "font.h"

#include <vector>
#include <cstring>
#include <string>

using namespace picamera;

// --- LiPo voltage → SOC tests ---

TEST(lipo_full_voltage_is_100) {
    CHECK_EQ(lipoVoltageToPercent(4.2), 100);
    CHECK_EQ(lipoVoltageToPercent(4.3), 100); // above max clamps to 100
    CHECK_EQ(lipoVoltageToPercent(5.0), 100);
}

TEST(lipo_empty_voltage_is_0) {
    CHECK_EQ(lipoVoltageToPercent(3.0), 0);
    CHECK_EQ(lipoVoltageToPercent(2.9), 0); // below min clamps to 0
    CHECK_EQ(lipoVoltageToPercent(0.0), 0);
}

TEST(lipo_midpoint_curve) {
    // Check known curve points exactly
    CHECK_EQ(lipoVoltageToPercent(4.10), 90);
    CHECK_EQ(lipoVoltageToPercent(4.00), 80);
    CHECK_EQ(lipoVoltageToPercent(3.90), 70);
    CHECK_EQ(lipoVoltageToPercent(3.70), 40);
    CHECK_EQ(lipoVoltageToPercent(3.50), 15);
    CHECK_EQ(lipoVoltageToPercent(3.30), 5);
}

TEST(lipo_monotonic_decreasing) {
    // As voltage decreases, SOC must not increase
    int prev = 101;
    for (double v = 4.2; v >= 3.0; v -= 0.05) {
        int pct = lipoVoltageToPercent(v);
        CHECK(pct <= prev);
        prev = pct;
    }
}

TEST(lipo_interpolation_midpoint) {
    // Midpoint between 4.10V (90%) and 4.00V (80%) → 4.05V should be ~85%
    int pct = lipoVoltageToPercent(4.05);
    CHECK(pct >= 83 && pct <= 87);
}

// --- Font rendering tests ---

TEST(font_draw_text_returns_width) {
    // "85%" = 3 chars × 6px (5+1 spacing) = 18px
    std::vector<uint8_t> fb(128 * 128 * 2, 0);
    int w = drawText(fb.data(), 128, 128, 0, 0, "85%",
                     COLOR_WHITE, COLOR_BLACK, false);
    CHECK_EQ(w, 18);
}

TEST(font_draw_text_writes_pixels) {
    // Drawing "8" at (0,0) with white fg on black bg should produce
    // at least one white pixel in the 5x7 area.
    std::vector<uint8_t> fb(128 * 128 * 2, 0);
    drawText(fb.data(), 128, 128, 0, 0, "8",
             COLOR_WHITE, COLOR_BLACK, false);
    bool foundWhite = false;
    for (int y = 0; y < 7 && !foundWhite; ++y) {
        for (int x = 0; x < 5 && !foundWhite; ++x) {
            size_t idx = (y * 128 + x) * 2;
            uint16_t px = (fb[idx] << 8) | fb[idx + 1];
            if (px == COLOR_WHITE) foundWhite = true;
        }
    }
    CHECK(foundWhite);
}

TEST(font_draw_text_transparent_no_bg) {
    // With transparent=true, background pixels should not be overwritten.
    // Fill fb with a known color, draw text transparently, check that
    // non-glyph pixels retain the original color.
    uint16_t bg = COLOR_GRAY;
    std::vector<uint8_t> fb(128 * 128 * 2);
    for (size_t i = 0; i < 128 * 128; ++i) {
        fb[i * 2]     = bg >> 8;
        fb[i * 2 + 1] = bg & 0xFF;
    }
    drawText(fb.data(), 128, 128, 0, 0, "1",
             COLOR_WHITE, COLOR_BLACK, true);
    // Pixel at (0,0) — row 0 of "1" is 0x04 (only row 2 is set), so (0,0) is bg
    size_t idx = 0;
    uint16_t px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, bg);
}

TEST(font_draw_battery_icon_outline) {
    // Battery icon at (0,0): outline should have white pixels at corners
    std::vector<uint8_t> fb(128 * 128 * 2, 0);
    drawBatteryIcon(fb.data(), 128, 128, 0, 0, 50);
    // Top-left corner of outline
    size_t idx = (0 * 128 + 0) * 2;
    uint16_t px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, COLOR_WHITE);
    // Bottom-left corner
    idx = (8 * 128 + 0) * 2;
    px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, COLOR_WHITE);
}

TEST(font_draw_battery_icon_fill_color) {
    // 50% → yellow fill
    std::vector<uint8_t> fb(128 * 128 * 2, 0);
    drawBatteryIcon(fb.data(), 128, 128, 0, 0, 50);
    // Interior pixel at (1,4) should be yellow (fill area)
    size_t idx = (4 * 128 + 1) * 2;
    uint16_t px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, COLOR_YELLOW);
}

TEST(font_draw_battery_icon_fill_proportional) {
    // 100% → full fill (all 13 interior columns), green
    std::vector<uint8_t> fb(128 * 128 * 2, 0);
    drawBatteryIcon(fb.data(), 128, 128, 0, 0, 100);
    // Last interior column (x=13) should be green
    size_t idx = (4 * 128 + 13) * 2;
    uint16_t px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, COLOR_GREEN);

    // 0% → no fill (all interior black)
    std::fill(fb.begin(), fb.end(), 0);
    drawBatteryIcon(fb.data(), 128, 128, 0, 0, 0);
    idx = (4 * 128 + 1) * 2;
    px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, COLOR_BLACK);
}

TEST(font_draw_battery_icon_clamps) {
    // Percent > 100 should clamp to 100 (full green fill)
    std::vector<uint8_t> fb(128 * 128 * 2, 0);
    drawBatteryIcon(fb.data(), 128, 128, 0, 0, 150);
    size_t idx = (4 * 128 + 13) * 2;
    uint16_t px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, COLOR_GREEN);

    // Percent < 0 should clamp to 0 (no fill)
    std::fill(fb.begin(), fb.end(), 0);
    drawBatteryIcon(fb.data(), 128, 128, 0, 0, -10);
    idx = (4 * 128 + 1) * 2;
    px = (fb[idx] << 8) | fb[idx + 1];
    CHECK_EQ(px, COLOR_BLACK);
}

TEST(font_rgb565_color_encoding) {
    // Verify color constants are correct RGB565
    CHECK_EQ(COLOR_WHITE, 0xFFFF);
    CHECK_EQ(COLOR_BLACK, 0x0000);
    // Green: R=0, G=200, B=0 → RGB565 = (0<<11)|(200>>2<<5)|(0)
    // G=200 → 200>>2 = 50 → 50<<5 = 0x0640
    CHECK_EQ(COLOR_GREEN, 0x0640);
}
