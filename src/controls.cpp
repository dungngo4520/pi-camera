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

// Apply green/magenta WB shift to ColourGains. A positive shift (magenta)
// increases R and B relative to G; a negative shift (green) decreases them.
// The factor is 1.0 + shift * 0.02 (±9 → ±18% gain adjustment).
void applyWbGmShift(float &r, float &b, float gmShift) {
  if (gmShift == 0.0f)
    return;
  float factor = 1.0f + gmShift * 0.02f;
  r *= factor;
  b *= factor;
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
    float r = cfg.wbRedGain;
    float b = cfg.wbBlueGain;
    applyWbGmShift(r, b, cfg.wbGmShift);
    setManualGains(ctrls, r, b);
  } else if (cfg.wbKelvin > 0) {
    // Kelvin -> approximate R/B gains, manual AWB off.
    float r = 1.0f;
    float b = 1.0f;
    kelvinToGains(cfg.wbKelvin, r, b);
    applyWbGmShift(r, b, cfg.wbGmShift);
    setManualGains(ctrls, r, b);
  } else if (cfg.awbMode == "shade") {
    float r = 1.4f; // ~7500K: cool, boosted red
    float b = 0.7f;
    applyWbGmShift(r, b, cfg.wbGmShift);
    setManualGains(ctrls, r, b);
  } else if (cfg.awbMode == "flash") {
    float r = 1.05f; // ~5500K: slightly cool
    float b = 1.1f;
    applyWbGmShift(r, b, cfg.wbGmShift);
    setManualGains(ctrls, r, b);
  } else {
    ctrls.set(controls::AwbEnable, true);
    ctrls.set(controls::AwbLocked, false);
    if (auto mode = lookupAwb(cfg.awbMode)) {
      ctrls.set(controls::AwbMode, *mode);
    }
    // Apply G/M shift even in auto AWB mode by setting ColourGains
    // after AWB — libcamera will use these as post-AWB adjustments.
    if (cfg.wbGmShift != 0.0f) {
      float r = 1.0f;
      float b = 1.0f;
      applyWbGmShift(r, b, cfg.wbGmShift);
      const float gains[2] = {r, b};
      ctrls.set(controls::ColourGains, Span<const float, 2>(gains, 2));
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
    } else if (cfg.minShutterUs > 0) {
      // Minimum shutter speed for auto ISO: clamp the auto exposure time
      // to not exceed the user's minimum (i.e., don't go slower than this).
      // This is applied as a fixed ExposureTime floor — the AE algorithm
      // will use this as the maximum exposure time and raise ISO instead.
      ctrls.set(controls::ExposureTime,
                static_cast<int32_t>(std::min<uint64_t>(cfg.minShutterUs,
                                                        INT32_MAX)));
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
