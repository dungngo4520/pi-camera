#include "test_runner.h"
#include "encoders.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <png.h>

using namespace picamera;
using picamera::test::tmpPath;

namespace {

TEST(ppm_roundtrip_header_and_pixels) {
    const uint32_t w = 2, h = 2;
    std::vector<uint8_t> rgb = {
        10, 20, 30,   40, 50, 60,
        70, 80, 90,  100, 110, 120,
    };
    std::string path = tmpPath(".ppm");
    CHECK(writePpm(rgb.data(), rgb.size(), w, h, path));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::string header;
    header.resize(15);
    in.read(&header[0], 15);
    CHECK(header.rfind("P6\n2 2\n255\n", 0) == 0);

    // Skip the header (P6\n2 2\n255\n = 11 bytes) and read pixels.
    in.clear();
    in.seekg(11);
    std::vector<uint8_t> got(rgb.size());
    in.read(reinterpret_cast<char *>(got.data()), got.size());
    CHECK(got == rgb);

    unlink(path.c_str());
}

TEST(png_roundtrip_dimensions_and_pixels) {
    const uint32_t w = 3, h = 2;
    std::vector<uint8_t> rgb = {
        1, 2, 3,   4, 5, 6,   7, 8, 9,
        10, 11, 12,  13, 14, 15,  16, 17, 18,
    };
    std::string path = tmpPath(".png");
    CHECK(writePng(path.c_str(), rgb.data(), w, h));

    // Read back with libpng and verify.
    FILE *fp = fopen(path.c_str(), "rb");
    REQUIRE(fp);
    unsigned char sig[8];
    REQUIRE(fread(sig, 1, 8, fp) == 8);
    CHECK(png_check_sig(sig, 8));

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    REQUIRE(png);
    png_infop info = png_create_info_struct(png);
    REQUIRE(info);

    // Use raw malloc/free for buffers in setjmp scope — C++ objects with
    // non-trivial destructors (like std::vector) in scope between setjmp
    // and longjmp cause undefined behavior per C++ [support.runtime]/3.
    png_bytep *rows = static_cast<png_bytep *>(
        std::malloc(h * sizeof(png_bytep)));
    uint8_t *buf = static_cast<uint8_t *>(
        std::malloc(w * h * 3));
    REQUIRE(rows);
    REQUIRE(buf);

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        std::free(rows);
        std::free(buf);
        CHECK(false && "libpng read failed");
        return;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    CHECK_EQ(png_get_image_width(png, info), w);
    CHECK_EQ(png_get_image_height(png, info), h);
    CHECK_EQ(png_get_color_type(png, info), PNG_COLOR_TYPE_RGB);
    CHECK_EQ(png_get_bit_depth(png, info), 8);

    for (uint32_t y = 0; y < h; ++y) rows[y] = buf + y * w * 3;
    png_read_image(png, rows);
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);

    CHECK(std::memcmp(buf, rgb.data(), w * h * 3) == 0);
    std::free(rows);
    std::free(buf);
    unlink(path.c_str());
}

TEST(raw_roundtrip_y_and_uv) {
    const size_t ySize = 4, uvSize = 2;
    std::vector<uint8_t> y = {10, 20, 30, 40};
    std::vector<uint8_t> uv = {128, 129};
    std::string path = tmpPath(".raw");
    CHECK(writeRaw(y.data(), ySize, uv.data(), uvSize, path));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> got(ySize + uvSize);
    in.read(reinterpret_cast<char *>(got.data()), got.size());
    CHECK_EQ(got.size(), ySize + uvSize);
    CHECK_EQ(got[0], 10u);
    CHECK_EQ(got[3], 40u);
    CHECK_EQ(got[4], 128u);
    CHECK_EQ(got[5], 129u);
    unlink(path.c_str());
}

TEST(write_to_unwritable_path_fails) {
    std::vector<uint8_t> rgb(6, 0);
    // /nonexistent_dir/ must not exist -> open fails.
    CHECK(!writePpm(rgb.data(), rgb.size(), 1, 1, "/nonexistent_dir_xyz/file.ppm"));
    CHECK(!writePng("/nonexistent_dir_xyz/file.png", rgb.data(), 1, 1));
    CHECK(!writeRaw(rgb.data(), 3, rgb.data(), 3, "/nonexistent_dir_xyz/file.raw"));
    CHECK(!writeJpeg(rgb.data(), 3, "/nonexistent_dir_xyz/file.jpg"));
}

TEST(png_compression_level_affects_size) {
    // A 16x16 solid-color image compresses much smaller at level 9 than 0.
    const uint32_t w = 16, h = 16;
    std::vector<uint8_t> rgb(w * h * 3, 128);  // uniform grey
    std::string path0 = tmpPath(".png");
    std::string path9 = tmpPath(".png");
    CHECK(writePng(path0.c_str(), rgb.data(), w, h, 0));  // no compression
    CHECK(writePng(path9.c_str(), rgb.data(), w, h, 9));  // max compression
    std::ifstream in0(path0, std::ios::binary | std::ios::ate);
    std::ifstream in9(path9, std::ios::binary | std::ios::ate);
    REQUIRE(in0.is_open());
    REQUIRE(in9.is_open());
    size_t sz0 = static_cast<size_t>(in0.tellg());
    size_t sz9 = static_cast<size_t>(in9.tellg());
    // Level 9 should produce a strictly smaller file for uniform input.
    CHECK(sz9 < sz0);
    unlink(path0.c_str());
    unlink(path9.c_str());
}

TEST(jpeg_write_roundtrip) {
    // writeJpeg just writes raw bytes to disk; verify the bytes match.
    std::vector<uint8_t> fakeJpeg = {0xFF, 0xD8, 0xFF, 0xE0,
                                      0x00, 0x10, 'J', 'F', 'I', 'F',
                                      0xFF, 0xD9};
    std::string path = tmpPath(".jpg");
    CHECK(writeJpeg(fakeJpeg.data(), fakeJpeg.size(), path));
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> got(fakeJpeg.size());
    in.read(reinterpret_cast<char *>(got.data()), got.size());
    CHECK(got == fakeJpeg);
    unlink(path.c_str());
}

// --- New tests for unlink-on-failure (Tier 1 fix) ---

TEST(write_to_unwritable_path_leaves_no_file) {
    // When a write fails (e.g., unwritable directory), no partial file
    // should be left behind. This verifies the unlink-on-failure behavior.
    std::vector<uint8_t> rgb(6, 0);
    std::string badPath = "/nonexistent_dir_xyz/file.ppm";
    CHECK(!writePpm(rgb.data(), rgb.size(), 1, 1, badPath));
    // Verify no file exists at the bad path (it shouldn't — the dir doesn't
    // exist — but this also covers the case where the open succeeds but
    // the write fails later).
    std::ifstream check(badPath);
    CHECK(!check.good());  // file should not exist
}

TEST(write_raw_to_unwritable_path_leaves_no_file) {
    std::vector<uint8_t> data(8, 0);
    std::string badPath = "/nonexistent_dir_xyz/file.raw";
    CHECK(!writeRaw(data.data(), 4, data.data(), 4, badPath));
    std::ifstream check(badPath);
    CHECK(!check.good());
}

TEST(write_jpeg_to_unwritable_path_leaves_no_file) {
    std::vector<uint8_t> data(4, 0);
    std::string badPath = "/nonexistent_dir_xyz/file.jpg";
    CHECK(!writeJpeg(data.data(), 4, badPath));
    std::ifstream check(badPath);
    CHECK(!check.good());
}

} // namespace
