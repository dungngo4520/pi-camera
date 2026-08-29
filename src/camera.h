#pragma once

#include "camera_config.h"
#include "camera_handle.h"

#include <libcamera/libcamera.h>
#include <memory>
#include <string>

namespace picamera {

class CameraApp {
public:
    CameraApp();
    ~CameraApp();

    bool init();
    bool configure(const CameraConfig &cfg);
    bool capture(const std::string &filename);
    bool captureBracket(const std::string &baseFilename);
    void listControls();
    void shutdown();

private:
    static void applyControls(libcamera::Request *req, const CameraConfig &cfg);
    bool saveFrame(const libcamera::Request *req, const std::string &filename);
    void stopCamera();  // idempotent: stop + clear started_ flag

    CameraHandle handle_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *stream_ = nullptr;
    CameraConfig config_;
    bool started_ = false;
    bool swJpegEncode_ = false;  // true when HW MJPEG unavailable, encode via libjpeg
};

}
