#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace picamera {

enum class OutputFormat {
    PPM,
    RAW_NV12,
    PNG,
    JPEG,  // ISP hardware-encoded MJPEG (Pi only); buffer is a complete JPEG
    DNG,   // Raw Bayer DNG (requires raw stream from libcamera)
};

// Parse a CLI/output format name ("ppm", "raw", "png", "jpeg"/"jpg", "dng")
// into an OutputFormat. Returns std::nullopt for an unknown name so callers
// can produce their own error message. Case-sensitive, matches --format.
inline std::optional<OutputFormat> parseOutputFormat(std::string_view name) {
    if (name == "ppm") return OutputFormat::PPM;
    if (name == "raw") return OutputFormat::RAW_NV12;
    if (name == "png") return OutputFormat::PNG;
    if (name == "jpeg" || name == "jpg") return OutputFormat::JPEG;
    if (name == "dng") return OutputFormat::DNG;
    return std::nullopt;
}

// Canonical file extension (without the dot) for an OutputFormat.
inline std::string_view extensionFor(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::PPM:      return "ppm";
        case OutputFormat::RAW_NV12: return "raw";
        case OutputFormat::PNG:      return "png";
        case OutputFormat::JPEG:     return "jpg";
        case OutputFormat::DNG:      return "dng";
    }
    return "ppm";
}

struct CameraConfig {
    uint32_t width = 4056;
    uint32_t height = 3040;
    uint64_t exposureTime = 0;
    float analogueGain = 0;
    float digitalGain = 0;
    std::string awbMode = "auto";
    bool aeEnable = true;
    bool awbEnable = true;
    OutputFormat format = OutputFormat::PPM;
    uint32_t warmupFrames = 8;  // frames to let AE/AWB converge before saving
    int pngLevel = 6;  // zlib compression level for PNG (0=none, 1=fast, 6=default, 9=best)
    // HDR bracketing: capture N frames at EV offsets relative to metered exposure.
    // e.g. --bracket 3,-2,0,+2 captures 3 frames at -2EV, 0EV, +2EV.
    // Empty = no bracketing (single shot).
    std::vector<float> bracketEv;  // EV offsets in stops
};

} // namespace picamera
