#include "test_runner.h"
#include "image.h"

#include <cstring>
#include <vector>

using namespace picamera;

namespace {

// Build a tiny NV12 frame and check nv12ToRgb against a hand-computed result.
// 4x2 image, stride 4. Grey (Y=128, U=V=128) should map to RGB(128,128,128)
// under BT.601 limited-range: C = 128-16 = 112; R = (298*112 + 0 + 128)>>8 = 33464>>8 = 130.
// Wait — let's compute precisely and assert against the function rather than
// hand-rolling the math, to verify the *relationship* (chroma neutrality, Y
// monotonicity, range clamping) rather than magic constants.
TEST(nv12_grey_is_neutral) {
    const uint32_t w = 4, h = 2, stride = 4;
    // Y plane: all 128 (mid-grey limited-range). UV: 128,128 (neutral chroma).
    std::vector<uint8_t> y(stride * h, 128);
    std::vector<uint8_t> uv(stride * (h / 2), 128);
    auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride);
    REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);
    // With neutral chroma, R == G == B for every pixel.
    for (uint32_t p = 0; p < w * h; ++p) {
        CHECK_EQ(rgb[p * 3 + 0], rgb[p * 3 + 1]);
        CHECK_EQ(rgb[p * 3 + 1], rgb[p * 3 + 2]);
    }
}

TEST(nv12_y_monotonic) {
    // Increasing Y must not decrease any RGB channel (with neutral chroma).
    const uint32_t w = 2, h = 2, stride = 2;
    std::vector<uint8_t> y = {16, 235,  16, 235};   // left col dark, right col bright
    std::vector<uint8_t> uv(stride * (h / 2), 128);
    auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride);
    // pixel (0,0) dark, pixel (1,0) bright — R must increase.
    CHECK(rgb[3] > rgb[0]);   // R of pixel 1 > R of pixel 0
    CHECK(rgb[4] > rgb[1]);   // G
    CHECK(rgb[5] > rgb[2]);   // B
}

TEST(nv12_extreme_values_no_garbage) {
    // Y = 255, U = 255, V = 255 — extreme inputs. With Y at max the luma
    // term dominates R (clamps to 255); G and B get large negative chroma
    // offsets so they land lower. Just verify R is saturated and nothing
    // is garbage (all-zero would indicate an underflow bug).
    const uint32_t w = 2, h = 2, stride = 2;
    std::vector<uint8_t> y(stride * h, 255);
    std::vector<uint8_t> uv = {255, 255, 255, 255};
    auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride);
    CHECK(rgb[0] == 255);   // R saturated
    CHECK(rgb[1] < rgb[0]); // G < R (negative chroma offset)
}

TEST(nv12_odd_dimensions_handled) {
    // 3x3 (odd width and height) — the inner loops guard yRow+dy<h and x+dx<w.
    // NV12 UV plane has ceil(h/2) rows, so for h=3 that's 2 UV rows.
    const uint32_t w = 3, h = 3, stride = 4;  // stride > w is realistic
    std::vector<uint8_t> y(stride * h, 64);
    std::vector<uint8_t> uv(stride * ((h + 1) / 2), 128);
    auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride);
    REQUIRE(rgb.size() == static_cast<size_t>(w) * h * 3);
    // No out-of-bounds read happened (would crash under ASan). Uniform input
    // -> uniform output: last pixel equals first.
    CHECK(rgb[rgb.size() - 1] == rgb[0]);
}

TEST(nv12_red_bias_with_positive_v) {
    // V > 128 biases toward red; U = 128 neutral. So R > B for a mid-grey Y.
    const uint32_t w = 2, h = 2, stride = 2;
    std::vector<uint8_t> y(stride * h, 128);
    std::vector<uint8_t> uv = {128, 200, 128, 200};  // U=128, V=200
    auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride);
    CHECK(rgb[0] > rgb[2]);   // R > B at pixel (0,0)
}

TEST(nv12_large_image_multithreaded) {
    // 64x32 image — large enough to trigger the multi-threaded path
    // (h >= 8, hardware_concurrency() > 1 on most systems). Uses a simple
    // Y gradient + neutral chroma so every pixel has R==G==B and the value
    // depends only on Y. This verifies the threaded strip boundaries don't
    // corrupt pixels or drop rows.
    const uint32_t w = 64, h = 32, stride = 64;
    std::vector<uint8_t> y(stride * h);
    for (uint32_t r = 0; r < h; ++r)
        for (uint32_t c = 0; c < w; ++c)
            y[r * stride + c] = static_cast<uint8_t>(16 + (r * w + c) % 220);
    std::vector<uint8_t> uv(stride * (h / 2), 128);  // neutral chroma
    auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride);
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
    const uint32_t w = 65, h = 33, stride = 66;  // stride > w
    std::vector<uint8_t> y(stride * h, 128);
    std::vector<uint8_t> uv(stride * ((h + 1) / 2), 128);
    auto rgb = nv12ToRgb(y.data(), uv.data(), w, h, stride);
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

} // namespace
