#include "test_runner.h"
#include "output_writer.h"
#include "camera_config.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <png.h>

using namespace picamera;
using picamera::test::tmpPath;

namespace {

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
    // Iterate over all known OutputFormat values. If a new format is added,
    // it must be included here and in makeOutputWriter.
    constexpr OutputFormat kAllFormats[] = {
        OutputFormat::PPM, OutputFormat::RAW_NV12, OutputFormat::PNG,
        OutputFormat::JPEG, OutputFormat::DNG
    };
    for (OutputFormat fmt : kAllFormats) {
        cfg.format = fmt;
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
    f.stride = 5;  // contiguous (no padding)
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

TEST(dng_writer_handles_padded_stride) {
    // 4x2 pixels, MIPI-packed 10-bit. Row packed size = 5 bytes.
    // Use stride=8 (3 bytes padding per row) to simulate DMA alignment.
    // Total source = 8 * 2 = 16 bytes.
    std::vector<uint8_t> rawBayer(16, 0xAA);  // fill with padding marker
    // Row 0: pixels 100, 200, 300, 400
    // p0=100=0x064: b0=0x64, b4[1:0]=0
    // p1=200=0x0C8: b1=0xC8, b4[3:2]=0
    // p2=300=0x12C: b2=0x2C, b4[5:4]=1 (0x10)
    // p3=400=0x190: b3=0x90, b4[7:6]=1 (0x40)
    rawBayer[0] = 0x64; rawBayer[1] = 0xC8; rawBayer[2] = 0x2C; rawBayer[3] = 0x90;
    rawBayer[4] = 0x50;  // b4: p2 bits 9-8 = 1, p3 bits 9-8 = 1
    // Row 1 (at offset 8): pixels 10, 20, 30, 40
    rawBayer[8] = 10; rawBayer[9] = 20; rawBayer[10] = 30; rawBayer[11] = 40;
    rawBayer[12] = 0x00;

    FrameView f;
    f.width = 4;
    f.height = 2;
    f.stride = 8;  // padded stride
    f.plane0 = rawBayer.data();
    f.plane0Size = rawBayer.size();

    CameraConfig cfg;
    cfg.format = OutputFormat::DNG;
    auto writer = makeOutputWriter(OutputFormat::DNG, cfg);
    REQUIRE(writer != nullptr);
    std::string path = tmpPath(".dng");
    CHECK(writer->write(f, path));

    // Read back and verify the unpacked data is correct.
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});

    // Find StripOffsets (273) and StripByteCounts (279).
    auto readU16 = [](const uint8_t *p) -> uint16_t {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    };
    auto readU32 = [](const uint8_t *p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    };
    uint16_t tagCount = readU16(&buf[8]);
    uint32_t stripOffset = 0;
    for (uint16_t i = 0; i < tagCount; ++i) {
        size_t entryOff = 8 + 2 + i * 12;
        if (readU16(&buf[entryOff]) == 273) {
            stripOffset = readU32(&buf[entryOff + 8]);
            break;
        }
    }
    REQUIRE(stripOffset + 16 <= buf.size());  // 8 pixels * 2 bytes = 16

    // Row 0: 100, 200, 300, 400
    CHECK_EQ(readU16(&buf[stripOffset + 0]), 100u);
    CHECK_EQ(readU16(&buf[stripOffset + 2]), 200u);
    CHECK_EQ(readU16(&buf[stripOffset + 4]), 300u);
    CHECK_EQ(readU16(&buf[stripOffset + 6]), 400u);
    // Row 1: 10, 20, 30, 40
    CHECK_EQ(readU16(&buf[stripOffset + 8]), 10u);
    CHECK_EQ(readU16(&buf[stripOffset + 10]), 20u);
    CHECK_EQ(readU16(&buf[stripOffset + 12]), 30u);
    CHECK_EQ(readU16(&buf[stripOffset + 14]), 40u);

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

// --- actualPath propagation tests ---
// When the requested filename is free, actualPath should equal the input.
// When it collides, the writer should report the suffixed path it used.

TEST(ppm_writer_reports_actual_path_when_free) {
    const uint32_t w = 2, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    auto writer = makeOutputWriter(OutputFormat::PPM, {});
    std::string path = tmpPath(".ppm");
    std::string actual;
    CHECK(writer->write(frame, path, &actual));
    CHECK_EQ(actual, path);
    unlink(path.c_str());
}

TEST(ppm_writer_reports_suffixed_path_on_collision) {
    const uint32_t w = 2, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    // Pre-create the target file so the writer must use a _2 suffix.
    std::string path = tmpPath(".ppm");
    int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    REQUIRE(fd >= 0);
    close(fd);

    auto writer = makeOutputWriter(OutputFormat::PPM, {});
    std::string actual;
    CHECK(writer->write(frame, path, &actual));
    CHECK(!actual.empty());
    CHECK(actual != path);
    // The suffixed path should exist and the original should be untouched.
    std::ifstream suffixed(actual);
    CHECK(suffixed.good());

    auto dot = path.rfind('.');
    std::string expected = path.substr(0, dot) + "_2" + path.substr(dot);
    CHECK_EQ(actual, expected);

    unlink(path.c_str());
    unlink(actual.c_str());
}

TEST(png_writer_reports_actual_path_when_free) {
    const uint32_t w = 2, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    auto writer = makeOutputWriter(OutputFormat::PNG, {});
    std::string path = tmpPath(".png");
    std::string actual;
    CHECK(writer->write(frame, path, &actual));
    CHECK_EQ(actual, path);
    unlink(path.c_str());
}

TEST(dng_writer_reports_actual_path_when_free) {
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
    std::string path = tmpPath(".dng");
    std::string actual;
    CHECK(writer->write(f, path, &actual));
    CHECK_EQ(actual, path);
    unlink(path.c_str());
}

#ifdef HAVE_JPEG
TEST(swjpeg_writer_reports_actual_path_when_free) {
    const uint32_t w = 2, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    auto writer = makeOutputWriter(OutputFormat::JPEG, {}, true);
    std::string path = tmpPath(".jpg");
    std::string actual;
    CHECK(writer->write(frame, path, &actual));
    CHECK_EQ(actual, path);
    unlink(path.c_str());
}
#endif

TEST(hwjpeg_writer_reports_actual_path_when_free) {
    std::vector<uint8_t> fakeJpeg = {0xFF, 0xD8, 0xFF, 0xD9};
    FrameView f;
    f.width = 2;
    f.height = 2;
    f.plane0 = fakeJpeg.data();
    f.plane0Size = fakeJpeg.size();

    auto writer = makeOutputWriter(OutputFormat::JPEG, {}, false);
    std::string path = tmpPath(".jpg");
    std::string actual;
    CHECK(writer->write(f, path, &actual));
    CHECK_EQ(actual, path);
    unlink(path.c_str());
}

TEST(raw_writer_reports_actual_path_when_free) {
    const uint32_t w = 4, h = 2;
    auto y = makeNv12Y(w, h);
    auto uv = makeNv12UV(w, h);
    auto frame = makeNv12Frame(y, uv, w, h);

    auto writer = makeOutputWriter(OutputFormat::RAW_NV12, {});
    std::string path = tmpPath(".raw");
    std::string actual;
    CHECK(writer->write(frame, path, &actual));
    CHECK_EQ(actual, path);
    unlink(path.c_str());
}
