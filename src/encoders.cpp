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

// --- EXIF/TIFF helpers (shared with dng.cpp's approach) ---

// TIFF tag IDs for EXIF
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

// LE binary writers
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

// Pack up to 4 bytes into a uint32_t as little-endian, for the IFD inline
// value field.
uint32_t exifPackLe32(const uint8_t *bytes, size_t len) {
  uint32_t val = 0;
  for (size_t i = 0; i < len; ++i)
    val |= static_cast<uint32_t>(bytes[i]) << (8 * i);
  return val;
}

// A single IFD entry (12 bytes on disk).
struct ExifIfdEntry {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  uint32_t valueOrOffset;
};

// Add an ASCII IFD entry: stored inline if ≤4 bytes, else spilled to the
// data area (word-aligned). dataStart is the TIFF-header-relative offset of
// the data area's beginning.
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

// Word-align a data area before appending an offset-based entry.
void alignData(std::vector<uint8_t> &data) {
  if (data.size() % 2 != 0)
    data.push_back(0);
}

// Compute the GCD of two uint32_t values for rational reduction.
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

// Format a Unix timestamp as "YYYY:MM:DD HH:MM:SS" (EXIF DateTime format).
// Returns an empty string on failure (e.g., timestamp == 0). Shared by
// buildExifData (JPEG) and writePng (tEXt chunk).
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

// Core PNG writer: contains the setjmp/longjmp and uses ONLY raw pointers
// and trivial types. No C++ objects with non-trivial destructors are in
// scope when longjmp could fire, avoiding the undefined behavior described
// in C++ [support.runtime]/3. Returns 0 on success, -1 on libpng error.
// On error, *outRows is set if the row array was allocated (caller frees),
// and *outInfo is set if the info struct was created (caller destroys).
// png_create_info_struct is called AFTER setjmp so that an OOM-triggered
// png_error longjmp is caught instead of aborting the process.
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

  // Compression level: 0=none, 1=fastest, 6=zlib default, 9=best.
  // Clamp to valid range in case cfg_.pngLevel was set programmatically
  // without CLI validation.
  int level = std::clamp(compressionLevel, 0, 9);
  png_set_compression_level(png, level);
  if (level <= 1)
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);

  png_init_io(png, fp);
  png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  // Embed metadata as tEXt chunks (if provided). Must be called before
  // png_write_info() so the chunks are written in the file header.
  if (textChunks && numText > 0)
    png_set_text(png, info, textChunks, numText);
  png_write_info(png, info);

  // Raw pointer — no destructor, safe across longjmp. Tracked via
  // outRows so the caller can free it if longjmp fires.
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

// Core JPEG writer: same pattern as writePngCore — setjmp/longjmp with
// only trivial locals. Returns 0 on success, -1 on libjpeg error.
// On error, *outRowPtrs is set if the row array was allocated (caller frees).
// *outCreated is set to true after jpeg_create_compress succeeds, so the
// caller knows whether jpeg_destroy_compress is safe to call.
// jerr must outlive cinfo (jpeg_destroy_compress may access cinfo->err),
// so it is declared in the caller and passed by pointer.
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

  // Write EXIF APP1 marker after jpeg_start_compress and before
  // writing scanlines. jpeg_write_marker inserts the marker code
  // (0xE1), the 2-byte length, and the data bytes into the bitstream.
  if (exifData && exifSize > 0) {
    jpeg_write_marker(cinfo, 0xE1, reinterpret_cast<const JOCTET *>(exifData),
                      static_cast<unsigned int>(exifSize));
  }

  // Raw pointer — tracked via outRowPtrs for cleanup on longjmp.
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

// Open a file for writing with O_CREAT|O_EXCL|O_NOFOLLOW to prevent
// symlink attacks and accidental overwrites. On EEXIST (race with another
// process or a same-ms collision), retries with _2, _3, ... suffixes
// atomically (no lstat probe, so no TOCTOU race). Returns a FILE* or
// nullptr. The fd is kept open and handed to fdopen() — no close/reopen race.
// On success, `outPath` is set to the actual file path (may differ from
// `path` if a suffix was needed). Callers should unlink(outPath) on write
// failure to avoid leaving partial files.
FILE *safeFileOpen(const std::string &path, std::string &outPath) {
  // Try open(O_CREAT|O_EXCL) directly — no lstat probe, so there's
  // no TOCTOU race between checking existence and creating. On EEXIST,
  // increment the suffix (_2, _3, ...) and retry. The stem/ext split
  // and candidate construction are shared with safeOfstream and
  // safeFileOpenFd via splitPathStemExt/suffixedCandidate.
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

// Flush stdio buffer to kernel, then kernel buffer to disk, then close.
// Same durability discipline as FdOutStreamBuf::finish() and writeDng().
// On any I/O error, unlinks localPath (no partial files) and returns false.
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

// Portable fd-based output streambuf. Replaces __gnu_cxx::stdio_filebuf
// (a libstdc++/GCC-only extension) with a minimal standard-library
// implementation. The fd is closed on destruction; the buffer is flushed
// via overflow/sync calls. This avoids the TOCTOU race of close()+reopen()
// — the fd stays open for the stream's lifetime.
class FdOutStreamBuf : public std::streambuf {
public:
  explicit FdOutStreamBuf(int fd) : fd_(fd) {
    setp(buffer_, buffer_ + sizeof(buffer_));
  }
  ~FdOutStreamBuf() override {
    if (fd_ >= 0) {
      sync();
      // fsync before close so data survives power cuts even on
      // exception paths where finish() was never called. Ignore
      // errors — destructors must not throw.
      (void)::fsync(fd_);
      close(fd_);
    }
  }
  FdOutStreamBuf(const FdOutStreamBuf &) = delete;
  FdOutStreamBuf &operator=(const FdOutStreamBuf &) = delete;

  // Flush pending data and close the fd, returning false on any I/O
  // error (including deferred errors reported by close()). Call this
  // before destruction to detect write-back failures.
  // Note: finish() is the authoritative success check — ostream::good()
  // may not reflect sync() failures if flush() was not called.
  bool finish() {
    if (fd_ < 0)
      return true;
    if (sync() != 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }
    // fsync before close so captures survive power cuts (camera appliance
    // may be turned off immediately after a capture). Without fsync,
    // close() only flushes to the kernel page cache — a sudden power-off
    // can leave a partial file on the SD card.
    if (::fsync(fd_) != 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }
    int rc = close(fd_);
    fd_ = -1;
    // On Linux, close() always deallocates the fd even if it returns
    // EINTR — retrying would close a different fd (fd reuse). Treat
    // EINTR as success to avoid false-negative write failures.
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

// Open a file for writing with O_CREAT|O_EXCL|O_NOFOLLOW, returning an ostream
// that writes directly to the safe fd via FdOutStreamBuf. This avoids
// the TOCTOU race of close()+reopen() — the fd stays open for the stream's
// lifetime, so a symlink swap between open and write is impossible.
//
// The streambuf owns the fd and closes it on destruction. The returned
// unique_ptr wraps a struct holding both the streambuf and ostream, ensuring
// both are destroyed together (the ostream does NOT own its rdbuf by default).
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
  // Try open(O_CREAT|O_EXCL) directly — no lstat probe, no TOCTOU race.
  // Stem/ext split and candidate construction shared via
  // splitPathStemExt/suffixedCandidate (see safeFileOpen).
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
  // Layout (all offsets relative to the TIFF header start, i.e., after
  // the 6-byte "Exif\0\0" prefix):
  //   [0..8)        TIFF header ("II" + 42 + offset to IFD0 = 8)
  //   [8..8+I0)     IFD0: count(2) + N*12 + nextIFD(4)
  //   [..+D0)       IFD0 data area (strings that don't fit inline)
  //   [..+I1)       ExifIFD: count(2) + M*12 + nextIFD(4)
  //   [..+D1)       ExifIFD data area (rational + string)

  const std::string dateStr = formatExifDateTime(meta.timestampSec);
  const bool hasDate = !dateStr.empty();
  const bool hasCopyright = !meta.copyright.empty();

  // IFD0 entries: Make, Model, Software, DateTime (if present), Copyright (if
  // present), ExifIFD ptr.
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

  // ExifIFD pointer (LONG, inline). Offset filled after computing the
  // ExifIFD start position.
  const uint32_t exifIfdStart =
      ifd0DataStart + static_cast<uint32_t>(ifd0Data.size());
  ifd0Entries.push_back({ExifTagExifIFD, ExifTypeLong, 1, exifIfdStart});

  // TIFF requires IFD entries sorted by tag.
  std::sort(ifd0Entries.begin(), ifd0Entries.end(),
            [](const ExifIfdEntry &a, const ExifIfdEntry &b) {
              return a.tag < b.tag;
            });

  // --- ExifIFD ---
  // Entries: ExposureTime (if present), ISOSpeedRatings, DateTimeOriginal (if
  // present).
  const bool hasExposure =
      (meta.exposureTimeUs > 0 &&
       meta.exposureTimeUs <= std::numeric_limits<uint32_t>::max());
  const uint16_t exifIfdCount =
      static_cast<uint16_t>((hasExposure ? 1 : 0) + 1 + (hasDate ? 1 : 0) + 1);
  const uint32_t exifIfdSize = 2 + static_cast<uint32_t>(exifIfdCount) * 12 + 4;
  const uint32_t exifIfdDataStart = exifIfdStart + exifIfdSize;

  std::vector<uint8_t> exifIfdData;
  std::vector<ExifIfdEntry> exifIfdEntries;

  // ExposureTime (0x829A) — RATIONAL (num/den in seconds)
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

  // ISOSpeedRatings (0x8827) — SHORT, inline (low 16 bits of the 4-byte field)
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

  // ColorSpace (0xA001) — SHORT, inline. sRGB = 1, AdobeRGB = 2 (uncalibrated).
  {
    uint32_t cs = (meta.colorSpace == 1) ? 0x0002 : 0x0001;
    exifIfdEntries.push_back({ExifTagColorSpace, ExifTypeShort, 1, cs});
  }

  // DateTimeOriginal (0x9003) — ASCII, 20 bytes
  if (hasDate)
    addAsciiEntry(exifIfdEntries, exifIfdData, exifIfdDataStart,
                  ExifTagDateTimeOriginal, dateStr);

  std::sort(exifIfdEntries.begin(), exifIfdEntries.end(),
            [](const ExifIfdEntry &a, const ExifIfdEntry &b) {
              return a.tag < b.tag;
            });

  // --- Serialize the complete EXIF buffer ---
  std::vector<uint8_t> buf;
  // "Exif\0\0" header (6 bytes)
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

  // Build PNG tEXt chunks from metadata (if provided). The strings
  // must stay alive until writePngCore returns — they're declared
  // here in the caller's scope, outside the setjmp/longjmp boundary.
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

  // info is created inside writePngCore (after setjmp) so that an
  // OOM during png_create_info_struct is caught by the longjmp guard
  // instead of aborting the process. Initialized to nullptr so
  // png_destroy_write_struct is safe even if creation never happened.
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
  // Validate that size matches w*h*3 to prevent writing corrupt/truncated
  // files.
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
  // Validate non-zero sizes to prevent writing empty files from mismatched
  // callers.
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

  // Build EXIF APP1 data from metadata (if provided). The buffer
  // stays alive in this scope until writeJpegRgbCore returns.
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

  // Only destroy if jpeg_create_compress actually succeeded — calling
  // jpeg_destroy_compress on an uninitialized cinfo is undefined behavior.
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
