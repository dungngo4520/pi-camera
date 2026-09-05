#pragma once

#include "camera_config.h"
#include "wb_utils.h"

#include <libcamera/libcamera.h>
#include <optional>
#include <string_view>

namespace picamera {

// lookupAwb() returns the real libcamera enum (defined in controls.cpp, which
// links libcamera). kelvinToGains() is a pure-logic inline defined in
// wb_utils.h (libcamera-free, unit-testable on x86).
std::optional<libcamera::controls::AwbModeEnum>
lookupAwb(std::string_view name);

// Apply the AWB section of CameraConfig to a ControlList: manual gains,
// kelvin-derived gains, shade/flash presets, or auto mode with a named mode.
// Shared by applyCameraControls (camera.cpp) and DualStream::applyControls.
void applyAwbControls(libcamera::ControlList &ctrls, const CameraConfig &cfg);

struct ExposureOverride {
  bool force = false;
  int32_t expTime = 0;
  float gain = 0.0f;
};

void applyCameraControls(libcamera::ControlList &ctrls, const CameraConfig &cfg,
                         const ExposureOverride &ovr = {});

} // namespace picamera
