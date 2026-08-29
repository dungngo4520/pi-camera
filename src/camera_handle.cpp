#include "camera_handle.h"

#include <iostream>

namespace picamera {

bool CameraHandle::init(std::string_view logPrefix) {
    cm_ = std::make_shared<libcamera::CameraManager>();
    if (cm_->start()) {
        std::cerr << logPrefix << ": CameraManager::start() failed\n";
        cm_.reset();
        return false;
    }

    auto cameras = cm_->cameras();
    if (cameras.empty()) {
        std::cerr << logPrefix << ": No cameras found\n";
        cm_->stop();
        cm_.reset();
        return false;
    }

    cam_ = cameras[0];
    if (cam_->acquire()) {
        std::cerr << logPrefix << ": Failed to acquire camera\n";
        cam_.reset();
        cm_->stop();
        cm_.reset();
        return false;
    }

    std::cout << logPrefix << ": " << cam_->id() << " acquired\n";
    return true;
}

void CameraHandle::shutdown() {
    if (cam_) {
        cam_->release();
        cam_.reset();
    }
    if (cm_) {
        cm_->stop();
        cm_.reset();
    }
}

} // namespace picamera
