#pragma once

#include "camera_config.h"

#include <cstdint>
#include <memory>
#include <string>

namespace picamera {

// A view onto a captured frame's already-mapped buffer data.
// Single-plane formats (DNG raw, HW MJPEG) set plane0 only.
// Two-plane NV12 sets plane0 (Y) and plane1 (UV).
// Pointers are valid only for the duration of the write() call.
struct FrameView {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    const uint8_t *plane0 = nullptr;  // Y (NV12) or raw/JPEG bitstream
    size_t plane0Size = 0;
    const uint8_t *plane1 = nullptr;  // UV (NV12 only)
    size_t plane1Size = 0;
};

// Abstract output writer. Each subclass handles one capture format,
// decoupling buffer interpretation/encoding from the libcamera lifecycle.
// This lets the writers be unit-tested with synthetic FrameViews on x86.
class OutputWriter {
public:
    virtual ~OutputWriter() = default;
    // Encode the frame and write to `filename`. Returns true on success.
    virtual bool write(const FrameView &frame, const std::string &filename) = 0;
};

// Factory: pick the writer for the configured format.
// swJpegEncode disambiguates JPEG (HW MJPEG buffer vs software libjpeg encode
// from RGB); it is determined at camera-configure time by CameraApp.
std::unique_ptr<OutputWriter> makeOutputWriter(OutputFormat fmt,
                                               const CameraConfig &cfg,
                                               bool swJpegEncode = false);

} // namespace picamera
