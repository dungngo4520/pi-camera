#include "test_runner.h"
#include "output_writer.h"
#include "camera_config.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <png.h>

using namespace picamera;

namespace {

std::string tmpPath(const char *suffix) {
    char tmpl[] = "/tmp/picamera_test_XXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    close(fd);
    unlink(tmpl);  // we want the path, not the file
    return std::string(tmpl) + suffix;
}

// Build a synthetic NV12 frame (w x h, stride == w) with a neutral grey:
// Y = 149 (maps to ~149 in full range; limited-range 149 -> ~195 RGB),
// U = V = 128 (no chroma offset -> R==G==B).
std::vector<uint8_t> makeNv12Y(uint32_t w, uint32_t h) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h, 149);
}
std::vector<uint8_t> makeNv12UV(uint32_t w, uint32_t h) {
    // NV12 UV plane: w*(h/2) bytes, interleaved U,V pairs. 128 = neutral.
    return std::vector<uint8_t>(static_cast<size_t>(w) * (h / 2), 128);
}

FrameView makeNv12Frame(const std::vector<uint8_t> &y,
                        const std::vector<uint8_t> &uv,
                        uint32_t w, uint32_t h) {
    FrameView f;
    f.width = w;
    f.height = h;
    f.stride = w;
    f.plane0 = y.data();
    f.plane0Size = y.size();
    f.plane1 = uv.data();
    f.plane1Size = uv.size();
    return f;
}

} // namespace

TEST(factory_returns_writer_for_each_format) {
    CameraConfig cfg;
    for (int i = 0; i <= 4; ++i) {
        cfg.format = static_cast<OutputFormat>(i);
        auto w = makeOutputWriter(cfg.format, cfg);
        CHECK(w != nullptr);
    }
}

TEST(factory_jpeg_sw_vs_hw) {
    CameraConfig cfg;
    cfg.format = OutputFormat::JPEG;
    auto hw = makeOutputWriter(OutputFormat::JPEG, cfg, false);
    auto sw = makeOutputWriter(OutputFormat::JPEG, cfg, true);
    CHECK(hw != nullptr);
    CHECK(sw != nullptr);
    // Both are valid JPEG writers; we can't easily check the concrete type
    // without RTTI, but they must both be non-null and distinct instances.
    CHECK(hw.get() != sw.get());
}

TEST(ppm_writer_produces_valid_ppm) {
    const uint32_t w = 4, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    auto writer = makeOutputWriter(OutputFormat::PPM, {});
    REQUIRE(writer != nullptr);
    std::string path = tmpPath(".ppm");
    CHECK(writer->write(frame, path));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::string header;
    header.resize(15);
    in.read(&header[0], 15);
    CHECK(header.rfind("P6\n4 2\n255\n", 0) == 0);
    unlink(path.c_str());
}

TEST(png_writer_produces_valid_png) {
    const uint32_t w = 4, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    auto writer = makeOutputWriter(OutputFormat::PNG, {});
    REQUIRE(writer != nullptr);
    std::string path = tmpPath(".png");
    CHECK(writer->write(frame, path));

    FILE *fp = fopen(path.c_str(), "rb");
    REQUIRE(fp);
    unsigned char sig[8];
    REQUIRE(fread(sig, 1, 8, fp) == 8);
    CHECK(png_check_sig(sig, 8));

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    REQUIRE(png);
    png_infop info = png_create_info_struct(png);
    REQUIRE(info);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        CHECK(!"libpng read failed");
        unlink(path.c_str());
        return;
    }
    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);
    CHECK_EQ(png_get_image_width(png, info), w);
    CHECK_EQ(png_get_image_height(png, info), h);
    CHECK_EQ(png_get_color_type(png, info), PNG_COLOR_TYPE_RGB);
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);
    unlink(path.c_str());
}

TEST(raw_writer_writes_y_then_uv) {
    const uint32_t w = 4, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    // Mark the Y and UV planes distinctly so we can verify ordering.
    y[0] = 11;
    uv[0] = 22;
    auto frame = makeNv12Frame(y, uv, w, h);

    auto writer = makeOutputWriter(OutputFormat::RAW_NV12, {});
    REQUIRE(writer != nullptr);
    std::string path = tmpPath(".raw");
    CHECK(writer->write(frame, path));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> got(y.size() + uv.size());
    in.read(reinterpret_cast<char *>(got.data()), got.size());
    CHECK_EQ(got[0], 11u);              // first Y byte
    CHECK_EQ(got[y.size()], 22u);       // first UV byte after Y plane
    unlink(path.c_str());
}

TEST(hwjpeg_writer_truncates_at_ffd9) {
    // A fake MJPEG buffer with trailing padding after the FFD9 end marker.
    std::vector<uint8_t> fakeJpeg = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F',
        0xFF, 0xD9,
        0x00, 0x00, 0xDE, 0xAD  // padding that must be stripped
    };
    FrameView f;
    f.width = 2;
    f.height = 2;
    f.plane0 = fakeJpeg.data();
    f.plane0Size = fakeJpeg.size();

    auto writer = makeOutputWriter(OutputFormat::JPEG, {}, false);
    REQUIRE(writer != nullptr);
    std::string path = tmpPath(".jpg");
    CHECK(writer->write(f, path));

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE(in.good());
    size_t sz = static_cast<size_t>(in.tellg());
    // The written file should end at FFD9 (index 11 + 1 = 12 bytes), not the
    // full padded buffer (16 bytes).
    CHECK_EQ(sz, 12u);
    unlink(path.c_str());
}

TEST(dng_writer_produces_valid_tiff_header) {
    // Minimal raw Bayer buffer: 4x2 pixels, MIPI-packed 10-bit.
    // 8 pixels -> 2 groups of 5 bytes = 10 bytes.
    std::vector<uint8_t> rawBayer = {
        0x10, 0x20, 0x30, 0x40, 0x00,
        0x50, 0x60, 0x70, 0x80, 0x00
    };
    FrameView f;
    f.width = 4;
    f.height = 2;
    f.plane0 = rawBayer.data();
    f.plane0Size = rawBayer.size();

    CameraConfig cfg;
    cfg.format = OutputFormat::DNG;
    auto writer = makeOutputWriter(OutputFormat::DNG, cfg);
    REQUIRE(writer != nullptr);
    std::string path = tmpPath(".dng");
    CHECK(writer->write(f, path));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    // DNG/TIFF little-endian header: "II" + magic 42 (0x2A00) + IFD0 offset.
    char hdr[8];
    in.read(hdr, 8);
    CHECK_EQ(hdr[0], 'I');
    CHECK_EQ(hdr[1], 'I');
    CHECK_EQ(static_cast<uint8_t>(hdr[2]), 0x2A);
    CHECK_EQ(static_cast<uint8_t>(hdr[3]), 0x00);
    unlink(path.c_str());
}

TEST(writer_to_unwritable_path_fails) {
    const uint32_t w = 2, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    auto ppm = makeOutputWriter(OutputFormat::PPM, {});
    auto png = makeOutputWriter(OutputFormat::PNG, {});
    auto raw = makeOutputWriter(OutputFormat::RAW_NV12, {});
    CHECK(!ppm->write(frame, "/nonexistent_dir_xyz/file.ppm"));
    CHECK(!png->write(frame, "/nonexistent_dir_xyz/file.png"));
    CHECK(!raw->write(frame, "/nonexistent_dir_xyz/file.raw"));
}
