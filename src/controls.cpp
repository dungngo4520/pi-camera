#include "controls.h"
#include "wb_utils.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>
#include <string_view>

namespace picamera {

using libcamera::ControlList;
using libcamera::Span;
namespace controls = libcamera::controls;

namespace {

// Manual WB: AWB off, fixed R/B gains.
void setManualGains(ControlList &ctrls, float r, float b) {
  ctrls.set(controls::AwbEnable, false);
  const float gains[2] = {r, b};
  ctrls.set(controls::ColourGains, Span<const float, 2>(gains, 2));
}

} // namespace

// lookupAwb() and kelvinToGains() are pure-logic helpers whose implementations
// live in wb_utils.h (libcamera-free, unit-tested). lookupAwb() wraps
// lookupAwbMode() and casts to the real libcamera enum so the hardware TUs
// keep their existing API. kelvinToGains() is provided inline by wb_utils.h.
std::optional<libcamera::controls::AwbModeEnum>
lookupAwb(std::string_view name) {
  if (auto mode = lookupAwbMode(name)) {
    return static_cast<libcamera::controls::AwbModeEnum>(*mode);
  }
  return std::nullopt;
}

void applyAwbControls(ControlList &ctrls, const CameraConfig &cfg) {
  if (!cfg.awbEnable) {
    setManualGains(ctrls, cfg.wbRedGain, cfg.wbBlueGain);
  } else if (cfg.wbKelvin > 0) {
    // Kelvin -> approximate R/B gains, manual AWB off.
    float r = 1.0f;
    float b = 1.0f;
    kelvinToGains(cfg.wbKelvin, r, b);
    setManualGains(ctrls, r, b);
  } else if (cfg.awbMode == "shade") {
    setManualGains(ctrls, 1.4f, 0.7f); // ~7500K: cool, boosted red
  } else if (cfg.awbMode == "flash") {
    setManualGains(ctrls, 1.05f, 1.1f); // ~5500K: slightly cool
  } else {
    ctrls.set(controls::AwbEnable, true);
    ctrls.set(controls::AwbLocked, false);
    if (auto mode = lookupAwb(cfg.awbMode)) {
      ctrls.set(controls::AwbMode, *mode);
    }
  }
}

void applyCameraControls(ControlList &ctrls, const CameraConfig &cfg,
                         const ExposureOverride &ovr) {
  // Shutter priority (manual shutter, auto gain) keeps AE on so the ISP
  // auto-adjusts gain to match the fixed shutter. Manual mode (both manual)
  // or AE explicitly disabled turns AE off.
  const bool manualShutter = ovr.force ? ovr.expTime > 0 : cfg.exposureTime > 0;
  const bool manualGain = ovr.force ? ovr.gain > 0.0f : cfg.analogueGain > 0.0f;
  const bool manualExposure = ovr.force || !cfg.aeEnable || manualGain;
  if (manualExposure) {
    ctrls.set(controls::AeEnable, false);
    int32_t expTime = ovr.force ? ovr.expTime
                                : static_cast<int32_t>(std::min<uint64_t>(
                                      cfg.exposureTime, INT32_MAX));
    if (expTime > 0) {
      ctrls.set(controls::ExposureTime, expTime);
    }
    float gain = ovr.force ? ovr.gain : cfg.analogueGain;
    if (gain > 0.0f) {
      // Clamp manual gain to the user's ISO range (ISO = gain*100).
      // libcamera has no AeAnalogueGainMin/Max, so this enforces
      // isoMin/isoMax in Manual exposure mode only.
      ctrls.set(controls::AnalogueGain,
                clampGainToIsoRange(gain, cfg.isoMin, cfg.isoMax));
    }
  } else {
    ctrls.set(controls::AeEnable, true);
    if (manualShutter) {
      ctrls.set(controls::ExposureTime, static_cast<int32_t>(std::min<uint64_t>(
                                            cfg.exposureTime, INT32_MAX)));
    }
    ctrls.set(controls::AeMeteringMode, static_cast<int32_t>(cfg.meteringMode));
    ctrls.set(controls::AeExposureMode,
              static_cast<int32_t>(cfg.aeExposureMode));
    ctrls.set(controls::AeConstraintMode,
              static_cast<int32_t>(cfg.aeConstraintMode));
    if (cfg.exposureValue != 0.0f) {
      ctrls.set(controls::ExposureValue, cfg.exposureValue);
    }
    if (cfg.antiFlicker && cfg.flickerPeriodUs > 0) {
      ctrls.set(controls::AeFlickerMode, static_cast<int32_t>(1));
      ctrls.set(controls::AeFlickerPeriod,
                static_cast<int32_t>(cfg.flickerPeriodUs));
    }
  }
  if (cfg.digitalGain > 0.0f) {
    ctrls.set(controls::DigitalGain, cfg.digitalGain);
  }
  applyAwbControls(ctrls, cfg);
  ctrls.set(controls::Brightness, cfg.brightness);
  ctrls.set(controls::Contrast, cfg.contrast);
  ctrls.set(controls::Saturation, cfg.saturation);
  ctrls.set(controls::Sharpness, cfg.sharpness);
  ctrls.set(controls::draft::NoiseReductionMode,
            static_cast<int32_t>(cfg.noiseReductionMode));
}

} // namespace picamera
