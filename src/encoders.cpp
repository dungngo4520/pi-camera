#include "encoders.h"
#include "safe_path.h"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

#include <png.h>
#ifdef HAVE_JPEG
#include <jpeglib.h>
#include <csetjmp>
#endif

namespace picamera {

namespace {

// Max retries on EEXIST — suffix scan goes _2.._999, so 999 is plenty.
constexpr int kMaxNameRetries = 999;

// Returns true if a size_t byte count fits in std::streamsize (the signed
// type used by ostream::write). On 32-bit platforms streamsize is 32-bit
// signed, so a >2GiB buffer would produce a negative cast and misbehavior.
bool fitsStreamSize(size_t size) {
    return size <= static_cast<size_t>(std::numeric_limits<std::streamsize>::max());
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
                 int compressionLevel, png_bytep **outRows) {
    *outRows = nullptr;
    *outInfo = nullptr;
    if (setjmp(png_jmpbuf(png))) return -1;

    png_infop info = png_create_info_struct(png);
    if (!info) return -1;
    *outInfo = info;

    // Compression level: 0=none, 1=fastest, 6=zlib default, 9=best.
    // Clamp to valid range in case cfg_.pngLevel was set programmatically
    // without CLI validation.
    int level = std::clamp(compressionLevel, 0, 9);
    png_set_compression_level(png, level);
    if (level <= 1)
        png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);

    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    // Raw pointer — no destructor, safe across longjmp. Tracked via
    // outRows so the caller can free it if longjmp fires.
    size_t rowsBytes = 0;
    if (!checkedMul(static_cast<size_t>(h), sizeof(png_bytep), rowsBytes)) return -1;
    png_bytep *rows = static_cast<png_bytep *>(std::malloc(rowsBytes));
    if (!rows) return -1;
    *outRows = rows;

    for (uint32_t y = 0; y < h; ++y)
        rows[y] = const_cast<png_bytep>(rgb + static_cast<size_t>(y) * w * 3);

    png_write_image(png, rows);
    png_write_end(png, nullptr);
    std::free(static_cast<void *>(rows));
    *outRows = nullptr;
    return 0;
}

#ifdef HAVE_JPEG
// JPEG error handler context — must outlive the jpeg_compress_struct
// because jpeg_destroy_compress may access cinfo->err.
struct JpegErrCtx {
    struct jpeg_error_mgr base;
    std::jmp_buf setjmpBuf;
};
static_assert(std::is_standard_layout_v<JpegErrCtx>,
              "JpegErrCtx must be standard-layout for reinterpret_cast from base");

// Core JPEG writer: same pattern as writePngCore — setjmp/longjmp with
// only trivial locals. Returns 0 on success, -1 on libjpeg error.
// On error, *outRowPtrs is set if the row array was allocated (caller frees).
// *outCreated is set to true after jpeg_create_compress succeeds, so the
// caller knows whether jpeg_destroy_compress is safe to call.
// jerr must outlive cinfo (jpeg_destroy_compress may access cinfo->err),
// so it is declared in the caller and passed by pointer.
int writeJpegRgbCore(struct jpeg_compress_struct *cinfo, JpegErrCtx *jerr, FILE *fp,
                     const uint8_t *rgb, uint32_t w, uint32_t h, int quality,
                     JSAMPROW **outRowPtrs, bool *outCreated) {
    *outRowPtrs = nullptr;
    *outCreated = false;
    cinfo->err = jpeg_std_error(&jerr->base);
    jerr->base.error_exit = [](j_common_ptr c) {
        auto *e = reinterpret_cast<JpegErrCtx *>(c->err);
        std::longjmp(e->setjmpBuf, 1);
    };

    if (setjmp(jerr->setjmpBuf)) return -1;

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

    // Raw pointer — tracked via outRowPtrs for cleanup on longjmp.
    size_t rowPtrsBytes = 0;
    if (!checkedMul(static_cast<size_t>(h), sizeof(JSAMPROW), rowPtrsBytes)) return -1;
    JSAMPROW *rowPtrs = static_cast<JSAMPROW *>(std::malloc(rowPtrsBytes));
    if (!rowPtrs) return -1;
    *outRowPtrs = rowPtrs;

    for (uint32_t r = 0; r < h; ++r)
        rowPtrs[r] = const_cast<JSAMPROW>(rgb + static_cast<size_t>(r) * w * 3);

    bool compressOk = true;
    while (cinfo->next_scanline < cinfo->image_height) {
        JDIMENSION written = jpeg_write_scanlines(cinfo, &rowPtrs[cinfo->next_scanline], 1);
        if (written != 1) { compressOk = false; break; }
    }

    if (compressOk) jpeg_finish_compress(cinfo);
    // Free rowPtrs after jpeg_finish_compress so the row pointers remain
    // valid for the duration of compression (libjpeg may reference them).
    std::free(static_cast<void *>(rowPtrs));
    *outRowPtrs = nullptr;
    return compressOk ? 0 : -1;
}
#endif

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
        int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      S_IRUSR | S_IWUSR | S_IRGRP);
        if (fd >= 0) {
            outPath = p;
            FILE *fp = fdopen(fd, "wb");
            if (!fp) { close(fd); unlink(p.c_str()); }
            return fp;
        }
        // EEXIST: file already exists (normal collision, retry with suffix).
        // ELOOP: final component is a symlink (O_NOFOLLOW rejects it).
        //   Retry with suffix — a symlink at photo.jpg shouldn't prevent
        //   saving photo_2.jpg.
        if (errno != EEXIST && errno != ELOOP) return nullptr;
    }
    return nullptr;
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
        if (fd_ < 0) return true;
        if (sync() != 0) { close(fd_); fd_ = -1; return false; }
        // fsync before close so captures survive power cuts (camera appliance
        // may be turned off immediately after a capture). Without fsync,
        // close() only flushes to the kernel page cache — a sudden power-off
        // can leave a partial file on the SD card.
        if (::fsync(fd_) != 0) {
            close(fd_); fd_ = -1;
            return false;
        }
        int rc = close(fd_);
        fd_ = -1;
        // On Linux, close() always deallocates the fd even if it returns
        // EINTR — retrying would close a different fd (fd reuse). Treat
        // EINTR as success to avoid false-negative write failures.
        if (rc != 0 && errno == EINTR) rc = 0;
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
        if (sync() != 0) return traits_type::eof();
        *pptr() = static_cast<char>(ch);
        pbump(1);
        return ch;
    }

    // Override xsputn to avoid the default byte-at-a-time sputc loop.
    // Copies data in bulk into the put area, flushing in chunks when full.
    std::streamsize xsputn(const char_type *s, std::streamsize n) override {
        if (n <= 0) return 0;
        std::streamsize written = 0;
        while (n > 0) {
            std::streamsize avail = epptr() - pptr();
            if (avail <= 0) {
                if (sync() != 0) return written;
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
                    if (errno == EINTR) continue;  // interrupted by signal — retry
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
    SafeOstream(std::unique_ptr<FdOutStreamBuf> b, std::unique_ptr<std::ostream> s,
                std::string p)
        : buf_(std::move(b)), stream_(std::move(s)), path_(std::move(p)) {}

    std::ostream &stream() { return *stream_; }
    const std::string &path() const { return path_; }

    // Flush and close the underlying fd, returning false on I/O error.
    bool finish() const { return buf_ ? buf_->finish() : false; }
private:
    std::unique_ptr<FdOutStreamBuf> buf_;
    std::unique_ptr<std::ostream> stream_;
    std::string path_;  // actual file path (for unlink on failure)
};
std::unique_ptr<SafeOstream> safeOfstream(const std::string &path) {
    // Try open(O_CREAT|O_EXCL) directly — no lstat probe, no TOCTOU race.
    // Stem/ext split and candidate construction shared via
    // splitPathStemExt/suffixedCandidate (see safeFileOpen).
    const PathStemExt se = splitPathStemExt(path);

    for (int i = 1; i <= kMaxNameRetries; ++i) {
        std::string p = suffixedCandidate(se, path, i);
        int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      S_IRUSR | S_IWUSR | S_IRGRP);
        if (fd >= 0) {
            auto buf = std::make_unique<FdOutStreamBuf>(fd);
            auto stream = std::make_unique<std::ostream>(buf.get());
            return std::make_unique<SafeOstream>(std::move(buf), std::move(stream), p);
        }
        if (errno != EEXIST && errno != ELOOP) return nullptr;
    }
    return nullptr;
}

} // namespace

bool writePng(const std::string &path, const uint8_t *rgb, uint32_t w, uint32_t h,
              int compressionLevel, std::string *actualPath) {
    if (w == 0 || h == 0) return false;
    if (!rgb) return false;
    // Validate dimensions to prevent integer overflow in row pointer math
    size_t rgbSize = 0;
    if (!checkedMul(static_cast<size_t>(w), h, rgbSize)) return false;
    if (!checkedMul(rgbSize, 3, rgbSize)) return false;

    std::string localPath;
    FILE *fp = safeFileOpen(path, localPath);
    if (!fp) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); unlink(localPath.c_str()); return false; }

    // info is created inside writePngCore (after setjmp) so that an
    // OOM during png_create_info_struct is caught by the longjmp guard
    // instead of aborting the process. Initialized to nullptr so
    // png_destroy_write_struct is safe even if creation never happened.
    png_infop info = nullptr;
    png_bytep *rows = nullptr;
    int rc = writePngCore(png, &info, fp, rgb, w, h, compressionLevel, &rows);

    png_destroy_write_struct(&png, &info);
    std::free(static_cast<void *>(rows));  // Free row array if longjmp left it allocated
    if (rc != 0) {
        fclose(fp);
        unlink(localPath.c_str());
        return false;
    }
    // Flush stdio buffer to kernel, then kernel buffer to disk — same
    // durability discipline as FdOutStreamBuf::finish() and writeDng().
    if (fflush(fp) != 0) {
        std::cerr << "PNG: fflush() failed: " << errnoString(errno) << "\n";
        fclose(fp);
        unlink(localPath.c_str());
        return false;
    }
    if (::fsync(fileno(fp)) != 0) {
        std::cerr << "PNG: fsync() failed: " << errnoString(errno) << "\n";
        fclose(fp);
        unlink(localPath.c_str());
        return false;
    }
    if (fclose(fp) != 0) {
        std::cerr << "PNG: fclose() failed: " << errnoString(errno) << "\n";
        unlink(localPath.c_str());
        return false;
    }
    if (actualPath) *actualPath = localPath;
    return true;
}

bool writePpm(const uint8_t *rgb, size_t size, uint32_t w, uint32_t h,
              const std::string &path, std::string *actualPath) {
    // Validate that size matches w*h*3 to prevent writing corrupt/truncated files.
    size_t expected = 0;
    if (!checkedMul(static_cast<size_t>(w), h, expected) ||
        !checkedMul(expected, 3, expected)) return false;
    if (size != expected) return false;
    if (!fitsStreamSize(size)) return false;

    auto so = safeOfstream(path);
    if (!so) return false;
    auto &out = so->stream();
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char *>(rgb), static_cast<std::streamsize>(size));
    out.flush();
    bool fdOk = so->finish();
    if (!out.good() || !fdOk) { unlink(so->path().c_str()); return false; }
    if (actualPath) *actualPath = so->path();
    return true;
}

bool writeRaw(const uint8_t *y, size_t ySize, const uint8_t *uv, size_t uvSize,
              const std::string &path, std::string *actualPath) {
    // Validate non-zero sizes to prevent writing empty files from mismatched callers.
    if (ySize == 0) return false;
    if (y == nullptr) return false;
    if (uvSize > 0 && uv == nullptr) return false;
    if (!fitsStreamSize(ySize) || !fitsStreamSize(uvSize)) return false;

    auto so = safeOfstream(path);
    if (!so) return false;
    auto &out = so->stream();
    out.write(reinterpret_cast<const char *>(y), static_cast<std::streamsize>(ySize));
    if (uvSize > 0 && uv) {
        out.write(reinterpret_cast<const char *>(uv), static_cast<std::streamsize>(uvSize));
    }
    out.flush();
    bool fdOk = so->finish();
    if (!out.good() || !fdOk) { unlink(so->path().c_str()); return false; }
    if (actualPath) *actualPath = so->path();
    return true;
}

bool writeJpeg(const uint8_t *data, size_t size, const std::string &path,
               std::string *actualPath) {
    // The Pi ISP produces a complete JPEG bitstream in the MJPEG buffer;
    // we just write it to disk. No software encode needed.
    if (!data || size == 0) return false;
    if (!fitsStreamSize(size)) return false;
    auto so = safeOfstream(path);
    if (!so) return false;
    auto &out = so->stream();
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    out.flush();
    bool fdOk = so->finish();
    if (!out.good() || !fdOk) { unlink(so->path().c_str()); return false; }
    if (actualPath) *actualPath = so->path();
    return true;
}

bool writeJpegRgb(const uint8_t *rgb, uint32_t w, uint32_t h,
                  const std::string &path, int quality,
                  std::string *actualPath) {
#ifdef HAVE_JPEG
    if (w == 0 || h == 0) return false;
    if (!rgb) return false;
    // Clamp quality to libjpeg's valid range [1,100] to guard against
    // callers passing out-of-range values.
    quality = std::clamp(quality, 1, 100);

    // Validate dimensions to prevent integer overflow in row pointer math
    size_t expectedSize = 0;
    if (!checkedMul(static_cast<size_t>(w), h, expectedSize)) return false;
    if (!checkedMul(expectedSize, 3, expectedSize)) return false;

    std::string localPath;
    FILE *fp = safeFileOpen(path, localPath);
    if (!fp) return false;

    struct jpeg_compress_struct cinfo;
    JpegErrCtx jerr;  // Must outlive cinfo (jpeg_destroy_compress accesses cinfo->err)
    JSAMPROW *rowPtrs = nullptr;
    bool cinfoCreated = false;
    int rc = writeJpegRgbCore(&cinfo, &jerr, fp, rgb, w, h, quality, &rowPtrs, &cinfoCreated);

    // Only destroy if jpeg_create_compress actually succeeded — calling
    // jpeg_destroy_compress on an uninitialized cinfo is undefined behavior.
    if (cinfoCreated) jpeg_destroy_compress(&cinfo);
    std::free(static_cast<void *>(rowPtrs));  // Free row array if longjmp left it allocated
    if (rc != 0) {
        fclose(fp);
        unlink(localPath.c_str());
        return false;
    }
    // Flush stdio buffer to kernel, then kernel buffer to disk — same
    // durability discipline as FdOutStreamBuf::finish() and writeDng().
    if (fflush(fp) != 0) {
        std::cerr << "JPEG: fflush() failed: " << errnoString(errno) << "\n";
        fclose(fp);
        unlink(localPath.c_str());
        return false;
    }
    if (::fsync(fileno(fp)) != 0) {
        std::cerr << "JPEG: fsync() failed: " << errnoString(errno) << "\n";
        fclose(fp);
        unlink(localPath.c_str());
        return false;
    }
    if (fclose(fp) != 0) {
        std::cerr << "JPEG: fclose() failed: " << errnoString(errno) << "\n";
        unlink(localPath.c_str());
        return false;
    }
    if (actualPath) *actualPath = localPath;
    return true;
#else
    (void)rgb; (void)w; (void)h; (void)path; (void)quality; (void)actualPath;
    std::cerr << "writeJpegRgb: libjpeg not available at build time\n";
    return false;
#endif
}

} // namespace picamera
