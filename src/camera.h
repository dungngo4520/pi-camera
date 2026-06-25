#pragma once

#include <libcamera/libcamera.h>
#include <memory>
#include <string>

namespace picamera {

enum class OutputFormat {
    PPM,
    RAW_NV12,
    PNG,
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
};

class CameraApp {
public:
    CameraApp();
    ~CameraApp();

    bool init();
    void configure(const CameraConfig &cfg);
    bool capture(const std::string &filename);
    bool timelapse(int intervalSec, int count, const std::string &pattern);
    void listControls();
    void shutdown();

private:
    void applyControls(libcamera::Request *req, const CameraConfig &cfg);
    bool saveFrame(const libcamera::Request *req, const std::string &filename);

    std::shared_ptr<libcamera::CameraManager> cm_;
    std::shared_ptr<libcamera::Camera> cam_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *stream_ = nullptr;
    CameraConfig config_;
};

}
