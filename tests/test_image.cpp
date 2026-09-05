#include "image.h"
#include "test_runner.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace picamera;

namespace {

// Build a tiny NV12 frame and check nv12ToRgb against a hand-computed result.
// 4x2 image, stride 4. Grey (Y=128, U=V=128) should map to RGB(128,128,128)
// under BT.601 limited-range: C = 128-16 = 112; R = (298*112 + 0 + 128)>>8 =
// 33464>>8 = 130. Wait — let's compute precisely and assert against the
// function rather than hand-rolling the math, to verify the *relationship*
// (chroma neutrality, Y monotonicity, range clamping) rather than magic
// constants.
TEST(nv12_grey_is_neutral) {
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  // Y plane: all 128 (mid-grey limited-range). UV: 128,128 (neutral chroma).
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);
  // With neutral chroma, R == G == B for every pixel.
  for (uint32_t p = 0; p < w * h; ++p) {
    CHECK_EQ(rgb[p * 3 + 0], rgb[p * 3 + 1]);
    CHECK_EQ(rgb[p * 3 + 1], rgb[p * 3 + 2]);
  }
}

TEST(nv12_y_monotonic) {
  // Increasing Y must not decrease any RGB channel (with neutral chroma).
  const uint32_t w = 2;
  const uint32_t h = 2;
  const uint32_t stride = 2;
  std::vector<uint8_t> y = {16, 235, 16,
                            235}; // left col dark, right col bright
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  // pixel (0,0) dark, pixel (1,0) bright — R must increase.
  CHECK(rgb[3] > rgb[0]); // R of pixel 1 > R of pixel 0
  CHECK(rgb[4] > rgb[1]); // G
  CHECK(rgb[5] > rgb[2]); // B
}

TEST(nv12_extreme_values_no_garbage) {
  // Y = 255, U = 255, V = 255 — extreme inputs. With Y at max the luma
  // term dominates R (clamps to 255); G and B get large negative chroma
  // offsets so they land lower. Just verify R is saturated and nothing
  // is garbage (all-zero would indicate an underflow bug).
  const uint32_t w = 2;
  const uint32_t h = 2;
  const uint32_t stride = 2;
  std::vector<uint8_t> y(stride * h, 255);
  std::vector<uint8_t> uv = {255, 255, 255, 255};
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  CHECK(rgb[0] == 255);   // R saturated
  CHECK(rgb[1] < rgb[0]); // G < R (negative chroma offset)
}

TEST(nv12_odd_dimensions_handled) {
  // 3x3 (odd width and height) — the inner loops guard yRow+dy<h and x+dx<w.
  // NV12 UV plane has ceil(h/2) rows, so for h=3 that's 2 UV rows.
  const uint32_t w = 3;
  const uint32_t h = 3;
  const uint32_t stride = 4; // stride > w is realistic
  std::vector<uint8_t> y(stride * h, 64);
  std::vector<uint8_t> uv(stride * ((h + 1) / 2), 128);
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);
  // No out-of-bounds read happened (would crash under ASan). Uniform input
  // -> uniform output: last pixel equals first.
  CHECK(rgb[rgb.size() - 1] == rgb[0]);
}

TEST(nv12_odd_width_stride_equals_width) {
  // 5x2 with stride == w — the exact case where uvRow[x+1] reads past
  // the UV row on the last iteration (x=4, x+1=5 which is out of bounds
  // when the UV row is only w bytes wide). Under ASan this would crash
  // without the bounds guard.
  const uint32_t w = 5;
  const uint32_t h = 2;
  const uint32_t stride = 5;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128); // exactly w bytes per UV row
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);
  // With neutral chroma (U=V=128), R==G==B for every pixel.
  for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p) {
    CHECK_EQ(rgb[p * 3 + 0], rgb[p * 3 + 1]);
    CHECK_EQ(rgb[p * 3 + 1], rgb[p * 3 + 2]);
  }
}

TEST(nv12_red_bias_with_positive_v) {
  // V > 128 biases toward red; U = 128 neutral. So R > B for a mid-grey Y.
  const uint32_t w = 2;
  const uint32_t h = 2;
  const uint32_t stride = 2;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv = {128, 200, 128, 200}; // U=128, V=200
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  CHECK(rgb[0] > rgb[2]); // R > B at pixel (0,0)
}

TEST(nv12_odd_width_stride_gt_width_uses_last_uv_pair) {
  // 5x2 with stride=6 (stride > w). The last luma pixel (x=4) has its
  // UV pair at positions 4,5 — within the stride. The guard should
  // allow reading this pair, not fall back to the previous one.
  // Set the last UV pair to a distinctive V value (200) and the
  // previous pair to neutral (128). If the guard works correctly,
  // the last pixel's R > B (red bias from V=200). If it falls back
  // to the previous pair (V=128), R == B (neutral).
  const uint32_t w = 5;
  const uint32_t h = 2;
  const uint32_t stride = 6;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128); // neutral by default
  // Last UV pair at positions 4,5: U=128, V=200
  uv[4] = 128;
  uv[5] = 200;
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);
  // Pixel at x=4, y=0: should have red bias from V=200
  size_t lastPixel = 4 * 3;
  CHECK(rgb[lastPixel] > rgb[lastPixel + 2]); // R > B
  // Pixel at x=3, y=0: uses UV pair at positions 2,3 (neutral) — R == B
  size_t prevPixel = 3 * 3;
  CHECK_EQ(rgb[prevPixel], rgb[prevPixel + 2]); // R == B
}

TEST(nv12_large_image_multithreaded) {
  // 64x32 image — large enough to trigger the multi-threaded path
  // (h >= 8, hardware_concurrency() > 1 on most systems). Uses a simple
  // Y gradient + neutral chroma so every pixel has R==G==B and the value
  // depends only on Y. This verifies the threaded strip boundaries don't
  // corrupt pixels or drop rows.
  const uint32_t w = 64;
  const uint32_t h = 32;
  const uint32_t stride = 64;
  std::vector<uint8_t> y(stride * h);
  for (uint32_t r = 0; r < h; ++r)
    for (uint32_t c = 0; c < w; ++c)
      y[r * stride + c] = static_cast<uint8_t>(16 + (r * w + c) % 220);
  std::vector<uint8_t> uv(stride * (h / 2), 128); // neutral chroma
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);

  // With neutral chroma, R==G==B for every pixel. Verify this holds across
  // all thread-strip boundaries (rows 0, 16, 32 if 2 threads; etc.).
  for (uint32_t p = 0; p < w * h; ++p) {
    CHECK_EQ(rgb[p * 3 + 0], rgb[p * 3 + 1]);
    CHECK_EQ(rgb[p * 3 + 1], rgb[p * 3 + 2]);
  }

  // Verify a few specific pixels against the scalar formula.
  // Pixel (0,0): Y=16, C=0, R=G=B=(298*0+0+128)>>8 = 0.
  CHECK_EQ(rgb[0], 0u);
  // Pixel (0,1): Y=17, C=1, R=(298*1+128)>>8 = 426>>8 = 1.
  CHECK_EQ(rgb[3], 1u);
}

TEST(nv12_large_image_odd_dimensions) {
  // 65x33 — odd width and height, large enough for threading. Verifies
  // the NEON remainder path and the last-odd-row handling both work under
  // threading.
  const uint32_t w = 65;
  const uint32_t h = 33;
  const uint32_t stride = 66; // stride > w
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * ((h + 1) / 2), 128);
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);
  // Uniform grey input -> uniform output. Check first, last, and a
  // boundary row.
  uint8_t first = rgb[0];
  uint8_t last = rgb[rgb.size() - 1];
  CHECK_EQ(first, last);
  // A mid-boundary pixel (row 16, col 32).
  size_t mid = (static_cast<size_t>(16) * w + 32) * 3;
  CHECK_EQ(rgb[mid], first);
}

TEST(nv12_to_rgb565_grey_neutral) {
  // 4x4 NV12 grey frame -> 2x2 RGB565. With neutral chroma (U=V=128),
  // the RGB565 pixel should have R==G==B in their respective bit widths.
  const uint32_t w = 4;
  const uint32_t h = 4;
  const uint32_t stride = 4;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  std::vector<uint8_t> out(2 * 2 * 2); // 2x2 RGB565
  nv12ToRgb565Scaled(y.data(), uv.data(), w, h, stride, y.size(), uv.size(),
                     out.data(), 2, 2, out.size());
  // Each pixel: high byte = RRRRRGGG, low byte = GGGBBBBB
  // With neutral chroma and Y=128: R≈130, G≈130, B≈128 (BT.601 limited).
  // R5 = 130>>3 = 16, G6 = 130>>2 = 32, B5 = 128>>3 = 16
  // pixel = (16<<11)|(32<<5)|16 = 0x8410
  // high = 0x84, low = 0x10
  for (size_t p = 0; p < 4; ++p) {
    uint8_t hi = out[p * 2];
    uint8_t lo = out[p * 2 + 1];
    uint16_t pix = (hi << 8) | lo;
    uint8_t r5 = (pix >> 11) & 0x1F;
    uint8_t g6 = (pix >> 5) & 0x3F;
    uint8_t b5 = pix & 0x1F;
    // With neutral chroma, R and G should be close (within 1 bit).
    int r8 = r5 << 3;
    int g8 = g6 << 2;
    int b8 = b5 << 3;
    CHECK(std::abs(r8 - g8) <= 8);
    CHECK(std::abs(g8 - b8) <= 8);
  }
}

TEST(nv12_to_rgb565_y_monotonic) {
  // Brighter Y should produce a higher RGB565 pixel value (with neutral
  // chroma).
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  std::vector<uint8_t> y = {16, 16, 235, 235, 16, 16, 235, 235};
  std::vector<uint8_t> uv(stride, 128);
  std::vector<uint8_t> out(4 * 2 * 2); // 4x2 RGB565
  nv12ToRgb565Scaled(y.data(), uv.data(), w, h, stride, y.size(), uv.size(),
                     out.data(), 4, 2, out.size());
  // Pixel (0,0) dark, pixel (2,0) bright — pixel value should increase.
  uint16_t dark = (out[0] << 8) | out[1];
  uint16_t bright = (out[4] << 8) | out[5];
  CHECK(bright > dark);
}

TEST(nv12_to_rgb565_center_crop) {
  // 4x2 source (2:1 aspect) scaled to 2x2 (1:1) should center-crop
  // to 2x2 region (columns 1-2). Verify output size is correct and
  // no crash occurs.
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  std::vector<uint8_t> y(stride * h, 100);
  std::vector<uint8_t> uv(stride, 128);
  std::vector<uint8_t> out(2 * 2 * 2);
  nv12ToRgb565Scaled(y.data(), uv.data(), w, h, stride, y.size(), uv.size(),
                     out.data(), 2, 2, out.size());
  // Just verify it doesn't crash and produces non-zero output (exercises
  // the write path without a trivially-true uint8_t <= 0xFF comparison).
  bool anyNonZero = false;
  for (size_t i = 0; i < out.size(); ++i)
    anyNonZero = anyNonZero || (out[i] != 0);
  CHECK(anyNonZero);
}

// --- New tests for stride/input validation (Tier 1 security fix) ---

TEST(nv12_to_rgb_rejects_null_input) {
  auto rgb = nv12ToRgb(nullptr, nullptr, 4, 2, 4, 0, 0);
  CHECK(rgb.empty());
}

TEST(nv12_to_rgb_rejects_zero_dimensions) {
  std::vector<uint8_t> y(8, 128);
  std::vector<uint8_t> uv(4, 128);
  CHECK(nv12ToRgb(y.data(), uv.data(), 0, 2, 4, y.size(), uv.size()).empty());
  CHECK(nv12ToRgb(y.data(), uv.data(), 4, 0, 4, y.size(), uv.size()).empty());
}

TEST(nv12_to_rgb_rejects_stride_less_than_width) {
  // stride < w would cause out-of-bounds reads — must be rejected.
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 2; // stride < w
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), uv.size());
  CHECK(rgb.empty());
}

TEST(nv12_to_rgb565_rejects_null_input) {
  std::vector<uint8_t> out(8, 0);
  nv12ToRgb565Scaled(nullptr, nullptr, 4, 2, 4, 0, 0, out.data(), 2, 2,
                     out.size());
  // Should be a no-op (out unchanged)
  CHECK(out[0] == 0);
}

TEST(nv12_to_rgb565_rejects_stride_less_than_width) {
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 2;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  std::vector<uint8_t> out(2 * 2 * 2, 0xAA);
  nv12ToRgb565Scaled(y.data(), uv.data(), w, h, stride, y.size(), uv.size(),
                     out.data(), 2, 2, out.size());
  // Should be a no-op (out unchanged)
  CHECK(out[0] == 0xAA);
}

TEST(nv12_to_rgb565_rejects_small_out_buffer) {
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  // Out buffer too small for 2x2 RGB565 (needs 8 bytes, provide 4)
  std::vector<uint8_t> out(4, 0xAA);
  bool ok = nv12ToRgb565Scaled(y.data(), uv.data(), w, h, stride, y.size(),
                               uv.size(), out.data(), 2, 2, out.size());
  CHECK(!ok);
  // Should be a no-op (out unchanged)
  CHECK(out[0] == 0xAA);
}

TEST(nv12_to_rgb_rejects_overflow_dimensions) {
  // Dimensions that would overflow size_t when multiplied — should return
  // empty. w * h * 3 must overflow. On 64-bit, use very large values.
  std::vector<uint8_t> y(4, 128);
  std::vector<uint8_t> uv(2, 128);
  auto rgb = nv12ToRgb(y.data(), uv.data(), 0xFFFFFFFFu, 0xFFFFFFFFu,
                       0xFFFFFFFFu, y.size(), uv.size());
  CHECK(rgb.empty());
}

TEST(nv12_to_rgb_rejects_small_y_plane) {
  // ySize too small for stride * h — must be rejected to prevent OOB read.
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  // Pass a ySize that's too small (1 byte instead of 8)
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, 1, uv.size());
  CHECK(rgb.empty());
}

TEST(nv12_to_rgb_rejects_small_uv_plane) {
  // uvSize too small for stride * ceil(h/2) — must be rejected.
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  // Pass a uvSize that's too small (1 byte instead of 4)
  auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride, y.size(), 1);
  CHECK(rgb.empty());
}

TEST(nv12_to_rgb565_rejects_small_y_plane) {
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  std::vector<uint8_t> out(2 * 2 * 2, 0xAA);
  bool ok = nv12ToRgb565Scaled(y.data(), uv.data(), w, h, stride, 1,
                               uv.size(), // ySize too small
                               out.data(), 2, 2, out.size());
  CHECK(!ok);
  CHECK(out[0] == 0xAA);
}

TEST(nv12_to_rgb565_rejects_small_uv_plane) {
  const uint32_t w = 4;
  const uint32_t h = 2;
  const uint32_t stride = 4;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  std::vector<uint8_t> out(2 * 2 * 2, 0xAA);
  bool ok = nv12ToRgb565Scaled(y.data(), uv.data(), w, h, stride, y.size(),
                               1, // uvSize too small
                               out.data(), 2, 2, out.size());
  CHECK(!ok);
  CHECK(out[0] == 0xAA);
}

// --- downscaleNv12 tests ---

TEST(downscale_nv12_2x_produces_half_dimensions) {
  const uint32_t w = 8;
  const uint32_t h = 4;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto out =
      downscaleNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 2);
  CHECK(!out.empty());
  // outW = 8/2 = 4, outH = 4/2 = 2. Y size = 4*2=8, UV size = 4*1=4. Total
  // = 12.
  CHECK(out.size() == 12u);
}

TEST(downscale_nv12_4x_produces_quarter_dimensions) {
  const uint32_t w = 16;
  const uint32_t h = 8;
  const uint32_t stride = 16;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto out =
      downscaleNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 4);
  CHECK(!out.empty());
  // outW = 16/4 = 4, outH = 8/4 = 2. Y=8, UV=4. Total=12.
  CHECK(out.size() == 12u);
}

TEST(downscale_nv12_averages_correctly) {
  // 4x4 source with a 2x2 block of distinct values. 2x downscale should
  // average each 2x2 block. Use Y values 0,100,200,255 in a 2x2 block.
  const uint32_t w = 4;
  const uint32_t h = 4;
  const uint32_t stride = 4;
  std::vector<uint8_t> y = {0, 100, 0, 100, 200, 255, 200, 255,
                            0, 100, 0, 100, 200, 255, 200, 255};
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto out =
      downscaleNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 2);
  CHECK(!out.empty());
  // outW=2, outH=2. Y plane = 4 bytes, UV = 2 bytes.
  // Block (0,0): avg(0,100,200,255) = 555/4 = 138 (with rounding:
  // (555+2)/4=139)
  CHECK(out[0] == 139u);
  // Block (1,0): same values -> 139
  CHECK(out[1] == 139u);
  // Block (0,1): same -> 139
  CHECK(out[2] == 139u);
  CHECK(out[3] == 139u);
}

TEST(downscale_nv12_rejects_invalid_factor) {
  const uint32_t w = 8;
  const uint32_t h = 4;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  CHECK(downscaleNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 3)
            .empty());
  CHECK(downscaleNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 1)
            .empty());
  CHECK(downscaleNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 0)
            .empty());
}

TEST(downscale_nv12_rejects_null) {
  CHECK(downscaleNv12(nullptr, nullptr, 4, 4, 4, 0, 0, 2).empty());
}

TEST(downscale_nv12_rounds_to_even) {
  // 7x5 source, 2x downscale -> outW = (7/2)&~1 = 2, outH = (5/2)&~1 = 2
  const uint32_t w = 7;
  const uint32_t h = 5;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * ((h + 1) / 2), 128);
  auto out =
      downscaleNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 2);
  CHECK(!out.empty());
  // outW=2, outH=2, Y=4, UV=2, total=6
  CHECK(out.size() == 6u);
}

// --- cropNv12 tests ---

TEST(crop_nv12_produces_correct_dimensions) {
  const uint32_t w = 8;
  const uint32_t h = 8;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto out = cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 2,
                      2, 4, 4);
  CHECK(!out.empty());
  // Y = 4*4=16, UV = 4*2=8, total = 24
  CHECK(out.size() == 24u);
}

TEST(crop_nv12_copies_correct_pixels) {
  const uint32_t w = 8;
  const uint32_t h = 4;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 0);
  // Set a distinctive value at (4,2) -> crop at (2,0,4,4) maps to (2,2)
  y[2 * stride + 4] = 200;
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  auto out = cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 2,
                      0, 4, 4);
  CHECK(!out.empty());
  // outW=4, outH=4. Pixel at (2,2) in crop = y[2*4+2] = out[10]
  CHECK(out[10] == 200u);
}

TEST(crop_nv12_rejects_odd_crop_origin) {
  const uint32_t w = 8;
  const uint32_t h = 8;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  // odd cropX
  CHECK(cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 1, 0,
                 4, 4)
            .empty());
  // odd cropY
  CHECK(cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 0, 1,
                 4, 4)
            .empty());
}

TEST(crop_nv12_rejects_odd_crop_dims) {
  const uint32_t w = 8;
  const uint32_t h = 8;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  CHECK(cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 0, 0,
                 5, 4)
            .empty());
  CHECK(cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 0, 0,
                 4, 5)
            .empty());
}

TEST(crop_nv12_rejects_out_of_bounds) {
  const uint32_t w = 8;
  const uint32_t h = 8;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  // crop extends past width
  CHECK(cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 6, 0,
                 4, 4)
            .empty());
  // crop extends past height
  CHECK(cropNv12(y.data(), uv.data(), w, h, stride, y.size(), uv.size(), 0, 6,
                 4, 4)
            .empty());
}

TEST(crop_nv12_rejects_null) {
  CHECK(cropNv12(nullptr, nullptr, 4, 4, 4, 0, 0, 0, 0, 4, 4).empty());
}

// --- nv12ToRgb565CroppedScaled tests ---

TEST(nv12_rgb565_cropped_scaled_basic) {
  // 8x4 source, crop center 4x4, scale to 2x2 display.
  const uint32_t w = 8;
  const uint32_t h = 4;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  std::vector<uint8_t> out(2 * 2 * 2, 0);
  bool ok = nv12ToRgb565CroppedScaled(y.data(), uv.data(), w, h, stride,
                                      y.size(), uv.size(), 2, 0, 4, 4,
                                      out.data(), 2, 2, out.size());
  CHECK(ok);
  // With neutral chroma, R≈G≈B for all pixels.
  for (size_t p = 0; p < 4; ++p) {
    uint16_t pix = (static_cast<uint16_t>(out[p * 2]) << 8) | out[p * 2 + 1];
    uint8_t r5 = (pix >> 11) & 0x1F;
    uint8_t g6 = (pix >> 5) & 0x3F;
    uint8_t b5 = pix & 0x1F;
    int r8 = r5 << 3;
    int g8 = g6 << 2;
    int b8 = b5 << 3;
    CHECK(std::abs(r8 - g8) <= 8);
    CHECK(std::abs(g8 - b8) <= 8);
  }
}

TEST(nv12_rgb565_cropped_scaled_rejects_odd_origin) {
  const uint32_t w = 8;
  const uint32_t h = 4;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  std::vector<uint8_t> out(8, 0xAA);
  bool ok = nv12ToRgb565CroppedScaled(y.data(), uv.data(), w, h, stride,
                                      y.size(), uv.size(), 1, 0, 4, 4,
                                      out.data(), 2, 2, out.size());
  CHECK(!ok);
  CHECK(out[0] == 0xAA);
}

TEST(nv12_rgb565_cropped_scaled_rejects_out_of_bounds) {
  const uint32_t w = 8;
  const uint32_t h = 4;
  const uint32_t stride = 8;
  std::vector<uint8_t> y(stride * h, 128);
  std::vector<uint8_t> uv(stride * (h / 2), 128);
  std::vector<uint8_t> out(8, 0xAA);
  bool ok = nv12ToRgb565CroppedScaled(y.data(), uv.data(), w, h, stride,
                                      y.size(), uv.size(), 6, 0, 4, 4,
                                      out.data(), 2, 2, out.size());
  CHECK(!ok);
  CHECK(out[0] == 0xAA);
}

} // namespace
