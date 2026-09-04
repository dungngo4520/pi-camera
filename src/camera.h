#pragma once

#include "camera_config.h"
#include "camera_handle.h"

#include <libcamera/libcamera.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace picamera {

class CameraApp {
public:
    CameraApp();
    ~CameraApp();

    [[nodiscard]] bool init();
    [[nodiscard]] bool configure(const CameraConfig &cfg);
    [[nodiscard]] bool capture(const std::string &filename,
                               std::string *actualPath = nullptr);
    [[nodiscard]] bool captureBracket(const std::string &baseFilename);
    void listControls();
    void shutdown() noexcept;
    bool fatalError() const { return fatalError_.load(std::memory_order_acquire); }

private:
    static void applyControls(libcamera::Request *req, const CameraConfig &cfg);
    bool saveFrame(const libcamera::Request *req, const std::string &filename,
                   std::string *actualPath = nullptr);
    void stopCamera() noexcept;  // idempotent: stop + clear started_ flag

    // Re-queue a request with controls re-applied. Used by the capture
    // callback for warmup/error/discard paths. Catches applyControls and
    // queueRequest exceptions to prevent std::terminate inside the callback.
    void requeueRequest(libcamera::Camera *cam, libcamera::Request *r);

    // Handle a requestCompleted callback during capture(). Dispatches
    // between warmup-discard, error-requeue, duplicate-discard, and the
    // final save path based on the completion count and request status.
    void onCaptureCompleted(libcamera::Request *r,
                            const std::string &filename, uint32_t warmup);

    CameraHandle handle_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *stream_ = nullptr;
    CameraConfig config_;
    bool started_ = false;
    bool swJpegEncode_ = false;  // true when HW MJPEG unavailable, encode via libjpeg

    // Capture completion state — stored as members so the libcamera callback
    // never references stack-local synchronization primitives. The callback
    // outlives the capture() call frame (libcamera may still invoke it during
    // stop()), so these must have class-level lifetime.
    std::mutex capMtx_;
    std::condition_variable capCv_;
    std::atomic<uint32_t> capCompleted_{0};
    bool capDone_ = false;
    bool capSaved_ = false;
    std::string capActualPath_;  // actual filename (may differ on collision)
    // In-flight callback counter: incremented on callback entry, decremented
    // on exit. stop()/shutdown() wait for this to reach 0 before clearing
    // capRequests_, ensuring no callback is dereferencing a Request when
    // its unique_ptr is destroyed.
    std::atomic<int> callbacksInFlight_{0};
    std::mutex callbacksMtx_;
    std::condition_variable callbacksCv_;
    // Set when callbacks are stuck after timeout — signals the caller to
    // exit gracefully instead of aborting (systemd Restart=on-failure).
    std::atomic<bool> fatalError_{false};
    // Request objects stored as members so they outlive the capture() call
    // frame — libcamera may still invoke callbacks during stop().
    std::vector<std::unique_ptr<libcamera::Request>> capRequests_;
};

}
