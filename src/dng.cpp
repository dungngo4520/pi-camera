#include "dng.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>
#include <algorithm>

namespace picamera {

namespace {

// TIFF tag IDs
enum : uint16_t {
    TagImageWidth         = 256,
    TagImageLength        = 257,
    TagBitsPerSample      = 258,
    TagCompression        = 259,
    TagPhotometric        = 262,
    TagStripOffsets       = 273,
    TagSamplesPerPixel    = 277,
    TagRowsPerStrip       = 278,
    TagStripByteCounts    = 279,
    TagPlanarConfig       = 284,
    TagExifIFD            = 34665,
    TagCFARepeatDim       = 33421,
    TagCFAPattern         = 33422,
    TagDNGVersion         = 50706,
    TagDNGBackwardVersion = 50707,
    TagUniqueCameraModel  = 50708,
    TagCFAPlaneColor      = 50710,
    TagCFALayout          = 50711,
    TagBlackLevel         = 50714,
    TagWhiteLevel         = 50717,
    TagDefaultCropOrigin  = 50719,
    TagDefaultCropSize    = 50720,
    TagActiveArea         = 50729,
    // EXIF tags
    TagExposureTime       = 33434,
    TagISOSpeed           = 34855,
    TagDateTimeOriginal   = 36867,
};

enum : uint16_t {
    TypeByte      = 1,
    TypeAscii     = 2,
    TypeShort     = 3,
    TypeLong      = 4,
    TypeRational  = 5,
};

// A single IFD entry (12 bytes on disk).
struct IfdEntry {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t valueOrOffset; // inline if total <= 4 bytes, else offset into data area
};

// LE binary writers
void putU16(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}
void putU32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

// Build a list of IFD entries + a data area for the main IFD.
// Returns the entries (sorted by tag) and fills `data` with the extra data.
// `dataBase` is the file offset where the data area will be written,
// so that offsets in entries point to absolute file positions.
struct IfdResult {
    std::vector<IfdEntry> entries;
    std::vector<uint8_t> data;
};

IfdResult buildMainIfd(const DngMetadata &m, uint32_t stripOffset,
                       uint32_t exifIfdOffset, uint32_t dataBase) {
    std::vector<IfdEntry> entries;
    std::vector<uint8_t> data;

    auto addInline = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t val) {
        entries.push_back({tag, type, count, val});
    };

    auto addData = [&](uint16_t tag, uint16_t type, uint32_t count,
                       const uint8_t *bytes, size_t len) {
        if (len <= 4) {
            uint32_t val = 0;
            memcpy(&val, bytes, len);
            entries.push_back({tag, type, count, val});
        } else {
            if (data.size() % 2) data.push_back(0); // word-align
            uint32_t off = dataBase + static_cast<uint32_t>(data.size());
            data.insert(data.end(), bytes, bytes + len);
            entries.push_back({tag, type, count, off});
        }
    };

    uint32_t whiteLvl = m.whiteLevel > 0 ? m.whiteLevel : (1u << m.bitsPerPixel) - 1;
    uint32_t cropW = m.activeRight - m.activeLeft;
    uint32_t cropH = m.activeBottom - m.activeTop;

    // TIFF baseline
    addInline(TagImageWidth, TypeLong, 1, m.width);
    addInline(TagImageLength, TypeLong, 1, m.height);
    addInline(TagBitsPerSample, TypeShort, 1, 16);
    addInline(TagCompression, TypeShort, 1, 1);
    addInline(TagPhotometric, TypeShort, 1, 32803); // CFA
    addInline(TagStripOffsets, TypeLong, 1, stripOffset);
    addInline(TagSamplesPerPixel, TypeShort, 1, 1);
    addInline(TagRowsPerStrip, TypeLong, 1, m.height);
    addInline(TagStripByteCounts, TypeLong, 1, m.width * m.height * 2);
    addInline(TagPlanarConfig, TypeShort, 1, 1);

    // DNG version 1.6.0.0 (4 bytes, inline)
    uint8_t dngVer[] = {1, 6, 0, 0};
    addData(TagDNGVersion, TypeByte, 4, dngVer, 4);
    uint8_t dngBw[] = {1, 1, 0, 0};
    addData(TagDNGBackwardVersion, TypeByte, 4, dngBw, 4);

    // Camera model string
    const char *model = "Raspberry Pi HQ Camera (IMX477)";
    addData(TagUniqueCameraModel, TypeAscii, 32,
            reinterpret_cast<const uint8_t *>(model), 32);

    // CFA pattern
    addInline(TagCFARepeatDim, TypeShort, 2, 2 | (2 << 16)); // 2x2 inline
    uint8_t cfa[4];
    for (int i = 0; i < 4; ++i) {
        cfa[i] = (m.bayerPattern[i] == 'R') ? 0 :
                 (m.bayerPattern[i] == 'B') ? 2 : 1;
    }
    addData(TagCFAPattern, TypeByte, 4, cfa, 4);
    uint8_t planeColor[] = {0, 1, 2};
    addData(TagCFAPlaneColor, TypeByte, 3, planeColor, 3);
    addInline(TagCFALayout, TypeShort, 1, 1);

    // Black/white levels
    addInline(TagBlackLevel, TypeLong, 1, m.blackLevel);
    addInline(TagWhiteLevel, TypeLong, 1, whiteLvl);

    // ActiveArea: 4 LONGs (top, left, bottom, right)
    uint8_t activeArea[16];
    for (int i = 0; i < 4; ++i) {
        uint32_t v = (&m.activeTop)[i];
        activeArea[static_cast<size_t>(i)*4]   = v & 0xFF;
        activeArea[static_cast<size_t>(i)*4+1] = (v >> 8) & 0xFF;
        activeArea[static_cast<size_t>(i)*4+2] = (v >> 16) & 0xFF;
        activeArea[static_cast<size_t>(i)*4+3] = (v >> 24) & 0xFF;
    }
    addData(TagActiveArea, TypeLong, 4, activeArea, 16);

    // DefaultCropOrigin: 2 RATIONALs (0/1, 0/1) = 16 bytes
    uint8_t cropOrigin[16] = {0};
    addData(TagDefaultCropOrigin, TypeRational, 2, cropOrigin, 16);

    // DefaultCropSize: 2 RATIONALs (cropW/1, cropH/1) = 16 bytes
    uint8_t cropSize[16] = {};
    {
        uint32_t cw = cropW;
        uint32_t ch = cropH;
        memcpy(cropSize, &cw, 4);
        uint32_t one = 1;
        memcpy(cropSize + 4, &one, 4);
        memcpy(cropSize + 8, &ch, 4);
        memcpy(cropSize + 12, &one, 4);
    }
    addData(TagDefaultCropSize, TypeRational, 2, cropSize, 16);

    // EXIF IFD pointer
    addInline(TagExifIFD, TypeLong, 1, exifIfdOffset);

    // Sort by tag (TIFF requirement)
    std::sort(entries.begin(), entries.end(),
              [](const IfdEntry &a, const IfdEntry &b) { return a.tag < b.tag; });

    return {std::move(entries), std::move(data)};
}

IfdResult buildExifIfd(const DngMetadata &m, uint32_t dataBase) {
    std::vector<IfdEntry> entries;
    std::vector<uint8_t> data;

    auto addData = [&](uint16_t tag, uint16_t type, uint32_t count,
                       const uint8_t *bytes, size_t len) {
        if (len <= 4) {
            uint32_t val = 0;
            memcpy(&val, bytes, len);
            entries.push_back({tag, type, count, val});
        } else {
            if (data.size() % 2) data.push_back(0);
            uint32_t off = dataBase + static_cast<uint32_t>(data.size());
            data.insert(data.end(), bytes, bytes + len);
            entries.push_back({tag, type, count, off});
        }
    };

    // ExposureTime as RATIONAL (num/den in seconds)
    if (m.exposureTimeUs > 0) {
        uint32_t num = m.exposureTimeUs;
        uint32_t den = 1000000;
        uint32_t a = num;
        uint32_t b = den;
        while (b) { uint32_t t = a % b; a = b; b = t; }
        num /= a; den /= a;
        uint8_t rat[8];
        memcpy(rat, &num, 4);
        memcpy(rat + 4, &den, 4);
        addData(TagExposureTime, TypeRational, 1, rat, 8);
    }

    // ISOSpeedRatings (SHORT, inline)
    uint32_t iso = m.isoSpeed > 0 ? m.isoSpeed
                   : (m.analogueGain > 0
                      ? static_cast<uint32_t>(m.analogueGain * 100) : 100);
    entries.push_back({TagISOSpeed, TypeShort, 1, iso});

    // DateTimeOriginal (ASCII, 20 bytes)
    if (m.timestampSec > 0) {
        std::time_t t = static_cast<std::time_t>(m.timestampSec);
        std::tm *tm = std::gmtime(&t);
        char dateStr[64];
        std::snprintf(dateStr, sizeof(dateStr), "%04d:%02d:%02d %02d:%02d:%02d",
                      tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                      tm->tm_hour, tm->tm_min, tm->tm_sec);
        addData(TagDateTimeOriginal, TypeAscii, 20,
                reinterpret_cast<const uint8_t *>(dateStr), 20);
    }

    std::sort(entries.begin(), entries.end(),
              [](const IfdEntry &a, const IfdEntry &b) { return a.tag < b.tag; });

    return {std::move(entries), std::move(data)};
}

} // namespace

bool writeDng(const char *path, const uint8_t *rawData, size_t rawSize,
              const DngMetadata &meta) {
    // Layout:
    //   [0..8)        TIFF header
    //   [8..8+I0)     IFD0: count(2) + N*12 + nextIFD(4)
    //   [..+D0)       IFD0 data area
    //   [..+I1)       EXIF IFD: count(2) + M*12 + nextIFD(4)
    //   [..+D1)       EXIF data area
    //   [..)          raw pixel data

    // First pass: build with dummy offsets to measure data area sizes.
    auto measureMain = buildMainIfd(meta, 0, 0, 0);
    auto measureExif = buildExifIfd(meta, 0);

    uint32_t mainTagCount = static_cast<uint32_t>(measureMain.entries.size());
    uint32_t mainIfdSize = 2 + mainTagCount * 12 + 4;
    uint32_t mainDataSize = static_cast<uint32_t>(measureMain.data.size());

    uint32_t exifTagCount = static_cast<uint32_t>(measureExif.entries.size());
    uint32_t exifIfdSize = 2 + exifTagCount * 12 + 4;
    uint32_t exifDataSize = static_cast<uint32_t>(measureExif.data.size());

    // Compute absolute offsets
    uint32_t mainDataOffset = 8 + mainIfdSize;
    uint32_t exifIfdOffset = mainDataOffset + mainDataSize;
    uint32_t exifDataOffset = exifIfdOffset + exifIfdSize;
    uint32_t stripOffset = exifDataOffset + exifDataSize;

    // Second pass: build with correct offsets
    auto mainIfd = buildMainIfd(meta, stripOffset, exifIfdOffset, mainDataOffset);
    auto exifIfd = buildExifIfd(meta, exifDataOffset);

    // Serialize to buffer
    std::vector<uint8_t> buf;
    buf.reserve(stripOffset + rawSize);

    // TIFF header: "II" + magic 42 + offset to IFD0
    buf.push_back('I'); buf.push_back('I');
    putU16(buf, 42);
    putU32(buf, 8);

    // IFD0
    putU16(buf, static_cast<uint16_t>(mainIfd.entries.size()));
    for (const auto &e : mainIfd.entries) {
        putU16(buf, e.tag);
        putU16(buf, e.type);
        putU32(buf, e.count);
        putU32(buf, e.valueOrOffset);
    }
    putU32(buf, 0); // next IFD = 0

    // IFD0 data area
    buf.insert(buf.end(), mainIfd.data.begin(), mainIfd.data.end());

    // EXIF IFD
    putU16(buf, static_cast<uint16_t>(exifIfd.entries.size()));
    for (const auto &e : exifIfd.entries) {
        putU16(buf, e.tag);
        putU16(buf, e.type);
        putU32(buf, e.count);
        putU32(buf, e.valueOrOffset);
    }
    putU32(buf, 0); // next IFD = 0

    // EXIF data area
    buf.insert(buf.end(), exifIfd.data.begin(), exifIfd.data.end());

    // Raw pixel data
    buf.insert(buf.end(), rawData, rawData + rawSize);

    // Write to file
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    out.flush();
    return out.good();
}

} // namespace picamera
