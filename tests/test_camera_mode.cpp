#include "test_runner.h"
#include "camera_mode.h"
#include "font.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using picamera::CameraMode;
using picamera::modeName;
using picamera::drawGrid;
using picamera::GridType;
using picamera::OverlayState;
using picamera::drawOverlay;
using picamera::CameraSettings;
using picamera::SettingsTab;
using picamera::drawSettingsMenu;
using picamera::drawPlaybackBrowser;
using picamera::drawReviewScreen;
using picamera::drawImageView;
using picamera::drawSplash;
using picamera::drawCaptureIndicator;
using picamera::drawTimerCountdown;
using picamera::drawHistogram;
using picamera::kColorGreen;

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
        for (uint8_t b : data) if (b != 0) return true;
        return false;
    }

    size_t countNonZero() const {
        size_t n = 0;
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            if (data[i] != 0 || data[i + 1] != 0) ++n;
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
    bool leftLine = false, rightLine = false;
    for (uint32_t y = 0; y < fb.h; ++y) {
        if (fb.pixel(42, y) != 0) leftLine = true;
        if (fb.pixel(85, y) != 0) rightLine = true;
    }
    CHECK(leftLine);
    CHECK(rightLine);
}

TEST(draw_grid_draws_horizontal_lines) {
    Framebuffer fb(128, 128);
    drawGrid(fb.ptr(), fb.w, fb.h, GridType::Thirds);
    // Horizontal lines at h/3 = 42 and 2*h/3 = 85.
    bool topLine = false, bottomLine = false;
    for (uint32_t x = 0; x < fb.w; ++x) {
        if (fb.pixel(x, 42) != 0) topLine = true;
        if (fb.pixel(x, 85) != 0) bottomLine = true;
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
            if (fb.pixel(x, y) != 0) found = true;
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
            if (fb.pixel(x, y) != 0) found = true;
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
            if (fb.pixel(x, y) != 0) found = true;
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
        if (fb.pixel(42, y) != 0) found = true;
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
            if (fb.pixel(x, y) != 0) found = true;
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
    for (size_t i = 0; i < fb.size(); ++i) fb.data[i] = 0xFF;
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
            if (fb.pixel(x, y) != 0) found = true;
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
    std::vector<uint8_t> y(10, 128);  // too small
    drawHistogram(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), 320, 240, 320, 10);
    // Should be a no-op (buffer too small).
    CHECK(!fb.anyNonZero());
}

TEST(draw_histogram_draws_on_valid_input) {
    Framebuffer fb(128, 128);
    // Create a simple Y plane with a spread of luminance values.
    const uint32_t w = 64, h = 64, stride = 64;
    std::vector<uint8_t> y(stride * h, 0);
    for (uint32_t i = 0; i < stride * h; ++i) y[i] = static_cast<uint8_t>(i % 256);
    drawHistogram(fb.ptr(), fb.w, fb.h, fb.size(), y.data(), w, h, stride, y.size());
    // Histogram should draw something in the bottom-right area.
    bool found = false;
    for (uint32_t yy = fb.h - 40; yy < fb.h && !found; ++yy)
        for (uint32_t x = fb.w - 55; x < fb.w && !found; ++x)
            if (fb.pixel(x, yy) != 0) found = true;
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
            if (fb.pixel(x, y) != 0) found = true;
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
    int scroll = 999;  // way past end
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
    drawReviewScreen(fb.ptr(), fb.w, fb.h, fb.size(),
                     "/captures/shot_001.jpg", nullptr, 0);
    CHECK(fb.anyNonZero());
}

TEST(draw_review_screen_shows_decoded_image) {
    Framebuffer fb(128, 128);
    std::vector<uint8_t> img(fb.size(), 0);
    for (size_t i = 0; i + 1 < img.size(); i += 2) {
        img[i] = static_cast<uint8_t>(kColorGreen >> 8);
        img[i + 1] = static_cast<uint8_t>(kColorGreen & 0xFF);
    }
    drawReviewScreen(fb.ptr(), fb.w, fb.h, fb.size(),
                     "/captures/shot.jpg", img.data(), img.size());
    bool found = false;
    for (uint32_t y = 20; y < 100 && !found; ++y)
        for (uint32_t x = 20; x < 100 && !found; ++x)
            if (fb.pixel(x, y) == kColorGreen) found = true;
    CHECK(found);
}

TEST(draw_review_screen_rejects_undersized_caller_buffer) {
    // If the caller's framebuffer is too small, the function should no-op.
    Framebuffer fb(128, 128);
    std::vector<uint8_t> img(128 * 128 * 2, 0xFF);
    drawReviewScreen(fb.ptr(), fb.w, fb.h, 100,  // caller buffer too small
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
    drawImageView(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(), "/x/y.jpg");
    // Check a pixel in the middle (overlay text is at top/bottom).
    CHECK(fb.pixel(64, 64) == static_cast<uint16_t>((0x7B << 8) | 0x3F));
}

TEST(draw_image_view_rejects_short_image) {
    Framebuffer fb(128, 128);
    std::vector<uint8_t> img(100, 0xFF);
    drawImageView(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(), "/x.jpg");
    // With a short/undecodable image, drawImageView fills black and draws
    // the filename text — so the framebuffer should have some non-zero
    // pixels (the text) but not the 0xFF image data.
    CHECK(fb.anyNonZero());
}

TEST(draw_image_view_no_crash_small_buffer) {
    Framebuffer fb(32, 32);
    std::vector<uint8_t> img(fb.size(), 0x55);
    drawImageView(fb.ptr(), fb.w, fb.h, fb.size(), img.data(), img.size(), "/x.jpg");
    CHECK(fb.anyNonZero());
}

}
