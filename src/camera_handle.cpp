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
    // Don't call cm_->stop() here — calling it while the CameraManager
    // holds internal Camera references causes a segfault. The destructor
    // handles cleanup ordering.
    cm_.reset();
    return false;
  }

  cam_ = cameras[0];
  if (cam_->acquire()) {
    std::cerr << logPrefix << ": Failed to acquire camera\n";
    // Let the destructor handle cleanup ordering.
    cam_.reset();
    cm_.reset();
    return false;
  }

  std::cout << logPrefix << ": " << cam_->id() << " acquired\n";
  return true;
}

void CameraHandle::shutdown() noexcept {
  // Snapshot under lock, then release outside the lock to avoid
  // invoking libcamera callbacks while holding the mutex.
  std::shared_ptr<libcamera::Camera> camLocal;
  std::shared_ptr<libcamera::CameraManager> cmLocal;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    camLocal = std::move(cam_);
    cmLocal = std::move(cm_);
  }
  try {
    if (camLocal) {
      // Stop before releasing — release() on a started camera is UB.
      // stop() on an unstarted camera is safe (no-op or throws, caught below).
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
  // Destroy the Camera BEFORE stopping the CameraManager — the Camera
  // references media devices owned by the manager. If the manager is
  // stopped first, the Camera destructor touches freed resources → segfault.
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
