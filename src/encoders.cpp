#include "encoders.h"
#include "safe_path.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include <png.h>
#include <csetjmp>
#include <jpeglib.h>

namespace picamera {

namespace {

enum : uint16_t {
  ExifTagMake = 271,               // 0x010F
  ExifTagModel = 272,              // 0x0110
  ExifTagSoftware = 305,           // 0x0131
  ExifTagDateTime = 306,           // 0x0132
  ExifTagCopyright = 33432,        // 0x8298
  ExifTagExifIFD = 34665,          // 0x8769
  ExifTagExposureTime = 33434,     // 0x829A
  ExifTagISOSpeed = 34855,         // 0x8827
  ExifTagColorSpace = 40961,       // 0xA001
  ExifTagDateTimeOriginal = 36867, // 0x9003
};

enum : uint16_t {
  ExifTypeByte = 1,
  ExifTypeAscii = 2,
  ExifTypeShort = 3,
  ExifTypeLong = 4,
  ExifTypeRational = 5,
};

void exifPutU16(std::vector<uint8_t> &buf, uint16_t v) {
  buf.push_back(v & 0xFF);
  buf.push_back((v >> 8) & 0xFF);
}
void exifPutU32(std::vector<uint8_t> &buf, uint32_t v) {
  buf.push_back(v & 0xFF);
  buf.push_back((v >> 8) & 0xFF);
  buf.push_back((v >> 16) & 0xFF);
  buf.push_back((v >> 24) & 0xFF);
}

// Pack little-endian bytes into a uint32_t inline value.
uint32_t exifPackLe32(const uint8_t *bytes, size_t len) {
  uint32_t val = 0;
  for (size_t i = 0; i < len; ++i)
    val |= static_cast<uint32_t>(bytes[i]) << (8 * i);
  return val;
}

struct ExifIfdEntry {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  uint32_t valueOrOffset;
};

// ASCII IFD entry: inline if ≤4 bytes, else spilled to the data area.
void addAsciiEntry(std::vector<ExifIfdEntry> &entries,
                   std::vector<uint8_t> &data, uint32_t dataStart, uint16_t tag,
                   const std::string &s) {
  std::string nulStr = s + '\0';
  uint32_t count = static_cast<uint32_t>(nulStr.size());
  if (nulStr.size() <= 4) {
    uint8_t tmp[4] = {};
    for (size_t i = 0; i < nulStr.size(); ++i)
      tmp[i] = static_cast<uint8_t>(nulStr[i]);
    entries.push_back(
        {tag, ExifTypeAscii, count, exifPackLe32(tmp, nulStr.size())});
  } else {
    if (data.size() % 2 != 0)
      data.push_back(0);
    uint32_t off = dataStart + static_cast<uint32_t>(data.size());
    data.insert(data.end(), nulStr.begin(), nulStr.end());
    entries.push_back({tag, ExifTypeAscii, count, off});
  }
}

void alignData(std::vector<uint8_t> &data) {
  if (data.size() % 2 != 0)
    data.push_back(0);
}

uint32_t exifGcd(uint32_t a, uint32_t b) {
  while (b) {
    uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

} // namespace

namespace {

// EXIF DateTime string from a Unix timestamp.
std::string formatExifDateTime(uint32_t timestampSec) {
  if (timestampSec == 0)
    return {};
  std::time_t t = static_cast<std::time_t>(timestampSec);
  std::tm tm;
  std::tm *tmPtr = nullptr;
#ifdef _WIN32
  if (std::gmtime_s(&tm, &t) == 0)
    tmPtr = &tm;
#else
  tmPtr = gmtime_r(&t, &tm);
#endif
  if (!tmPtr)
    return {};
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

// Max retries on EEXIST — suffix scan goes _2.._999, so 999 is plenty.
constexpr int kMaxNameRetries = 999;

// Returns true if a size_t byte count fits in std::streamsize (the signed
// type used by ostream::write). On 32-bit platforms streamsize is 32-bit
// signed, so a >2GiB buffer would produce a negative cast and misbehavior.
bool fitsStreamSize(size_t size) {
  return size <=
         static_cast<size_t>(std::numeric_limits<std::streamsize>::max());
}

// libpng longjmp wrapper: only trivial locals across setjmp.
int writePngCore(png_structp png, png_infop *outInfo, FILE *fp,
                 const uint8_t *rgb, uint32_t w, uint32_t h,
                 int compressionLevel, png_bytep **outRows,
                 png_textp textChunks, int numText) {
  *outRows = nullptr;
  *outInfo = nullptr;
  if (setjmp(png_jmpbuf(png)))
    return -1;

  png_infop info = png_create_info_struct(png);
  if (!info)
    return -1;
  *outInfo = info;

  // Clamp libpng compression to [0,9].
  int level = std::clamp(compressionLevel, 0, 9);
  png_set_compression_level(png, level);
  if (level <= 1)
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);

  png_init_io(png, fp);
  png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  if (textChunks && numText > 0)
    png_set_text(png, info, textChunks, numText);
  png_write_info(png, info);

  size_t rowsBytes = 0;
  if (!checkedMul(static_cast<size_t>(h), sizeof(png_bytep), rowsBytes))
    return -1;
  png_bytep *rows = static_cast<png_bytep *>(std::malloc(rowsBytes));
  if (!rows)
    return -1;
  *outRows = rows;

  for (uint32_t y = 0; y < h; ++y)
    rows[y] = const_cast<png_bytep>(rgb + static_cast<size_t>(y) * w * 3);

  png_write_image(png, rows);
  png_write_end(png, nullptr);
  std::free(static_cast<void *>(rows));
  *outRows = nullptr;
  return 0;
}

// JPEG error handler context — must outlive the jpeg_compress_struct
// because jpeg_destroy_compress may access cinfo->err.
struct JpegErrCtx {
  struct jpeg_error_mgr base;
  std::jmp_buf setjmpBuf;
};
static_assert(
    std::is_standard_layout_v<JpegErrCtx>,
    "JpegErrCtx must be standard-layout for reinterpret_cast from base");

int writeJpegRgbCore(struct jpeg_compress_struct *cinfo, JpegErrCtx *jerr,
                     FILE *fp, const uint8_t *rgb, uint32_t w, uint32_t h,
                     int quality, JSAMPROW **outRowPtrs, bool *outCreated,
                     const uint8_t *exifData, size_t exifSize) {
  *outRowPtrs = nullptr;
  *outCreated = false;
  cinfo->err = jpeg_std_error(&jerr->base);
  jerr->base.error_exit = [](j_common_ptr c) {
    auto *e = reinterpret_cast<JpegErrCtx *>(c->err);
    std::longjmp(e->setjmpBuf, 1);
  };

  if (setjmp(jerr->setjmpBuf))
    return -1;

  jpeg_create_compress(cinfo);
  *outCreated = true;
  jpeg_stdio_dest(cinfo, fp);

  cinfo->image_width = w;
  cinfo->image_height = h;
  cinfo->input_components = 3;
  cinfo->in_color_space = JCS_RGB;

  jpeg_set_defaults(cinfo);
  jpeg_set_quality(cinfo, quality, TRUE);
  jpeg_start_compress(cinfo, TRUE);

  // Insert EXIF APP1 marker.
  if (exifData && exifSize > 0) {
    jpeg_write_marker(cinfo, 0xE1, reinterpret_cast<const JOCTET *>(exifData),
                      static_cast<unsigned int>(exifSize));
  }

  size_t rowPtrsBytes = 0;
  if (!checkedMul(static_cast<size_t>(h), sizeof(JSAMPROW), rowPtrsBytes))
    return -1;
  JSAMPROW *rowPtrs = static_cast<JSAMPROW *>(std::malloc(rowPtrsBytes));
  if (!rowPtrs)
    return -1;
  *outRowPtrs = rowPtrs;

  for (uint32_t r = 0; r < h; ++r)
    rowPtrs[r] = const_cast<JSAMPROW>(rgb + static_cast<size_t>(r) * w * 3);

  bool compressOk = true;
  while (cinfo->next_scanline < cinfo->image_height) {
    JDIMENSION written =
        jpeg_write_scanlines(cinfo, &rowPtrs[cinfo->next_scanline], 1);
    if (written != 1) {
      compressOk = false;
      break;
    }
  }

  if (compressOk)
    jpeg_finish_compress(cinfo);
  // Free rowPtrs after jpeg_finish_compress so the row pointers remain
  // valid for the duration of compression (libjpeg may reference them).
  std::free(static_cast<void *>(rowPtrs));
  *outRowPtrs = nullptr;
  return compressOk ? 0 : -1;
}

// Atomic O_EXCL file open with _N suffix collision retry.
FILE *safeFileOpen(const std::string &path, std::string &outPath) {
  const PathStemExt se = splitPathStemExt(path);

  for (int i = 1; i <= kMaxNameRetries; ++i) {
    std::string p = suffixedCandidate(se, path, i);
    int fd =
        open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
             S_IRUSR | S_IWUSR | S_IRGRP);
    if (fd >= 0) {
      outPath = p;
      FILE *fp = fdopen(fd, "wb");
      if (!fp) {
        close(fd);
        unlink(p.c_str());
      }
      return fp;
    }
    // EEXIST: file already exists (normal collision, retry with suffix).
    // ELOOP: final component is a symlink (O_NOFOLLOW rejects it).
    //   Retry with suffix — a symlink at photo.jpg shouldn't prevent
    //   saving photo_2.jpg.
    if (errno != EEXIST && errno != ELOOP)
      return nullptr;
  }
  return nullptr;
}

// Flush, fsync, close; unlink on error.
bool finishFile(FILE *fp, const std::string &localPath, const char *tag) {
  if (fflush(fp) != 0) {
    std::cerr << tag << ": fflush() failed: " << errnoString(errno) << "\n";
    fclose(fp);
    unlink(localPath.c_str());
    return false;
  }
  if (::fsync(fileno(fp)) != 0) {
    std::cerr << tag << ": fsync() failed: " << errnoString(errno) << "\n";
    fclose(fp);
    unlink(localPath.c_str());
    return false;
  }
  if (fclose(fp) != 0) {
    std::cerr << tag << ": fclose() failed: " << errnoString(errno) << "\n";
    unlink(localPath.c_str());
    return false;
  }
  return true;
}

// Portable fd-based output streambuf (avoids close()+reopen() TOCTOU race).
class FdOutStreamBuf : public std::streambuf {
public:
  explicit FdOutStreamBuf(int fd) : fd_(fd) {
    setp(buffer_, buffer_ + sizeof(buffer_));
  }
  ~FdOutStreamBuf() override {
    if (fd_ >= 0) {
      sync();
      (void)::fsync(fd_);
      close(fd_);
    }
  }
  FdOutStreamBuf(const FdOutStreamBuf &) = delete;
  FdOutStreamBuf &operator=(const FdOutStreamBuf &) = delete;

  // Flush, fsync, close, returning I/O status.
  bool finish() {
    if (fd_ < 0)
      return true;
    if (sync() != 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }
    if (::fsync(fd_) != 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }
    int rc = close(fd_);
    fd_ = -1;
    // Treat EINTR as success.
    if (rc != 0 && errno == EINTR)
      rc = 0;
    return rc == 0;
  }

protected:
  int_type overflow(int_type ch) override {
    if (ch == traits_type::eof()) {
      return sync() == 0 ? traits_type::not_eof(0) : traits_type::eof();
    }
    // Buffer is full (pptr() == epptr()). Flush it first, then write
    // the new character into the reset buffer. Writing before flushing
    // would store one past the end of buffer_ (OOB write).
    if (sync() != 0)
      return traits_type::eof();
    *pptr() = static_cast<char>(ch);
    pbump(1);
    return ch;
  }

  // Override xsputn to avoid the default byte-at-a-time sputc loop.
  // Copies data in bulk into the put area, flushing in chunks when full.
  std::streamsize xsputn(const char_type *s, std::streamsize n) override {
    if (n <= 0)
      return 0;
    std::streamsize written = 0;
    while (n > 0) {
      std::streamsize avail = epptr() - pptr();
      if (avail <= 0) {
        if (sync() != 0)
          return written;
        avail = epptr() - pptr();
      }
      std::streamsize chunk = std::min(n, avail);
      std::memcpy(pptr(), s, static_cast<size_t>(chunk));
      pbump(static_cast<int>(chunk));
      s += chunk;
      n -= chunk;
      written += chunk;
    }
    return written;
  }

  int sync() override {
    if (pptr() > pbase()) {
      size_t remaining = static_cast<size_t>(pptr() - pbase());
      const char *data = pbase();
      while (remaining > 0) {
        ssize_t n = write(fd_, data, remaining);
        if (n < 0) {
          if (errno == EINTR)
            continue; // interrupted by signal — retry
          setp(buffer_, buffer_ + sizeof(buffer_));
          return -1;
        }
        if (n == 0) {
          setp(buffer_, buffer_ + sizeof(buffer_));
          return -1;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
      }
      setp(buffer_, buffer_ + sizeof(buffer_));
    }
    return 0;
  }

private:
  int fd_;
  char buffer_[4096];
};

// O_EXCL ostream wrapper over a safe fd.
class SafeOstream {
public:
  SafeOstream(std::unique_ptr<FdOutStreamBuf> b,
              std::unique_ptr<std::ostream> s, std::string p)
      : buf_(std::move(b)), stream_(std::move(s)), path_(std::move(p)) {}

  std::ostream &stream() { return *stream_; }
  const std::string &path() const { return path_; }

  // Flush and close the underlying fd, returning false on I/O error.
  bool finish() const { return buf_ ? buf_->finish() : false; }

private:
  std::unique_ptr<FdOutStreamBuf> buf_;
  std::unique_ptr<std::ostream> stream_;
  std::string path_; // actual file path (for unlink on failure)
};
std::unique_ptr<SafeOstream> safeOfstream(const std::string &path) {
  const PathStemExt se = splitPathStemExt(path);

  for (int i = 1; i <= kMaxNameRetries; ++i) {
    std::string p = suffixedCandidate(se, path, i);
    int fd =
        open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
             S_IRUSR | S_IWUSR | S_IRGRP);
    if (fd >= 0) {
      auto buf = std::make_unique<FdOutStreamBuf>(fd);
      auto stream = std::make_unique<std::ostream>(buf.get());
      return std::make_unique<SafeOstream>(std::move(buf), std::move(stream),
                                           p);
    }
    if (errno != EEXIST && errno != ELOOP)
      return nullptr;
  }
  return nullptr;
}

} // namespace

std::vector<uint8_t> buildExifData(const ExifMetadata &meta) {
  // Layout (offsets relative to TIFF header, after "Exif\0\0"):
  //   TIFF header | IFD0 + data | ExifIFD + data

  const std::string dateStr = formatExifDateTime(meta.timestampSec);
  const bool hasDate = !dateStr.empty();
  const bool hasCopyright = !meta.copyright.empty();

  const uint16_t ifd0Count =
      static_cast<uint16_t>(4 + (hasDate ? 1 : 0) + (hasCopyright ? 1 : 0));
  const uint32_t ifd0Size = 2 + static_cast<uint32_t>(ifd0Count) * 12 + 4;
  const uint32_t ifd0DataStart = 8 + ifd0Size;

  std::vector<uint8_t> ifd0Data;
  std::vector<ExifIfdEntry> ifd0Entries;
  addAsciiEntry(ifd0Entries, ifd0Data, ifd0DataStart, ExifTagMake,
                "Raspberry Pi");
  addAsciiEntry(ifd0Entries, ifd0Data, ifd0DataStart, ExifTagModel, "IMX477");
  addAsciiEntry(ifd0Entries, ifd0Data, ifd0DataStart, ExifTagSoftware,
                "picamera");
  if (hasDate)
    addAsciiEntry(ifd0Entries, ifd0Data, ifd0DataStart, ExifTagDateTime,
                  dateStr);
  if (hasCopyright)
    addAsciiEntry(ifd0Entries, ifd0Data, ifd0DataStart, ExifTagCopyright,
                  meta.copyright);

  const uint32_t exifIfdStart =
      ifd0DataStart + static_cast<uint32_t>(ifd0Data.size());
  ifd0Entries.push_back({ExifTagExifIFD, ExifTypeLong, 1, exifIfdStart});

  // TIFF requires IFD entries sorted by tag.
  std::sort(ifd0Entries.begin(), ifd0Entries.end(),
            [](const ExifIfdEntry &a, const ExifIfdEntry &b) {
              return a.tag < b.tag;
            });

  // --- ExifIFD ---
  const bool hasExposure =
      (meta.exposureTimeUs > 0 &&
       meta.exposureTimeUs <= std::numeric_limits<uint32_t>::max());
  const uint16_t exifIfdCount =
      static_cast<uint16_t>((hasExposure ? 1 : 0) + 1 + (hasDate ? 1 : 0) + 1);
  const uint32_t exifIfdSize = 2 + static_cast<uint32_t>(exifIfdCount) * 12 + 4;
  const uint32_t exifIfdDataStart = exifIfdStart + exifIfdSize;

  std::vector<uint8_t> exifIfdData;
  std::vector<ExifIfdEntry> exifIfdEntries;

  if (hasExposure) {
    uint32_t num = static_cast<uint32_t>(meta.exposureTimeUs);
    uint32_t den = 1000000;
    uint32_t g = exifGcd(num, den);
    if (g > 0) {
      num /= g;
      den /= g;
    }
    alignData(exifIfdData);
    uint32_t off = exifIfdDataStart + static_cast<uint32_t>(exifIfdData.size());
    exifPutU32(exifIfdData, num);
    exifPutU32(exifIfdData, den);
    exifIfdEntries.push_back({ExifTagExposureTime, ExifTypeRational, 1, off});
  }

  {
    uint32_t iso;
    if (meta.analogueGain > 0) {
      float isoF = meta.analogueGain * 100.0f;
      if (!std::isfinite(isoF) || isoF < 0.0f)
        isoF = 100.0f;
      isoF = std::min(isoF, static_cast<float>(0xFFFF));
      iso = static_cast<uint32_t>(isoF);
    } else {
      iso = 100;
    }
    iso = std::min(iso, 0xFFFFu);
    exifIfdEntries.push_back({ExifTagISOSpeed, ExifTypeShort, 1, iso});
  }

  {
    uint32_t cs = (meta.colorSpace == 1) ? 0x0002 : 0x0001;
    exifIfdEntries.push_back({ExifTagColorSpace, ExifTypeShort, 1, cs});
  }

  if (hasDate)
    addAsciiEntry(exifIfdEntries, exifIfdData, exifIfdDataStart,
                  ExifTagDateTimeOriginal, dateStr);

  std::sort(exifIfdEntries.begin(), exifIfdEntries.end(),
            [](const ExifIfdEntry &a, const ExifIfdEntry &b) {
              return a.tag < b.tag;
            });

  // --- Serialize the complete EXIF buffer ---
  std::vector<uint8_t> buf;
  buf.push_back('E');
  buf.push_back('x');
  buf.push_back('i');
  buf.push_back('f');
  buf.push_back(0);
  buf.push_back(0);

  // TIFF header: "II" (little-endian) + magic 42 + offset to IFD0 (8)
  buf.push_back('I');
  buf.push_back('I');
  exifPutU16(buf, 42);
  exifPutU32(buf, 8);

  // IFD0: count + entries + nextIFD(0)
  exifPutU16(buf, ifd0Count);
  for (const auto &e : ifd0Entries) {
    exifPutU16(buf, e.tag);
    exifPutU16(buf, e.type);
    exifPutU32(buf, e.count);
    exifPutU32(buf, e.valueOrOffset);
  }
  exifPutU32(buf, 0);

  buf.insert(buf.end(), ifd0Data.begin(), ifd0Data.end());
  alignData(buf);

  // ExifIFD: count + entries + nextIFD(0)
  exifPutU16(buf, exifIfdCount);
  for (const auto &e : exifIfdEntries) {
    exifPutU16(buf, e.tag);
    exifPutU16(buf, e.type);
    exifPutU32(buf, e.count);
    exifPutU32(buf, e.valueOrOffset);
  }
  exifPutU32(buf, 0);

  buf.insert(buf.end(), exifIfdData.begin(), exifIfdData.end());
  alignData(buf);

  return buf;
}

bool writePng(const std::string &path, const uint8_t *rgb, uint32_t w,
              uint32_t h, int compressionLevel, std::string *actualPath,
              const ExifMetadata *meta) {
  if (w == 0 || h == 0)
    return false;
  if (!rgb)
    return false;
  // Validate dimensions to prevent integer overflow in row pointer math
  size_t rgbSize = 0;
  if (!checkedMul(static_cast<size_t>(w), h, rgbSize))
    return false;
  if (!checkedMul(rgbSize, 3, rgbSize))
    return false;

  // Build tEXt chunks; strings must outlive writePngCore.
  std::string dateStr;
  std::string expStr;
  std::string isoStr;
  std::string makeStr;
  std::string modelStr;
  std::string softStr;
  png_text textChunks[6];
  int numText = 0;
  if (meta) {
    auto addText = [&](const char *key, const std::string &val) {
      if (val.empty())
        return;
      textChunks[numText].compression = PNG_TEXT_COMPRESSION_NONE;
      textChunks[numText].key = const_cast<char *>(key);
      textChunks[numText].text = const_cast<char *>(val.c_str());
      textChunks[numText].text_length = val.size();
      ++numText;
    };
    // Format DateTime from timestamp
    dateStr = formatExifDateTime(meta->timestampSec);
    addText("DateTime", dateStr);
    if (meta->exposureTimeUs > 0) {
      expStr = std::to_string(meta->exposureTimeUs) + "us";
      addText("ExposureTime", expStr);
    }
    if (meta->analogueGain > 0) {
      isoStr = std::to_string(static_cast<int>(meta->analogueGain * 100));
      addText("ISOSpeedRatings", isoStr);
    }
    makeStr = "Raspberry Pi";
    addText("Make", makeStr);
    modelStr = "IMX477";
    addText("Model", modelStr);
    softStr = "picamera";
    addText("Software", softStr);
  }

  std::string localPath;
  FILE *fp = safeFileOpen(path, localPath);
  if (!fp)
    return false;

  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) {
    fclose(fp);
    unlink(localPath.c_str());
    return false;
  }

  png_infop info = nullptr;
  png_bytep *rows = nullptr;
  int rc = writePngCore(png, &info, fp, rgb, w, h, compressionLevel, &rows,
                        textChunks, numText);

  png_destroy_write_struct(&png, &info);
  std::free(
      static_cast<void *>(rows)); // Free row array if longjmp left it allocated
  if (rc != 0) {
    fclose(fp);
    unlink(localPath.c_str());
    return false;
  }
  if (!finishFile(fp, localPath, "PNG"))
    return false;
  if (actualPath)
    *actualPath = localPath;
  return true;
}

bool writePpm(const uint8_t *rgb, size_t size, uint32_t w, uint32_t h,
              const std::string &path, std::string *actualPath) {
  // Validate size matches w*h*3.
  size_t expected = 0;
  if (!checkedMul(static_cast<size_t>(w), h, expected) ||
      !checkedMul(expected, 3, expected))
    return false;
  if (size != expected)
    return false;
  if (!fitsStreamSize(size))
    return false;

  auto so = safeOfstream(path);
  if (!so)
    return false;
  auto &out = so->stream();
  out << "P6\n" << w << " " << h << "\n255\n";
  out.write(reinterpret_cast<const char *>(rgb),
            static_cast<std::streamsize>(size));
  out.flush();
  bool fdOk = so->finish();
  if (!out.good() || !fdOk) {
    unlink(so->path().c_str());
    return false;
  }
  if (actualPath)
    *actualPath = so->path();
  return true;
}

bool writeRaw(const uint8_t *y, size_t ySize, const uint8_t *uv, size_t uvSize,
              const std::string &path, std::string *actualPath) {
  if (ySize == 0)
    return false;
  if (y == nullptr)
    return false;
  if (uvSize > 0 && uv == nullptr)
    return false;
  if (!fitsStreamSize(ySize) || !fitsStreamSize(uvSize))
    return false;

  auto so = safeOfstream(path);
  if (!so)
    return false;
  auto &out = so->stream();
  out.write(reinterpret_cast<const char *>(y),
            static_cast<std::streamsize>(ySize));
  if (uvSize > 0 && uv) {
    out.write(reinterpret_cast<const char *>(uv),
              static_cast<std::streamsize>(uvSize));
  }
  out.flush();
  bool fdOk = so->finish();
  if (!out.good() || !fdOk) {
    unlink(so->path().c_str());
    return false;
  }
  if (actualPath)
    *actualPath = so->path();
  return true;
}

bool writeJpeg(const uint8_t *data, size_t size, const std::string &path,
               std::string *actualPath) {
  // The Pi ISP produces a complete JPEG bitstream in the MJPEG buffer;
  // we just write it to disk. No software encode needed.
  if (!data || size == 0)
    return false;
  if (!fitsStreamSize(size))
    return false;
  auto so = safeOfstream(path);
  if (!so)
    return false;
  auto &out = so->stream();
  out.write(reinterpret_cast<const char *>(data),
            static_cast<std::streamsize>(size));
  out.flush();
  bool fdOk = so->finish();
  if (!out.good() || !fdOk) {
    unlink(so->path().c_str());
    return false;
  }
  if (actualPath)
    *actualPath = so->path();
  return true;
}

bool writeJpegRgb(const uint8_t *rgb, uint32_t w, uint32_t h,
                  const std::string &path, int quality, std::string *actualPath,
                  const ExifMetadata *meta) {
  if (w == 0 || h == 0)
    return false;
  if (!rgb)
    return false;
  // Clamp quality to libjpeg's valid range [1,100] to guard against
  // callers passing out-of-range values.
  quality = std::clamp(quality, 1, 100);

  // Validate dimensions to prevent integer overflow in row pointer math
  size_t expectedSize = 0;
  if (!checkedMul(static_cast<size_t>(w), h, expectedSize))
    return false;
  if (!checkedMul(expectedSize, 3, expectedSize))
    return false;

  // Build EXIF APP1 data.
  std::vector<uint8_t> exifData;
  if (meta) {
    ExifMetadata exifMeta = *meta;
    // Fill in image dimensions if not already set.
    if (exifMeta.width == 0)
      exifMeta.width = w;
    if (exifMeta.height == 0)
      exifMeta.height = h;
    exifData = buildExifData(exifMeta);
  }

  std::string localPath;
  FILE *fp = safeFileOpen(path, localPath);
  if (!fp)
    return false;

  struct jpeg_compress_struct cinfo;
  JpegErrCtx
      jerr; // Must outlive cinfo (jpeg_destroy_compress accesses cinfo->err)
  JSAMPROW *rowPtrs = nullptr;
  bool cinfoCreated = false;
  int rc = writeJpegRgbCore(&cinfo, &jerr, fp, rgb, w, h, quality, &rowPtrs,
                            &cinfoCreated, exifData.data(), exifData.size());

  // jpeg_destroy_compress only safe after jpeg_create_compress succeeded.
  if (cinfoCreated)
    jpeg_destroy_compress(&cinfo);
  std::free(static_cast<void *>(
      rowPtrs)); // Free row array if longjmp left it allocated
  if (rc != 0) {
    fclose(fp);
    unlink(localPath.c_str());
    return false;
  }
  if (!finishFile(fp, localPath, "JPEG"))
    return false;
  if (actualPath)
    *actualPath = localPath;
  return true;
}

} // namespace picamera
