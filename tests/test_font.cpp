#include "font.h"
#include "test_runner.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace picamera;

namespace {

// drawCharScaled at scale=1 should produce the same output as drawChar.
TEST(draw_char_scaled_1x_matches_draw_char) {
  constexpr uint32_t W = 16;
  constexpr uint32_t H = 16;
  uint8_t buf1[W * H * 2] = {0};
  uint8_t buf2[W * H * 2] = {0};

  drawChar(buf1, W, H, 2, 2, 'A', kColorWhite, kColorBlack, false);
  drawCharScaled(buf2, W, H, 2, 2, 'A', 1, kColorWhite, kColorBlack, false);

  CHECK(std::memcmp(buf1, buf2, sizeof(buf1)) == 0);
}

// drawCharScaled at scale=2 should produce 2x2 blocks for each set pixel.
// We verify by checking that the glyph area is larger (more non-black pixels).
TEST(draw_char_scaled_2x_produces_larger_glyph) {
  constexpr uint32_t W = 32;
  constexpr uint32_t H = 32;
  uint8_t buf1x[W * H * 2] = {0};
  uint8_t buf2x[W * H * 2] = {0};

  drawCharScaled(buf1x, W, H, 0, 0, '8', 1, kColorWhite, kColorBlack, true);
  drawCharScaled(buf2x, W, H, 0, 0, '8', 2, kColorWhite, kColorBlack, true);

  int count1x = 0;
  int count2x = 0;
  for (size_t i = 0; i < sizeof(buf1x); i += 2) {
    if (buf1x[i] != 0 || buf1x[i + 1] != 0)
      ++count1x;
    if (buf2x[i] != 0 || buf2x[i + 1] != 0)
      ++count2x;
  }
  // 2x scale should have ~4x the pixels
  CHECK(count2x > count1x * 3);
  CHECK(count2x < count1x * 5);
}

// drawCharScaled with scale=0 should draw nothing.
TEST(draw_char_scaled_0_draws_nothing) {
  constexpr uint32_t W = 16;
  constexpr uint32_t H = 16;
  uint8_t buf[W * H * 2] = {0};
  drawCharScaled(buf, W, H, 0, 0, 'A', 0, kColorWhite, kColorBlack, false);
  for (size_t i = 0; i < sizeof(buf); ++i)
    CHECK(buf[i] == 0);
}

// drawChar with transparent=true should only set foreground pixels,
// leaving background pixels untouched.
TEST(draw_char_transparent_leaves_background_untouched) {
  constexpr uint32_t W = 16;
  constexpr uint32_t H = 16;
  uint8_t buf[W * H * 2];
  // Pre-fill with a sentinel color
  for (size_t i = 0; i < sizeof(buf); i += 2) {
    buf[i] = 0x12;
    buf[i + 1] = 0x34;
  }
  drawChar(buf, W, H, 2, 2, 'A', kColorWhite, kColorBlack, true);
  // Check that some pixels changed (foreground = white) and some
  // didn't (background = sentinel 0x1234). 'A' has both set and
  // unset pixels in its 5x7 area.
  bool hasChanged = false;
  bool hasUnchanged = false;
  for (int y = 0; y < (int)H; ++y) {
    for (int x = 0; x < (int)W; ++x) {
      size_t idx = (y * W + x) * 2;
      if (buf[idx] == 0xFF && buf[idx + 1] == 0xFF)
        hasChanged = true;
      else if (buf[idx] == 0x12 && buf[idx + 1] == 0x34)
        hasUnchanged = true;
    }
  }
  CHECK(hasChanged);
  CHECK(hasUnchanged);
}

// drawChar with negative coordinates should clip safely (no crash, no write
// outside the buffer).
TEST(draw_char_negative_coords_no_crash) {
  constexpr uint32_t W = 16;
  constexpr uint32_t H = 16;
  uint8_t buf[W * H * 2] = {0};
  // Should not crash or write out of bounds
  drawChar(buf, W, H, -3, -3, 'A', kColorWhite, kColorBlack, false);
  drawChar(buf, W, H, -10, -10, 'Z', kColorWhite, kColorBlack, false);
  // Verify no out-of-bounds writes by checking buffer is still valid
  // (some pixels that are in-bounds from the negative draw should be set)
  bool hasPixel = false;
  for (size_t i = 0; i < sizeof(buf); i += 2) {
    if (buf[i] != 0 || buf[i + 1] != 0) {
      hasPixel = true;
      break;
    }
  }
  // With FreeType at 8px, 'A' at (-3,-3) should have some in-bounds pixels.
  // With bitmap font, col 3,4 are in-bounds. Either way, should have pixels.
  CHECK(hasPixel);
}

// drawChar with coordinates beyond the buffer should clip safely.
TEST(draw_char_beyond_buffer_no_crash) {
  constexpr uint32_t W = 16;
  constexpr uint32_t H = 16;
  uint8_t buf[W * H * 2] = {0};
  drawChar(buf, W, H, 100, 100, 'A', kColorWhite, kColorBlack, false);
  // Nothing should be written
  for (size_t i = 0; i < sizeof(buf); ++i)
    CHECK(buf[i] == 0);
}

// Unknown glyph (e.g., '@') should not crash. With the bitmap font,
// unknown chars fall back to space (all zeros). With FreeType, the
// font's .notdef glyph is rendered (may be a box). Either way, it
// should not crash and should produce some output (or blank).
TEST(draw_char_unknown_glyph_no_crash) {
  constexpr uint32_t W = 16;
  constexpr uint32_t H = 16;
  uint8_t buf[W * H * 2] = {0};
  drawChar(buf, W, H, 0, 0, '@', kColorWhite, kColorBlack, false);
  // Should not crash. No specific pixel assertions — behavior differs
  // between FreeType (.notdef glyph) and bitmap fallback (space).
}

// drawText should return the total width consumed (advance * num chars).
TEST(draw_text_returns_width) {
  constexpr uint32_t W = 128;
  constexpr uint32_t H = 32;
  uint8_t buf[W * H * 2] = {0};
  int w = drawText(buf, W, H, 0, 0, "ABC", kColorWhite, kColorBlack, false);
  // With FreeType, advance is the font's monospace advance width (~5px at 8pt).
  // With bitmap fallback, advance is 6px. Either way, 3 chars should produce
  // a positive width that's 3 * advance.
  CHECK(w > 0);
  CHECK(w == 3 * (w / 3)); // divisible by 3 (monospace)
}

// drawText at the right edge should clip safely without crashing.
TEST(draw_text_right_edge_clips_safely) {
  constexpr uint32_t W = 10;
  constexpr uint32_t H = 16;
  uint8_t buf[W * H * 2] = {0};
  // "ABC" needs 18px but buffer is only 10px wide — should clip
  drawText(buf, W, H, 0, 0, "ABC", kColorWhite, kColorBlack, false);
  // Should not crash; some pixels in the first char should be set
  bool hasPixel = false;
  for (size_t i = 0; i < sizeof(buf); i += 2) {
    if (buf[i] != 0 || buf[i + 1] != 0) {
      hasPixel = true;
      break;
    }
  }
  CHECK(hasPixel);
}

// drawBatteryIcon with percent > 50 should use green fill.
TEST(draw_battery_icon_high_percent_uses_green) {
  constexpr uint32_t W = 32;
  constexpr uint32_t H = 32;
  uint8_t buf[W * H * 2] = {0};
  drawBatteryIcon(buf, W, H, 0, 0, 80);
  // Check interior fill area for green pixels
  bool hasGreen = false;
  for (int y = 1; y <= 7; ++y) {
    for (int x = 1; x <= 14; ++x) {
      size_t idx = (y * W + x) * 2;
      uint16_t px = (buf[idx] << 8) | buf[idx + 1];
      if (px == kColorGreen) {
        hasGreen = true;
        break;
      }
    }
  }
  CHECK(hasGreen);
}

// drawBatteryIcon with 20 < percent <= 50 should use yellow fill.
TEST(draw_battery_icon_mid_percent_uses_yellow) {
  constexpr uint32_t W = 32;
  constexpr uint32_t H = 32;
  uint8_t buf[W * H * 2] = {0};
  drawBatteryIcon(buf, W, H, 0, 0, 35);
  bool hasYellow = false;
  for (int y = 1; y <= 7; ++y) {
    for (int x = 1; x <= 14; ++x) {
      size_t idx = (y * W + x) * 2;
      uint16_t px = (buf[idx] << 8) | buf[idx + 1];
      if (px == kColorYellow) {
        hasYellow = true;
        break;
      }
    }
  }
  CHECK(hasYellow);
}

// drawBatteryIcon with percent <= 20 should use red fill.
TEST(draw_battery_icon_low_percent_uses_red) {
  constexpr uint32_t W = 32;
  constexpr uint32_t H = 32;
  uint8_t buf[W * H * 2] = {0};
  drawBatteryIcon(buf, W, H, 0, 0, 10);
  bool hasRed = false;
  for (int y = 1; y <= 7; ++y) {
    for (int x = 1; x <= 14; ++x) {
      size_t idx = (y * W + x) * 2;
      uint16_t px = (buf[idx] << 8) | buf[idx + 1];
      if (px == kColorRed) {
        hasRed = true;
        break;
      }
    }
  }
  CHECK(hasRed);
}

// drawBatteryIcon should clamp out-of-range percentages.
TEST(draw_battery_icon_clamps_percent) {
  constexpr uint32_t W = 32;
  constexpr uint32_t H = 32;
  uint8_t buf1[W * H * 2] = {0};
  uint8_t buf2[W * H * 2] = {0};
  drawBatteryIcon(buf1, W, H, 0, 0, 100);
  drawBatteryIcon(buf2, W, H, 0, 0, 200);
  CHECK(std::memcmp(buf1, buf2, sizeof(buf1)) == 0);
}

} // namespace
