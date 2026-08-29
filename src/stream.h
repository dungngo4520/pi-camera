#pragma once

#include "camera_handle.h"

#include <libcamera/libcamera.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>

namespace picamera {

// A frame grabbed from the camera stream (NV12 format).
struct StreamFrame {
    const uint8_t *y = nullptr;
    const uint8_t *uv = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
};

// Camera streaming via libcamera Viewfinder role.
// Continuously captures frames at low resolution for live preview.
// Owns its own CameraManager/Camera lifecycle via CameraHandle — shutdown()
// must be called before any other CameraApp can acquire the camera.
class CameraStream {
public:
    CameraStream();
    ~CameraStream();

    bool init();
    bool start(uint32_t width, uint32_t height);
    void stop();
    void shutdown(); // stop + release camera + stop CameraManager

    // Wait for the next frame. Returns frame with y=nullptr on timeout.
    // The returned pointers are valid until the next grabFrame() call.
    StreamFrame grabFrame(int timeoutMs = 2000);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t stride() const { return stride_; }

private:
    CameraHandle handle_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *stream_ = nullptr;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    bool started_ = false;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool frameReady_ = false;

    // Current frame data (copied from dmabuf on each completion)
    std::vector<uint8_t> yData_;
    std::vector<uint8_t> uvData_;
    uint32_t width_ = 0, height_ = 0, stride_ = 0;
};

} // namespace picamera
