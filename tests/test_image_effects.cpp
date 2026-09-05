#include "image_effects.h"
#include "preview_helpers.h"
#include "test_runner.h"

#include <string>
#include <vector>

using picamera::applyGrainEffect;
using picamera::applyNightBoost;
using picamera::darkFrameSubtract;
using picamera::hdrMergeY;
using picamera::isPortrait;
using picamera::readFileRating;
using picamera::rotateRgb565Cw;
using picamera::writeFileRating;

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
