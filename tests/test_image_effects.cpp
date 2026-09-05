#include "image_effects.h"
#include "preview_helpers.h"
#include "test_runner.h"

#include <string>
#include <vector>

using picamera::applyGrainEffect;
using picamera::applyNightBoost;
using picamera::copyrightCharAt;
using picamera::copyrightCharCount;
using picamera::copyrightCycleChar;
using picamera::darkFrameSubtract;
using picamera::estimateBlackLevel;
using picamera::hdrMergeY;
using picamera::isPortrait;
using picamera::readFileRating;
using picamera::rgb24ToUv;
using picamera::rgb24ToY;
using picamera::rotateRgb565Cw;
using picamera::subtractBlackLevel;
using picamera::writeFileRating;
using picamera::yuvToRgb24;

// --- HDR merge tests ---

TEST(hdr_merge_empty_frames_returns_empty) {
  auto result = hdrMergeY({}, 4, 4, 4);
  CHECK(result.empty());
}

TEST(hdr_merge_single_frame_passthrough) {
  std::vector<uint8_t> frame = {10, 20, 30, 40};
  std::vector<const uint8_t *> frames = {frame.data()};
  auto result = hdrMergeY(frames, 4, 1, 4);
  CHECK(result.size() == 4);
  CHECK(result[0] == 10);
  CHECK(result[1] == 20);
  CHECK(result[2] == 30);
  CHECK(result[3] == 40);
}

TEST(hdr_merge_two_frames_averages) {
  std::vector<uint8_t> f1 = {10, 20, 30, 40};
  std::vector<uint8_t> f2 = {30, 40, 50, 60};
  std::vector<const uint8_t *> frames = {f1.data(), f2.data()};
  auto result = hdrMergeY(frames, 4, 1, 4);
  CHECK(result.size() == 4);
  CHECK(result[0] == 20); // (10+30)/2
  CHECK(result[1] == 30); // (20+40)/2
  CHECK(result[2] == 40); // (30+50)/2
  CHECK(result[3] == 50); // (40+60)/2
}

TEST(hdr_merge_three_frames_averages) {
  std::vector<uint8_t> f1 = {0, 90, 180, 255};
  std::vector<uint8_t> f2 = {60, 90, 120, 255};
  std::vector<uint8_t> f3 = {120, 90, 60, 255};
  std::vector<const uint8_t *> frames = {f1.data(), f2.data(), f3.data()};
  auto result = hdrMergeY(frames, 4, 1, 4);
  CHECK(result.size() == 4);
  CHECK(result[0] == 60);  // (0+60+120)/3
  CHECK(result[1] == 90);  // (90+90+90)/3
  CHECK(result[2] == 120); // (180+120+60)/3
  CHECK(result[3] == 255); // (255+255+255)/3
}

TEST(hdr_merge_null_frame_returns_empty) {
  std::vector<uint8_t> f1 = {10, 20};
  std::vector<const uint8_t *> frames = {f1.data(), nullptr};
  auto result = hdrMergeY(frames, 2, 1, 2);
  CHECK(result.empty());
}

// --- Dark frame subtraction tests ---

TEST(dark_frame_subtract_basic) {
  std::vector<uint8_t> image = {100, 200, 50, 10};
  std::vector<uint8_t> dark = {10, 20, 5, 0};
  auto result = darkFrameSubtract(image.data(), dark.data(), 4, 1, 4);
  CHECK(result.size() == 4);
  CHECK(result[0] == 90);  // 100-10
  CHECK(result[1] == 180); // 200-20
  CHECK(result[2] == 45);  // 50-5
  CHECK(result[3] == 10);  // 10-0
}

TEST(dark_frame_subtract_clamps_to_zero) {
  std::vector<uint8_t> image = {10, 5, 0};
  std::vector<uint8_t> dark = {20, 10, 5};
  auto result = darkFrameSubtract(image.data(), dark.data(), 3, 1, 3);
  CHECK(result.size() == 3);
  CHECK(result[0] == 0); // 10-20 clamped
  CHECK(result[1] == 0); // 5-10 clamped
  CHECK(result[2] == 0); // 0-5 clamped
}

TEST(dark_frame_subtract_null_returns_empty) {
  std::vector<uint8_t> dark = {10, 20};
  auto result = darkFrameSubtract(nullptr, dark.data(), 2, 1, 2);
  CHECK(result.empty());
}

// --- Grain effect tests ---

TEST(grain_effect_zero_strength_noop) {
  std::vector<uint8_t> y = {128, 128, 128, 128};
  applyGrainEffect(y.data(), 4, 1, 4, 0, 42);
  CHECK(y[0] == 128);
  CHECK(y[1] == 128);
  CHECK(y[2] == 128);
  CHECK(y[3] == 128);
}

TEST(grain_effect_modifies_pixels) {
  std::vector<uint8_t> y1 = {128, 128, 128, 128};
  std::vector<uint8_t> y2 = {128, 128, 128, 128};
  applyGrainEffect(y1.data(), 4, 1, 4, 20, 42);
  applyGrainEffect(y2.data(), 4, 1, 4, 20, 42);
  // Same seed should produce same result (deterministic)
  CHECK(y1 == y2);
  // At least some pixels should have changed
  bool changed = false;
  for (int i = 0; i < 4; ++i) {
    if (y1[i] != 128) {
      changed = true;
      break;
    }
  }
  CHECK(changed);
}

TEST(grain_effect_clamps_values) {
  std::vector<uint8_t> y = {0, 255, 0, 255};
  applyGrainEffect(y.data(), 4, 1, 4, 50, 123);
  for (int i = 0; i < 4; ++i) {
    CHECK(y[i] >= 0);
    CHECK(y[i] <= 255);
  }
}

// --- Portrait detection tests ---

TEST(is_portrait_tall_image) {
  CHECK(isPortrait(100, 200) == true);
}

TEST(is_portrait_wide_image) {
  CHECK(isPortrait(200, 100) == false);
}

TEST(is_portrait_square_image) {
  CHECK(isPortrait(100, 100) == false);
}

// --- RGB565 rotation tests ---

TEST(rotate_rgb565_cw_2x2) {
  // 2x2 RGB565 image. Each pixel is 2 bytes.
  // Source layout:
  //   [A][B]
  //   [C][D]
  // After 90° CW rotation:
  //   [C][A]
  //   [D][B]
  std::vector<uint8_t> src = {
      0xAA, 0xBB, // pixel (0,0) = A
      0xCC, 0xDD, // pixel (1,0) = B
      0x11, 0x22, // pixel (0,1) = C
      0x33, 0x44, // pixel (1,1) = D
  };
  auto dst = rotateRgb565Cw(src.data(), 2, 2);
  CHECK(dst.size() == 8);
  // dst[0,0] = src[0,1] = C = 0x11, 0x22
  CHECK(dst[0] == 0x11);
  CHECK(dst[1] == 0x22);
  // dst[1,0] = src[0,0] = A = 0xAA, 0xBB
  CHECK(dst[2] == 0xAA);
  CHECK(dst[3] == 0xBB);
  // dst[0,1] = src[1,1] = D = 0x33, 0x44
  CHECK(dst[4] == 0x33);
  CHECK(dst[5] == 0x44);
  // dst[1,1] = src[1,0] = B = 0xCC, 0xDD
  CHECK(dst[6] == 0xCC);
  CHECK(dst[7] == 0xDD);
}

TEST(rotate_rgb565_cw_null_returns_empty) {
  auto dst = rotateRgb565Cw(nullptr, 2, 2);
  CHECK(dst.empty());
}

TEST(rotate_rgb565_cw_1x1) {
  std::vector<uint8_t> src = {0xAB, 0xCD};
  auto dst = rotateRgb565Cw(src.data(), 1, 1);
  CHECK(dst.size() == 2);
  CHECK(dst[0] == 0xAB);
  CHECK(dst[1] == 0xCD);
}

// --- Night boost tests ---

TEST(night_boost_doubles_brightness) {
  std::vector<uint8_t> y = {10, 50, 100, 127};
  applyNightBoost(y.data(), 4, 1, 4, 2.0f);
  CHECK(y[0] == 20);
  CHECK(y[1] == 100);
  CHECK(y[2] == 200);
  CHECK(y[3] == 254); // 127*2=254
}

TEST(night_boost_clamps_to_255) {
  std::vector<uint8_t> y = {200, 250, 255};
  applyNightBoost(y.data(), 3, 1, 3, 2.0f);
  CHECK(y[0] == 255); // 400 clamped
  CHECK(y[1] == 255); // 500 clamped
  CHECK(y[2] == 255); // 510 clamped
}

TEST(night_boost_factor_le_one_noop) {
  std::vector<uint8_t> y = {10, 50, 100};
  applyNightBoost(y.data(), 3, 1, 3, 1.0f);
  CHECK(y[0] == 10);
  CHECK(y[1] == 50);
  CHECK(y[2] == 100);
}

// --- Rating sidecar tests ---

TEST(rating_read_nonexistent_returns_zero) {
  CHECK(readFileRating("/tmp", "nonexistent_file.jpg") == 0);
}

TEST(rating_write_and_read_roundtrip) {
  std::string dir = "/tmp";
  std::string file = "test_rating_" + std::to_string(getpid()) + ".jpg";
  CHECK(writeFileRating(dir, file, 3));
  CHECK(readFileRating(dir, file) == 3);
  // Clean up
  writeFileRating(dir, file, 0);
  CHECK(readFileRating(dir, file) == 0);
}

TEST(rating_clamps_to_5) {
  std::string dir = "/tmp";
  std::string file = "test_rating_clamp_" + std::to_string(getpid()) + ".jpg";
  CHECK(writeFileRating(dir, file, 10)); // should clamp to 5
  CHECK(readFileRating(dir, file) == 5);
  writeFileRating(dir, file, 0);
}

TEST(rating_zero_deletes_sidecar) {
  std::string dir = "/tmp";
  std::string file = "test_rating_del_" + std::to_string(getpid()) + ".jpg";
  writeFileRating(dir, file, 4);
  CHECK(readFileRating(dir, file) == 4);
  CHECK(writeFileRating(dir, file, 0));
  CHECK(readFileRating(dir, file) == 0);
}

// --- rgb24ToY tests ---

TEST(rgb24_to_y_null_input_returns_empty) {
  auto result = rgb24ToY(nullptr, 4, 4, 4);
  CHECK(result.empty());
}

TEST(rgb24_to_y_basic_white_pixel) {
  // White pixel (255,255,255) should give Y=255.
  std::vector<uint8_t> rgb = {255, 255, 255};
  auto y = rgb24ToY(rgb.data(), 1, 1, 1);
  CHECK(y.size() == 1);
  CHECK(y[0] == 255);
}

TEST(rgb24_to_y_basic_black_pixel) {
  // Black pixel (0,0,0) should give Y=0.
  std::vector<uint8_t> rgb = {0, 0, 0};
  auto y = rgb24ToY(rgb.data(), 1, 1, 1);
  CHECK(y.size() == 1);
  CHECK(y[0] == 0);
}

TEST(rgb24_to_y_stride_padding) {
  // 2x1 image with stride 4: Y values at positions 0 and 1, padding at 2-3.
  std::vector<uint8_t> rgb = {100, 100, 100, 200, 200, 200};
  auto y = rgb24ToY(rgb.data(), 2, 1, 4);
  CHECK(y.size() == 4);
  CHECK(y[0] > 0);
  CHECK(y[1] > y[0]); // brighter pixel
}

// --- yuvToRgb24 tests ---

TEST(yuv_to_rgb24_null_input_returns_empty) {
  auto result = yuvToRgb24(nullptr, nullptr, 4, 4, 4);
  CHECK(result.empty());
}

TEST(yuv_to_rgb24_basic_white) {
  // Y=255, U=128, V=128 (neutral chroma) → white (255,255,255).
  std::vector<uint8_t> y = {255};
  std::vector<uint8_t> uv = {128, 128};
  auto rgb = yuvToRgb24(y.data(), uv.data(), 1, 1, 1);
  CHECK(rgb.size() == 3);
  CHECK(rgb[0] == 255);
  CHECK(rgb[1] == 255);
  CHECK(rgb[2] == 255);
}

TEST(yuv_to_rgb24_basic_black) {
  // Y=0, U=128, V=128 → black (0,0,0).
  std::vector<uint8_t> y = {0};
  std::vector<uint8_t> uv = {128, 128};
  auto rgb = yuvToRgb24(y.data(), uv.data(), 1, 1, 1);
  CHECK(rgb.size() == 3);
  CHECK(rgb[0] == 0);
  CHECK(rgb[1] == 0);
  CHECK(rgb[2] == 0);
}

TEST(yuv_to_rgb24_2x2_image) {
  // 2x2 image: all Y=128, neutral chroma → gray.
  std::vector<uint8_t> y = {128, 128, 128, 128};
  std::vector<uint8_t> uv = {128, 128}; // 1 chroma sample for 2x2
  auto rgb = yuvToRgb24(y.data(), uv.data(), 2, 2, 2);
  CHECK(rgb.size() == 12); // 2*2*3
  for (size_t i = 0; i < 12; ++i)
    CHECK(rgb[i] == 128);
}

// --- Copyright character entry tests ---

TEST(copyright_char_count_is_39) {
  // A-Z (26) + 0-9 (10) + space + '-' + '.' = 39
  CHECK(copyrightCharCount() == 39);
}

TEST(copyright_char_at_first_is_A) {
  CHECK(copyrightCharAt(0) == 'A');
}

TEST(copyright_char_at_26_is_0) {
  CHECK(copyrightCharAt(26) == '0');
}

TEST(copyright_char_at_36_is_space) {
  CHECK(copyrightCharAt(36) == ' ');
}

TEST(copyright_char_at_wraps_negative) {
  // -1 should wrap to last char ('.')
  CHECK(copyrightCharAt(-1) == '.');
}

TEST(copyright_char_at_wraps_past_end) {
  // 39 should wrap to 0 ('A')
  CHECK(copyrightCharAt(39) == 'A');
}

TEST(copyright_cycle_char_advances) {
  std::string buf = "A";
  copyrightCycleChar(buf, 0, 1);
  CHECK(buf == "B");
}

TEST(copyright_cycle_char_decreases) {
  std::string buf = "B";
  copyrightCycleChar(buf, 0, -1);
  CHECK(buf == "A");
}

TEST(copyright_cycle_char_wraps_forward) {
  std::string buf = ".";
  copyrightCycleChar(buf, 0, 1);
  CHECK(buf == "A");
}

TEST(copyright_cycle_char_wraps_backward) {
  std::string buf = "A";
  copyrightCycleChar(buf, 0, -1);
  CHECK(buf == ".");
}

TEST(copyright_cycle_char_invalid_pos_noop) {
  std::string buf = "ABC";
  copyrightCycleChar(buf, -1, 1);
  copyrightCycleChar(buf, 3, 1);
  CHECK(buf == "ABC");
}

// --- Black level estimation tests ---

TEST(estimate_black_level_uniform_border) {
  // 8x8 image, all pixels = 100, border width = 2.
  // All border pixels are 100, so black level = 100.
  std::vector<uint8_t> y(8 * 8, 100);
  CHECK(estimateBlackLevel(y.data(), 8, 8, 8, 2) == 100);
}

TEST(estimate_black_level_dark_border_bright_center) {
  // 8x8 image: border = 10, center = 200.
  std::vector<uint8_t> y(8 * 8, 200);
  for (int r = 0; r < 2; ++r)
    for (int c = 0; c < 8; ++c) {
      y[r * 8 + c] = 10;
      y[(7 - r) * 8 + c] = 10;
    }
  for (int r = 2; r < 6; ++r)
    for (int c = 0; c < 2; ++c) {
      y[r * 8 + c] = 10;
      y[r * 8 + (7 - c)] = 10;
    }
  CHECK(estimateBlackLevel(y.data(), 8, 8, 8, 2) == 10);
}

TEST(estimate_black_level_null_returns_zero) {
  CHECK(estimateBlackLevel(nullptr, 8, 8, 8, 2) == 0);
}

TEST(estimate_black_level_border_too_large_returns_zero) {
  std::vector<uint8_t> y(4 * 4, 50);
  // border=2 means 2*2 >= 4 (width), so can't sample.
  CHECK(estimateBlackLevel(y.data(), 4, 4, 4, 2) == 0);
}

TEST(estimate_black_level_averages_border) {
  // 8x8 image: top border row 0 = 20, row 1 = 40, rest = 200.
  // Border width = 2. Average of all border pixels:
  // Top rows: 8*2 = 16 pixels, sum = 8*20 + 8*40 = 480
  // Bottom rows: 8*2 = 16 pixels, all = 200, sum = 3200
  // Left/right cols (rows 2-5): 4*2 + 4*2 = 16 pixels, all = 200, sum = 3200
  // Total: 48 pixels, sum = 480 + 3200 + 3200 = 6880, avg = 143
  std::vector<uint8_t> y(8 * 8, 200);
  for (int c = 0; c < 8; ++c) {
    y[0 * 8 + c] = 20;
    y[1 * 8 + c] = 40;
  }
  CHECK(estimateBlackLevel(y.data(), 8, 8, 8, 2) == 143);
}

// --- subtractBlackLevel tests ---

TEST(subtract_black_level_basic) {
  std::vector<uint8_t> y = {100, 200, 50, 10};
  auto result = subtractBlackLevel(y.data(), 4, 1, 4, 10);
  CHECK(result.size() == 4);
  CHECK(result[0] == 90);
  CHECK(result[1] == 190);
  CHECK(result[2] == 40);
  CHECK(result[3] == 0);
}

TEST(subtract_black_level_clamps_to_zero) {
  std::vector<uint8_t> y = {5, 10, 3};
  auto result = subtractBlackLevel(y.data(), 3, 1, 3, 10);
  CHECK(result.size() == 3);
  CHECK(result[0] == 0);
  CHECK(result[1] == 0);
  CHECK(result[2] == 0);
}

TEST(subtract_black_level_zero_offset_passthrough) {
  std::vector<uint8_t> y = {0, 128, 255};
  auto result = subtractBlackLevel(y.data(), 3, 1, 3, 0);
  CHECK(result.size() == 3);
  CHECK(result[0] == 0);
  CHECK(result[1] == 128);
  CHECK(result[2] == 255);
}

TEST(subtract_black_level_null_returns_empty) {
  auto result = subtractBlackLevel(nullptr, 4, 1, 4, 10);
  CHECK(result.empty());
}

// --- rgb24ToUv tests ---

TEST(rgb24_to_uv_null_returns_empty) {
  auto result = rgb24ToUv(nullptr, 4, 4);
  CHECK(result.empty());
}

TEST(rgb24_to_uv_odd_width_returns_empty) {
  std::vector<uint8_t> rgb(3 * 3, 128);
  auto result = rgb24ToUv(rgb.data(), 3, 2);
  CHECK(result.empty());
}

TEST(rgb24_to_uv_neutral_gray) {
  // 2x2 gray image (128,128,128) → U=128, V=128 (neutral chroma).
  std::vector<uint8_t> rgb(2 * 2 * 3, 128);
  auto uv = rgb24ToUv(rgb.data(), 2, 2);
  CHECK(uv.size() == 2); // 1*1*2
  CHECK(uv[0] == 128);   // U
  CHECK(uv[1] == 128);   // V
}

TEST(rgb24_to_uv_red_pixel) {
  // 2x2 red image (255,0,0):
  // U = (-38*255)/256 + 128 = -9690/256 + 128 = -37 + 128 = 91
  // V = (112*255)/256 + 128 = 28560/256 + 128 = 111 + 128 = 239
  std::vector<uint8_t> rgb(2 * 2 * 3);
  for (size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = 255;
    rgb[i + 1] = 0;
    rgb[i + 2] = 0;
  }
  auto uv = rgb24ToUv(rgb.data(), 2, 2);
  CHECK(uv.size() == 2);
  CHECK(uv[0] == 91);   // U
  CHECK(uv[1] == 239);  // V
}

TEST(rgb24_to_uv_blue_pixel) {
  // 2x2 blue image (0,0,255):
  // U = (-38*0 - 74*0 + 112*255)/256 + 128 = 28560/256 + 128 = 111 + 128 = 239
  // V = (112*0 - 94*0 - 18*255)/256 + 128 = -4590/256 + 128 = -17 + 128 = 111
  std::vector<uint8_t> rgb(2 * 2 * 3);
  for (size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = 0;
    rgb[i + 1] = 0;
    rgb[i + 2] = 255;
  }
  auto uv = rgb24ToUv(rgb.data(), 2, 2);
  CHECK(uv.size() == 2);
  CHECK(uv[0] == 239);  // U
  CHECK(uv[1] == 111);  // V
}

TEST(rgb24_to_uv_4x4_image) {
  // 4x4 gray image → 2x2 chroma, all 128.
  std::vector<uint8_t> rgb(4 * 4 * 3, 128);
  auto uv = rgb24ToUv(rgb.data(), 4, 4);
  CHECK(uv.size() == 8); // 2*2*2
  for (size_t i = 0; i < uv.size(); ++i)
    CHECK(uv[i] == 128);
}
