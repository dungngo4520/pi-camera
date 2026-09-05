#include "dng.h"
#include "test_runner.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace picamera;
using picamera::test::tmpPath;

namespace {

// Read a little-endian uint16 from a byte buffer at a given offset.
uint16_t readU16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t readU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

TEST(dng_writes_valid_tiff_header) {
  DngMetadata meta;
  meta.width = 4;
  meta.height = 4;
  meta.bitsPerPixel = 12;
  meta.blackLevel = 64;
  meta.whiteLevel = 4095;
  meta.activeTop = 0;
  meta.activeLeft = 0;
  meta.activeBottom = 4;
  meta.activeRight = 4;

  std::vector<uint8_t> raw(4 * 4 * 2, 0); // 4x4 16-bit pixels
  for (size_t i = 0; i < raw.size(); i += 2) {
    raw[i] = 200;
    raw[i + 1] = 0; // value 200
  }

  std::string path = tmpPath(".dng");
  CHECK(writeDng(path.c_str(), raw.data(), raw.size(), meta));

  // Read the file and verify TIFF header.
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});
  REQUIRE(buf.size() >= 8);

  // TIFF header: "II" (little-endian) + magic 42 + IFD0 offset.
  CHECK_EQ(buf[0], 'I');
  CHECK_EQ(buf[1], 'I');
  CHECK_EQ(readU16(&buf[2]), 42u);
  uint32_t ifd0Offset = readU32(&buf[4]);
  CHECK_EQ(ifd0Offset, 8u);

  // Parse IFD0: tag count + entries.
  uint16_t tagCount = readU16(&buf[ifd0Offset]);
  CHECK(tagCount > 10); // should have ~20+ tags

  // Find ImageWidth (tag 256) and verify.
  bool foundWidth = false;
  bool foundHeight = false;
  bool foundCompression = false;
  for (uint16_t i = 0; i < tagCount; ++i) {
    size_t entryOff = ifd0Offset + 2 + i * 12;
    uint16_t tag = readU16(&buf[entryOff]);
    uint32_t val = readU32(&buf[entryOff + 8]);
    if (tag == 256) {
      CHECK_EQ(val, 4u);
      foundWidth = true;
    }
    if (tag == 257) {
      CHECK_EQ(val, 4u);
      foundHeight = true;
    }
    if (tag == 259) {
      CHECK_EQ(val, 1u);
      foundCompression = true;
    } // uncompressed
  }
  CHECK(foundWidth);
  CHECK(foundHeight);
  CHECK(foundCompression);

  unlink(path.c_str());
}

TEST(dng_raw_data_preserved) {
  DngMetadata meta;
  meta.width = 2;
  meta.height = 2;
  meta.bitsPerPixel = 12;
  meta.blackLevel = 0;
  meta.whiteLevel = 4095;
  meta.activeTop = 0;
  meta.activeLeft = 0;
  meta.activeBottom = 2;
  meta.activeRight = 2;

  // Create 2x2 raw data with known values: 100, 200, 300, 400 (16-bit LE).
  std::vector<uint8_t> raw(8);
  uint16_t vals[4] = {100, 200, 300, 400};
  for (int i = 0; i < 4; ++i) {
    raw[i * 2] = vals[i] & 0xFF;
    raw[i * 2 + 1] = (vals[i] >> 8) & 0xFF;
  }

  std::string path = tmpPath(".dng");
  CHECK(writeDng(path.c_str(), raw.data(), raw.size(), meta));

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});

  // Find StripOffsets tag (273) to locate raw data in the file.
  uint16_t tagCount = readU16(&buf[8]);
  uint32_t stripOffset = 0;
  uint32_t stripByteCount = 0;
  for (uint16_t i = 0; i < tagCount; ++i) {
    size_t entryOff = 8 + 2 + i * 12;
    uint16_t tag = readU16(&buf[entryOff]);
    if (tag == 273)
      stripOffset = readU32(&buf[entryOff + 8]);
    if (tag == 279)
      stripByteCount = readU32(&buf[entryOff + 8]);
  }
  CHECK_EQ(stripByteCount, 8u); // 2*2*2 bytes
  REQUIRE(stripOffset + 8 <= buf.size());

  // Verify raw data is preserved at stripOffset.
  for (int i = 0; i < 4; ++i) {
    uint16_t got = readU16(&buf[stripOffset + i * 2]);
    CHECK_EQ(got, vals[i]);
  }

  unlink(path.c_str());
}

TEST(dng_exif_metadata_embedded) {
  DngMetadata meta;
  meta.width = 2;
  meta.height = 2;
  meta.bitsPerPixel = 12;
  meta.blackLevel = 0;
  meta.whiteLevel = 4095;
  meta.activeTop = 0;
  meta.activeLeft = 0;
  meta.activeBottom = 2;
  meta.activeRight = 2;
  meta.exposureTimeUs = 30000;    // 30ms = 30/1000 sec = 3/100
  meta.analogueGain = 2.0;        // ISO 200
  meta.timestampSec = 1700000000; // 2023-11-14 22:13:20 UTC

  std::vector<uint8_t> raw(8, 0);
  std::string path = tmpPath(".dng");
  CHECK(writeDng(path.c_str(), raw.data(), raw.size(), meta));

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});

  // Find ExifIFD pointer (tag 34665) in IFD0.
  uint16_t tagCount = readU16(&buf[8]);
  uint32_t exifOffset = 0;
  bool foundExifPtr = false;
  for (uint16_t i = 0; i < tagCount; ++i) {
    size_t entryOff = 8 + 2 + i * 12;
    if (readU16(&buf[entryOff]) == 34665) {
      exifOffset = readU32(&buf[entryOff + 8]);
      foundExifPtr = true;
      break;
    }
  }
  CHECK(foundExifPtr);
  REQUIRE(exifOffset > 0);

  // Parse EXIF IFD for ExposureTime (33434) and ISOSpeed (34855).
  uint16_t exifTagCount = readU16(&buf[exifOffset]);
  bool foundExposure = false;
  bool foundIso = false;
  for (uint16_t i = 0; i < exifTagCount; ++i) {
    size_t entryOff = exifOffset + 2 + i * 12;
    uint16_t tag = readU16(&buf[entryOff]);
    if (tag == 33434) { // ExposureTime
      uint32_t ratOff = readU32(&buf[entryOff + 8]);
      // Rational: num/den. 30000us = 3/100 sec.
      uint32_t num = readU32(&buf[ratOff]);
      uint32_t den = readU32(&buf[ratOff + 4]);
      // 3/100 or 30/1000 (GCD-reduced)
      CHECK(num * 100 == den * 3);
      foundExposure = true;
    }
    if (tag == 34855) { // ISOSpeedRatings
      uint32_t iso = readU32(&buf[entryOff + 8]);
      CHECK_EQ(iso, 200u);
      foundIso = true;
    }
  }
  CHECK(foundExposure);
  CHECK(foundIso);

  unlink(path.c_str());
}

TEST(dng_cfa_pattern_rggb) {
  DngMetadata meta;
  meta.width = 4;
  meta.height = 4;
  meta.bitsPerPixel = 12;
  meta.activeTop = 0;
  meta.activeLeft = 0;
  meta.activeBottom = 4;
  meta.activeRight = 4;
  // Default bayerPattern is RGGB.

  std::vector<uint8_t> raw(32, 0);
  std::string path = tmpPath(".dng");
  CHECK(writeDng(path.c_str(), raw.data(), raw.size(), meta));

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});

  // Find CFAPattern tag (33422) — should be {0,1,1,2} for RGGB.
  uint16_t tagCount = readU16(&buf[8]);
  for (uint16_t i = 0; i < tagCount; ++i) {
    size_t entryOff = 8 + 2 + i * 12;
    if (readU16(&buf[entryOff]) == 33422) {
      // 4 bytes, inline
      uint32_t val = readU32(&buf[entryOff + 8]);
      CHECK_EQ(static_cast<uint8_t>(val & 0xFF), 0u);         // R
      CHECK_EQ(static_cast<uint8_t>((val >> 8) & 0xFF), 1u);  // G
      CHECK_EQ(static_cast<uint8_t>((val >> 16) & 0xFF), 1u); // G
      CHECK_EQ(static_cast<uint8_t>((val >> 24) & 0xFF), 2u); // B
    }
  }

  unlink(path.c_str());
}

TEST(dng_rejects_non_canonical_bayer_pattern) {
  DngMetadata meta;
  meta.width = 4;
  meta.height = 4;
  meta.bitsPerPixel = 12;
  meta.activeTop = 0;
  meta.activeLeft = 0;
  meta.activeBottom = 4;
  meta.activeRight = 4;
  // "RRRR" passes per-character validation but is not a valid 2x2 CFA.
  std::memcpy(meta.bayerPattern, "RRRR", 4);

  std::vector<uint8_t> raw(32, 0);
  std::string path = tmpPath(".dng");
  CHECK(!writeDng(path.c_str(), raw.data(), raw.size(), meta));
  unlink(path.c_str());
}

TEST(dng_bits_per_sample_matches_metadata) {
  DngMetadata meta;
  meta.width = 2;
  meta.height = 2;
  meta.bitsPerPixel = 10;
  meta.blackLevel = 64;
  meta.whiteLevel = 1023;
  meta.activeTop = 0;
  meta.activeLeft = 0;
  meta.activeBottom = 2;
  meta.activeRight = 2;

  std::vector<uint8_t> raw(8, 0);
  std::string path = tmpPath(".dng");
  CHECK(writeDng(path.c_str(), raw.data(), raw.size(), meta));

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});

  // BitsPerSample is tag 258 (TypeShort, inline).
  uint16_t tagCount = readU16(&buf[8]);
  bool found = false;
  for (uint16_t i = 0; i < tagCount; ++i) {
    size_t entryOff = 8 + 2 + i * 12;
    if (readU16(&buf[entryOff]) == 258) {
      uint32_t val = readU32(&buf[entryOff + 8]);
      CHECK_EQ(val, 10u);
      found = true;
    }
  }
  CHECK(found);

  unlink(path.c_str());
}

TEST(dng_default_crop_origin_valid_rational) {
  // DefaultCropOrigin (tag 0xC61F = 50719) should be 0/1, 0/1 — not 0/0.
  DngMetadata meta;
  meta.width = 2;
  meta.height = 2;
  meta.bitsPerPixel = 10;
  meta.blackLevel = 64;
  meta.whiteLevel = 1023;
  meta.activeTop = 0;
  meta.activeLeft = 0;
  meta.activeBottom = 2;
  meta.activeRight = 2;

  std::vector<uint8_t> raw(8, 0);
  std::string path = tmpPath(".dng");
  CHECK(writeDng(path.c_str(), raw.data(), raw.size(), meta));

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});

  uint16_t tagCount = readU16(&buf[8]);
  bool found = false;
  for (uint16_t i = 0; i < tagCount; ++i) {
    size_t entryOff = 8 + 2 + i * 12;
    if (readU16(&buf[entryOff]) == 50719) { // DefaultCropOrigin
      uint32_t dataOff = readU32(&buf[entryOff + 8]);
      // 2 RATIONALs = 16 bytes: num1/den1, num2/den2
      uint32_t num1 = readU32(&buf[dataOff]);
      uint32_t den1 = readU32(&buf[dataOff + 4]);
      uint32_t num2 = readU32(&buf[dataOff + 8]);
      uint32_t den2 = readU32(&buf[dataOff + 12]);
      CHECK_EQ(num1, 0u);
      CHECK_EQ(den1, 1u); // must be non-zero
      CHECK_EQ(num2, 0u);
      CHECK_EQ(den2, 1u); // must be non-zero
      found = true;
    }
  }
  CHECK(found);

  unlink(path.c_str());
}

} // namespace
