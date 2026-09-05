#include "camera_mode.h"
#include "font.h"
#include "test_runner.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using picamera::AspectRatio;
using picamera::CameraMode;
using picamera::CameraSettings;
using picamera::computeWbGainsFromNv12;
using picamera::drawAspectRatioMask;
using picamera::drawBulbTimer;
using picamera::drawCaptureIndicator;
using picamera::drawFocusMagnifyIndicator;
using picamera::drawFocusPeaking;
using picamera::drawGrid;
using picamera::drawHistogram;
using picamera::drawImageView;
using picamera::drawImageViewHistogramAndBlinkies;
using picamera::drawImageViewZoomed;
using picamera::drawOverlay;
using picamera::drawPlaybackBrowser;
using picamera::drawProtectionIndicator;
using picamera::drawReviewScreen;
using picamera::drawSettingsMenu;
using picamera::drawSplash;
using picamera::drawTimerCountdown;
using picamera::drawZebra;
using picamera::GridType;
using picamera::kColorGreen;
using picamera::modeName;
using picamera::OverlayState;
using picamera::SettingsTab;
using picamera::ZebraMode;
using picamera::zebraThreshold;

namespace {

struct Framebuffer {
  uint32_t w, h;
  std::vector<uint8_t> data;
  Framebuffer(uint32_t w, uint32_t h)
      : w(w), h(h), data(static_cast<size_t>(w) * h * 2, 0) {}
  uint8_t *ptr() { return data.data(); }
  size_t size() const { return data.size(); }

  // Read the RGB565 pixel at (x, y) as a uint16_t (big-endian in memory:
  // high byte at lower address, matching the fillRect convention).
  uint16_t pixel(int x, int y) const {
    size_t idx = (static_cast<size_t>(y) * w + x) * 2;
    return static_cast<uint16_t>((data[idx] << 8) | data[idx + 1]);
  }

  bool anyNonZero() const {
    for (uint8_t b : data)
      if (b != 0)
        return true;
    return false;
  }

  size_t countNonZero() const {
    size_t n = 0;
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
      if (data[i] != 0 || data[i + 1] != 0)
        ++n;
    }
    return n;
  }
};

// --- modeName tests ---

TEST(mode_name_viewfinder) {
  CHECK_EQ(std::string(modeName(CameraMode::Viewfinder)), std::string("VF"));
}

TEST(mode_name_review) {
  CHECK_EQ(std::string(modeName(CameraMode::Review)), std::string("REV"));
}

TEST(mode_name_playback) {
  CHECK_EQ(std::string(modeName(CameraMode::Playback)), std::string("PLAY"));
}

TEST(mode_name_image_view) {
  CHECK_EQ(std::string(modeName(CameraMode::ImageView)), std::string("VIEW"));
}

TEST(mode_name_settings) {
  CHECK_EQ(std::string(modeName(CameraMode::Settings)), std::string("SET"));
}

TEST(mode_name_splash) {
  CHECK_EQ(std::string(modeName(CameraMode::Splash)), std::string("BOOT"));
}

// --- drawGrid tests ---

TEST(draw_grid_draws_vertical_lines) {
  Framebuffer fb(128, 128);
  drawGrid(fb.ptr(), fb.w, fb.h, GridType::Thirds);
  // Vertical lines at w/3 = 42 and 2*w/3 = 85.
  bool leftLine = false;
  bool rightLine = false;
  for (uint32_t y = 0; y < fb.h; ++y) {
    if (fb.pixel(42, y) != 0)
      leftLine = true;
    if (fb.pixel(85, y) != 0)
      rightLine = true;
  }
  CHECK(leftLine);
  CHECK(rightLine);
}

TEST(draw_grid_draws_horizontal_lines) {
  Framebuffer fb(128, 128);
  drawGrid(fb.ptr(), fb.w, fb.h, GridType::Thirds);
  // Horizontal lines at h/3 = 42 and 2*h/3 = 85.
  bool topLine = false;
  bool bottomLine = false;
  for (uint32_t x = 0; x < fb.w; ++x) {
    if (fb.pixel(x, 42) != 0)
      topLine = true;
    if (fb.pixel(x, 85) != 0)
      bottomLine = true;
  }
  CHECK(topLine);
  CHECK(bottomLine);
}

TEST(draw_grid_leaves_center_clear) {
  Framebuffer fb(128, 128);
  drawGrid(fb.ptr(), fb.w, fb.h, GridType::Thirds);
  CHECK(fb.pixel(64, 64) == 0);
}

TEST(draw_grid_does_not_overflow_small_buffer) {
  Framebuffer fb(16, 16);
  drawGrid(fb.ptr(), fb.w, fb.h, GridType::Thirds);
  // If it didn't crash/ASan, it's within bounds. Verify some pixels set.
  CHECK(fb.anyNonZero());
}

// --- drawOverlay tests ---

TEST(draw_overlay_draws_mode_indicator) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  // The mode indicator "VF" is drawn at (kMargin=2, kMargin=2).
  bool found = false;
  for (uint32_t y = 0; y < 12 && !found; ++y)
    for (uint32_t x = 0; x < 20 && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

TEST(draw_overlay_draws_capture_count) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.captureCount = 42;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  CHECK(fb.anyNonZero());
}

TEST(draw_overlay_draws_battery_when_valid) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.batteryValid = true;
  state.battery.percent = 75;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  // Battery icon is drawn at top-right (x = fbW - 22 = 106).
  bool found = false;
  for (uint32_t y = 0; y < 12 && !found; ++y)
    for (uint32_t x = 100; x < fb.w && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

TEST(draw_overlay_skips_battery_when_invalid) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.batteryValid = false;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  bool found = false;
  for (uint32_t y = 0; y < 12 && !found; ++y)
    for (uint32_t x = 100; x < fb.w && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(!found);
}

TEST(draw_overlay_draws_grid_when_enabled) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.settings.gridType = GridType::Thirds;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  bool found = false;
  for (uint32_t y = 0; y < fb.h && !found; ++y)
    if (fb.pixel(42, y) != 0)
      found = true;
  CHECK(found);
}

TEST(draw_overlay_draws_error_banner) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.errorMessage = "ERR";
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  // Error banner is at the bottom.
  bool found = false;
  for (uint32_t y = fb.h - 25; y < fb.h && !found; ++y)
    for (uint32_t x = 0; x < fb.w && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

TEST(draw_overlay_no_crash_on_small_buffer) {
  Framebuffer fb(32, 32);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.captureCount = 5;
  state.batteryValid = true;
  state.battery.percent = 50;
  state.errorMessage = "X";
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  // No crash = within bounds.
  CHECK(fb.anyNonZero());
}

// --- drawSplash tests ---

TEST(draw_splash_draws_something) {
  Framebuffer fb(128, 128);
  drawSplash(fb.ptr(), fb.w, fb.h);
  CHECK(fb.anyNonZero());
}

TEST(draw_splash_clears_background_first) {
  // drawSplash fills with black first, then draws text. On a pre-filled
  // buffer, the result should only have text pixels (not the old content).
  Framebuffer fb(128, 128);
  for (size_t i = 0; i < fb.size(); ++i)
    fb.data[i] = 0xFF;
  drawSplash(fb.ptr(), fb.w, fb.h);
  // Most pixels should be black (cleared), only text pixels non-zero.
  size_t nz = fb.countNonZero();
  CHECK(nz < fb.w * fb.h / 2);
}

TEST(draw_splash_no_crash_small_buffer) {
  Framebuffer fb(32, 32);
  drawSplash(fb.ptr(), fb.w, fb.h);
  CHECK(fb.anyNonZero());
}

// --- drawCaptureIndicator tests ---

TEST(draw_capture_indicator_draws_border) {
  Framebuffer fb(128, 128);
  drawCaptureIndicator(fb.ptr(), fb.w, fb.h);
  CHECK(fb.pixel(0, 0) != 0);
  CHECK(fb.pixel(127, 0) != 0);
  CHECK(fb.pixel(0, 127) != 0);
  CHECK(fb.pixel(127, 127) != 0);
}

// --- drawTimerCountdown tests ---

TEST(draw_timer_countdown_draws_number) {
  Framebuffer fb(128, 128);
  drawTimerCountdown(fb.ptr(), fb.w, fb.h, 3);
  // Scan a generous area to accommodate different font metrics.
  bool found = false;
  for (uint32_t y = 30; y < 100 && !found; ++y)
    for (uint32_t x = 30; x < 100 && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

// --- drawHistogram tests ---

TEST(draw_histogram_no_crash_on_null_plane) {
  Framebuffer fb(128, 128);
  drawHistogram(fb.ptr(), fb.w, fb.h, fb.size(), nullptr, 320, 240, 320, 0);
  CHECK(!fb.anyNonZero());
}

TEST(draw_histogram_no_crash_on_short_buffer) {
  Framebuffer fb(128, 128);
  std::vector<uint8_t> y(10, 128); // too small
  drawHistogram(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), 320, 240, 320, 10);
  // Should be a no-op (buffer too small).
  CHECK(!fb.anyNonZero());
}

TEST(draw_histogram_draws_on_valid_input) {
  Framebuffer fb(128, 128);
  // Create a simple Y plane with a spread of luminance values.
  const uint32_t w = 64;
  const uint32_t h = 64;
  const uint32_t stride = 64;
  std::vector<uint8_t> y(stride * h, 0);
  for (uint32_t i = 0; i < stride * h; ++i)
    y[i] = static_cast<uint8_t>(i % 256);
  drawHistogram(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), w, h, stride,
                y.size());
  // Histogram should draw something in the bottom-right area.
  bool found = false;
  for (uint32_t yy = fb.h - 40; yy < fb.h && !found; ++yy)
    for (uint32_t x = fb.w - 55; x < fb.w && !found; ++x)
      if (fb.pixel(x, yy) != 0)
        found = true;
  CHECK(found);
}

// --- drawSettingsMenu tests ---

TEST(draw_settings_menu_renders_without_crash) {
  Framebuffer fb(128, 128);
  CameraSettings settings;
  drawSettingsMenu(fb.ptr(), fb.w, fb.h, settings, SettingsTab::Shooting, 0);
  CHECK(fb.anyNonZero());
}

TEST(draw_settings_menu_draws_tab_bar) {
  Framebuffer fb(128, 128);
  CameraSettings settings;
  drawSettingsMenu(fb.ptr(), fb.w, fb.h, settings, SettingsTab::Shooting, 0);
  // Tab bar at top.
  bool found = false;
  for (uint32_t y = 0; y < 12 && !found; ++y)
    for (uint32_t x = 0; x < fb.w && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

TEST(draw_settings_menu_highlight_selected) {
  Framebuffer fb(128, 128);
  CameraSettings settings;
  drawSettingsMenu(fb.ptr(), fb.w, fb.h, settings, SettingsTab::Image, 2);
  CHECK(fb.anyNonZero());
}

TEST(draw_settings_menu_no_crash_small_buffer) {
  Framebuffer fb(32, 32);
  CameraSettings settings;
  drawSettingsMenu(fb.ptr(), fb.w, fb.h, settings, SettingsTab::Display, 0);
}

// --- drawPlaybackBrowser tests ---

TEST(draw_playback_browser_empty_shows_no_images) {
  Framebuffer fb(128, 128);
  std::vector<std::string> files;
  int scroll = 0;
  drawPlaybackBrowser(fb.ptr(), fb.w, fb.h, files, 0, scroll);
  CHECK(fb.anyNonZero());
}

TEST(draw_playback_browser_lists_files) {
  Framebuffer fb(128, 128);
  std::vector<std::string> files = {"/captures/img1.jpg", "/captures/img2.jpg"};
  int scroll = 0;
  drawPlaybackBrowser(fb.ptr(), fb.w, fb.h, files, 0, scroll);
  CHECK(fb.anyNonZero());
}

TEST(draw_playback_browser_clamps_scroll) {
  Framebuffer fb(128, 128);
  std::vector<std::string> files = {"/captures/a.jpg", "/captures/b.jpg"};
  int scroll = 999; // way past end
  drawPlaybackBrowser(fb.ptr(), fb.w, fb.h, files, 0, scroll);
  CHECK(scroll >= 0);
  CHECK(scroll <= static_cast<int>(files.size()));
}

TEST(draw_playback_browser_no_crash_small_buffer) {
  Framebuffer fb(32, 32);
  std::vector<std::string> files = {"a.jpg"};
  int scroll = 0;
  drawPlaybackBrowser(fb.ptr(), fb.w, fb.h, files, 0, scroll);
}

// --- drawReviewScreen tests ---

TEST(draw_review_screen_text_fallback) {
  Framebuffer fb(128, 128);
  drawReviewScreen(fb.ptr(), fb.w, fb.h, fb.size(), "/captures/shot_001.jpg",
                   nullptr, 0);
  CHECK(fb.anyNonZero());
}

TEST(draw_review_screen_shows_decoded_image) {
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  for (size_t i = 0; i + 1 < img.size(); i += 2) {
    img[i] = static_cast<uint8_t>(kColorGreen >> 8);
    img[i + 1] = static_cast<uint8_t>(kColorGreen & 0xFF);
  }
  drawReviewScreen(fb.ptr(), fb.w, fb.h, fb.size(), "/captures/shot.jpg",
                   img.data(), img.size());
  bool found = false;
  for (uint32_t y = 20; y < 100 && !found; ++y)
    for (uint32_t x = 20; x < 100 && !found; ++x)
      if (fb.pixel(x, y) == kColorGreen)
        found = true;
  CHECK(found);
}

TEST(draw_review_screen_rejects_undersized_caller_buffer) {
  // If the caller's framebuffer is too small, the function should no-op.
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(128 * 128 * 2, 0xFF);
  drawReviewScreen(fb.ptr(), fb.w, fb.h, 100, // caller buffer too small
                   "/captures/x.jpg", img.data(), img.size());
  CHECK(!fb.anyNonZero());
}

// --- drawImageView tests ---

TEST(draw_image_view_copies_pixels) {
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  for (size_t i = 0; i + 1 < img.size(); i += 2) {
    img[i] = 0x7B;
    img[i + 1] = 0x3F;
  }
  drawImageView(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                "/x/y.jpg");
  // Check a pixel in the middle (overlay text is at top/bottom).
  CHECK(fb.pixel(64, 64) == static_cast<uint16_t>((0x7B << 8) | 0x3F));
}

TEST(draw_image_view_rejects_short_image) {
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(100, 0xFF);
  drawImageView(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                "/x.jpg");
  // With a short/undecodable image, drawImageView fills black and draws
  // the filename text — so the framebuffer should have some non-zero
  // pixels (the text) but not the 0xFF image data.
  CHECK(fb.anyNonZero());
}

TEST(draw_image_view_no_crash_small_buffer) {
  Framebuffer fb(32, 32);
  std::vector<uint8_t> img(fb.size(), 0x55);
  drawImageView(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                "/x.jpg");
  CHECK(fb.anyNonZero());
}

// --- drawAspectRatioMask tests ---

TEST(draw_aspect_ratio_mask_native_no_op) {
  Framebuffer fb(128, 128);
  // Pre-fill with non-zero
  for (size_t i = 0; i < fb.size(); ++i)
    fb.data[i] = 0x55;
  drawAspectRatioMask(fb.ptr(), fb.w, fb.h, AspectRatio::Native);
  // Native should not draw any black bars
  CHECK(fb.pixel(0, 0) != 0);
  CHECK(fb.pixel(64, 64) != 0);
}

TEST(draw_aspect_ratio_mask_1_1_on_square_is_noop) {
  Framebuffer fb(128, 128);
  for (size_t i = 0; i < fb.size(); ++i)
    fb.data[i] = 0x55;
  drawAspectRatioMask(fb.ptr(), fb.w, fb.h, AspectRatio::Ratio11);
  // 1:1 on a square display = no bars
  CHECK(fb.pixel(0, 0) != 0);
  CHECK(fb.pixel(127, 127) != 0);
}

TEST(draw_aspect_ratio_mask_16_9_draws_black_bars) {
  Framebuffer fb(128, 128);
  for (size_t i = 0; i < fb.size(); ++i)
    fb.data[i] = 0x55;
  drawAspectRatioMask(fb.ptr(), fb.w, fb.h, AspectRatio::Ratio169);
  // 16:9 is wider than 1:1, so top/bottom bars should be black
  CHECK(fb.pixel(64, 0) == 0);   // top bar
  CHECK(fb.pixel(64, 127) == 0); // bottom bar
  // Center should still be non-zero
  CHECK(fb.pixel(64, 64) != 0);
}

TEST(draw_aspect_ratio_mask_no_crash_small_buffer) {
  Framebuffer fb(16, 16);
  drawAspectRatioMask(fb.ptr(), fb.w, fb.h, AspectRatio::Ratio169);
  // No crash = pass
}

// --- drawFocusMagnifyIndicator tests ---

TEST(draw_focus_magnify_indicator_off_is_noop) {
  Framebuffer fb(128, 128);
  drawFocusMagnifyIndicator(fb.ptr(), fb.w, fb.h, 0);
  CHECK(!fb.anyNonZero());
}

TEST(draw_focus_magnify_indicator_2x_draws_text) {
  Framebuffer fb(128, 128);
  drawFocusMagnifyIndicator(fb.ptr(), fb.w, fb.h, 2);
  CHECK(fb.anyNonZero());
}

TEST(draw_focus_magnify_indicator_4x_draws_text) {
  Framebuffer fb(128, 128);
  drawFocusMagnifyIndicator(fb.ptr(), fb.w, fb.h, 4);
  CHECK(fb.anyNonZero());
}

// --- drawImageViewHistogramAndBlinkies tests ---

TEST(draw_image_view_histogram_no_crash_on_black) {
  Framebuffer fb(128, 128);
  // All black — no blinkies, histogram should be flat
  drawImageViewHistogramAndBlinkies(fb.ptr(), fb.w, fb.h, fb.size());
  // Histogram background is drawn, so some pixels may be set
  // Just check no crash
}

TEST(draw_image_view_histogram_draws_on_white) {
  Framebuffer fb(128, 128);
  // Fill with white
  for (size_t i = 0; i + 1 < fb.size(); i += 2) {
    fb.data[i] = 0xFF;
    fb.data[i + 1] = 0xFF;
  }
  drawImageViewHistogramAndBlinkies(fb.ptr(), fb.w, fb.h, fb.size());
  // Should draw blinkies on clipped (white) regions
  CHECK(fb.anyNonZero());
}

TEST(draw_image_view_histogram_no_crash_small_buffer) {
  Framebuffer fb(16, 16);
  drawImageViewHistogramAndBlinkies(fb.ptr(), fb.w, fb.h, fb.size());
  // No crash = pass
}

// --- New grid type tests ---

TEST(draw_grid_diagonal_draws_lines) {
  Framebuffer fb(128, 128);
  drawGrid(fb.ptr(), fb.w, fb.h, GridType::Diagonal);
  // Diagonal from top-left to bottom-right
  CHECK(fb.pixel(0, 0) != 0);
  CHECK(fb.pixel(64, 64) != 0);
  CHECK(fb.pixel(127, 127) != 0);
}

TEST(draw_grid_golden_ratio_draws_lines) {
  Framebuffer fb(128, 128);
  drawGrid(fb.ptr(), fb.w, fb.h, GridType::GoldenRatio);
  // Golden ratio lines at ~0.382 and ~0.618
  int g1 = static_cast<int>(128 * 0.382f);
  int g2 = static_cast<int>(128 * 0.618f);
  bool foundG1 = false;
  bool foundG2 = false;
  for (uint32_t y = 0; y < fb.h; ++y) {
    if (fb.pixel(g1, y) != 0)
      foundG1 = true;
    if (fb.pixel(g2, y) != 0)
      foundG2 = true;
  }
  CHECK(foundG1);
  CHECK(foundG2);
}

// --- Timelapse indicator in overlay ---

TEST(draw_overlay_draws_timelapse_indicator) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.timelapseRunning = true;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  CHECK(fb.anyNonZero());
}

// --- Focus magnify indicator in overlay ---

TEST(draw_overlay_draws_focus_magnify) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.focusMagnify = 2;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  CHECK(fb.anyNonZero());
}

// --- zebraThreshold tests ---

TEST(zebra_threshold_off_returns_zero) {
  CHECK(zebraThreshold(ZebraMode::Off) == 0);
}

TEST(zebra_threshold_70_returns_178) {
  CHECK(zebraThreshold(ZebraMode::Threshold70) == 178);
}

TEST(zebra_threshold_80_returns_204) {
  CHECK(zebraThreshold(ZebraMode::Threshold80) == 204);
}

TEST(zebra_threshold_100_returns_255) {
  CHECK(zebraThreshold(ZebraMode::Threshold100) == 255);
}

TEST(zebra_threshold_monotonic) {
  CHECK(zebraThreshold(ZebraMode::Threshold70) <
        zebraThreshold(ZebraMode::Threshold80));
  CHECK(zebraThreshold(ZebraMode::Threshold80) <
        zebraThreshold(ZebraMode::Threshold100));
}

// --- drawZebra tests ---

TEST(draw_zebra_marks_bright_pixels) {
  Framebuffer fb(16, 16);
  // Y plane: all pixels at 255 (above any threshold).
  const uint32_t w = 16;
  const uint32_t h = 16;
  const uint32_t stride = 16;
  std::vector<uint8_t> y(stride * h, 255);
  drawZebra(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), w, h, stride, y.size(),
            200);
  // Bright pixels should be overwritten with zebra stripes (non-zero).
  CHECK(fb.anyNonZero());
}

TEST(draw_zebra_skips_dark_pixels) {
  Framebuffer fb(16, 16);
  // Pre-fill with a non-zero color so we can detect overwrites.
  for (size_t i = 0; i < fb.size(); i += 2) {
    fb.data[i] = 0x1F; // blue
    fb.data[i + 1] = 0x00;
  }
  // Y plane: all pixels at 50 (below threshold 200).
  const uint32_t w = 16;
  const uint32_t h = 16;
  const uint32_t stride = 16;
  std::vector<uint8_t> y(stride * h, 50);
  size_t before = fb.countNonZero();
  drawZebra(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), w, h, stride, y.size(),
            200);
  // No pixels should change (all below threshold).
  CHECK_EQ(fb.countNonZero(), before);
}

TEST(draw_zebra_no_crash_on_null) {
  Framebuffer fb(16, 16);
  drawZebra(fb.ptr(), fb.w, fb.h, fb.size(), nullptr, 16, 16, 16, 0, 200);
  CHECK(!fb.anyNonZero());
}

// --- drawFocusPeaking tests ---

TEST(draw_focus_peaking_marks_edges) {
  Framebuffer fb(16, 16);
  // Y plane with a sharp vertical edge: left half dark, right half bright.
  const uint32_t w = 16;
  const uint32_t h = 16;
  const uint32_t stride = 16;
  std::vector<uint8_t> y(stride * h, 0);
  for (uint32_t row = 0; row < h; ++row)
    for (uint32_t x = 8; x < w; ++x)
      y[row * stride + x] = 255;
  drawFocusPeaking(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), w, h, stride,
                   y.size());
  // Edges should produce green peak pixels (non-zero).
  CHECK(fb.anyNonZero());
}

TEST(draw_focus_peaking_no_crash_on_null) {
  Framebuffer fb(16, 16);
  drawFocusPeaking(fb.ptr(), fb.w, fb.h, fb.size(), nullptr, 16, 16, 16, 0);
  CHECK(!fb.anyNonZero());
}

TEST(draw_focus_peaking_flat_image_no_marks) {
  Framebuffer fb(16, 16);
  // Flat Y plane — no edges.
  const uint32_t w = 16;
  const uint32_t h = 16;
  const uint32_t stride = 16;
  std::vector<uint8_t> y(stride * h, 128);
  drawFocusPeaking(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), w, h, stride,
                   y.size());
  CHECK(!fb.anyNonZero());
}

// --- drawBulbTimer tests ---

TEST(draw_bulb_timer_renders_text) {
  Framebuffer fb(128, 128);
  drawBulbTimer(fb.ptr(), fb.w, fb.h, 5);
  CHECK(fb.anyNonZero());
}

TEST(draw_bulb_timer_no_crash_on_zero_dims) { drawBulbTimer(nullptr, 0, 0, 5); }

// --- drawOverlay bulb timer integration ---

TEST(draw_overlay_draws_bulb_timer) {
  Framebuffer fb(128, 128);
  OverlayState state;
  state.mode = CameraMode::Viewfinder;
  state.bulbSeconds = 3;
  drawOverlay(fb.ptr(), fb.w, fb.h, state);
  CHECK(fb.anyNonZero());
}

// --- computeWbGainsFromNv12 tests ---

TEST(compute_wb_gains_neutral_returns_unit) {
  // Neutral chroma (Cb=Cr=128) -> gains of 1.0.
  const uint32_t w = 16;
  const uint32_t h = 16;
  size_t uvSize = (w / 2) * (h / 2) * 2;
  std::vector<uint8_t> uv(uvSize, 128);
  float red = 0;
  float blue = 0;
  CHECK(computeWbGainsFromNv12(uv.data(), w, h, uvSize, red, blue));
  CHECK(red == 1.0f);
  CHECK(blue == 1.0f);
}

TEST(compute_wb_gains_warm_scene_reduces_red) {
  // Warm scene: Cr > 128 (red-dominant), Cb < 128 (blue-deficient).
  // Red gain should drop below 1.0; blue gain should rise above 1.0.
  const uint32_t w = 16;
  const uint32_t h = 16;
  size_t uvSize = (w / 2) * (h / 2) * 2;
  std::vector<uint8_t> uv(uvSize, 128);
  for (size_t i = 0; i < uvSize; i += 2) {
    uv[i] = 96;      // Cb (blue-diff) below 128
    uv[i + 1] = 160; // Cr (red-diff) above 128
  }
  float red = 0;
  float blue = 0;
  CHECK(computeWbGainsFromNv12(uv.data(), w, h, uvSize, red, blue));
  CHECK(red < 1.0f);
  CHECK(blue > 1.0f);
}

TEST(compute_wb_gains_cool_scene_increases_red) {
  // Cool scene: Cr < 128 (red-deficient), Cb > 128 (blue-dominant).
  const uint32_t w = 16;
  const uint32_t h = 16;
  size_t uvSize = (w / 2) * (h / 2) * 2;
  std::vector<uint8_t> uv(uvSize, 128);
  for (size_t i = 0; i < uvSize; i += 2) {
    uv[i] = 160;    // Cb above 128
    uv[i + 1] = 96; // Cr below 128
  }
  float red = 0;
  float blue = 0;
  CHECK(computeWbGainsFromNv12(uv.data(), w, h, uvSize, red, blue));
  CHECK(red > 1.0f);
  CHECK(blue < 1.0f);
}

TEST(compute_wb_gains_null_returns_false) {
  float red = 0;
  float blue = 0;
  CHECK(!computeWbGainsFromNv12(nullptr, 16, 16, 0, red, blue));
}

TEST(compute_wb_gains_small_dims_returns_false) {
  float red = 0;
  float blue = 0;
  CHECK(!computeWbGainsFromNv12(nullptr, 1, 16, 0, red, blue));
}

TEST(compute_wb_gains_short_buffer_returns_false) {
  const uint32_t w = 16;
  const uint32_t h = 16;
  std::vector<uint8_t> uv(4, 128); // too small
  float red = 0;
  float blue = 0;
  CHECK(!computeWbGainsFromNv12(uv.data(), w, h, uv.size(), red, blue));
}

// --- drawProtectionIndicator tests ---

TEST(draw_protection_indicator_draws_text) {
  Framebuffer fb(128, 128);
  drawProtectionIndicator(fb.ptr(), fb.w, fb.h);
  // "PROT" text should appear in the top-right area
  bool found = false;
  for (uint32_t y = 0; y < 30 && !found; ++y)
    for (uint32_t x = 80; x < fb.w && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

TEST(draw_protection_indicator_no_crash_zero_dims) {
  drawProtectionIndicator(nullptr, 0, 0);
}

// --- drawImageViewZoomed tests ---
//
// drawImageViewZoomed() renders a decoded RGB565 image with 1x/2x/4x zoom
// and pan. The source image is imageW x imageH RGB565 (big-endian: high byte
// at the lower address, matching the Framebuffer::pixel() convention). The
// framebuffer is fbW x fbH. Overlay text (filename, zoom indicator, nav hint)
// is drawn on top, so pixel checks avoid the top/bottom text bands.

namespace {

// Set the RGB565 pixel at (x, y) in a source image of width imgW.
void setSrcPixel(std::vector<uint8_t> &img, uint32_t imgW, uint32_t x,
                 uint32_t y, uint16_t color) {
  size_t idx = (static_cast<size_t>(y) * imgW + x) * 2;
  img[idx] = static_cast<uint8_t>(color >> 8);
  img[idx + 1] = static_cast<uint8_t>(color & 0xFF);
}

} // namespace

TEST(draw_image_view_zoomed_1x_copies_image) {
  // 1x zoom should memcpy the decoded image directly (same as drawImageView).
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  for (size_t i = 0; i + 1 < img.size(); i += 2) {
    img[i] = 0x7B;
    img[i + 1] = 0x3F;
  }
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 1, 0, 0, "/x/y.jpg");
  // Center pixel (away from overlay text) should match the source.
  CHECK(fb.pixel(64, 64) == static_cast<uint16_t>((0x7B << 8) | 0x3F));
}

TEST(draw_image_view_zoomed_1x_no_zoom_indicator) {
  // At 1x there is no "1X" zoom indicator drawn in the top-right corner.
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 1, 0, 0, "/x.jpg");
  // The zoom indicator would sit at roughly x >= 110, y < 12. The black
  // image has no content there, so those pixels should stay black.
  CHECK(fb.pixel(120, 4) == 0);
}

TEST(draw_image_view_zoomed_2x_centered_crop) {
  // 2x zoom with pan (32, 32): visible source region is (32,32,64,64).
  // Each source pixel maps to a 2x2 destination block.
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0); // black source
  // Mark two source pixels so we can verify the nearest-neighbor mapping.
  constexpr uint16_t kRed = 0xF800;
  constexpr uint16_t kBlue = 0x001F;
  setSrcPixel(img, fb.w, 52, 52, kRed);
  setSrcPixel(img, fb.w, 53, 53, kBlue);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 2, 32, 32, "/x.jpg");
  // dst(40,40): sx = 32 + 40/2 = 52, sy = 52 -> red.
  CHECK(fb.pixel(40, 40) == kRed);
  // dst(41,40) and dst(40,41) share the same source pixel (2x2 block).
  CHECK(fb.pixel(41, 40) == kRed);
  CHECK(fb.pixel(40, 41) == kRed);
  // dst(42,42): sx = 32 + 42/2 = 53, sy = 53 -> blue.
  CHECK(fb.pixel(42, 42) == kBlue);
}

TEST(draw_image_view_zoomed_2x_draws_zoom_indicator) {
  // At 2x a yellow "2X" indicator is drawn in the top-right corner.
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 2, 0, 0, "/x.jpg");
  bool found = false;
  for (uint32_t y = 0; y < 12 && !found; ++y)
    for (uint32_t x = 108; x < fb.w && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

TEST(draw_image_view_zoomed_4x_centered_crop) {
  // 4x zoom with pan (48, 48): visible source region is (48,48,32,32).
  // Each source pixel maps to a 4x4 destination block.
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0); // black source
  constexpr uint16_t kGreen = 0x07E0;
  setSrcPixel(img, fb.w, 64, 64, kGreen);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 4, 48, 48, "/x.jpg");
  // dst(64,64): sx = 48 + 64/4 = 64, sy = 64 -> green.
  CHECK(fb.pixel(64, 64) == kGreen);
  // The 4x4 block around (64,64) should all be green.
  CHECK(fb.pixel(65, 65) == kGreen);
  CHECK(fb.pixel(67, 67) == kGreen);
  // dst(68,68): sx = 48 + 68/4 = 48+17 = 65 -> black (source(65,65)=0).
  CHECK(fb.pixel(68, 68) == 0);
}

TEST(draw_image_view_zoomed_4x_draws_zoom_indicator) {
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 4, 0, 0, "/x.jpg");
  bool found = false;
  for (uint32_t y = 0; y < 12 && !found; ++y)
    for (uint32_t x = 108; x < fb.w && !found; ++x)
      if (fb.pixel(x, y) != 0)
        found = true;
  CHECK(found);
}

TEST(draw_image_view_zoomed_pan_clamped_to_bounds) {
  // Pan offsets beyond the image are clamped so the visible region stays
  // within the source. With 2x zoom the visible region is 64x64; a panX of
  // 1000 must clamp to imageW - 64 = 64 (showing the right half).
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  // Left half red, right half green.
  constexpr uint16_t kRed = 0xF800;
  constexpr uint16_t kGreen = 0x07E0;
  for (uint32_t y = 0; y < fb.h; ++y) {
    for (uint32_t x = 0; x < fb.w; ++x) {
      setSrcPixel(img, fb.w, x, y, x < 64 ? kRed : kGreen);
    }
  }
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 2, 1000, 1000, "/x.jpg");
  // After clamping pan to (64, 64), dst(40,40) -> sx = 64 + 20 = 84 -> green.
  CHECK(fb.pixel(40, 40) == kGreen);
  // dst(40,40) must NOT be red (proves pan did not stay at 0).
  CHECK(fb.pixel(40, 40) != kRed);
}

TEST(draw_image_view_zoomed_pan_negative_clamped_to_zero) {
  // Negative pan offsets clamp to 0 (top-left corner of the source).
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(fb.size(), 0);
  constexpr uint16_t kRed = 0xF800;
  constexpr uint16_t kGreen = 0x07E0;
  for (uint32_t y = 0; y < fb.h; ++y) {
    for (uint32_t x = 0; x < fb.w; ++x) {
      setSrcPixel(img, fb.w, x, y, x < 64 ? kRed : kGreen);
    }
  }
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 2, -500, -500, "/x.jpg");
  // pan clamped to (0, 0): dst(40,40) -> sx = 0 + 20 = 20 -> red.
  CHECK(fb.pixel(40, 40) == kRed);
}

TEST(draw_image_view_zoomed_zero_dimensions_no_crash) {
  // Zero framebuffer dimensions must not crash (loops don't execute).
  std::vector<uint8_t> buf(16, 0xAA);
  std::vector<uint8_t> img(16, 0x55);
  drawImageViewZoomed(buf.data(), 0, 0, buf.size(), img.data(), img.size(), 128,
                      128, 2, 0, 0, "/x.jpg");
  // No crash = pass.
}

TEST(draw_image_view_zoomed_null_source_draws_text) {
  // A null/empty source should memset black and draw the filename + hint
  // text (so the framebuffer has some non-zero text pixels but no image).
  Framebuffer fb(128, 128);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), nullptr, 0, fb.w, fb.h,
                      2, 0, 0, "/x.jpg");
  CHECK(fb.anyNonZero());
}

TEST(draw_image_view_zoomed_short_source_draws_text) {
  // An undersized source image (imageSize < fbW*fbH*2) falls back to the
  // black + text path.
  Framebuffer fb(128, 128);
  std::vector<uint8_t> img(100, 0xFF);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 1, 0, 0, "/x.jpg");
  // Some text pixels should be set, but the 0xFF image data must not have
  // been copied (the framebuffer was not filled with 0xFFFF).
  CHECK(fb.pixel(64, 64) == 0);
}

TEST(draw_image_view_zoomed_no_crash_small_buffer) {
  Framebuffer fb(32, 32);
  std::vector<uint8_t> img(fb.size(), 0x55);
  drawImageViewZoomed(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(),
                      fb.w, fb.h, 4, 8, 8, "/x.jpg");
  CHECK(fb.anyNonZero());
}

} // namespace
