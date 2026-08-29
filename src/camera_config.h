#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace picamera {

enum class OutputFormat {
    PPM,
    RAW_NV12,
    PNG,
    JPEG,  // ISP hardware-encoded MJPEG (Pi only); buffer is a complete JPEG
    DNG,   // Raw Bayer DNG (requires raw stream from libcamera)
};

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
