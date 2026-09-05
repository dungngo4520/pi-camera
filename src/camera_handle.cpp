#include "camera_handle.h"

#include <iostream>

namespace picamera {

bool CameraHandle::init(std::string_view logPrefix) {
  std::lock_guard<std::mutex> lk(mtx_);
  cm_ = std::make_shared<libcamera::CameraManager>();
  if (cm_->start()) {
    std::cerr << logPrefix << ": CameraManager::start() failed\n";
    cm_.reset();
    return false;
  }

  auto cameras = cm_->cameras();
  if (cameras.empty()) {
    std::cerr << logPrefix << ": No cameras found\n";
    // Don't call cm_->stop() here — the destructor's shutdown() handles
    // the Camera-before-Manager destruction order. Calling cm_->stop()
    // while the CameraManager holds internal Camera references causes
    // a segfault ("Removing media device while still in use").
    cm_.reset();
    return false;
  }

  cam_ = cameras[0];
  if (cam_->acquire()) {
    std::cerr << logPrefix << ": Failed to acquire camera\n";
    // Same as above — let the destructor handle cleanup ordering.
    cam_.reset();
    cm_.reset();
    return false;
  }

  std::cout << logPrefix << ": " << cam_->id() << " acquired\n";
  return true;
}

void CameraHandle::shutdown() noexcept {
  // Snapshot under lock, then release outside the lock to avoid
  // invoking libcamera callbacks while holding the mutex (which
  // could deadlock if a callback thread is blocked on camera()).
  std::shared_ptr<libcamera::Camera> camLocal;
  std::shared_ptr<libcamera::CameraManager> cmLocal;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    camLocal = std::move(cam_);
    cmLocal = std::move(cm_);
  }
  try {
    if (camLocal) {
      // Stop the camera before releasing it. release() on a
      // started camera is undefined in libcamera. Callers should
      // have already called stop(), but this is a safety net.
      // stop() on an unstarted camera is safe (no-op or throws,
      // caught by the try/catch below).
      try {
        camLocal->stop();
      } catch (const std::exception &e) {
        std::cerr << "CameraHandle: camera stop threw: " << e.what() << "\n";
      }
      camLocal->release();
    }
  } catch (const std::exception &e) {
    std::cerr << "CameraHandle: camera release threw: " << e.what() << "\n";
  }
  // Destroy the Camera BEFORE stopping/destroying the CameraManager.
  // The Camera object references media devices and other resources owned
  // by the manager. If the manager is stopped first, the Camera destructor
  // touches freed resources → "Removing media device while still in use"
  // followed by a segfault. Explicitly reset camLocal here to guarantee
  // destruction order regardless of declaration order.
  camLocal.reset();
  try {
    if (cmLocal) {
      cmLocal->stop();
    }
  } catch (const std::exception &e) {
    std::cerr << "CameraHandle: camera manager stop threw: " << e.what()
              << "\n";
  }
}

} // namespace picamera
