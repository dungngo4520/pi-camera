#pragma once

#include <libcamera/libcamera.h>
#include <memory>
#include <string_view>

namespace picamera {

// RAII wrapper for the libcamera CameraManager + Camera lifecycle.
//
// Owns a started CameraManager and an acquired Camera. The destructor
// releases the camera and stops the manager, so callers don't need to call
// shutdown() explicitly (but can, for early release or to reset state).
//
// Both CameraApp (still capture) and CameraStream (viewfinder) share this
// lifecycle; they differ only in what they do with the camera once acquired
// (configure for StillCapture vs Viewfinder, allocate buffers, run loops).
// CameraHandle centralizes the acquire/release ordering and error messages.
class CameraHandle {
public:
    CameraHandle() = default;
    ~CameraHandle() { shutdown(); }
    CameraHandle(const CameraHandle &) = delete;
    CameraHandle &operator=(const CameraHandle &) = delete;

    // Start CameraManager and acquire camera[0]. `logPrefix` is prepended
    // to error messages (e.g. "Camera:" or "Stream:"). Returns false on
    // any failure (cleans up partial state before returning).
    bool init(std::string_view logPrefix = "Camera");

    // Release the camera and stop the CameraManager. Idempotent.
    void shutdown();

    // The acquired camera. nullptr if not initialized or after shutdown().
    std::shared_ptr<libcamera::Camera> camera() const { return cam_; }

private:
    std::shared_ptr<libcamera::CameraManager> cm_;
    std::shared_ptr<libcamera::Camera> cam_;
};

} // namespace picamera
