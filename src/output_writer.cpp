#include "output_writer.h"

#include "dng.h"
#include "image.h"
#include "output.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace picamera {

namespace {

// ---------------------------------------------------------------------------
// DNG writer: unpacks MIPI-packed 10-bit Bayer (SRGGB10_CSI2P) to 16-bit
// samples and writes a DNG container with EXIF metadata.
// ---------------------------------------------------------------------------
class DngWriter : public OutputWriter {
public:
    explicit DngWriter(const CameraConfig &cfg) : cfg_(cfg) {}

    bool write(const FrameView &f, const std::string &filename) override {
        const uint8_t *rawData = f.plane0;
        size_t numPixels = static_cast<size_t>(f.width) * f.height;
        std::vector<uint8_t> unpacked(numPixels * 2);
        // SRGGB10_CSI2P packs 4 10-bit pixels into 5 bytes. Each 5-byte
        // group: [b0 b1 b2 b3 | b4]
        //   pixel0 = b0 | ((b4 & 0x03) << 8)
        //   pixel1 = b1 | ((b4 & 0x0C) << 6)
        //   pixel2 = b2 | ((b4 & 0x30) << 4)
        //   pixel3 = b3 | ((b4 & 0xC0) << 2)
        size_t packedSize = (numPixels / 4) * 5;
        for (size_t i = 0, p = 0; p < packedSize; i += 4, p += 5) {
            uint16_t p0 = rawData[p]     | ((rawData[p + 4] & 0x03) << 8);
            uint16_t p1 = rawData[p + 1] | ((rawData[p + 4] & 0x0C) << 6);
            uint16_t p2 = rawData[p + 2] | ((rawData[p + 4] & 0x30) << 4);
            uint16_t p3 = rawData[p + 3] | ((rawData[p + 4] & 0xC0) << 2);
            unpacked[i * 2]     = p0 & 0xFF;
            unpacked[i * 2 + 1] = (p0 >> 8) & 0xFF;
            unpacked[(i+1) * 2]     = p1 & 0xFF;
            unpacked[(i+1) * 2 + 1] = (p1 >> 8) & 0xFF;
            unpacked[(i+2) * 2]     = p2 & 0xFF;
            unpacked[(i+2) * 2 + 1] = (p2 >> 8) & 0xFF;
            unpacked[(i+3) * 2]     = p3 & 0xFF;
            unpacked[(i+3) * 2 + 1] = (p3 >> 8) & 0xFF;
        }

        DngMetadata dngMeta;
        dngMeta.width = f.width;
        dngMeta.height = f.height;
        dngMeta.bitsPerPixel = 10;
        dngMeta.blackLevel = 64;    // typical IMX477 black level
        dngMeta.whiteLevel = 1023;  // 10-bit max
        dngMeta.activeTop = 0;
        dngMeta.activeLeft = 0;
        dngMeta.activeBottom = f.height;
        dngMeta.activeRight = f.width;
        dngMeta.exposureTimeUs = cfg_.exposureTime;
        dngMeta.analogueGain = cfg_.analogueGain;
        dngMeta.timestampSec = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        bool ok = writeDng(filename.c_str(), unpacked.data(), unpacked.size(),
                           dngMeta);
        if (ok) {
            std::cout << "Saved DNG: " << filename << " (" << f.width << "x"
                      << f.height << ") " << unpacked.size() << " bytes\n";
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
    bool write(const FrameView &f, const std::string &filename) override {
        const uint8_t *data = f.plane0;
        size_t writeLen = f.plane0Size;
        for (size_t i = 0; i + 1 < f.plane0Size; ++i) {
            if (data[i] == 0xFF && data[i + 1] == 0xD9) {
                writeLen = i + 2;
                break;
            }
        }
        bool ok = writeJpeg(data, writeLen, filename);
        if (ok) {
            std::cout << "Saved JPEG: " << filename << " (" << f.width << "x"
                      << f.height << ") " << writeLen << " bytes\n";
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
    bool write(const FrameView &f, const std::string &filename) override {
        size_t ySize = static_cast<size_t>(f.stride) * f.height;
        size_t uvSize = static_cast<size_t>(f.stride) * (f.height / 2);
        bool ok = writeRaw(f.plane0, ySize, f.plane1, uvSize, filename);
        if (ok) {
            std::cout << "Saved RAW: " << filename << " (" << f.width << "x"
                      << f.height << ")\n";
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

    bool write(const FrameView &f, const std::string &filename) override {
        auto rgb = nv12ToRgb(f.plane0, f.plane1, f.width, f.height, f.stride);
        if (rgb.empty()) return false;
        bool ok = writePng(filename.c_str(), rgb.data(), f.width, f.height,
                           cfg_.pngLevel);
        if (ok) {
            std::cout << "Saved PNG: " << filename << " (" << f.width << "x"
                      << f.height << ")\n";
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
// ---------------------------------------------------------------------------
class SwJpegWriter : public OutputWriter {
public:
    bool write(const FrameView &f, const std::string &filename) override {
        auto rgb = nv12ToRgb(f.plane0, f.plane1, f.width, f.height, f.stride);
        if (rgb.empty()) return false;
        bool ok = writeJpegRgb(rgb.data(), f.width, f.height, filename, 90);
        if (ok) {
            std::cout << "Saved JPEG: " << filename << " (" << f.width << "x"
                      << f.height << ") [sw encode]\n";
        } else {
            std::cerr << "Failed to write JPEG: " << filename << "\n";
        }
        return ok;
    }
};

// ---------------------------------------------------------------------------
// PPM writer: NV12 -> RGB24, then uncompressed PPM write.
// ---------------------------------------------------------------------------
class PpmWriter : public OutputWriter {
public:
    bool write(const FrameView &f, const std::string &filename) override {
        auto rgb = nv12ToRgb(f.plane0, f.plane1, f.width, f.height, f.stride);
        if (rgb.empty()) return false;
        bool ok = writePpm(rgb.data(), rgb.size(), f.width, f.height, filename);
        if (ok) {
            std::cout << "Saved PPM: " << filename << " (" << f.width << "x"
                      << f.height << ") " << rgb.size() << " bytes\n";
        } else {
            std::cerr << "Failed to write PPM: " << filename << "\n";
        }
        return ok;
    }
};

} // namespace

std::unique_ptr<OutputWriter> makeOutputWriter(OutputFormat fmt,
                                               const CameraConfig &cfg,
                                               bool swJpegEncode) {
    switch (fmt) {
        case OutputFormat::DNG:     return std::make_unique<DngWriter>(cfg);
        case OutputFormat::JPEG:
            return swJpegEncode
                ? std::unique_ptr<OutputWriter>(std::make_unique<SwJpegWriter>())
                : std::unique_ptr<OutputWriter>(std::make_unique<HwJpegWriter>());
        case OutputFormat::RAW_NV12: return std::make_unique<RawNv12Writer>();
        case OutputFormat::PNG:      return std::make_unique<PngWriter>(cfg);
        case OutputFormat::PPM:      return std::make_unique<PpmWriter>();
    }
    return nullptr;
}

} // namespace picamera
