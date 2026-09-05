#include "encoders.h"
#include "image_decode.h"
#include "safe_path.h"
#include "test_runner.h"

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace picamera;

namespace {

// Create a small test PNG and verify it decodes to the right size.
TEST(decode_png_to_rgb565) {
  // Create a 4x3 red PNG
  uint32_t w = 4;
  uint32_t h = 3;
  std::vector<uint8_t> rgb(w * h * 3);
  for (size_t i = 0; i < w * h; ++i) {
    rgb[i * 3] = 255;   // R
    rgb[i * 3 + 1] = 0; // G
    rgb[i * 3 + 2] = 0; // B
  }

  std::string path = picamera::test::tmpPath(".png");
  CHECK(writePng(path, rgb.data(), w, h));

  // Decode to 2x2 display — should produce 2*2*2 = 8 bytes
  auto out = decodeImageToRgb565(path, 2, 2);
  CHECK_EQ(out.size(), static_cast<size_t>(2 * 2 * 2));

  // Red in RGB565 = 0xF800. Big-endian: 0xF8, 0x00.
  // All pixels should be red (center-crop of a solid color image).
  for (size_t i = 0; i < out.size(); i += 2) {
    uint16_t px = (static_cast<uint16_t>(out[i]) << 8) | out[i + 1];
    // R=31, G=0, B=0 -> 0xF800
    CHECK_EQ(px, 0xF800);
  }

  unlink(path.c_str());
}

TEST(decode_nonexistent_file_returns_empty) {
  auto out = decodeImageToRgb565("/nonexistent/path/file.jpg", 128, 128);
  CHECK(out.empty());
}

TEST(decode_unsupported_format_returns_empty) {
  std::string path = picamera::test::tmpPath(".ppm");
  int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
  REQUIRE(fd >= 0);
  const char *data = "P6\n1 1\n255\nRGB";
  write(fd, data, 13);
  close(fd);

  auto out = decodeImageToRgb565(path, 128, 128);
  CHECK(out.empty());

  unlink(path.c_str());
}

// A corrupt JPEG must not crash the process (the default libjpeg error
// handler calls exit()). The custom error handler should longjmp back
// and return an empty vector.
TEST(decode_corrupt_jpeg_returns_empty) {
  std::string path = picamera::test::tmpPath(".jpg");
  int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
  REQUIRE(fd >= 0);
  // Valid JPEG SOI marker followed by garbage — enough to pass the
  // initial magic check but fail in jpeg_read_header.
  const unsigned char data[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J',  'F',
                                'I',  'F',  0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
                                0x00, 0x01, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
  write(fd, data, sizeof(data));
  close(fd);

  auto out = decodeImageToRgb565(path, 128, 128);
  CHECK(out.empty());

  unlink(path.c_str());
}

// --- New tests for dimension caps (Tier 1 security fix) ---

// Create a PNG with a huge IHDR claiming 65535x65535 dimensions.
// The decoder must reject this without trying to allocate ~12GB.
TEST(decode_png_oversized_dimensions_returns_empty) {
  std::string path = picamera::test::tmpPath(".png");
  int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
  REQUIRE(fd >= 0);

  // PNG signature
  const unsigned char sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  write(fd, sig, 8);

  // IHDR chunk: 13 bytes of data
  // Length = 13 (big-endian), Type = "IHDR"
  // Width = 65535, Height = 65535, BitDepth = 8, ColorType = 2 (RGB),
  // Compression = 0, Filter = 0, Interlace = 0
  const unsigned char ihdr[] = {
      0x00, 0x00, 0x00, 0x0D,                     // length = 13
      'I', 'H', 'D', 'R', 0x00, 0x00, 0xFF, 0xFF, // width = 65535
      0x00, 0x00, 0xFF, 0xFF,                     // height = 65535
      0x08,                                       // bit depth = 8
      0x02,                                       // color type = RGB
      0x00,                                       // compression = 0
      0x00,                                       // filter = 0
      0x00,                                       // interlace = 0
      // CRC (dummy — we don't need a valid CRC for the dimension check
      // to trigger; libpng will reject the CRC, but the user_limits check
      // happens before CRC validation in png_read_info)
      0xDE, 0xAD, 0xBE, 0xEF};
  write(fd, ihdr, sizeof(ihdr));
  close(fd);

  // This must return empty, not crash or allocate 12GB.
  auto out = decodeImageToRgb565(path, 128, 128);
  CHECK(out.empty());

  unlink(path.c_str());
}

TEST(decode_png_zero_dimensions_returns_empty) {
  std::string path = picamera::test::tmpPath(".png");
  int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
  REQUIRE(fd >= 0);

  // PNG signature
  const unsigned char sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  write(fd, sig, 8);

  // IHDR with 0x0 dimensions
  const unsigned char ihdr[] = {0x00, 0x00, 0x00, 0x0D, 'I',  'H',
                                'D',  'R',  0x00, 0x00, 0x00, 0x00, // width = 0
                                0x00, 0x00, 0x00, 0x00, // height = 0
                                0x08, 0x02, 0x00, 0x00, 0x00, 0xDE,
                                0xAD, 0xBE, 0xEF};
  write(fd, ihdr, sizeof(ihdr));
  close(fd);

  auto out = decodeImageToRgb565(path, 128, 128);
  CHECK(out.empty());

  unlink(path.c_str());
}

// A JPEG with a valid header but truncated scan data triggers a libjpeg
// error DURING jpeg_read_scanlines — after the output buffer has been
// allocated. This verifies the longjmp-safe buffer ownership: the
// allocated vector must be freed (no leak) and the function must return
// empty instead of crashing. Run under ASan to catch the leak.
TEST(decode_truncated_jpeg_after_alloc_no_leak) {
#ifdef HAVE_JPEG
  // Encode a real 16x16 JPEG so the header is valid.
  uint32_t w = 16;
  uint32_t h = 16;
  std::vector<uint8_t> rgb(w * h * 3, 128);
  std::string full = picamera::test::tmpPath(".jpg");
  CHECK(writeJpegRgb(rgb.data(), w, h, full, 90));

  // Read it back, then truncate to ~half length (cuts into scan data).
  FILE *fp = fopen(full.c_str(), "rb");
  REQUIRE(fp);
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> data(sz);
  REQUIRE(static_cast<long>(fread(data.data(), 1, sz, fp)) == sz);
  fclose(fp);
  unlink(full.c_str());

  // Write the truncated JPEG.
  std::string trunc = picamera::test::tmpPath(".jpg");
  int fd = open(trunc.c_str(), O_CREAT | O_WRONLY, 0600);
  REQUIRE(fd >= 0);
  size_t cutLen = static_cast<size_t>(sz) / 2;
  write(fd, data.data(), cutLen);
  close(fd);

  // Must return empty (decode failed) without leaking the output buffer.
  auto out = decodeImageToRgb565(trunc, 128, 128);
  CHECK(out.empty());

  unlink(trunc.c_str());
#else
  // Without libjpeg, writeJpegRgb returns false and decode is a no-op.
  // The test passes trivially — no JPEG functionality to verify.
  CHECK(true);
#endif
}

// A PNG with a valid header but truncated IDAT triggers a libpng error
// DURING png_read_image — after the output buffer has been allocated.
// Verifies the longjmp-safe buffer ownership for the PNG path.
TEST(decode_truncated_png_after_alloc_no_leak) {
  // Encode a real 16x16 PNG so the header + IHDR are valid.
  uint32_t w = 16;
  uint32_t h = 16;
  std::vector<uint8_t> rgb(w * h * 3, 128);
  std::string full = picamera::test::tmpPath(".png");
  CHECK(writePng(full, rgb.data(), w, h, 6));

  // Read it back, then truncate to ~half length (cuts into IDAT).
  FILE *fp = fopen(full.c_str(), "rb");
  REQUIRE(fp);
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> data(sz);
  REQUIRE(static_cast<long>(fread(data.data(), 1, sz, fp)) == sz);
  fclose(fp);
  unlink(full.c_str());

  // Write the truncated PNG.
  std::string trunc = picamera::test::tmpPath(".png");
  int fd = open(trunc.c_str(), O_CREAT | O_WRONLY, 0600);
  REQUIRE(fd >= 0);
  size_t cutLen = static_cast<size_t>(sz) / 2;
  write(fd, data.data(), cutLen);
  close(fd);

  // Must return empty (decode failed) without leaking the output buffer.
  auto out = decodeImageToRgb565(trunc, 128, 128);
  CHECK(out.empty());

  unlink(trunc.c_str());
}

// A symlink in the capture directory must not be followed (O_NOFOLLOW).
// This prevents a malicious symlink from redirecting the decoder to read
// arbitrary files (e.g., /etc/shadow).
TEST(decode_symlink_rejected) {
  // Create a real PNG to point at.
  uint32_t w = 4;
  uint32_t h = 3;
  std::vector<uint8_t> rgb(w * h * 3, 200);
  std::string real = picamera::test::tmpPath(".png");
  CHECK(writePng(real, rgb.data(), w, h, 6));

  // Create a symlink to it with a .png extension.
  std::string link = picamera::test::tmpPath(".png");
  int rc = symlink(real.c_str(), link.c_str());
  REQUIRE(rc == 0);

  // Must return empty — O_NOFOLLOW rejects the symlink.
  auto out = decodeImageToRgb565(link, 128, 128);
  CHECK(out.empty());

  unlink(link.c_str());
  unlink(real.c_str());
}

} // namespace
