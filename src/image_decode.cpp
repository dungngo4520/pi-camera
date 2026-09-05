#include "image_decode.h"
#include "safe_path.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <png.h>
#ifdef HAVE_JPEG
#include <csetjmp>
#include <jpeglib.h>
#endif

namespace picamera {

namespace {

// Cap decoded image dimensions to prevent OOM from malicious/corrupt files.
// 8192 is well above any real camera capture (IMX477 max is 4056x3040) but
// bounds the allocation to a manageable ~200MB worst case (8192*8192*3).
constexpr uint32_t kMaxDecodeDim = 8192;

// RAII guard for std::malloc-allocated buffers. Used by both PNG and JPEG
// decoders to ensure the buffer is freed if an exception fires before
// ownership is transferred to the caller.
class MallocGuard {
public:
  explicit MallocGuard(uint8_t *p) : p_(p) {}
  ~MallocGuard() {
    if (p_)
      std::free(p_);
  }
  MallocGuard(const MallocGuard &) = delete;
  MallocGuard &operator=(const MallocGuard &) = delete;
  uint8_t *get() const { return p_; }
  uint8_t *release() {
    uint8_t *t = p_;
    p_ = nullptr;
    return t;
  }

private:
  uint8_t *p_;
};

// Core PNG decoder: contains the setjmp/longjmp and uses ONLY raw pointers
// and trivial types. No C++ objects with non-trivial destructors are in
// scope when longjmp could fire, avoiding UB (C++ [support.runtime]/3).
// On success, fills *outRgb (caller must free) and *w/*h. On error, any
// allocations are returned via *outRgb/*outRows so the caller can free them
// (longjmp skips cleanup in this function). *outInfo is set if the info
// struct was created (caller destroys). png_create_info_struct is called
// AFTER setjmp so that an OOM-triggered png_error longjmp is caught.
// Returns 0 on success, -1 on error.
int decodePngCore(png_structp png, png_infop *outInfo, FILE *fp,
                  uint8_t **outRgb, png_bytep **outRows, uint32_t *w,
                  uint32_t *h) {
  *outRgb = nullptr;
  *outRows = nullptr;
  *outInfo = nullptr;
  if (setjmp(png_jmpbuf(png)))
    return -1;

  png_infop info = png_create_info_struct(png);
  if (!info)
    return -1;
  *outInfo = info;

  png_init_io(png, fp);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);

  uint32_t width = png_get_image_width(png, info);
  uint32_t height = png_get_image_height(png, info);
  int colorType = png_get_color_type(png, info);
  int bitDepth = png_get_bit_depth(png, info);

  if (width == 0 || height == 0 || width > kMaxDecodeDim ||
      height > kMaxDecodeDim)
    return -1;

  if (bitDepth == 16)
    png_set_strip_16(png);
  if (colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  png_set_strip_alpha(png);
  if (colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);

  png_read_update_info(png, info);

  size_t rowBytes = static_cast<size_t>(width) * 3;
  size_t rgbSize = rowBytes * height;
  uint8_t *rgb = static_cast<uint8_t *>(std::malloc(rgbSize));
  if (!rgb)
    return -1;
  *outRgb = rgb; // Track for cleanup on longjmp

  png_bytep *rows = static_cast<png_bytep *>(
      std::malloc(static_cast<size_t>(height) * sizeof(png_bytep)));
  if (!rows)
    return -1;
  *outRows = rows; // Track for cleanup on longjmp

  for (uint32_t y = 0; y < height; ++y)
    rows[y] = rgb + static_cast<size_t>(y) * rowBytes;

  png_read_image(png, rows);
  // On success, free the row pointer array (pixel data stays in rgb).
  std::free(static_cast<void *>(rows));
  *outRows = nullptr;

  *w = width;
  *h = height;
  return 0;
}

#ifdef HAVE_JPEG
// Error context for libjpeg — must outlive the jpeg_decompress_struct
// because jpeg_destroy_decompress may access cinfo->err. Defined here
// so both decodeJpegCore and decodeJpegToRgb24 can see it.
struct JpegErrCtx {
  struct jpeg_error_mgr base;
  std::jmp_buf setjmpBuf;
};
static_assert(
    std::is_standard_layout_v<JpegErrCtx>,
    "JpegErrCtx must be standard-layout for reinterpret_cast from base");

// Core JPEG decoder: same pattern — setjmp/longjmp with only trivial locals.
// On success, fills *outRgb (caller must free) and *w/*h. Returns 0 on
// success, -1 on error. *outCreated is set true after jpeg_create_decompress
// succeeds, so the caller knows whether jpeg_destroy_decompress is safe.
// jerr must be owned by the caller and outlive cinfo.
int decodeJpegCore(struct jpeg_decompress_struct *cinfo, JpegErrCtx *jerr,
                   FILE *fp, uint8_t **outRgb, uint32_t *w, uint32_t *h,
                   bool *outCreated) {
  *outRgb = nullptr;
  *outCreated = false;
  cinfo->err = jpeg_std_error(&jerr->base);
  jerr->base.error_exit = [](j_common_ptr c) {
    auto *e = reinterpret_cast<JpegErrCtx *>(c->err);
    std::longjmp(e->setjmpBuf, 1);
  };

  if (setjmp(jerr->setjmpBuf))
    return -1;

  jpeg_create_decompress(cinfo);
  *outCreated = true;
  jpeg_stdio_src(cinfo, fp);

  if (jpeg_read_header(cinfo, TRUE) != JPEG_HEADER_OK)
    return -1;

  cinfo->out_color_space = JCS_RGB;
  if (!jpeg_start_decompress(cinfo))
    return -1;

  uint32_t width = cinfo->output_width;
  uint32_t height = cinfo->output_height;
  if (width == 0 || height == 0 || width > kMaxDecodeDim ||
      height > kMaxDecodeDim)
    return -1;

  size_t rowBytes = static_cast<size_t>(width) * 3;
  size_t rgbSize = rowBytes * height;
  uint8_t *rgb = static_cast<uint8_t *>(std::malloc(rgbSize));
  if (!rgb)
    return -1;
  *outRgb = rgb; // Track for cleanup on longjmp

  uint32_t scanlinesRead = 0;
  while (cinfo->output_scanline < height) {
    JSAMPROW rowPtr =
        rgb + static_cast<size_t>(cinfo->output_scanline) * rowBytes;
    JDIMENSION n = jpeg_read_scanlines(cinfo, &rowPtr, 1);
    if (n == 0)
      break;
    scanlinesRead += n;
    if (scanlinesRead > height)
      break;
  }

  if (scanlinesRead != height) {
    jpeg_abort_decompress(cinfo);
    return -1;
  }

  if (!jpeg_finish_decompress(cinfo))
    return -1; // rgb freed by caller

  *w = width;
  *h = height;
  return 0;
}
#endif

// Convert RGB24 to big-endian RGB565, scaled to dispW x dispH with
// center-crop + nearest-neighbor. Used by both PNG and JPEG decoders.
std::vector<uint8_t> rgb24ToRgb565Scaled(const uint8_t *rgb, uint32_t srcW,
                                         uint32_t srcH, uint32_t dispW,
                                         uint32_t dispH) {
  if (srcW == 0 || srcH == 0 || dispW == 0 || dispH == 0)
    return {};
  size_t outSize = 0;
  if (!checkedMul(static_cast<size_t>(dispW), dispH, outSize))
    return {};
  if (!checkedMul(outSize, 2, outSize))
    return {};
  std::vector<uint8_t> out(outSize);

  // Center-crop to match display aspect ratio
  float srcAspect = static_cast<float>(srcW) / srcH;
  float dispAspect = static_cast<float>(dispW) / dispH;
  uint32_t cropW;
  uint32_t cropH;
  uint32_t cropX;
  uint32_t cropY;
  if (srcAspect > dispAspect) {
    cropH = srcH;
    cropW = static_cast<uint32_t>(srcH * dispAspect);
  } else {
    cropW = srcW;
    cropH = static_cast<uint32_t>(srcW / dispAspect);
  }
  // Clamp crop dimensions BEFORE computing crop offsets to prevent
  // unsigned underflow in (srcW - cropW) / (srcH - cropH).
  cropW = std::min(cropW, srcW);
  cropH = std::min(cropH, srcH);
  // Clamp to at least 1 to prevent zero-dimension crops.
  cropW = std::max(cropW, 1u);
  cropH = std::max(cropH, 1u);
  if (srcAspect > dispAspect) {
    cropX = (srcW - cropW) / 2;
    cropY = 0;
  } else {
    cropX = 0;
    cropY = (srcH - cropH) / 2;
  }

  for (uint32_t dy = 0; dy < dispH; ++dy) {
    uint32_t sy = cropY + static_cast<uint32_t>(
                              (static_cast<uint64_t>(dy) * cropH) / dispH);
    if (sy >= srcH)
      sy = srcH - 1;
    for (uint32_t dx = 0; dx < dispW; ++dx) {
      uint32_t sx = cropX + static_cast<uint32_t>(
                                (static_cast<uint64_t>(dx) * cropW) / dispW);
      if (sx >= srcW)
        sx = srcW - 1;
      const uint8_t *px = rgb + (static_cast<size_t>(sy) * srcW + sx) * 3;
      uint8_t r = px[0];
      uint8_t g = px[1];
      uint8_t b = px[2];
      uint16_t pixel =
          static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      size_t idx = (static_cast<size_t>(dy) * dispW + dx) * 2;
      out[idx] = static_cast<uint8_t>(pixel >> 8);
      out[idx + 1] = static_cast<uint8_t>(pixel & 0xFF);
    }
  }
  return out;
}

// Decode PNG to RGB24. Returns a malloc'd buffer (caller must free) or
// nullptr on failure. Sets w/h on success.
uint8_t *decodePngToRgb24(FILE *fp, uint32_t &w, uint32_t &h) {
  unsigned char sig[8];
  if (fread(sig, 1, 8, fp) != 8 || !png_check_sig(sig, 8)) {
    return nullptr;
  }

  png_structp png =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) {
    return nullptr;
  }

  // Defense-in-depth: cap PNG dimensions at the libpng level too.
  png_set_user_limits(png, kMaxDecodeDim, kMaxDecodeDim);

  // info is created inside decodePngCore (after setjmp) so that an
  // OOM during png_create_info_struct is caught by the longjmp guard
  // instead of aborting the process. Initialized to nullptr so
  // png_destroy_read_struct is safe even if creation never happened.
  png_infop info = nullptr;
  uint8_t *rgb = nullptr;
  png_bytep *rows = nullptr;
  int rc = decodePngCore(png, &info, fp, &rgb, &rows, &w, &h);

  png_destroy_read_struct(&png, &info, nullptr);

  // Free row pointer array if it was allocated (always, since it's
  // just an array of pointers — the pixel data is in rgb).
  std::free(static_cast<void *>(rows));
  if (rc != 0) {
    std::free(rgb);
    return nullptr;
  }

  // RAII guard so rgb is freed if anything throws before return.
  MallocGuard guard{rgb};
  return guard.release(); // ownership transferred to caller
}

#ifdef HAVE_JPEG
// Decode JPEG to RGB24. Returns a malloc'd buffer (caller must free) or
// nullptr on failure. Never calls exit().
uint8_t *decodeJpegToRgb24(FILE *fp, uint32_t &w, uint32_t &h) {
  struct jpeg_decompress_struct cinfo;
  JpegErrCtx
      jerr; // Must outlive cinfo (jpeg_destroy_decompress accesses cinfo->err)
  uint8_t *rgb = nullptr;
  bool cinfoCreated = false;
  int rc = decodeJpegCore(&cinfo, &jerr, fp, &rgb, &w, &h, &cinfoCreated);

  // Only destroy if jpeg_create_decompress actually succeeded.
  if (cinfoCreated)
    jpeg_destroy_decompress(&cinfo);

  if (rc != 0) {
    std::free(rgb);
    return nullptr;
  }

  // RAII guard so rgb is freed if anything throws before return.
  MallocGuard guard{rgb};
  return guard.release(); // ownership transferred to caller
}
#endif // HAVE_JPEG

} // namespace

std::vector<uint8_t> decodeImageToRgb565(const std::string &path,
                                         uint32_t dispW, uint32_t dispH) {
  // Determine format by file signature (magic bytes), not extension.
  // This prevents misnamed files from being routed to the wrong decoder,
  // which could cause silent failures or parser confusion.
  // JPEG: FF D8 (SOI marker)
  // PNG:  89 50 4E 47 0D 0A 1A 0A (8-byte signature)
  //
  // The fd is kept open across magic detection and decoding to eliminate
  // a TOCTOU window (file could be swapped between close and reopen).
  int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    return {};
  // Reject non-regular files (devices, FIFOs, sockets) that could
  // block or produce unexpected behavior in the decoder.
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return {};
  }
  unsigned char magic[8] = {};
  // Read magic bytes with retry on partial reads (EINTR, slow FS).
  ssize_t total = 0;
  while (total < static_cast<ssize_t>(sizeof(magic))) {
    ssize_t n = ::read(fd, magic + total, sizeof(magic) - total);
    if (n > 0) {
      total += n;
      continue;
    }
    if (n == 0)
      break; // EOF
    if (errno == EINTR)
      continue;
    break; // error
  }
  if (total < 2) {
    ::close(fd);
    return {};
  }

  // Rewind so the decoder sees the full file from the start.
  if (::lseek(fd, 0, SEEK_SET) < 0) {
    ::close(fd);
    return {};
  }

  bool isJpeg = (magic[0] == 0xFF && magic[1] == 0xD8);
  bool isPng = (total >= 8 && magic[0] == 0x89 && magic[1] == 0x50 &&
                magic[2] == 0x4E && magic[3] == 0x47 && magic[4] == 0x0D &&
                magic[5] == 0x0A && magic[6] == 0x1A && magic[7] == 0x0A);

  // Transfer fd ownership to FILE* for the decoder.
  FILE *fp = ::fdopen(fd, "rb");
  if (!fp) {
    ::close(fd);
    return {};
  }

  // RAII guard for FILE* so it is closed even if the decoder throws.
  class FileGuard {
  public:
    explicit FileGuard(FILE *fp) : fp_(fp) {}
    ~FileGuard() {
      if (fp_)
        fclose(fp_);
    }

  private:
    FILE *fp_;
  } fg{fp};

  uint32_t w = 0;
  uint32_t h = 0;
  uint8_t *rgb24 = nullptr;

  try {
    if (isPng) {
      rgb24 = decodePngToRgb24(fp, w, h);
    } else if (isJpeg) {
#ifdef HAVE_JPEG
      rgb24 = decodeJpegToRgb24(fp, w, h);
#else
      (void)w;
      (void)h;
      return {};
#endif
    } else {
      return {};
    }
  } catch (const std::bad_alloc &) {
    std::cerr << "decodeImageToRgb565: out of memory\n";
    return {};
  } catch (const std::exception &e) {
    std::cerr << "decodeImageToRgb565: " << e.what() << "\n";
    return {};
  }

  if (!rgb24 || w == 0 || h == 0) {
    std::free(rgb24);
    return {};
  }

  // Scale directly from the malloc'd buffer — no intermediate vector copy.
  // MallocGuard keeps ownership so rgb24 is freed even if the scaling
  // std::vector allocation throws std::bad_alloc.
  MallocGuard guard{rgb24};
  return rgb24ToRgb565Scaled(guard.get(), w, h, dispW, dispH);
}

} // namespace picamera
