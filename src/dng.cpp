#include "dng.h"
#include "safe_path.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// DNG uses little-endian ("II") byte order. The raw pixel data is
// written directly from the uint16_t buffer, so the build target must
// be little-endian. Pi (ARM) is always little-endian.
static_assert(std::endian::native == std::endian::little,
              "DNG writer requires little-endian host");
#include <unistd.h>

namespace picamera {

namespace {

// TIFF tag IDs
enum : uint16_t {
  TagImageWidth = 256,
  TagImageLength = 257,
  TagBitsPerSample = 258,
  TagCompression = 259,
  TagPhotometric = 262,
  TagStripOffsets = 273,
  TagSamplesPerPixel = 277,
  TagRowsPerStrip = 278,
  TagStripByteCounts = 279,
  TagPlanarConfig = 284,
  TagExifIFD = 34665,
  TagCFARepeatDim = 33421,
  TagCFAPattern = 33422,
  TagDNGVersion = 50706,
  TagDNGBackwardVersion = 50707,
  TagUniqueCameraModel = 50708,
  TagCFAPlaneColor = 50710,
  TagCFALayout = 50711,
  TagBlackLevel = 50714,
  TagWhiteLevel = 50717,
  TagDefaultCropOrigin = 50719,
  TagDefaultCropSize = 50720,
  TagActiveArea = 50729,
  // EXIF tags
  TagExposureTime = 33434,
  TagISOSpeed = 34855,
  TagDateTimeOriginal = 36867,
};

enum : uint16_t {
  TypeByte = 1,
  TypeAscii = 2,
  TypeShort = 3,
  TypeLong = 4,
  TypeRational = 5,
};

// A single IFD entry (12 bytes on disk).
struct IfdEntry {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  uint32_t
      valueOrOffset; // inline if total <= 4 bytes, else offset into data area
};

// Pack up to 4 bytes into a uint32_t as little-endian, for the IFD inline
// value field. Avoids host-endian memcpy which would produce wrong byte
// order on big-endian hosts when serialized via putU32.
inline uint32_t packLe32(const uint8_t *bytes, size_t len) {
  uint32_t val = 0;
  for (size_t i = 0; i < len; ++i) {
    val |= static_cast<uint32_t>(bytes[i]) << (8 * i);
  }
  return val;
}

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

  auto addInline = [&](uint16_t tag, uint16_t type, uint32_t count,
                       uint32_t val) {
    if (entries.size() >= 65535)
      throw std::overflow_error("DNG IFD entry count exceeds 65535");
    entries.push_back({tag, type, count, val});
  };

  auto addData = [&](uint16_t tag, uint16_t type, uint32_t count,
                     const uint8_t *bytes, size_t len) {
    if (entries.size() >= 65535)
      throw std::overflow_error("DNG IFD entry count exceeds 65535");
    if (len <= 4) {
      uint32_t val = packLe32(bytes, len);
      entries.push_back({tag, type, count, val});
    } else {
      if (data.size() % 2)
        data.push_back(0); // word-align
      size_t off64 = static_cast<size_t>(dataBase) + data.size();
      if (off64 > std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("DNG IFD data offset exceeds 4GB");
      uint32_t off = static_cast<uint32_t>(off64);
      data.insert(data.end(), bytes, bytes + len);
      entries.push_back({tag, type, count, off});
    }
  };

  uint32_t bpp = std::min(m.bitsPerPixel, 31u);
  uint32_t whiteLvl = m.whiteLevel > 0 ? m.whiteLevel : ((1u << bpp) - 1);
  // Clamp active-area values to prevent underflow on malformed metadata.
  uint32_t cropW =
      (m.activeRight > m.activeLeft) ? m.activeRight - m.activeLeft : 0;
  uint32_t cropH =
      (m.activeBottom > m.activeTop) ? m.activeBottom - m.activeTop : 0;

  // TIFF baseline
  addInline(TagImageWidth, TypeLong, 1, m.width);
  addInline(TagImageLength, TypeLong, 1, m.height);
  addInline(TagBitsPerSample, TypeShort, 1, bpp);
  addInline(TagCompression, TypeShort, 1, 1);
  addInline(TagPhotometric, TypeShort, 1, 32803); // CFA
  addInline(TagStripOffsets, TypeLong, 1, stripOffset);
  addInline(TagSamplesPerPixel, TypeShort, 1, 1);
  addInline(TagRowsPerStrip, TypeLong, 1, m.height);
  const uint64_t stripBytes = static_cast<uint64_t>(m.width) * m.height * 2;
  if (stripBytes > std::numeric_limits<uint32_t>::max())
    throw std::overflow_error("DNG StripByteCounts exceeds uint32_t");
  addInline(TagStripByteCounts, TypeLong, 1, static_cast<uint32_t>(stripBytes));
  addInline(TagPlanarConfig, TypeShort, 1, 1);

  // DNG version 1.6.0.0 (4 bytes, inline)
  uint8_t dngVer[] = {1, 6, 0, 0};
  addData(TagDNGVersion, TypeByte, 4, dngVer, 4);
  uint8_t dngBw[] = {1, 1, 0, 0};
  addData(TagDNGBackwardVersion, TypeByte, 4, dngBw, 4);

  // Camera model string — pad to a fixed 32-byte buffer (NUL-padded)
  // so the DNG count field matches the actual data written.
  const char *model = "Raspberry Pi HQ Camera (IMX477)";
  char modelBuf[32] = {};
  std::snprintf(modelBuf, sizeof(modelBuf), "%s", model);
  addData(TagUniqueCameraModel, TypeAscii, 32,
          reinterpret_cast<const uint8_t *>(modelBuf), 32);

  // CFA pattern — validate against the four canonical 2x2 Bayer orders.
  // A syntactically valid but non-canonical pattern (e.g. "RRRR") would
  // produce a DNG that raw processors reject.
  addInline(TagCFARepeatDim, TypeShort, 2, 2 | (2 << 16)); // 2x2 inline
  uint8_t cfa[4];
  for (int i = 0; i < 4; ++i) {
    char c = m.bayerPattern[i];
    cfa[i] = (c == 'R') ? 0 : (c == 'B') ? 2 : 1;
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
  const uint32_t activeVals[4] = {m.activeTop, m.activeLeft, m.activeBottom,
                                  m.activeRight};
  for (int i = 0; i < 4; ++i) {
    uint32_t v = activeVals[i];
    activeArea[static_cast<size_t>(i) * 4] = v & 0xFF;
    activeArea[static_cast<size_t>(i) * 4 + 1] = (v >> 8) & 0xFF;
    activeArea[static_cast<size_t>(i) * 4 + 2] = (v >> 16) & 0xFF;
    activeArea[static_cast<size_t>(i) * 4 + 3] = (v >> 24) & 0xFF;
  }
  addData(TagActiveArea, TypeLong, 4, activeArea, 16);

  // DefaultCropOrigin: 2 RATIONALs (0/1, 0/1) = 16 bytes
  // Denominators must be non-zero; 0/0 is an invalid rational.
  uint8_t cropOrigin[16] = {};
  cropOrigin[4] = 1;  // denominator of first rational (0/1)
  cropOrigin[12] = 1; // denominator of second rational (0/1)
  addData(TagDefaultCropOrigin, TypeRational, 2, cropOrigin, 16);

  // DefaultCropSize: 2 RATIONALs (cropW/1, cropH/1) = 16 bytes
  // Use explicit LE packing to avoid host-endian dependency.
  uint8_t cropSize[16] = {};
  {
    uint32_t cw = cropW;
    uint32_t ch = cropH;
    cropSize[0] = cw & 0xFF;
    cropSize[1] = (cw >> 8) & 0xFF;
    cropSize[2] = (cw >> 16) & 0xFF;
    cropSize[3] = (cw >> 24) & 0xFF;
    cropSize[4] = 1;
    cropSize[5] = 0;
    cropSize[6] = 0;
    cropSize[7] = 0;
    cropSize[8] = ch & 0xFF;
    cropSize[9] = (ch >> 8) & 0xFF;
    cropSize[10] = (ch >> 16) & 0xFF;
    cropSize[11] = (ch >> 24) & 0xFF;
    cropSize[12] = 1;
    cropSize[13] = 0;
    cropSize[14] = 0;
    cropSize[15] = 0;
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
    if (entries.size() >= 65535)
      throw std::overflow_error("DNG EXIF IFD entry count exceeds 65535");
    if (len <= 4) {
      uint32_t val = packLe32(bytes, len);
      entries.push_back({tag, type, count, val});
    } else {
      if (data.size() % 2)
        data.push_back(0);
      size_t off64 = static_cast<size_t>(dataBase) + data.size();
      if (off64 > std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("DNG EXIF IFD data offset exceeds 4GB");
      uint32_t off = static_cast<uint32_t>(off64);
      data.insert(data.end(), bytes, bytes + len);
      entries.push_back({tag, type, count, off});
    }
  };

  // ExposureTime as RATIONAL (num/den in seconds)
  if (m.exposureTimeUs > 0 &&
      m.exposureTimeUs <= std::numeric_limits<uint32_t>::max()) {
    uint32_t num = static_cast<uint32_t>(m.exposureTimeUs);
    uint32_t den = 1000000;
    uint32_t a = num;
    uint32_t b = den;
    while (b) {
      uint32_t t = a % b;
      a = b;
      b = t;
    }
    num /= a;
    den /= a;
    // Explicit LE packing for the RATIONAL (num/den).
    uint8_t rat[8];
    rat[0] = num & 0xFF;
    rat[1] = (num >> 8) & 0xFF;
    rat[2] = (num >> 16) & 0xFF;
    rat[3] = (num >> 24) & 0xFF;
    rat[4] = den & 0xFF;
    rat[5] = (den >> 8) & 0xFF;
    rat[6] = (den >> 16) & 0xFF;
    rat[7] = (den >> 24) & 0xFF;
    addData(TagExposureTime, TypeRational, 1, rat, 8);
  }

  // ISOSpeedRatings (SHORT, inline) — clamp to 16-bit range since the
  // tag type is TypeShort and values above 0xFFFF would be truncated.
  // Clamp the float before the cast to avoid UB from out-of-range
  // floating-to-integer conversion (C++20 [conv.fpint]).
  uint32_t iso;
  if (m.isoSpeed > 0) {
    iso = m.isoSpeed;
  } else if (m.analogueGain > 0) {
    float isoF = m.analogueGain * 100.0f;
    if (!std::isfinite(isoF) || isoF < 0.0f) {
      iso = 100;
    } else if (isoF > static_cast<float>(UINT32_MAX)) {
      iso = UINT32_MAX;
    } else {
      iso = static_cast<uint32_t>(isoF);
    }
  } else {
    iso = 100;
  }
  iso = std::min(iso, 0xFFFFu);
  if (entries.size() >= 65535)
    throw std::overflow_error("DNG EXIF IFD entry count exceeds 65535");
  entries.push_back({TagISOSpeed, TypeShort, 1, iso});

  // DateTimeOriginal (ASCII, 20 bytes)
  if (m.timestampSec > 0) {
    std::time_t t = static_cast<std::time_t>(m.timestampSec);
    std::tm tm;
    std::tm *tmPtr = nullptr;
#ifdef _WIN32
    if (std::gmtime_s(&tm, &t) == 0)
      tmPtr = &tm;
#else
    tmPtr = gmtime_r(&t, &tm);
#endif
    if (tmPtr) {
      char dateStr[64];
      std::snprintf(dateStr, sizeof(dateStr), "%04d:%02d:%02d %02d:%02d:%02d",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                    tm.tm_min, tm.tm_sec);
      addData(TagDateTimeOriginal, TypeAscii, 20,
              reinterpret_cast<const uint8_t *>(dateStr), 20);
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const IfdEntry &a, const IfdEntry &b) { return a.tag < b.tag; });

  return {std::move(entries), std::move(data)};
}

} // namespace

bool writeDng(const char *path, const uint8_t *rawData, size_t rawSize,
              const DngMetadata &meta, std::string *actualPath) {
  // Defensive validation: reject null data or zero size (would cause
  // malformed output or UB in buf.insert with a null base pointer).
  if (!rawData || rawSize == 0)
    return false;

  // Validate that rawSize matches the expected Bayer data size
  // (width * height * 2 bytes for 16-bit samples).
  size_t expectedSize = 0;
  if (!checkedMul(static_cast<size_t>(meta.width), meta.height, expectedSize) ||
      !checkedMul(expectedSize, 2, expectedSize)) {
    std::cerr << "DNG: pixel dimensions overflow\n";
    return false;
  }
  if (rawSize != expectedSize) {
    std::cerr << "DNG: rawSize (" << rawSize << ") != expected ("
              << expectedSize << " = " << meta.width << "x" << meta.height
              << "x2)\n";
    return false;
  }

  // Validate Bayer pattern against the four canonical 2x2 CFA orders.
  // A non-canonical pattern (e.g. "RRRR") would produce a DNG that raw
  // processors reject.
  {
    static constexpr std::string_view kCanonicalBayer[] = {"RGGB", "GRBG",
                                                           "GBRG", "BGGR"};
    std::string_view pat(meta.bayerPattern, 4);
    bool canonical = false;
    for (const auto &cb : kCanonicalBayer) {
      if (pat == cb) {
        canonical = true;
        break;
      }
    }
    if (!canonical) {
      std::cerr << "DNG: bayer pattern must be RGGB, GRBG, GBRG, or BGGR\n";
      return false;
    }
  }

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

  // Second pass: build with correct offsets.
  // If offsets overflow 32 bits, the builder throws std::overflow_error
  // and we fail the write instead of producing a corrupt DNG.
  IfdResult mainIfd;
  IfdResult exifIfd;
  try {
    mainIfd = buildMainIfd(meta, stripOffset, exifIfdOffset, mainDataOffset);
    exifIfd = buildExifIfd(meta, exifDataOffset);
  } catch (const std::overflow_error &e) {
    std::cerr << "DNG: " << e.what() << "\n";
    return false;
  } catch (const std::invalid_argument &e) {
    std::cerr << "DNG: " << e.what() << "\n";
    return false;
  }

  // Serialize to buffer
  size_t totalSize = 0;
  if (!checkedAdd(static_cast<size_t>(stripOffset), rawSize, totalSize)) {
    std::cerr << "DNG: stripOffset + rawSize overflow\n";
    return false;
  }
  std::vector<uint8_t> buf;
  buf.reserve(stripOffset);

  // TIFF header: "II" + magic 42 + offset to IFD0
  buf.push_back('I');
  buf.push_back('I');
  putU16(buf, 42);
  putU32(buf, 8);

  // IFD0 — guard against empty entries (would produce a corrupt DNG).
  if (mainIfd.entries.empty()) {
    std::cerr << "DNG: main IFD has no entries\n";
    return false;
  }
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
  // Pad to even boundary — TIFF requires word alignment for IFDs.
  if (buf.size() % 2 != 0)
    buf.push_back(0);

  // EXIF IFD — guard against empty entries (would produce a corrupt DNG).
  if (exifIfd.entries.empty()) {
    std::cerr << "DNG: EXIF IFD has no entries\n";
    return false;
  }
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
  // Pad to even boundary before the raw pixel data.
  if (buf.size() % 2 != 0)
    buf.push_back(0);

  // Write to file with O_EXCL|O_NOFOLLOW to prevent symlink attacks.
  // On EEXIST (same-ms collision), retry with _2, _3, ... suffixes.
  // Uses safeFileOpenFd (atomic open loop, no lstat probe) to avoid
  // TOCTOU races.
  std::string p;
  int fd = safeFileOpenFd(path, p);
  if (fd < 0)
    return false;

  // Write the TIFF header + IFD + EXIF (in buf), then the raw pixel
  // data directly from the source buffer — avoids copying ~49MB of
  // raw data into the vector on a 512MB Pi Zero 2 W.
  // Loop to handle short writes and EINTR (which can happen when
  // SIGINT/SIGTERM handlers are installed).
  auto writeAll = [fd](const uint8_t *data, size_t len) -> bool {
    while (len > 0) {
      ssize_t n = ::write(fd, data, len);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
      if (n == 0)
        return false;
      data += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  };

  bool ok = writeAll(buf.data(), buf.size()) && writeAll(rawData, rawSize);
  if (!ok) {
    // write() failed — close and remove the partial file.
    close(fd);
    unlink(p.c_str());
    return false;
  }
  // fsync before close so DNG captures survive power cuts (camera appliance
  // may be turned off immediately after a capture).
  if (::fsync(fd) != 0) {
    std::cerr << "DNG: fsync() failed: " << errnoString(errno) << "\n";
    close(fd);
    unlink(p.c_str());
    return false;
  }
  // close() reports deferred write errors (full FS, media removal).
  if (close(fd) != 0) {
    std::cerr << "DNG: close() failed: " << errnoString(errno) << "\n";
    unlink(p.c_str());
    return false;
  }
  if (actualPath)
    *actualPath = p;
  return true;
}

} // namespace picamera
