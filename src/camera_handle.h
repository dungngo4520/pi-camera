#pragma once

#include <libcamera/libcamera.h>
#include <memory>
#include <mutex>
#include <string_view>

namespace picamera {

// RAII wrapper for the libcamera CameraManager + Camera lifecycle.
//
// Owns a started CameraManager and an acquired Camera. The destructor
// releases the camera and stops the manager, so callers don't need to call
// shutdown() explicitly (but can, for early release or to reset state).
//
// Both CameraApp (still capture) and DualStream (viewfinder + still) share
// this lifecycle; they differ only in what they do with the camera once
// acquired (configure for StillCapture vs Viewfinder, allocate buffers,
// run loops). CameraHandle centralizes the acquire/release ordering and
// error messages.
//
// cam_ and cm_ are guarded by mtx_ because camera() is called from
// libcamera's request-completion callback thread while shutdown() may
// be called from the main thread. Without synchronization, concurrent
// shared_ptr copy and reset is a data race (CWE-362).
class CameraHandle {
public:
  CameraHandle() = default;
  ~CameraHandle() noexcept { shutdown(); }
  CameraHandle(const CameraHandle &) = delete;
  CameraHandle &operator=(const CameraHandle &) = delete;

  // Start CameraManager and acquire camera[0]. `logPrefix` is prepended
  // to error messages (e.g. "Camera:" or "Stream:"). Returns false on
  // any failure (cleans up partial state before returning).
  bool init(std::string_view logPrefix = "Camera");

  // Release the camera and stop the CameraManager. Idempotent.
  // noexcept: wraps libcamera calls in try/catch to prevent
  // std::terminate if called from the destructor.
  void shutdown() noexcept;

  // The acquired camera. nullptr if not initialized or after shutdown().
  // Returns a snapshot under the mutex for thread-safe access.
  std::shared_ptr<libcamera::Camera> camera() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cam_;
  }

private:
  mutable std::mutex mtx_;
  std::shared_ptr<libcamera::CameraManager> cm_;
  std::shared_ptr<libcamera::Camera> cam_;
};

} // namespace picamera
