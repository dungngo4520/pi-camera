#pragma once

#include <libcamera/libcamera.h>
#include <memory>
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

class CameraApp {
public:
    CameraApp();
    ~CameraApp();

    bool init();
    bool configure(const CameraConfig &cfg);
    bool capture(const std::string &filename);
    bool captureBracket(const std::string &baseFilename);
    bool timelapse(int intervalSec, int count, const std::string &pattern);
    void listControls();
    void shutdown();

private:
    static void applyControls(libcamera::Request *req, const CameraConfig &cfg);
    bool saveFrame(const libcamera::Request *req, const std::string &filename);
    void stopCamera();  // idempotent: stop + clear started_ flag

    std::shared_ptr<libcamera::CameraManager> cm_;
    std::shared_ptr<libcamera::Camera> cam_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *stream_ = nullptr;
    CameraConfig config_;
    bool started_ = false;
};

}
