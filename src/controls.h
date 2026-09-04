#pragma once

#include "camera_config.h"

#include <libcamera/libcamera.h>
#include <optional>
#include <string_view>

namespace picamera {

std::optional<libcamera::controls::AwbModeEnum> lookupAwb(std::string_view name);

struct ExposureOverride {
    bool force = false;
    int32_t expTime = 0;
    float gain = 0.0f;
};

void applyCameraControls(libcamera::ControlList &ctrls, const CameraConfig &cfg,
                         const ExposureOverride &ovr = {});

}
