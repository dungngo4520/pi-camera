#include "output_writer.h"

#include "dng.h"
#include "image.h"
#include "encoders.h"
#include "safe_path.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace picamera {

namespace {

// Returns the path that should be shown to the user: the actual saved path
// (which may carry a uniqueness suffix from O_EXCL collision handling) when
// available, otherwise the requested filename. Returns by value to avoid
// dangling references when callers pass temporaries.
std::string shownPath(const std::string &filename,
                      const std::string *actualPath) {
    return (actualPath && !actualPath->empty()) ? *actualPath : filename;
}

// ---------------------------------------------------------------------------
// DNG writer: unpacks MIPI-packed 10-bit Bayer (SRGGB10_CSI2P) to 16-bit
// samples and writes a DNG container with EXIF metadata.
// ---------------------------------------------------------------------------
class DngWriter : public OutputWriter {
public:
    explicit DngWriter(const CameraConfig &cfg) : cfg_(cfg) {}

    [[nodiscard]] bool write(const FrameView &f, const std::string &filename,
               std::string *actualPath) override {
        const uint8_t *rawData = f.plane0;
        if (!rawData) return false;
        // Reject zero-dimension frames — they produce malformed DNGs.
        if (f.width == 0 || f.height == 0) {
            std::cerr << "DNG: zero-dimension frame (" << f.width << "x" << f.height << ")\n";
            return false;
        }
        // Checked size computation to prevent integer overflow
        size_t numPixels = 0;
        if (!checkedMul(static_cast<size_t>(f.width), f.height, numPixels)) {
            std::cerr << "DNG: dimensions overflow (" << f.width << "x" << f.height << ")\n";
            return false;
        }
        // Validate that the pixel count is reasonable.
        if (numPixels == 0) {
            std::cerr << "DNG: zero pixels\n";
            return false;
        }
        // SRGGB10_CSI2P packs 4 10-bit pixels into 5 bytes per group.
        // Each row has ceil(width/4) groups = ceil(width/4)*5 bytes of
        // packed data, but the source buffer may have a larger stride
        // (DMA/cacheline padding). Use f.stride if non-zero, otherwise
        // assume contiguous.
        size_t rowPacked = 0;
        if (!checkedMul((static_cast<size_t>(f.width) + 3) / 4, 5, rowPacked)) {
            std::cerr << "DNG: row packed size overflow\n";
            return false;
        }
        size_t stride = (f.stride > 0) ? static_cast<size_t>(f.stride) : rowPacked;
        if (stride < rowPacked) {
            std::cerr << "DNG: stride (" << stride << ") < row packed size ("
                      << rowPacked << ")\n";
            return false;
        }
        // Validate source buffer is large enough for stride * height.
        size_t sourceSize = 0;
        if (!checkedMul(stride, static_cast<size_t>(f.height), sourceSize)) {
            std::cerr << "DNG: source size overflow\n";
            return false;
        }
        if (f.plane0Size < sourceSize) {
            std::cerr << "DNG: source buffer too small (" << f.plane0Size
                      << " < " << sourceSize << ")\n";
            return false;
        }
        size_t unpackedSize = 0;
        if (!checkedMul(numPixels, 2, unpackedSize)) {
            std::cerr << "DNG: unpacked size overflow\n";
            return false;
        }
        // Use uint16_t vector for correct alignment (reinterpret_cast from
        // uint8_t* to uint16_t* would be UB due to alignment requirements).
        static_assert(sizeof(uint16_t) == 2, "uint16_t must be 2 bytes for DNG packing");
        std::vector<uint16_t> unpacked(numPixels);
        // Unpack row-by-row, advancing by stride in the source buffer.
        // SRGGB10_CSI2P packs 4 10-bit pixels into 5 bytes. Each 5-byte
        // group: [b0 b1 b2 b3 | b4]
        //   pixel0 = b0 | ((b4 & 0x03) << 8)
        //   pixel1 = b1 | ((b4 & 0x0C) << 6)
        //   pixel2 = b2 | ((b4 & 0x30) << 4)
        //   pixel3 = b3 | ((b4 & 0xC0) << 2)
        for (uint32_t y = 0; y < f.height; ++y) {
            const uint8_t *row = rawData + y * stride;
            uint16_t *out = unpacked.data() + static_cast<size_t>(y) * f.width;
            uint32_t x = 0;
            for (; x + 4 <= f.width; x += 4) {
                size_t p = static_cast<size_t>(x) / 4 * 5;
                uint16_t p0 = row[p]     | ((row[p + 4] & 0x03) << 8);
                uint16_t p1 = row[p + 1] | ((row[p + 4] & 0x0C) << 6);
                uint16_t p2 = row[p + 2] | ((row[p + 4] & 0x30) << 4);
                uint16_t p3 = row[p + 3] | ((row[p + 4] & 0xC0) << 2);
                out[x]     = p0;
                out[x + 1] = p1;
                out[x + 2] = p2;
                out[x + 3] = p3;
            }
            // Handle remainder pixels when width is not a multiple of 4.
            for (; x < f.width; ++x) {
                size_t p = static_cast<size_t>(x) / 4 * 5;
                size_t bitOff = static_cast<size_t>(x) % 4;
                uint16_t val = row[p + bitOff];
                uint8_t bits = row[p + 4];
                // Bit extraction pattern for each pixel position within the
                // 4-pixel group: shifts the 2-bit component from byte 4.
                constexpr uint8_t kBitMasks[4] = {0x03, 0x0C, 0x30, 0xC0};
                constexpr int kBitShifts[4] = {8, 6, 4, 2};
                val |= (bits & kBitMasks[bitOff]) << kBitShifts[bitOff];
                out[x] = val;
            }
        }

        // IMX477-specific DNG metadata. The Bayer pattern (RGGB), black
        // level (64), and white level (1023 for 10-bit) are fixed
        // properties of this sensor. If picamera is ever ported to a
        // different sensor, these should be queried from
        // libcamera::Camera::properties() instead of hardcoded.
        DngMetadata dngMeta;
        dngMeta.width = f.width;
        dngMeta.height = f.height;
        dngMeta.bitsPerPixel = 10;
        dngMeta.blackLevel = 64;    // IMX477 black level
        dngMeta.whiteLevel = 1023;  // 10-bit max
        // Bayer pattern is RGGB for IMX477. Set explicitly so the
        // hard-coding is visible at the point of use.
        dngMeta.bayerPattern[0] = 'R';
        dngMeta.bayerPattern[1] = 'G';
        dngMeta.bayerPattern[2] = 'G';
        dngMeta.bayerPattern[3] = 'B';
        dngMeta.activeTop = 0;
        dngMeta.activeLeft = 0;
        dngMeta.activeBottom = f.height;
        dngMeta.activeRight = f.width;
        dngMeta.exposureTimeUs = cfg_.exposureTime;
        dngMeta.analogueGain = cfg_.analogueGain;
        // Clamp ISO below UINT32_MAX before llround — float(UINT32_MAX)
        // may round to 2^32 which wraps to 0 in uint32_t. Also guard
        // non-finite gains (NaN/Inf) to prevent llround UB. Use llround
        // (long long) instead of lround (long) for 32-bit platform safety
        // (Pi Zero 2 W armhf has 32-bit long; UINT32_MAX-1 > LONG_MAX).
        float isoF = std::max(0.0f, cfg_.analogueGain) * 100.0f;
        if (!std::isfinite(isoF)) isoF = 0.0f;
        isoF = std::min(isoF, static_cast<float>(UINT32_MAX - 1));
        dngMeta.isoSpeed = static_cast<uint32_t>(
            std::min<long long>(std::llround(isoF), UINT32_MAX - 1));
        dngMeta.timestampSec = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        bool ok = writeDng(filename.c_str(),
                           reinterpret_cast<const uint8_t *>(unpacked.data()),
                           unpackedSize, dngMeta, actualPath);
        if (ok) {
            std::cout << "Saved DNG: " << shownPath(filename, actualPath)
                      << " (" << f.width << "x" << f.height << ") "
                      << unpacked.size() * 2 << " bytes\n";
        } else {
            std::cerr << "Failed to write DNG: " << filename << "\n";
        }
        return ok;
    }

private:
    CameraConfig cfg_;
};

// ---------------------------------------------------------------------------
// HW JPEG writer: the Pi ISP produces a complete MJPEG bitstream in the
// buffer. Find the JPEG end marker (FFD9) in case the plane is padded,
// then write the bitstream directly — no software encode.
// ---------------------------------------------------------------------------
class HwJpegWriter : public OutputWriter {
public:
    [[nodiscard]] bool write(const FrameView &f, const std::string &filename,
               std::string *actualPath) override {
        if (!f.plane0 || f.plane0Size < 2) return false;
        const uint8_t *data = f.plane0;
        // Validate the JPEG Start-Of-Image marker before writing — a
        // malformed or zeroed buffer from the pipeline would produce an
        // invalid .jpg file that downstream decoders reject.
        if (data[0] != 0xFF || data[1] != 0xD8) {
            std::cerr << "HwJpegWriter: buffer missing SOI marker\n";
            return false;
        }
        // Find the JPEG End-Of-Image marker to trim padding/unused buffer.
        // If no EOI is found, the buffer is truncated/corrupt — reject it
        // rather than writing an oversized or incomplete JPEG.
        size_t writeLen = 0;
        for (size_t i = 0; i + 1 < f.plane0Size; ++i) {
            if (data[i] == 0xFF && data[i + 1] == 0xD9) {
                writeLen = i + 2;
                break;
            }
        }
        if (writeLen == 0) {
            std::cerr << "HwJpegWriter: buffer missing EOI marker (truncated JPEG)\n";
            return false;
        }
        bool ok = writeJpeg(data, writeLen, filename, actualPath);
        if (ok) {
            std::cout << "Saved JPEG: " << shownPath(filename, actualPath)
                      << " (" << f.width << "x" << f.height << ") "
                      << writeLen << " bytes\n";
        } else {
            std::cerr << "Failed to write JPEG: " << filename << "\n";
        }
        return ok;
    }
};

// ---------------------------------------------------------------------------
// Raw NV12 writer: writes the Y and UV planes verbatim (no conversion).
// ---------------------------------------------------------------------------
class RawNv12Writer : public OutputWriter {
public:
    [[nodiscard]] bool write(const FrameView &f, const std::string &filename,
               std::string *actualPath) override {
        // Validate plane sizes before writing to prevent out-of-bounds reads.
        size_t ySize = 0;
        size_t uvSize = 0;
        if (!checkedMul(static_cast<size_t>(f.stride), f.height, ySize)) return false;
        // NV12 UV plane has ceil(height/2) rows for odd heights.
        if (!checkedMul(static_cast<size_t>(f.stride), (f.height + 1) / 2, uvSize)) return false;
        if (!f.plane0 || f.plane0Size < ySize || !f.plane1 || f.plane1Size < uvSize)
            return false;
        bool ok = writeRaw(f.plane0, ySize, f.plane1, uvSize, filename, actualPath);
        if (ok) {
            std::cout << "Saved RAW: " << shownPath(filename, actualPath)
                      << " (" << f.width << "x" << f.height << ")\n";
        } else {
            std::cerr << "Failed to write RAW: " << filename << "\n";
        }
        return ok;
    }
};

// ---------------------------------------------------------------------------
// PNG writer: NV12 -> RGB24 conversion, then libpng encode.
// ---------------------------------------------------------------------------
class PngWriter : public OutputWriter {
public:
    explicit PngWriter(const CameraConfig &cfg) : cfg_(cfg) {}

    [[nodiscard]] bool write(const FrameView &f, const std::string &filename,
               std::string *actualPath) override {
        auto rgb = nv12ToRgb(f.plane0, f.plane1, f.width, f.height, f.stride, f.plane0Size, f.plane1Size);
        if (rgb.empty()) return false;
        bool ok = writePng(filename, rgb.data(), f.width, f.height,
                           cfg_.pngLevel, actualPath);
        if (ok) {
            std::cout << "Saved PNG: " << shownPath(filename, actualPath)
                      << " (" << f.width << "x" << f.height << ")\n";
        } else {
            std::cerr << "Failed to write PNG: " << filename << "\n";
        }
        return ok;
    }

private:
    CameraConfig cfg_;
};

// ---------------------------------------------------------------------------
// Software JPEG writer: NV12 -> RGB24, then libjpeg-turbo encode.
// Used when the Pi VC4 pipeline handler rejects HW MJPEG at high res.
// Only compiled when libjpeg is available (HAVE_JPEG); otherwise
// makeOutputWriter returns nullptr for the sw-encode path so the caller
// fails fast without the expensive NV12→RGB conversion.
// ---------------------------------------------------------------------------
#ifdef HAVE_JPEG
class SwJpegWriter : public OutputWriter {
public:
    explicit SwJpegWriter(const CameraConfig &cfg)
        : quality_(std::clamp(cfg.jpegQuality, 1, 100)) {}

    [[nodiscard]] bool write(const FrameView &f, const std::string &filename,
               std::string *actualPath) override {
        auto rgb = nv12ToRgb(f.plane0, f.plane1, f.width, f.height, f.stride, f.plane0Size, f.plane1Size);
        if (rgb.empty()) return false;
        bool ok = writeJpegRgb(rgb.data(), f.width, f.height, filename, quality_,
                               actualPath);
        if (ok) {
            std::cout << "Saved JPEG: " << shownPath(filename, actualPath)
                      << " (" << f.width << "x" << f.height
                      << ") [sw encode q" << quality_ << "]\n";
        } else {
            std::cerr << "Failed to write JPEG: " << filename << "\n";
        }
        return ok;
    }

private:
    int quality_;
};
#endif // HAVE_JPEG

// ---------------------------------------------------------------------------
// PPM writer: NV12 -> RGB24, then uncompressed PPM write.
// ---------------------------------------------------------------------------
class PpmWriter : public OutputWriter {
public:
    [[nodiscard]] bool write(const FrameView &f, const std::string &filename,
               std::string *actualPath) override {
        auto rgb = nv12ToRgb(f.plane0, f.plane1, f.width, f.height, f.stride, f.plane0Size, f.plane1Size);
        if (rgb.empty()) return false;
        bool ok = writePpm(rgb.data(), rgb.size(), f.width, f.height, filename,
                           actualPath);
        if (ok) {
            std::cout << "Saved PPM: " << shownPath(filename, actualPath)
                      << " (" << f.width << "x" << f.height << ") "
                      << rgb.size() << " bytes\n";
        } else {
            std::cerr << "Failed to write PPM: " << filename << "\n";
        }
        return ok;
    }
};

} // namespace

[[nodiscard]] std::unique_ptr<OutputWriter> makeOutputWriter(OutputFormat fmt,
                                               const CameraConfig &cfg,
                                               bool swJpegEncode) {
    switch (fmt) {
        case OutputFormat::DNG:     return std::make_unique<DngWriter>(cfg);
        case OutputFormat::JPEG:
            if (swJpegEncode) {
#ifdef HAVE_JPEG
                return std::make_unique<SwJpegWriter>(cfg);
#else
                std::cerr << "makeOutputWriter: software JPEG encode requested"
                             " but libjpeg was not compiled in\n";
                return nullptr;
#endif
            }
            return std::make_unique<HwJpegWriter>();
        case OutputFormat::RAW_NV12: return std::make_unique<RawNv12Writer>();
        case OutputFormat::PNG:      return std::make_unique<PngWriter>(cfg);
        case OutputFormat::PPM:      return std::make_unique<PpmWriter>();
        default:
            std::cerr << "makeOutputWriter: unknown format " << static_cast<int>(fmt) << "\n";
            return nullptr;
    }
}

} // namespace picamera
