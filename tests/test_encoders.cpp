#include "encoders.h"
#include "test_runner.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using picamera::ExifMetadata;
using picamera::buildExifData;

// --- Helper: read a little-endian uint16 from a byte buffer ---
static uint16_t readLeU16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// --- Helper: read a little-endian uint32 from a byte buffer ---
static uint32_t readLeU32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// --- Helper: find an IFD entry by tag in a buffer at a given IFD offset ---
// Returns the byte offset of the entry within the buffer, or 0 if not found.
static size_t findIfdEntry(const std::vector<uint8_t> &buf, size_t ifdOff, uint16_t tag) {
    if (ifdOff + 2 > buf.size())
        return 0;
    uint16_t count = readLeU16(buf.data() + ifdOff);
    for (uint16_t i = 0; i < count; ++i) {
        size_t entryOff = ifdOff + 2 + static_cast<size_t>(i) * 12;
        if (entryOff + 12 > buf.size())
            return 0;
        if (readLeU16(buf.data() + entryOff) == tag)
            return entryOff;
    }
    return 0;
}

// --- Tests ---

TEST(exif_starts_with_exif_header) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 1.0f;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    // Must start with "Exif\0\0"
    CHECK(data.size() >= 6);
    CHECK(data[0] == 'E');
    CHECK(data[1] == 'x');
    CHECK(data[2] == 'i');
    CHECK(data[3] == 'f');
    CHECK(data[4] == 0);
    CHECK(data[5] == 0);
}

TEST(exif_tiff_header_is_little_endian) {
    ExifMetadata meta;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    // TIFF header starts at offset 6 (after "Exif\0\0")
    CHECK(data.size() >= 14);
    CHECK(data[6] == 'I');  // little-endian byte order
    CHECK(data[7] == 'I');
    // Magic number 42
    CHECK(readLeU16(data.data() + 8) == 42);
    // Offset to IFD0 = 8 (relative to TIFF start = offset 6 in buffer)
    CHECK(readLeU32(data.data() + 10) == 8);
}

TEST(exif_contains_make_entry) {
    ExifMetadata meta;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    // IFD0 starts at TIFF offset 8 = buffer offset 14
    size_t ifd0Off = 14;
    size_t entry = findIfdEntry(data, ifd0Off, 271);  // Make tag
    CHECK(entry != 0);
    // Type should be ASCII (2)
    CHECK(readLeU16(data.data() + entry + 2) == 2);
    // Read the value/offset
    uint32_t count = readLeU32(data.data() + entry + 4);
    uint32_t valOff = readLeU32(data.data() + entry + 8);
    // "Raspberry Pi\0" = 13 bytes, > 4 so stored in data area
    CHECK(count == 13);
    // The offset points into the TIFF data area (relative to TIFF start = buffer offset 6)
    size_t strBufOff = 6 + valOff;
    CHECK(strBufOff + 13 <= data.size());
    CHECK(std::memcmp(data.data() + strBufOff, "Raspberry Pi\0", 13) == 0);
}

TEST(exif_contains_model_entry) {
    ExifMetadata meta;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    size_t ifd0Off = 14;
    size_t entry = findIfdEntry(data, ifd0Off, 272);  // Model tag
    CHECK(entry != 0);
    uint32_t count = readLeU32(data.data() + entry + 4);
    uint32_t valOff = readLeU32(data.data() + entry + 8);
    // "IMX477\0" = 7 bytes, > 4 so stored in data area
    CHECK(count == 7);
    size_t strBufOff = 6 + valOff;
    CHECK(strBufOff + 7 <= data.size());
    CHECK(std::memcmp(data.data() + strBufOff, "IMX477\0", 7) == 0);
}

TEST(exif_contains_software_entry) {
    ExifMetadata meta;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    size_t ifd0Off = 14;
    size_t entry = findIfdEntry(data, ifd0Off, 305);  // Software tag
    CHECK(entry != 0);
    uint32_t count = readLeU32(data.data() + entry + 4);
    uint32_t valOff = readLeU32(data.data() + entry + 8);
    // "picamera\0" = 9 bytes, > 4 so stored in data area
    CHECK(count == 9);
    size_t strBufOff = 6 + valOff;
    CHECK(strBufOff + 9 <= data.size());
    CHECK(std::memcmp(data.data() + strBufOff, "picamera\0", 9) == 0);
}

TEST(exif_contains_exif_ifd_pointer) {
    ExifMetadata meta;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    size_t ifd0Off = 14;
    size_t entry = findIfdEntry(data, ifd0Off, 34665);  // ExifIFD pointer tag
    CHECK(entry != 0);
    // Type should be LONG (4)
    CHECK(readLeU16(data.data() + entry + 2) == 4);
    uint32_t exifIfdOff = readLeU32(data.data() + entry + 8);
    // The ExifIFD offset is relative to TIFF start (buffer offset 6)
    size_t exifIfdBufOff = 6 + exifIfdOff;
    CHECK(exifIfdBufOff < data.size());
    // ExifIFD should have a valid count
    uint16_t exifCount = readLeU16(data.data() + exifIfdBufOff);
    CHECK(exifCount > 0);
}

TEST(exif_exposure_time_rational) {
    ExifMetadata meta;
    meta.exposureTimeUs = 8000;  // 8ms = 1/125 sec
    meta.analogueGain = 1.0f;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    // Find ExifIFD pointer
    size_t ifd0Off = 14;
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    CHECK(exifPtrEntry != 0);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    // Find ExposureTime tag (33434 = 0x829A)
    size_t expEntry = findIfdEntry(data, exifIfdBufOff, 33434);
    CHECK(expEntry != 0);
    // Type should be RATIONAL (5)
    CHECK(readLeU16(data.data() + expEntry + 2) == 5);
    uint32_t valOff = readLeU32(data.data() + expEntry + 8);
    // RATIONAL is 8 bytes: num/den. 8000/1000000 = 1/125
    size_t ratBufOff = 6 + valOff;
    CHECK(ratBufOff + 8 <= data.size());
    uint32_t num = readLeU32(data.data() + ratBufOff);
    uint32_t den = readLeU32(data.data() + ratBufOff + 4);
    // 8000/1000000 reduced by GCD(8000,1000000)=1000 -> 8/1250
    // Wait: GCD(8000, 1000000) = 1000? Let me check: 1000000 / 8000 = 125, so 8000*125=1000000.
    // GCD = 8000. So 8000/8000=1, 1000000/8000=125. Result: 1/125.
    CHECK(num == 1);
    CHECK(den == 125);
}

TEST(exif_iso_speed_ratings) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 4.0f;  // ISO 400
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    // Find ExifIFD
    size_t ifd0Off = 14;
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    // Find ISOSpeedRatings tag (34855 = 0x8827)
    size_t isoEntry = findIfdEntry(data, exifIfdBufOff, 34855);
    CHECK(isoEntry != 0);
    // Type should be SHORT (3)
    CHECK(readLeU16(data.data() + isoEntry + 2) == 3);
    // Value is inline (SHORT fits in 4 bytes)
    uint32_t isoVal = readLeU32(data.data() + isoEntry + 8);
    CHECK(isoVal == 400);
}

TEST(exif_iso_default_when_gain_zero) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 0;  // no gain info
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    size_t ifd0Off = 14;
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    size_t isoEntry = findIfdEntry(data, exifIfdBufOff, 34855);
    CHECK(isoEntry != 0);
    uint32_t isoVal = readLeU32(data.data() + isoEntry + 8);
    CHECK(isoVal == 100);  // default
}

TEST(exif_date_time_original) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 1.0f;
    meta.timestampSec = 1700000000;  // Tue Nov 14 22:13:20 2023 UTC
    auto data = buildExifData(meta);
    // Find ExifIFD
    size_t ifd0Off = 14;
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    // Find DateTimeOriginal tag (36867 = 0x9003)
    size_t dtEntry = findIfdEntry(data, exifIfdBufOff, 36867);
    CHECK(dtEntry != 0);
    // Type should be ASCII (2)
    CHECK(readLeU16(data.data() + dtEntry + 2) == 2);
    uint32_t valOff = readLeU32(data.data() + dtEntry + 8);
    size_t strBufOff = 6 + valOff;
    // Should be "2023:11:14 22:13:20\0" (20 bytes)
    CHECK(strBufOff + 20 <= data.size());
    char dateStr[21] = {};
    std::memcpy(dateStr, data.data() + strBufOff, 20);
    CHECK(std::string(dateStr) == "2023:11:14 22:13:20");
}

TEST(exif_ifd_entries_sorted_by_tag) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 1.0f;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    // IFD0 entries must be sorted by tag
    size_t ifd0Off = 14;
    uint16_t count = readLeU16(data.data() + ifd0Off);
    uint16_t prevTag = 0;
    for (uint16_t i = 0; i < count; ++i) {
        size_t entryOff = ifd0Off + 2 + static_cast<size_t>(i) * 12;
        uint16_t tag = readLeU16(data.data() + entryOff);
        CHECK(tag > prevTag);
        prevTag = tag;
    }
    // ExifIFD entries must be sorted by tag
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    uint16_t exifCount = readLeU16(data.data() + exifIfdBufOff);
    prevTag = 0;
    for (uint16_t i = 0; i < exifCount; ++i) {
        size_t entryOff = exifIfdBufOff + 2 + static_cast<size_t>(i) * 12;
        uint16_t tag = readLeU16(data.data() + entryOff);
        CHECK(tag > prevTag);
        prevTag = tag;
    }
}

TEST(exif_no_timestamp_omits_date_entries) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 1.0f;
    meta.timestampSec = 0;  // no timestamp
    auto data = buildExifData(meta);
    size_t ifd0Off = 14;
    // DateTime (306) should NOT be present
    size_t dtEntry = findIfdEntry(data, ifd0Off, 306);
    CHECK(dtEntry == 0);
    // ExifIFD should still exist
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    CHECK(exifPtrEntry != 0);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    // DateTimeOriginal (36867) should NOT be present
    size_t dtoEntry = findIfdEntry(data, exifIfdBufOff, 36867);
    CHECK(dtoEntry == 0);
}

TEST(exif_no_exposure_time_omits_entry) {
    ExifMetadata meta;
    meta.exposureTimeUs = 0;  // no exposure time
    meta.analogueGain = 1.0f;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    size_t ifd0Off = 14;
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    // ExposureTime (33434) should NOT be present
    size_t expEntry = findIfdEntry(data, exifIfdBufOff, 33434);
    CHECK(expEntry == 0);
    // ISOSpeedRatings should still be present
    size_t isoEntry = findIfdEntry(data, exifIfdBufOff, 34855);
    CHECK(isoEntry != 0);
}

TEST(exif_buffer_is_word_aligned) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 1.0f;
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    // The total buffer size should be even (word-aligned)
    CHECK(data.size() % 2 == 0);
}

TEST(exif_iso_clamped_to_16bit) {
    ExifMetadata meta;
    meta.exposureTimeUs = 1000;
    meta.analogueGain = 1000.0f;  // ISO 100000, should clamp to 65535
    meta.timestampSec = 1700000000;
    auto data = buildExifData(meta);
    size_t ifd0Off = 14;
    size_t exifPtrEntry = findIfdEntry(data, ifd0Off, 34665);
    uint32_t exifIfdOff = readLeU32(data.data() + exifPtrEntry + 8);
    size_t exifIfdBufOff = 6 + exifIfdOff;
    size_t isoEntry = findIfdEntry(data, exifIfdBufOff, 34855);
    CHECK(isoEntry != 0);
    uint32_t isoVal = readLeU32(data.data() + isoEntry + 8);
    CHECK(isoVal == 65535);
}
